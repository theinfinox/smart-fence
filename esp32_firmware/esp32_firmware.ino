/**
 * ============================================================================
 * FenceGuard AI - Smart Electric & Laser Fence Safety System
 * Microcontroller Firmware for Standard ESP32 (WROOM-32 / NodeMCU-32S)
 * ============================================================================
 * 
 * Hardware Description:
 * - Microcontroller: ESP32 Dev Module (WROOM-32)
 * - BLE Profile: Nordic UART Service (NUS)
 * - Actuators & Inputs:
 *   * GPIO 23: 5V Relay Module (Controls Laser Fence / Energizer Power Rail)
 *   * GPIO 19: Active Buzzer & Warning Hazard Strobe LED
 *   * GPIO 02: Onboard Status Blue LED (Heartbeat / BLE State)
 *   * GPIO 04: Optional Physical Emergency Stop (E-Stop) Push Button (Active LOW)
 * 
 * Communication Protocol (BLE Nordic UART Service):
 * - Service UUID: 6e400001-b5a3-f393-e0a9-e50e24dcca9e
 * - RX Char UUID: 6e400002-b5a3-f393-e0a9-e50e24dcca9e (PWA -> ESP32 Write)
 * - TX Char UUID: 6e400003-b5a3-f393-e0a9-e50e24dcca9e (ESP32 -> PWA Notify)
 * 
 * Accepted Commands from Mobile PWA:
 * - "TRIP\n" : Human detected! Cut relay power immediately. Sound buzzer alarm.
 * - "ARM\n"  : Perimeter clear / Animal deterrence. Energize laser fence relay.
 * - "PING\n" : Heartbeat keepalive ping from smartphone. Responds with status.
 * 
 * Fail-Safe & Robustness Design:
 * - System boots in SAFE DE-ENERGIZED state (Relay OFF).
 * - BLE Disconnect: Relay instantly de-energizes within 50ms and advertising restarts.
 * - Watchdog Timeout: If no ping/command received for > 6s, auto-trips to SAFE mode.
 * - Relay Dwell Filter: Minimum 300ms dwell time prevents mechanical coil chatter.
 * - Packet Accumulator: Newline-delimited command accumulator handles fragmented RF writes.
 * - Physical E-Stop: GPIO 4 button press immediately trips relay independent of phone.
 * 
 * Baud Rate: 115200 bps
 * ============================================================================
 */

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// ----------------------------------------------------------------------------
// PIN CONFIGURATION & HARDWARE CONSTANTS
// ----------------------------------------------------------------------------
#define PIN_RELAY         23    // GPIO 23 -> 5V Relay IN pin (Switches Laser Fence)
#define PIN_BUZZER        19    // GPIO 19 -> Active 5V Buzzer / Warning Strobe LED
#define PIN_STATUS_LED     2    // GPIO 2  -> Built-in Blue Status LED
#define PIN_MANUAL_ESTOP   4    // GPIO 4  -> Optional Physical E-Stop Button (to GND)

// Most 5V hobby relay modules (Songle optoisolated) are ACTIVE LOW.
// If your relay turns ON when pin is HIGH, change this to 'false'.
#define RELAY_ACTIVE_LOW true

// Fail-Safe Watchdog Timeout in milliseconds.
// If smartphone app crashes or freezes for > 6 seconds, fence auto-trips to safe.
#define WATCHDOG_TIMEOUT_MS 6000

// Relay minimum dwell time in milliseconds (prevents coil chatter/arcing)
#define RELAY_MIN_DWELL_MS 300

// ----------------------------------------------------------------------------
// NORDIC UART SERVICE (NUS) UUID DEFINITIONS
// ----------------------------------------------------------------------------
#define BLE_DEVICE_NAME        "FenceGuard-ESP32"
#define SERVICE_UUID           "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define CHARACTERISTIC_UUID_RX "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
#define CHARACTERISTIC_UUID_TX "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

