import os
import sys
import ssl
import http.server
import socketserver
import threading
import time

BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CERTS_DIR = os.path.join(BASE_DIR, 'build', 'certs')
SERVER_CRT = os.path.join(CERTS_DIR, 'server.crt')
SERVER_KEY = os.path.join(CERTS_DIR, 'server.key')

HTML_CONTENT = """<!DOCTYPE html>
<html>
<head>
<title>AstraOS Secure Web</title>
<style>
h1 { color: #2dd4bf; text-align: center; }
.card { background-color: #1e293b; border: 1px solid #38bdf8; padding: 8px; margin: 6px; }
.sec-badge { color: #22c55e; font-weight: bold; }
</style>
</head>
<body>
<h1>AstraOS TLS 1.3 Verified</h1>
<div class="card">
<p class="sec-badge">Secure HTTPS connection established over TLS 1.3 (ChaCha20-Poly1305).</p>
<p>Certificate verified against embedded Root CA and RTC clock.</p>
</div>
<hr>
<p>CSS stylesheets and layout rendering active.</p>
</body>
</html>
"""

class AstraHTTPSServerHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = HTML_CONTENT.encode('utf-8')
        self.send_response(200)
        self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Connection', 'close')
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format, *args):
        sys.stderr.write(f"[HTTPS SERVER] {format % args}\n")

def start_server(port=8443, host='0.0.0.0'):
    if not os.path.exists(SERVER_CRT) or not os.path.exists(SERVER_KEY):
        raise FileNotFoundError(f"Server certificate or key not found in {CERTS_DIR}")

    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_3
    ctx.maximum_version = ssl.TLSVersion.TLSv1_3
    ctx.load_cert_chain(certfile=SERVER_CRT, keyfile=SERVER_KEY)

    server = socketserver.TCPServer((host, port), AstraHTTPSServerHandler)
    server.socket = ctx.wrap_socket(server.socket, server_side=True)

    print(f"[HTTPS SERVER] TLS 1.3 server listening on https://{host}:{port}/")
    sys.stdout.flush()
    return server

def run_server_thread(port=8443, host='0.0.0.0'):
    server = start_server(port, host)
    t = threading.Thread(target=server.serve_forever, daemon=True)
    t.start()
    return server, t

if __name__ == '__main__':
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8443
    server = start_server(port=port)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[HTTPS SERVER] Shutting down.")
        server.shutdown()
