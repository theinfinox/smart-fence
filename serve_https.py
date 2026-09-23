#!/usr/bin/env python3
"""
FenceGuard AI - Dual-Mode Local Development Server
Provides:
  1. Plain HTTP on port 8000 (http://localhost:8000)
     -> Zero SSL certificate errors! Chrome treats http://localhost as a Secure Context
        for Web Bluetooth & Camera out of the box!
  2. Secure HTTPS on port 8443 (https://<local-ip>:8443)
     -> For testing on mobile smartphones over local Wi-Fi.

Usage:
    python serve_https.py
"""

import http.server
import ssl
import os
import socket
import subprocess
import threading
import sys
import time

HTTP_PORT = 8000
HTTPS_PORT = 8443
CERT_FILE = 'cert.pem'
KEY_FILE = 'key.pem'

def get_local_ip():
    """Retrieve local Wi-Fi IP address so smartphone can connect."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(('10.255.255.255', 1))
        ip = s.getsockname()[0]
    except Exception:
        ip = '127.0.0.1'
    finally:
        s.close()
    return ip

def ensure_ssl_certificates():
    """Generate self-signed certificate if not present."""
    if not (os.path.exists(CERT_FILE) and os.path.exists(KEY_FILE)):
        print("[*] Generating self-signed SSL certificates for HTTPS...")
        try:
            cmd = [
                'openssl', 'req', '-new', '-x509', '-keyout', KEY_FILE,
                '-out', CERT_FILE, '-days', '365', '-nodes',
                '-subj', '/CN=FenceGuard-Local'
            ]
            subprocess.run(cmd, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            print("[+] SSL certificates generated successfully via OpenSSL.")
            return True
        except Exception:
            try:
                from cryptography import x509
                from cryptography.x509.oid import NameOID
                from cryptography.hazmat.primitives import hashes
                from cryptography.hazmat.primitives.asymmetric import rsa
                from cryptography.hazmat.primitives import serialization
                import datetime

                key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
                subject = issuer = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, u"FenceGuard-Local")])
                cert = x509.CertificateBuilder().subject_name(subject).issuer_name(issuer).public_key(
                    key.public_key()
                ).serial_number(x509.random_serial_number()).not_valid_before(
                    datetime.datetime.utcnow()
                ).not_valid_after(
                    datetime.datetime.utcnow() + datetime.timedelta(days=365)
                ).sign(key, hashes.SHA256())

                with open(KEY_FILE, "wb") as f:
                    f.write(key.bytes_to_pem(serialization.PrivateFormat.TraditionalOpenSSL, serialization.NoEncryption()))
                with open(CERT_FILE, "wb") as f:
                    f.write(cert.public_bytes(serialization.Encoding.PEM))
                print("[+] SSL certificates generated successfully via Python cryptography.")
                return True
            except Exception as e:
                print("[-] Could not automatically generate SSL certificates:")
                print(f"    {e}")
                return False
    return True

class CustomHandler(http.server.SimpleHTTPRequestHandler):
    extensions_map = http.server.SimpleHTTPRequestHandler.extensions_map.copy()
    extensions_map.update({
        '.webmanifest': 'application/manifest+json',
        '.json': 'application/json',
        '.svg': 'image/svg+xml',
        '.js': 'application/javascript',
        '.css': 'text/css',
    })

    def end_headers(self):
        # Prevent aggressive caching of development files
        self.send_header('Cache-Control', 'no-cache, no-store, must-revalidate')
        self.send_header('Pragma', 'no-cache')
        self.send_header('Expires', '0')
        super().end_headers()

def run_http_server():
    try:
        server_address = ('0.0.0.0', HTTP_PORT)
        httpd = http.server.HTTPServer(server_address, CustomHandler)
        httpd.serve_forever()
    except Exception as e:
        print(f"[HTTP] Port {HTTP_PORT} notice: {e}")

def run_https_server():
    if not (os.path.exists(CERT_FILE) and os.path.exists(KEY_FILE)):
        return
    try:
        server_address = ('0.0.0.0', HTTPS_PORT)
        httpd = http.server.HTTPServer(server_address, CustomHandler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(certfile=CERT_FILE, keyfile=KEY_FILE)
        httpd.socket = context.wrap_socket(httpd.socket, server_side=True)
        httpd.serve_forever()
    except Exception as e:
        print(f"[HTTPS] Port {HTTPS_PORT} notice: {e}")

def main():
    local_ip = get_local_ip()
    ensure_ssl_certificates()

    # Start HTTP server thread (Port 8000)
    t_http = threading.Thread(target=run_http_server, daemon=True)
    t_http.start()

    # Start HTTPS server thread (Port 8443)
    t_https = threading.Thread(target=run_https_server, daemon=True)
    t_https.start()

    print("\n" + "=" * 65)
    print("  ⚡ FenceGuard AI: Dual-Mode Development Server Active")
    print("=" * 65)
    print(f"  [1] Recommended (Desktop Zero-SSL Warning):")
    print(f"      👉 http://localhost:{HTTP_PORT}")
    print(f"      (Chrome treats localhost as Secure: Web Bluetooth & Cam work!)")
    print("-" * 65)
    print(f"  [2] Mobile Smartphone Wi-Fi Access (HTTPS):")
    print(f"      👉 https://{local_ip}:{HTTPS_PORT}")
    print(f"      (Tap 'Advanced' -> 'Proceed to site' when prompted)")
    print("=" * 65 + "\n")

    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("\n[*] Server shutdown.")

if __name__ == '__main__':
    main()