// ----------------------------------------------------------------------------
// GLOBAL STATE VARIABLES
// ----------------------------------------------------------------------------
BLEServer* pServer = nullptr;
BLECharacteristic* pTxCharacteristic = nullptr;
BLECharacteristic* pRxCharacteristic = nullptr;

bool deviceConnected = false;
bool oldDeviceConnected = false;

enum FenceState {
  STATE_SAFE_TRIPPED = 0,   // Relay OFF (Laser de-energized), Alarm ON
  STATE_ARMED = 1           // Relay ON (Laser energized), Alarm OFF
};

volatile FenceState currentFenceState = STATE_SAFE_TRIPPED;
unsigned long lastHeartbeatTime = 0;
unsigned long lastRelayToggleTime = 0;

// Non-blocking LED blink timer
unsigned long lastLedToggleTime = 0;
bool ledState = false;

// Non-blocking alarm buzzer tone generator timer
unsigned long lastBuzzerToggleTime = 0;
bool buzzerState = false;

// Physical E-Stop button debouncing
unsigned long lastEstopCheckTime = 0;

// ----------------------------------------------------------------------------
// ACTUATION HELPER FUNCTIONS
// ----------------------------------------------------------------------------

/**
 * Energizes or de-energizes the 5V Relay controlling the Laser Fence.
 * Enforces minimum dwell time to eliminate mechanical coil chatter.
 */
void setRelayPower(bool energize) {
  unsigned long now = millis();
  
  // Safety cutoff always executes immediately; re-arming obeys dwell filter
  if (energize && (now - lastRelayToggleTime < RELAY_MIN_DWELL_MS)) {
    return;
  }

  if (RELAY_ACTIVE_LOW) {
    digitalWrite(PIN_RELAY, energize ? LOW : HIGH);
  } else {
    digitalWrite(PIN_RELAY, energize ? HIGH : LOW);
  }

  lastRelayToggleTime = now;
}

/**
 * Transitions fence to SAFE TRIP state:
 * - Cuts off relay power immediately to protect human lives.
 * - Sounds audio/visual warning alarm.
 */
void triggerSafeTrip(const char* reason) {
  currentFenceState = STATE_SAFE_TRIPPED;
  setRelayPower(false); // DE-ENERGIZE RELAY IMMEDIATELY

  Serial.println();
  Serial.println("==================================================");
  Serial.printf("[ALERT] SAFE TRIP ACTIVATED! Reason: %s\n", reason);
  Serial.println(">> 5V Relay: DE-ENERGIZED (Laser Fence CUT OFF)");
  Serial.println(">> Buzzer & Warning LED: ALARM SOUNDING");
  Serial.println("==================================================");

  // Notify smartphone over BLE TX
  if (deviceConnected && pTxCharacteristic != nullptr) {
    String telemetry = "STATUS:TRIPPED," + String(reason);
    pTxCharacteristic->setValue(telemetry.c_str());
    pTxCharacteristic->notify();
  }
}

/**
 * Transitions fence to ARMED state:
 * - Energizes relay to power the laser light fence perimeter.
 * - Silences buzzer alarm.
 */
void armFence() {
  currentFenceState = STATE_ARMED;
  setRelayPower(true); // ENERGIZE RELAY
  digitalWrite(PIN_BUZZER, LOW); // Silence alarm

  Serial.println();
  Serial.println("--------------------------------------------------");
  Serial.println("[SYSTEM] FENCE ARMED & ENERGIZED");
  Serial.println(">> 5V Relay: ENERGIZED (Laser Light Beams ON)");
  Serial.println(">> Buzzer: SILENT (Monitoring perimeter)");
  Serial.println("--------------------------------------------------");

  // Notify smartphone over BLE TX
  if (deviceConnected && pTxCharacteristic != nullptr) {
    pTxCharacteristic->setValue("STATUS:ARMED");
    pTxCharacteristic->notify();
  }
}

