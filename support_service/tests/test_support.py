import copy
import http.client
import json
import re
import tempfile
import threading
import time
import unittest
import urllib.parse
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from unittest.mock import patch

from support_service.app import App, BoundedServer, COOKIE, cents_from_input, load_config
from support_service.paypal import EvidenceError, PayPal, ProviderError, amount_text
from support_service.store import CheckoutError, Store


class FixturePayPal(PayPal):
    """Exercises the real payload/evidence code without any PayPal network access."""
    def __init__(self):
        super().__init__({"mode": "sandbox", "client_id": "fixture", "secret": "fixture", "merchant_id": "MERCHANT"})
        self.calls = []
        self.orders = {}
        self.lose_capture = False
        self.lose_create = False

    def call(self, method, path, order_id="", operation="", payload=None, before_send=None):
        if before_send:
            before_send()
        self.calls.append((method, path, order_id, operation, copy.deepcopy(payload)))
        if operation == "create":
            identifier = "PP" + order_id.upper()
            self.orders.setdefault(identifier, {
                "id": identifier, "intent": "CAPTURE", "status": "APPROVED",
                "purchase_units": copy.deepcopy(payload["purchase_units"])})
            if self.lose_create:
                self.lose_create = False
                raise ProviderError("Lost create response")
            return {"id": identifier, "status": "PAYER_ACTION_REQUIRED", "links": [{
                "rel": "payer-action", "href": "https://www.sandbox.paypal.com/checkoutnow?token=" + identifier}]}
        identifier = path.split("/")[4]
        result = self.orders[identifier]
        if operation == "capture":
            result["status"] = "COMPLETED"
            unit = result["purchase_units"][0]
            unit["payments"] = {"captures": [{"id": "CP" + order_id.upper(), "status": "COMPLETED",
                "final_capture": True, "amount": copy.deepcopy(unit["amount"])}]}
            if self.lose_capture:
                self.lose_capture = False
                raise ProviderError("Lost capture response")
        return copy.deepcopy(result)


class CheckoutTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.config = json.loads(Path("support_service/config.example.json").read_text())
        self.config.update(database=str(Path(self.temp.name) / "support.sqlite3"),
                           public_url="https://support.test", cookie_secret="11" * 32)
        self.provider = FixturePayPal()
        self.app = App(self.config, self.provider)
        self.owner = self.app.digest("owner:browser")
        self.project = self.config["projects"][0]
        self.identifier = "ab" * 16

    def tearDown(self):
        self.temp.cleanup()

    def order(self):
        return self.app.store.create(self.identifier, self.owner, "client", self.project, 1500)

    def ready(self):
        self.order()
        return self.app.operate(self.identifier, "create")

    def clear_backoff(self):
        with self.app.store.transaction() as db:
            db.execute("UPDATE orders SET next_try=0,lease_until=0")

    def test_amounts_use_integer_cents(self):
        for text, cents in [("1", 100), ("5.1", 510), ("500.00", 50000), ("15.99", 1599)]:
            self.assertEqual(cents_from_input(text), cents)
        for text in ["0", "500.01", "-1", "NaN", "1e2", "1.001", "00.10", "1000", "1,50", " 5 "]:
            with self.assertRaises(CheckoutError, msg=text):
                cents_from_input(text)

    def test_immutable_idempotent_price_snapshot(self):
        original = self.order()
        self.project["title"] = "Renamed"
        self.assertEqual(self.order()["title"], original["title"])
        for owner, cents in [(self.owner, 2000), ("other-browser", 1500)]:
            with self.assertRaises(CheckoutError):
                self.app.store.create(self.identifier, owner, "client", self.project, cents)

    def test_database_environment_binding(self):
        with self.assertRaises(ValueError):
            Store(self.config["database"], {"mode": "live"})
        reloaded = App(self.config, self.provider)
        self.order()
        self.assertEqual(reloaded.store.get(self.identifier, self.owner)["cents"], 1500)

    def test_lease_fences_late_writes(self):
        self.order()
        _, old = self.app.store.claim(self.identifier, "create")
        with self.assertRaises(CheckoutError):
            self.app.store.claim(self.identifier, "create")
        self.clear_backoff()
        _, new = self.app.store.claim(self.identifier, "create")
        with self.assertRaises(CheckoutError):
            self.app.store.finish(self.identifier, old, state="ready")
        self.app.store.finish(self.identifier, new, state="review")

    def test_concurrent_claim_has_one_winner(self):
        self.order()
        def claim(_):
            try:
                return self.app.store.claim(self.identifier, "create")[1]
            except CheckoutError:
                return None
        with ThreadPoolExecutor(max_workers=8) as pool:
            leases = list(pool.map(claim, range(8)))
        self.assertEqual(sum(bool(lease) for lease in leases), 1)

    def test_creation_retry_uses_same_order_after_restart(self):
        self.order()
        self.provider.lose_create = True
        self.assertEqual(self.app.operate(self.identifier, "create")["state"], "creating")
        self.clear_backoff()
        reloaded = App(self.config, self.provider)
        result = reloaded.operate(self.identifier, "create")
        self.assertEqual(result["state"], "ready")
        creates = [c for c in self.provider.calls if c[3] == "create"]
        self.assertEqual(creates[0], creates[1])
        self.assertEqual(len(self.provider.orders), 1)

    def test_capture_success_is_not_repeated(self):
        self.ready()
        result = self.app.operate(self.identifier, "capture")
        self.assertEqual(result["state"], "completed")
        reloaded = App(self.config, self.provider)
        reloaded.operate(self.identifier, "capture")
        self.assertEqual(len([c for c in self.provider.calls if c[3] == "capture"]), 1)

    def test_lost_capture_response_recovers_by_get_only(self):
        self.ready()
        self.provider.lose_capture = True
        result = self.app.operate(self.identifier, "capture")
        self.assertEqual(result["state"], "capturing")
        self.clear_backoff()
        reloaded = App(self.config, self.provider)
        result = reloaded.operate(self.identifier, "reconcile")
        self.assertEqual(result["state"], "completed")
        self.assertEqual(len([c for c in self.provider.calls if c[3] == "capture"]), 1)
        self.assertEqual(self.provider.calls[-1][0], "GET")

    def test_sparse_capture_post_uses_canonical_get(self):
        order = self.ready()
        original = self.provider.capture
        def sparse(row, authorize=None):
            original(row, authorize)
            return {"id": row["provider_id"], "status": "COMPLETED"}
        with patch.object(self.provider, "capture", side_effect=sparse):
            result = self.app.operate(self.identifier, "capture")
        self.assertEqual(result["state"], "completed")
        self.assertEqual(self.provider.calls[-1][0], "GET")

    def test_reconcile_never_captures_approved_order(self):
        self.ready()
        result = self.app.operate(self.identifier, "reconcile")
        self.assertEqual(result["state"], "ready")
        self.assertFalse(any(c[3] == "capture" for c in self.provider.calls))

    def test_pending_is_not_success_or_recaptured(self):
        order = self.ready()
        self.app.operate(self.identifier, "capture")
        response = self.provider.orders[order["provider_id"]]
        response["purchase_units"][0]["payments"]["captures"][0]["status"] = "PENDING"
        state, _ = self.provider.evidence(response, order)
        self.assertEqual(state, "pending")
        with self.app.store.transaction() as db:
            db.execute("UPDATE orders SET state='pending',next_try=0 WHERE id=?", (self.identifier,))
        with self.assertRaises(CheckoutError):
            self.app.operate(self.identifier, "capture")
        page = self.app.receipt_page(self.app.store.get(self.identifier), self.owner)
        self.assertIn("not a completed contribution", page)
        self.assertNotIn("Thank you for your support.", page)

    def test_mismatched_evidence_is_held_before_capture(self):
        order = self.ready()
        self.provider.orders[order["provider_id"]]["purchase_units"][0]["payee"]["merchant_id"] = "OTHER"
        result = self.app.operate(self.identifier, "capture")
        self.assertEqual(result["state"], "review")
        self.assertFalse(any(c[3] == "capture" for c in self.provider.calls))

    def test_capture_evidence_checks_all_consequential_fields(self):
        order = self.ready()
        self.app.operate(self.identifier, "capture")
        good = self.provider.orders[order["provider_id"]]
        variants = []
        for key, value in [("custom_id", "another-project"), ("invoice_id", "wrong"), ("reference_id", "wrong")]:
            bad = copy.deepcopy(good);bad["purchase_units"][0][key] = value;variants.append(bad)
        for target, key, value in [("amount", "value", "1500"), ("amount", "currency_code", "EUR"),
                                    ("payee", "merchant_id", "OTHER")]:
            bad = copy.deepcopy(good);bad["purchase_units"][0][target][key] = value;variants.append(bad)
        for key, value in [("final_capture", False), ("amount", {"currency_code": "USD", "value": "1.50"}), ("id", "../../bad")]:
            bad = copy.deepcopy(good);bad["purchase_units"][0]["payments"]["captures"][0][key] = value;variants.append(bad)
        bad = copy.deepcopy(good);bad["purchase_units"][0]["payments"]["captures"].append(copy.deepcopy(bad["purchase_units"][0]["payments"]["captures"][0]));variants.append(bad)
        bad = copy.deepcopy(good);bad["purchase_units"][0]["payments"] = {};variants.append(bad)
        for response in variants:
            with self.assertRaises(EvidenceError):
                self.provider.evidence(response, order)

    def test_approval_host_cannot_cross_environments(self):
        self.order()
        with patch.object(self.provider, "call", return_value={"id": "PP123", "status": "CREATED", "links": [{
                "rel": "approve", "href": "https://www.paypal.com/checkoutnow?token=PP123"}]}):
            with self.assertRaises(EvidenceError):
                self.provider.create(self.app.store.get(self.identifier), self.app.origin)

    def test_no_mutation_after_retry_window_or_attempt_limit(self):
        self.order()
        for _ in range(3):
            row, lease = self.app.store.claim(self.identifier, "create")
            self.app.store.finish(self.identifier, lease)
        row, lease = self.app.store.claim(self.identifier, "create")
        self.assertIsNone(lease)
        self.assertEqual(row["state"], "review")
        self.assertFalse(self.provider.calls)

    def test_rate_limits_are_persistent(self):
        for n in range(3):
            self.app.store.create(f"{n:032x}", self.owner, "client", self.project, 100)
        reloaded = App(self.config, self.provider)
        with self.assertRaises(CheckoutError) as error:
            reloaded.store.create("f" * 32, self.owner, "client", self.project, 100)
        self.assertEqual(error.exception.status, 429)

    def test_expired_lease_cannot_send_late_provider_mutation(self):
        self.order()
        row, lease = self.app.store.claim(self.identifier, "create")
        with self.app.store.transaction() as db:
            db.execute("UPDATE orders SET lease_until=0 WHERE id=?", (self.identifier,))
        with self.assertRaises(CheckoutError):
            self.provider.create(row, self.app.origin,
                lambda: self.app.store.authorize_mutation(self.identifier, lease, "create"))
        self.assertFalse(self.provider.calls)

    def test_expired_mutation_window_survives_restart(self):
        self.order()
        with self.app.store.transaction() as db:
            db.execute("UPDATE orders SET create_started=? WHERE id=?", (time.time() - 7201, self.identifier))
        reloaded = App(self.config, self.provider)
        result = reloaded.operate(self.identifier, "create")
        self.assertEqual(result["state"], "review")
        self.assertFalse(self.provider.calls)

    def test_http_checkout_return_csrf_and_private_receipt(self):
        server = BoundedServer(("127.0.0.1", 0), self.app)
        thread = threading.Thread(target=server.serve_forever, daemon=True);thread.start()
        cookie = ""
        def request(method, path, fields=None, use_cookie=True, origin="https://support.test"):
            conn = http.client.HTTPConnection(*server.server_address, timeout=5)
            headers = {"Host": "support.test", "Origin": origin, "Content-Type": "application/x-www-form-urlencoded"}
            if cookie and use_cookie: headers["Cookie"] = cookie
            conn.request(method, path, urllib.parse.urlencode(fields) if fields is not None else None, headers)
            response = conn.getresponse();body = response.read().decode();meta = dict(response.getheaders());conn.close()
            return response.status, meta, body
        try:
            status, headers, page = request("GET", "/projects/agent-c")
            self.assertEqual(status, 200)
            cookie = headers["Set-Cookie"].split(";")[0]
            self.assertIn("Secure; HttpOnly; SameSite=Lax", headers["Set-Cookie"])
            fields = dict(re.findall(r'<input type="hidden" name="([^"]+)" value="([^"]+)">', page))
            fields["amount"] = "15.00"
            self.assertEqual(request("POST", "/checkout", fields, origin="https://evil.test")[0], 403)
            self.assertEqual(request("POST", "/checkout", fields, use_cookie=False)[0], 403)
            self.assertFalse(self.provider.calls)
            status, headers, _ = request("POST", "/checkout", fields)
            self.assertEqual(status, 303)
            self.assertTrue(headers["Location"].startswith("/orders/"))
            identifier = fields["intent"]
            order = self.app.store.get(identifier)
            self.assertEqual(request("GET", "/orders/" + identifier, use_cookie=False)[0], 403)
            self.assertEqual(request("GET", "/return?order=" + identifier + "&token=WRONG")[0], 403)
            status, _, page = request("GET", "/return?order=" + identifier + "&token=" + order["provider_id"])
            self.assertEqual(status, 200)
            self.assertIn("data-return-confirm", page)
            self.assertFalse(any(c[3] == "capture" for c in self.provider.calls))
            status, _, _ = request("POST", "/orders/" + identifier + "/capture", {"csrf": fields["csrf"]})
            self.assertEqual(status, 303)
            self.assertEqual(request("GET", "/orders/" + identifier)[0], 200)
            self.assertIn("Thank you for your support.", request("GET", "/orders/" + identifier)[2])
            # A second browser cannot use a leaked receipt address.
            cookie = COOKIE + "=" + "ff" * 32
            self.assertEqual(request("GET", "/orders/" + identifier)[0], 404)
        finally:
            server.shutdown();server.server_close();thread.join()

    def test_example_config_requires_real_private_values(self):
        p = Path(self.temp.name) / "config.json"
        p.write_text(json.dumps(self.config))
        self.assertEqual(load_config(p)["projects"][0]["id"], "agent-c")
        self.config["public_url"] = "http://support.test"
        p.write_text(json.dumps(self.config))
        with self.assertRaises(ValueError):load_config(p)


if __name__ == "__main__":
    unittest.main()
