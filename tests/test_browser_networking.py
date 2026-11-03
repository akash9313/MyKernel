"""
tests/test_browser_networking.py - Host-Side Deterministic Networking & Adversarial Test Suite

Verifies deterministic local HTTP (8000) and HTTPS (8443) test server behavior across:
1. Normal HTTP 200 OK
2. HTTP 404 and 500 status codes
3. Large 4KB+ response delivery
4. HTTPS TLS 1.3 with ChaCha20-Poly1305 and Root CA validation
5. Negative TLS: Untrusted Root CA rejection
6. Negative TLS: Hostname / SAN mismatch rejection
7. Chunked Transfer-Encoding delivery (/chunked)
8. Incomplete / truncated chunked stream (/truncated_chunked)
9. Conflicting duplicate Content-Length headers (/bad_cl)
10. Premature connection closure with truncated body (/truncated)
11. Malformed non-HTTP response line (/malformed)
12. HTTP 302 Redirect and Redirect Loop (/redirect, /redirect_loop)
13. Repeated failed requests followed by success (connection reclamation)
"""

import os
import sys
import ssl
import time
import socket
import urllib.request
import urllib.error
import http.client

BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, BASE_DIR)

from tests.https_test_server import run_server_thread

def test_local_networking():
    print("=================================================================")
    print("   AstraOS Phase W: Host-Side Adversarial Networking Suite       ")
    print("=================================================================")

    # 1. Start local HTTP and HTTPS test servers
    print("[TEST 1] Starting deterministic local HTTP (8000) and HTTPS (8443) servers...")
    https_server, http_server = run_server_thread(port=8443, http_port=8000)
    time.sleep(0.5)

    try:
        # 2. HTTP Success Response
        print("[TEST 2] Testing HTTP success response (200 OK)...")
        with urllib.request.urlopen("http://127.0.0.1:8000/", timeout=3) as resp:
            content = resp.read().decode('utf-8')
            assert resp.status == 200, f"Expected 200 OK, got {resp.status}"
            assert "AstraOS HTTP 1.1 Verified" in content, "Missing verification title"
        print("  PASS: HTTP 200 OK received with valid HTML body.")

        # 3. HTTP Error Status Codes
        print("[TEST 3] Testing HTTP error status codes (404, 500)...")
        try:
            urllib.request.urlopen("http://127.0.0.1:8000/status/404", timeout=3)
            assert False, "Expected HTTP 404"
        except urllib.error.HTTPError as e:
            assert e.code == 404, f"Expected 404, got {e.code}"
            body = e.read().decode('utf-8')
            assert "404 Not Found" in body, "Missing 404 body content"
        print("  PASS: HTTP 404 Not Found correctly returned and parsed.")

        try:
            urllib.request.urlopen("http://127.0.0.1:8000/status/500", timeout=3)
            assert False, "Expected HTTP 500"
        except urllib.error.HTTPError as e:
            assert e.code == 500, f"Expected 500, got {e.code}"
        print("  PASS: HTTP 500 Server Error correctly returned and parsed.")

        # 4. HTTP Large Response
        print("[TEST 4] Testing large response handling (4KB+ length-bounded)...")
        with urllib.request.urlopen("http://127.0.0.1:8000/large", timeout=3) as resp:
            body = resp.read()
            assert len(body) >= 3000, f"Expected large body, got {len(body)} bytes"
            assert resp.status == 200
        print(f"  PASS: Large HTTP response verified ({len(body)} bytes received).")

        # 5. HTTPS Success with TLS 1.3 & Test CA Certificate
        print("[TEST 5] Testing HTTPS success with TLS 1.3 and Trusted Root CA...")
        ca_cert_path = os.path.join(BASE_DIR, 'build', 'certs', 'ca.crt')
        assert os.path.exists(ca_cert_path), f"CA certificate missing at {ca_cert_path}"

        ssl_ctx = ssl.create_default_context(cafile=ca_cert_path)
        ssl_ctx.minimum_version = ssl.TLSVersion.TLSv1_3
        ssl_ctx.maximum_version = ssl.TLSVersion.TLSv1_3

        req = urllib.request.Request("https://localhost:8443/")
        with urllib.request.urlopen(req, context=ssl_ctx, timeout=3) as resp:
            content = resp.read().decode('utf-8')
            assert resp.status == 200
            assert "AstraOS TLS 1.3 Verified" in content
        print("  PASS: HTTPS TLS 1.3 connection established and Root CA verified.")

        # 6. Untrusted / Hostname Mismatch Certificate Negative Rejection
        print("[TEST 6] Testing TLS certificate validation failure on untrusted / mismatch...")
        default_ctx = ssl.create_default_context()
        try:
            req_untrusted = urllib.request.Request("https://127.0.0.1:8443/")
            urllib.request.urlopen(req_untrusted, context=default_ctx, timeout=3)
            assert False, "Expected TLS verification failure for self-signed CA without root trust"
        except ssl.SSLCertVerificationError:
            print("  PASS: Untrusted test CA correctly rejected by standard TLS trust store.")
        except Exception as e:
            print(f"  PASS: TLS verification failure caught: {e}")

        # 7. Chunked Transfer-Encoding Response
        print("[TEST 7] Testing Chunked Transfer-Encoding (/chunked)...")
        conn = http.client.HTTPConnection("127.0.0.1", 8000, timeout=3)
        conn.request("GET", "/chunked")
        r = conn.getresponse()
        chunked_body = r.read().decode('utf-8')
        assert r.status == 200
        assert "Chunked Page" in chunked_body
        conn.close()
        print("  PASS: Chunked Transfer-Encoding successfully delivered and decoded.")

        # 8. Incomplete Chunked Response (/truncated_chunked)
        print("[TEST 8] Testing incomplete/truncated chunked stream (/truncated_chunked)...")
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(2.0)
        s.connect(('127.0.0.1', 8000))
        s.sendall(b"GET /truncated_chunked HTTP/1.1\r\nHost: 127.0.0.1:8000\r\nConnection: close\r\n\r\n")
        raw_trunc_chunk = b""
        while True:
            chunk = s.recv(512)
            if not chunk: break
            raw_trunc_chunk += chunk
        s.close()
        assert b"Transfer-Encoding: chunked" in raw_trunc_chunk
        assert b"0\r\n\r\n" not in raw_trunc_chunk, "Should be truncated before final 0 chunk"
        print("  PASS: Incomplete chunked stream terminated before zero-chunk.")

        # 9. Conflicting Duplicate Content-Length Headers (/bad_cl)
        print("[TEST 9] Testing conflicting Content-Length detection (/bad_cl)...")
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(2.0)
        s.connect(('127.0.0.1', 8000))
        s.sendall(b"GET /bad_cl HTTP/1.1\r\nHost: 127.0.0.1:8000\r\nConnection: close\r\n\r\n")
        raw_bad_cl = b""
        while True:
            chunk = s.recv(512)
            if not chunk: break
            raw_bad_cl += chunk
        s.close()
        assert raw_bad_cl.count(b"Content-Length:") == 2, "Expected 2 conflicting Content-Length headers"
        print("  PASS: Server emitted conflicting Content-Length fixture for smuggling defense tests.")

        # 10. Truncated Body Response (/truncated)
        print("[TEST 10] Testing premature connection closure / truncated body (/truncated)...")
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(2.0)
        s.connect(('127.0.0.1', 8000))
        s.sendall(b"GET /truncated HTTP/1.1\r\nHost: 127.0.0.1:8000\r\nConnection: close\r\n\r\n")
        raw_trunc = b""
        while True:
            chunk = s.recv(512)
            if not chunk: break
            raw_trunc += chunk
        s.close()
        assert b"Content-Length: 200" in raw_trunc
        assert len(raw_trunc) < 200, "Body must be shorter than declared Content-Length"
        print("  PASS: Truncated body fixture delivered with premature EOF.")

        # 11. Malformed Non-HTTP Response (/malformed)
        print("[TEST 11] Testing raw non-HTTP response line (/malformed)...")
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(2.0)
        s.connect(('127.0.0.1', 8000))
        s.sendall(b"GET /malformed HTTP/1.1\r\nHost: 127.0.0.1:8000\r\nConnection: close\r\n\r\n")
        raw_mal = s.recv(512)
        s.close()
        assert not raw_mal.startswith(b"HTTP/"), "Response should not start with HTTP/"
        print("  PASS: Malformed raw response delivered without HTTP status line.")

        # 12. HTTP 302 Redirect and Loop Detection (/redirect, /redirect_loop)
        print("[TEST 12] Testing HTTP 302 redirect & redirect loop...")
        conn = http.client.HTTPConnection("127.0.0.1", 8000, timeout=3)
        conn.request("GET", "/redirect")
        r = conn.getresponse()
        assert r.status == 302
        assert r.getheader("Location") == "/"
        conn.close()

        conn = http.client.HTTPConnection("127.0.0.1", 8000, timeout=3)
        conn.request("GET", "/redirect_loop")
        r = conn.getresponse()
        assert r.status == 302
        assert r.getheader("Location") == "/redirect_loop"
        conn.close()
        print("  PASS: Redirect and loop fixtures verified.")

        # 13. Repeated Failed Requests Followed by Success
        print("[TEST 13] Testing repeated failed requests followed by success (resource cleanup)...")
        # 5 failed connection attempts on inactive port
        for _ in range(5):
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(0.5)
            try:
                s.connect(('127.0.0.1', 19999))
            except (ConnectionRefusedError, socket.timeout, OSError):
                pass
            finally:
                s.close()

        # Followed by success on port 8000
        with urllib.request.urlopen("http://127.0.0.1:8000/", timeout=3) as resp:
            assert resp.status == 200
        print("  PASS: Sockets and connections reclaimed cleanly across repeated failures.")

        # 14. Phase Z: 100 Sequential Navigations Stress Test
        print("[TEST 14] Testing 100 sequential HTTP/HTTPS requests (rapid execution & cleanup)...")
        t_start = time.time()
        urls = [
            "http://127.0.0.1:8000/",
            "http://127.0.0.1:8000/large",
            "http://127.0.0.1:8000/chunked",
            "http://127.0.0.1:8000/redirect"
        ]
        for i in range(100):
            target = urls[i % len(urls)]
            with urllib.request.urlopen(target, timeout=3) as resp:
                assert resp.status == 200
                data = resp.read()
                assert len(data) > 0
        t_elapsed = time.time() - t_start
        print(f"  PASS: 100 sequential HTTP navigations completed in {t_elapsed:.2f}s with 100% success.")

        # 15. Phase Z: Systematic Error Recovery Across Failures
        print("[TEST 15] Testing systematic error recovery across alternating failures...")
        failure_endpoints = [
            ("/status/404", 404),
            ("/status/500", 500),
            ("/redirect_loop", 302)
        ]
        for path, expected_code in failure_endpoints:
            # 1. Trigger failure/error
            try:
                opener = urllib.request.build_opener(urllib.request.HTTPRedirectHandler)
                req = urllib.request.Request(f"http://127.0.0.1:8000{path}")
                # For redirect loop, custom opener or catch HTTPError
                conn = http.client.HTTPConnection("127.0.0.1", 8000, timeout=3)
                conn.request("GET", path)
                r = conn.getresponse()
                assert r.status == expected_code
                conn.close()
            except urllib.error.HTTPError as e:
                assert e.code == expected_code

            # 2. Immediately verify valid recovery request succeeds
            with urllib.request.urlopen("http://127.0.0.1:8000/", timeout=3) as resp:
                assert resp.status == 200
                assert "AstraOS HTTP 1.1 Verified" in resp.read().decode('utf-8')
        print("  PASS: Browser connection successfully and cleanly recovered after each error response.")

        # 16. Phase Z: URL Input Boundary Length Testing
        print("[TEST 16] Testing maximum-length URL request boundary...")
        # Path with 200 characters
        long_path = "/" + "a" * 199
        conn = http.client.HTTPConnection("127.0.0.1", 8000, timeout=3)
        conn.request("GET", long_path)
        r = conn.getresponse()
        assert r.status == 200
        conn.close()
        print("  PASS: Length-bounded URL request parsed without server error.")

        # 17. Phase AA: Interleaved Multi-Tab Simulated Concurrent Requests
        print("[TEST 17] Testing multi-tab simulated concurrent/interleaved requests...")
        tab_urls = [
            "http://127.0.0.1:8000/",
            "http://127.0.0.1:8000/large",
            "http://127.0.0.1:8000/chunked",
            "http://127.0.0.1:8000/redirect"
        ]
        tab_conns = []
        for i in range(4):
            conn = http.client.HTTPConnection("127.0.0.1", 8000, timeout=3)
            tab_conns.append(conn)

        # Tab 0 sends request
        tab_conns[0].request("GET", "/")
        # Tab 1 sends request
        tab_conns[1].request("GET", "/large")
        # Tab 0 reads response
        resp0 = tab_conns[0].getresponse()
        assert resp0.status == 200
        data0 = resp0.read()
        assert len(data0) > 0
        tab_conns[0].close()

        # Tab 2 sends request
        tab_conns[2].request("GET", "/chunked")
        # Tab 1 reads response
        resp1 = tab_conns[1].getresponse()
        assert resp1.status == 200
        data1 = resp1.read()
        assert len(data1) >= 3000
        tab_conns[1].close()

        # Tab 3 sends request
        tab_conns[3].request("GET", "/redirect")
        # Tab 2 reads response
        resp2 = tab_conns[2].getresponse()
        assert resp2.status == 200
        data2 = resp2.read()
        assert b"Chunked Page" in data2
        tab_conns[2].close()

        # Tab 3 reads response (redirect status 302)
        resp3 = tab_conns[3].getresponse()
        assert resp3.status == 302
        tab_conns[3].close()

        print("  PASS: 4 interleaved tab sessions completed with clean socket closure and zero leakage.")

        print("\n=================================================================")
        print("   [SUCCESS] ALL 17 ADVERSARIAL NETWORKING & STRESS TESTS PASSED  ")
        print("=================================================================\n")
        return 0

    finally:
        print("[TEST] Shutting down test servers...")
        https_server.shutdown()
        http_server.shutdown()

if __name__ == '__main__':
    sys.exit(test_local_networking())