// ----------------------------------------------------------------------------
// BLE SERVER CALLBACKS (CONNECTION & DISCONNECTION FAIL-SAFE)
// ----------------------------------------------------------------------------
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) override {
    deviceConnected = true;
    lastHeartbeatTime = millis();
    Serial.println("\n[BLE] Smartphone Connected via Nordic UART Service!");
    digitalWrite(PIN_STATUS_LED, HIGH);
  }

  void onDisconnect(BLEServer* pServer) override {
    deviceConnected = false;
    Serial.println("\n[BLE WARNING] Smartphone Disconnected!");
    
    // CRITICAL FAIL-SAFE:
    // If the phone loses connection, the fence MUST fail safe immediately.
    triggerSafeTrip("BLE Disconnect Fail-Safe");
    digitalWrite(PIN_STATUS_LED, LOW);
  }
};

// ----------------------------------------------------------------------------
// BLE RX CHARACTERISTIC CALLBACKS (FRAGMENTATION-PROOF COMMAND ACCUMULATOR)
// ----------------------------------------------------------------------------
class RxCallbacks : public BLECharacteristicCallbacks {
  String commandBuffer = "";

  void onWrite(BLECharacteristic* pCharacteristic) override {
    String rxChunk = pCharacteristic->getValue().c_str();

    // Accumulate characters until newline/carriage return to prevent partial packet parsing
    for (unsigned int i = 0; i < rxChunk.length(); i++) {
      char c = rxChunk[i];

      if (c == '\n' || c == '\r') {
        if (commandBuffer.length() > 0) {
          commandBuffer.trim();
          commandBuffer.toUpperCase();
          processParsedCommand(commandBuffer);
          commandBuffer = "";
        }
      } else {
        if (commandBuffer.length() < 32) { // Safeguard buffer length
          commandBuffer += c;
        }
      }
    }
  }

  void processParsedCommand(String cmd) {
    Serial.print("[BLE RX Command] Received: \"");
    Serial.print(cmd);
    Serial.println("\"");

    lastHeartbeatTime = millis();

    if (cmd == "TRIP") {
      triggerSafeTrip("Human Detected by AI Vision");
    } 
    else if (cmd == "ARM") {
      armFence();
    } 
    else if (cmd == "PING") {
      if (pTxCharacteristic != nullptr) {
        const char* response = (currentFenceState == STATE_ARMED) ? "PONG:ARMED" : "PONG:TRIP";
        pTxCharacteristic->setValue(response);
        pTxCharacteristic->notify();
      }
    } 
    else {
      Serial.printf("[BLE RX] Unknown command: %s\n", cmd.c_str());
    }
  }
};

// ----------------------------------------------------------------------------
// HARDWARE INITIALIZATION (SETUP)
// ----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("\n\n");
  Serial.println("************************************************************");
  Serial.println("*      FenceGuard AI - Smart Laser Fence Safety System     *");
  Serial.println("*           ESP32 Microcontroller Actuator Firmware        *");
  Serial.println("************************************************************");

  // Configure Output GPIO Pins
  pinMode(PIN_RELAY, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_STATUS_LED, OUTPUT);

  // Configure Input GPIO Pins (Physical Emergency Stop Push Button)
  pinMode(PIN_MANUAL_ESTOP, INPUT_PULLUP);

  // Initial Boot State: ALWAYS FAIL-SAFE (Relay OFF, Laser Fence OFF)
  setRelayPower(false);
  digitalWrite(PIN_BUZZER, LOW);
  digitalWrite(PIN_STATUS_LED, LOW);

  Serial.printf("[INIT] Relay configured on GPIO %d (Active %s)\n", PIN_RELAY, RELAY_ACTIVE_LOW ? "LOW" : "HIGH");
  Serial.printf("[INIT] Buzzer/Strobe configured on GPIO %d\n", PIN_BUZZER);
  Serial.printf("[INIT] Built-in LED configured on GPIO %d\n", PIN_STATUS_LED);
  Serial.printf("[INIT] Physical E-Stop Button configured on GPIO %d (Pull-Up Active)\n", PIN_MANUAL_ESTOP);
  Serial.println("[INIT] Initial State: RELAY DE-ENERGIZED (SAFE)");

  // Initialize ESP32 BLE Stack
  Serial.print("[INIT] Initializing Bluetooth Low Energy (BLE)... ");
  BLEDevice::init(BLE_DEVICE_NAME);

  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  BLEService* pService = pServer->createService(SERVICE_UUID);

  pTxCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID_TX,
    BLECharacteristic::PROPERTY_NOTIFY
  );
  pTxCharacteristic->addDescriptor(new BLE2902());

  pRxCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID_RX,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
  );
  pRxCharacteristic->setCallbacks(new RxCallbacks());

  pService->start();

  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  pAdvertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();

  Serial.println("SUCCESS!");
  Serial.println("------------------------------------------------------------");
  Serial.printf("[BLE] Broadcasting as: \"%s\"\n", BLE_DEVICE_NAME);
  Serial.printf("[BLE] Nordic UART Service: %s\n", SERVICE_UUID);
  Serial.println("[BLE] Waiting for smartphone PWA connection...");
  Serial.println("------------------------------------------------------------\n");

  lastHeartbeatTime = millis();
}

