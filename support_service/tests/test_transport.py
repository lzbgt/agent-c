import json
import threading
import unittest
from http.server import BaseHTTPRequestHandler, HTTPServer

from support_service.paypal import PayPal, ProviderError


class TransportTests(unittest.TestCase):
    def test_real_http_headers_are_stable_bounded_and_redirects_refused(self):
        calls = []
        class Fixture(BaseHTTPRequestHandler):
            def log_message(self, *args): pass
            def do_POST(self):
                body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
                calls.append((self.path, dict(self.headers), body))
                if self.path == "/v1/oauth2/token":
                    payload = {"access_token": "fixture-token", "expires_in": 3600}
                elif self.path == "/redirect":
                    self.send_response(302);self.send_header("Location", "/must-not-follow");self.end_headers();return
                else:
                    payload = {"id": "PP1"}
                encoded = json.dumps(payload).encode()
                self.send_response(200);self.send_header("Content-Length", str(len(encoded)));self.end_headers();self.wfile.write(encoded)
        server = HTTPServer(("127.0.0.1", 0), Fixture)
        thread = threading.Thread(target=server.serve_forever, daemon=True);thread.start()
        try:
            client = PayPal({"mode": "sandbox", "client_id": "fixture-id", "secret": "fixture-secret", "merchant_id": "FIXTURE"})
            # Only the test object can point to a fixture. Production config has
            # no endpoint override and always selects a fixed PayPal HTTPS host.
            client.base = f"http://127.0.0.1:{server.server_port}"
            for operation in ("create", "create", "capture"):
                client.call("POST", "/order", "local-order", operation, {"test": True})
            self.assertEqual(sum(path == "/v1/oauth2/token" for path, _, _ in calls), 1)
            requests = [headers["Paypal-Request-Id"] for path, headers, _ in calls if path == "/order"]
            self.assertEqual(requests[0], requests[1])
            self.assertNotEqual(requests[0], requests[2])
            self.assertEqual(len(requests[0]), 36)
            self.assertEqual(calls[0][2], b"grant_type=client_credentials")
            self.assertTrue(calls[0][1]["Authorization"].startswith("Basic "))
            self.assertTrue(all(headers["Authorization"] == "Bearer fixture-token" for path, headers, _ in calls if path == "/order"))
            with self.assertRaises(ProviderError):
                client.call("POST", "/redirect", "local-order", "create", {})
            self.assertFalse(any(path == "/must-not-follow" for path, _, _ in calls))
        finally:
            server.shutdown();server.server_close();thread.join()


if __name__ == "__main__":
    unittest.main()
