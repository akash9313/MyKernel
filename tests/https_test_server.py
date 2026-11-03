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

HTTPS_HTML_CONTENT = """<!DOCTYPE html>
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

# Alias for backwards compatibility
HTML_CONTENT = HTTPS_HTML_CONTENT

HTTP_HTML_CONTENT = """<!DOCTYPE html>
<html>
<head>
<title>AstraOS HTTP Web</title>
<style>
h1 { color: #38bdf8; text-align: center; }
.card { background-color: #0f172a; border: 1px solid #06b6d4; padding: 8px; margin: 6px; }
</style>
</head>
<body>
<h1>AstraOS HTTP 1.1 Verified</h1>
<div class="card">
<p>Local HTTP connection established over TCP port 8000.</p>
<p>Status code 200 OK and response parsing verified.</p>
</div>
</body>
</html>
"""

class AstraHTTPSServerHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == '/status/404':
            body = b"<html><head><title>404 Not Found</title></head><body><h1>404 Not Found</h1></body></html>"
            self.send_response(404)
        elif self.path == '/status/500':
            body = b"<html><head><title>500 Internal Error</title></head><body><h1>500 Server Error</h1></body></html>"
            self.send_response(500)
        else:
            body = HTTPS_HTML_CONTENT.encode('utf-8')
            self.send_response(200)

        self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Connection', 'close')
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format, *args):
        sys.stderr.write(f"[HTTPS SERVER] {format % args}\n")

class AstraHTTPServerHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == '/status/404':
            body = b"<html><head><title>404 Not Found</title></head><body><h1>404 Not Found</h1></body></html>"
            self.send_response(404)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Connection', 'close')
            self.end_headers()
            self.wfile.write(body)
        elif self.path == '/status/500':
            body = b"<html><head><title>500 Error</title></head><body><h1>500 Server Error</h1></body></html>"
            self.send_response(500)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Connection', 'close')
            self.end_headers()
            self.wfile.write(body)
        elif self.path == '/redirect':
            self.send_response(302)
            self.send_header('Location', '/')
            self.send_header('Content-Length', '0')
            self.send_header('Connection', 'close')
            self.end_headers()
        elif self.path == '/redirect_loop':
            self.send_response(302)
            self.send_header('Location', '/redirect_loop')
            self.send_header('Content-Length', '0')
            self.send_header('Connection', 'close')
            self.end_headers()
        elif self.path == '/large':
            # Generate a deterministic 4096-byte HTML document
            padding = "<!-- AstraOS large response padding block -->\n" * 70
            body = f"<html><head><title>Large Response</title></head><body><h1>Large Document</h1><p>{padding}</p></body></html>".encode('utf-8')
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Connection', 'close')
            self.end_headers()
            self.wfile.write(body)
        elif self.path == '/chunked':
            # Send RFC chunked response
            c1 = b"<h1>Chunked Page</h1>"
            c2 = b"<p>Decoded stream</p>"
            self.wfile.write(b"HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n")
            self.wfile.write(f"{len(c1):x}\r\n".encode('ascii') + c1 + b"\r\n")
            self.wfile.write(f"{len(c2):x}\r\n".encode('ascii') + c2 + b"\r\n")
            self.wfile.write(b"0\r\n\r\n")
        elif self.path == '/truncated_chunked':
            self.wfile.write(b"HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n")
            self.wfile.write(b"40\r\nTruncated incomplete chunk")
            # Immediately finish without closing chunk
        elif self.path == '/bad_cl':
            # Send conflicting Content-Length headers
            self.wfile.write(b"HTTP/1.1 200 OK\r\nContent-Length: 10\r\nContent-Length: 30\r\nConnection: close\r\n\r\nConflictingCL")
        elif self.path == '/truncated':
            # Send Content-Length: 200 but only send 25 bytes
            self.wfile.write(b"HTTP/1.1 200 OK\r\nContent-Length: 200\r\nConnection: close\r\n\r\n<html>Truncated Body")
        elif self.path == '/malformed':
            # Send raw non-standard status line without valid HTTP header format
            raw = b"MALFORMED_HEADER_DATA_NO_HTTP\r\n\r\n<html><body>Malformed Raw Response</body></html>"
            self.wfile.write(raw)
        else:
            body = HTTP_HTML_CONTENT.encode('utf-8')
            self.send_response(200)
            self.send_header('Content-Type', 'text/html; charset=utf-8')
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Connection', 'close')
            self.end_headers()
            self.wfile.write(body)

    def log_message(self, format, *args):
        sys.stderr.write(f"[HTTP SERVER] {format % args}\n")

class ReusableTCPServer(socketserver.TCPServer):
    allow_reuse_address = True

def start_server(port=8443, host='0.0.0.0'):
    if not os.path.exists(SERVER_CRT) or not os.path.exists(SERVER_KEY):
        raise FileNotFoundError(f"Server certificate or key not found in {CERTS_DIR}")

    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_3
    ctx.maximum_version = ssl.TLSVersion.TLSv1_3
    ctx.load_cert_chain(certfile=SERVER_CRT, keyfile=SERVER_KEY)

    server = ReusableTCPServer((host, port), AstraHTTPSServerHandler)
    server.socket = ctx.wrap_socket(server.socket, server_side=True)

    print(f"[HTTPS SERVER] TLS 1.3 server listening on https://{host}:{port}/")
    sys.stdout.flush()
    return server

def start_http_server(port=8000, host='0.0.0.0'):
    server = ReusableTCPServer((host, port), AstraHTTPServerHandler)
    print(f"[HTTP SERVER] HTTP 1.1 server listening on http://{host}:{port}/")
    sys.stdout.flush()
    return server

def run_server_thread(port=8443, host='0.0.0.0', http_port=8000):
    https_server = start_server(port, host)
    t_https = threading.Thread(target=https_server.serve_forever, daemon=True)
    t_https.start()

    http_server = start_http_server(http_port, host)
    t_http = threading.Thread(target=http_server.serve_forever, daemon=True)
    t_http.start()

    return https_server, http_server

if __name__ == '__main__':
    https_port = int(sys.argv[1]) if len(sys.argv) > 1 else 8443
    http_port = int(sys.argv[2]) if len(sys.argv) > 2 else 8000
    s_https, s_http = run_server_thread(port=https_port, http_port=http_port)
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("\n[SERVERS] Shutting down.")
        s_https.shutdown()
        s_http.shutdown()