// ----------------------------------------------------------------------------
// MAIN SUPERVISORY LOOP (WATCHDOG, E-STOP & NON-BLOCKING ALARM SCHEDULING)
// ----------------------------------------------------------------------------
void loop() {
  unsigned long now = millis();

  // 1. Connection Transition Handling & Re-Advertising
  if (!deviceConnected && oldDeviceConnected) {
    delay(100);
    pServer->startAdvertising();
    Serial.println("[BLE] Restarted BLE advertising for reconnection...");
    oldDeviceConnected = deviceConnected;
  }
  if (deviceConnected && !oldDeviceConnected) {
    oldDeviceConnected = deviceConnected;
  }

  // 2. Physical Hardware E-Stop Button Check (Debounced every 50ms)
  if (now - lastEstopCheckTime >= 50) {
    lastEstopCheckTime = now;
    if (digitalRead(PIN_MANUAL_ESTOP) == LOW) {
      if (currentFenceState == STATE_ARMED) {
        triggerSafeTrip("Physical Hardware E-Stop Button Pressed");
      }
    }
  }

  // 3. Watchdog Fail-Safe Check:
  // If connected, ensure the phone is actively sending heartbeats/commands.
  if (deviceConnected) {
    if (currentFenceState == STATE_ARMED && (now - lastHeartbeatTime > WATCHDOG_TIMEOUT_MS)) {
      triggerSafeTrip("Watchdog Heartbeat Timeout");
    }
  }

  // 4. Actuator State Execution
  if (currentFenceState == STATE_SAFE_TRIPPED) {
    setRelayPower(false);

    // Rapid warning chirp (120ms ON / 120ms OFF)
    if (now - lastBuzzerToggleTime >= 120) {
      lastBuzzerToggleTime = now;
      buzzerState = !buzzerState;
      digitalWrite(PIN_BUZZER, buzzerState ? HIGH : LOW);
    }

  } else {
    setRelayPower(true);
    digitalWrite(PIN_BUZZER, LOW);
  }

  // 5. Onboard Status LED Heartbeat:
  // - Disconnected: Fast flashing (150ms)
  // - Connected & Armed: Solid ON
  // - Connected & Tripped: Slow pulse (400ms)
  if (!deviceConnected) {
    if (now - lastLedToggleTime >= 150) {
      lastLedToggleTime = now;
      ledState = !ledState;
      digitalWrite(PIN_STATUS_LED, ledState ? HIGH : LOW);
    }
  } else {
    if (currentFenceState == STATE_ARMED) {
      digitalWrite(PIN_STATUS_LED, HIGH);
    } else {
      if (now - lastLedToggleTime >= 400) {
        lastLedToggleTime = now;
        ledState = !ledState;
        digitalWrite(PIN_STATUS_LED, ledState ? HIGH : LOW);
      }
    }
  }

  delay(5);
}
