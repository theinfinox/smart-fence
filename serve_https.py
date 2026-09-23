#!/usr/bin/env python3
"""
FenceGuard AI - Local HTTPS Development Server
Generates a self-signed SSL certificate on the fly and serves
the current directory over HTTPS on port 8443.

Usage:
    python serve_https.py
"""

import http.server
import ssl
import os
import socket
import subprocess
import sys

PORT = 8443
CERT_FILE = 'cert.pem'
KEY_FILE = 'key.pem'

def get_local_ip():
    """Retrieve local Wi-Fi IP address so smartphone can connect."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        # doesn't need to be reachable
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
            # Try openssl command if available
            cmd = [
                'openssl', 'req', '-new', '-x509', '-keyout', KEY_FILE,
                '-out', CERT_FILE, '-days', '365', '-nodes',
                '-subj', '/CN=FenceGuard-Local'
            ]
            subprocess.run(cmd, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            print("[+] SSL certificates generated successfully via OpenSSL.")
        except Exception:
            # Fallback using Python cryptography module if installed
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
                    f.write(key.private_bytes(
                        encoding=serialization.Encoding.PEM,
                        format=serialization.PrivateFormat.TraditionalOpenSSL,
                        encryption_algorithm=serialization.NoEncryption()
                    ))
                with open(CERT_FILE, "wb") as f:
                    f.write(cert.public_bytes(serialization.Encoding.PEM))
                print("[+] SSL certificates generated successfully via Python cryptography.")
            except Exception as e:
                print("[-] Could not automatically generate SSL certificates:")
                print(f"    {e}")
                print("\nTip: You can test directly on Chrome desktop via http://localhost:8000")
                print("     or host on GitHub Pages / Vercel for zero-config HTTPS.")
                return False
    return True

def run_server():
    local_ip = get_local_ip()
    has_ssl = ensure_ssl_certificates()

    Handler = http.server.SimpleHTTPRequestHandler
    Handler.extensions_map.update({
        '.webmanifest': 'application/manifest+json',
        '.json': 'application/json',
        '.svg': 'image/svg+xml',
    })

    if has_ssl and os.path.exists(CERT_FILE) and os.path.exists(KEY_FILE):
        server_address = ('0.0.0.0', PORT)
        httpd = http.server.HTTPServer(server_address, Handler)

        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(certfile=CERT_FILE, keyfile=KEY_FILE)
        httpd.socket = context.wrap_socket(httpd.socket, server_side=True)

        print("\n" + "=" * 60)
        print("  ⚡ FenceGuard AI: Secure Local HTTPS Server Active")
        print("=" * 60)
        print(f"  > Desktop Access:    https://localhost:{PORT}")
        print(f"  > Smartphone Access: https://{local_ip}:{PORT}")
        print("-" * 60)
        print("  NOTE: Your browser will show a 'Self-Signed Certificate' warning.")
        print("  Click 'Advanced' -> 'Proceed to site' to continue.")
        print("=" * 60 + "\n")
    else:
        # Fallback to plain HTTP on port 8000 (works on localhost)
        server_address = ('0.0.0.0', 8000)
        httpd = http.server.HTTPServer(server_address, Handler)
        print("\n" + "=" * 60)
        print("  ⚡ FenceGuard AI: Local HTTP Server Active")
        print("=" * 60)
        print("  > Localhost Access:  http://localhost:8000")
        print("  (Note: Web Bluetooth requires localhost or HTTPS)")
        print("=" * 60 + "\n")

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n[*] Server stopped.")

if __name__ == '__main__':
    run_server()
