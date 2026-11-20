"""Bounded Orders v2 transport, following LangLearn's server-side integration.

No buyer or card details are retained. Only allowlisted PayPal origins receive
credentials; redirects are refused, including redirects from the OAuth endpoint.
"""
import base64
import json
import re
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
from decimal import Decimal


class ProviderError(Exception):
    """A retryable transport/provider failure, without response bodies or secrets."""


class EvidenceError(Exception):
    """A provider response does not match the immutable local payment request."""


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        return None


def amount_text(cents):
    return f"{cents // 100}.{cents % 100:02}"


def provider_id(value):
    return isinstance(value, str) and re.fullmatch(r"[A-Za-z0-9]{1,64}", value) is not None


class PayPal:
    def __init__(self, config):
        self.mode = config["mode"]
        if self.mode not in ("live", "sandbox"):
            raise ValueError("PayPal mode must be live or sandbox")
        self.base = ("https://api-m.paypal.com" if self.mode == "live"
                     else "https://api-m.sandbox.paypal.com")
        self.approval_host = "www.paypal.com" if self.mode == "live" else "www.sandbox.paypal.com"
        self.client_id = config["client_id"]
        self.secret = config["secret"]
        self.merchant_id = config["merchant_id"]
        if not all(isinstance(v, str) and v.strip() for v in
                   (self.client_id, self.secret, self.merchant_id)):
            raise ValueError("PayPal credentials and merchant ID are required")
        self.opener = urllib.request.build_opener(NoRedirect)
        self.lock = threading.Lock()
        self.slots = threading.BoundedSemaphore(4)
        self.token = ""
        self.expiry = 0

    def _request(self, method, path, data, headers):
        request = urllib.request.Request(self.base + path, data=data, headers=headers, method=method)
        try:
            with self.opener.open(request, timeout=12) as response:
                body = response.read((1 << 20) + 1)
                if len(body) > 1 << 20:
                    raise ProviderError("PayPal response exceeded the size limit")
                result = json.loads(body)
                if not isinstance(result, dict):
                    raise ProviderError("PayPal response was not an object")
                return result
        except urllib.error.HTTPError as error:
            status = error.code
            error.close()
            if status == 401:
                self.expiry = 0
            raise ProviderError(f"PayPal HTTP {status}") from None
        except (OSError, ValueError) as error:
            raise ProviderError("PayPal could not be reached or returned invalid JSON") from None

    def access_token(self):
        with self.lock:
            if self.expiry > time.monotonic():
                return self.token
            basic = base64.b64encode(f"{self.client_id}:{self.secret}".encode()).decode()
            result = self._request("POST", "/v1/oauth2/token", b"grant_type=client_credentials", {
                "Authorization": "Basic " + basic,
                "Content-Type": "application/x-www-form-urlencoded", "Accept": "application/json"})
            token, expires = result.get("access_token"), result.get("expires_in")
            if not isinstance(token, str) or not token or not isinstance(expires, (int, float)) or expires <= 60:
                raise ProviderError("PayPal OAuth response was incomplete")
            self.token, self.expiry = token, time.monotonic() + min(expires - 60, 3600)
            return token

    def call(self, method, path, order_id="", operation="", payload=None, before_send=None):
        if not self.slots.acquire(timeout=1):
            raise ProviderError("PayPal is busy; retry this request shortly")
        try:
            headers = {"Authorization": "Bearer " + self.access_token(), "Accept": "application/json"}
            data = None
            if payload is not None:
                data = json.dumps(payload).encode()
                headers.update({"Content-Type": "application/json", "Prefer": "return=representation",
                                "PayPal-Request-Id": str(uuid.uuid5(uuid.NAMESPACE_URL,
                                    "hubstack-support/" + order_id + "/" + operation))})
            if before_send:
                before_send()
            return self._request(method, path, data, headers)
        finally:
            self.slots.release()

    def create(self, order, origin, authorize=None):
        local = order["id"]
        response = self.call("POST", "/v2/checkout/orders", local, "create", {
            "intent": "CAPTURE",
            "purchase_units": [{
                "reference_id": local, "custom_id": order["project"],
                "invoice_id": "support-" + local,
                "description": ("Open-source support: " + order["title"])[:127],
                "payee": {"merchant_id": self.merchant_id},
                "amount": {"currency_code": order["currency"], "value": amount_text(order["cents"])}}],
            "payment_source": {"paypal": {"experience_context": {
                "user_action": "PAY_NOW", "shipping_preference": "NO_SHIPPING",
                "return_url": origin + "/return?order=" + local,
                "cancel_url": origin + "/cancel?order=" + local}}}}, before_send=authorize)
        identifier = response.get("id")
        if not provider_id(identifier) or response.get("status") not in ("CREATED", "PAYER_ACTION_REQUIRED"):
            raise EvidenceError("Unexpected PayPal order response")
        links = response.get("links")
        if not isinstance(links, list):
            raise EvidenceError("PayPal approval links are missing")
        for link in links:
            if not isinstance(link, dict):
                raise EvidenceError("PayPal approval link is malformed")
            if link.get("rel") not in ("approve", "payer-action"):
                continue
            url = link.get("href", "")
            try:
                parsed = urllib.parse.urlsplit(url)
            except (TypeError, ValueError):
                raise EvidenceError("PayPal approval URL is malformed") from None
            if (parsed.scheme == "https" and parsed.netloc == self.approval_host
                    and parsed.path == "/checkoutnow" and not parsed.fragment
                    and urllib.parse.parse_qs(parsed.query).get("token") == [identifier]):
                return identifier, url
        raise EvidenceError("PayPal returned an unexpected approval destination")

    def get(self, identifier):
        if not provider_id(identifier):
            raise EvidenceError("Invalid PayPal order ID")
        return self.call("GET", "/v2/checkout/orders/" + identifier)

    def capture(self, order, authorize=None):
        return self.call("POST", "/v2/checkout/orders/" + order["provider_id"] + "/capture",
                         order["id"], "capture", {}, before_send=authorize)

    def evidence(self, response, order):
        """Validate order AND individual capture; COMPLETED on an order is insufficient."""
        try:
            return self._evidence(response, order)
        except (AttributeError, KeyError, TypeError):
            raise EvidenceError("PayPal order evidence is malformed") from None

    def _evidence(self, response, order):
        units = response.get("purchase_units", [])
        if (response.get("id") != order["provider_id"] or response.get("intent") != "CAPTURE"
                or not isinstance(units, list) or len(units) != 1):
            raise EvidenceError("PayPal order identity does not match")
        unit = units[0]
        if (unit.get("reference_id") != order["id"] or unit.get("custom_id") != order["project"]
                or unit.get("invoice_id") != "support-" + order["id"]
                or unit.get("payee", {}).get("merchant_id") != self.merchant_id):
            raise EvidenceError("PayPal project, invoice or recipient does not match")
        self._amount(unit.get("amount", {}), order)
        captures = unit.get("payments", {}).get("captures", [])
        if not isinstance(captures, list) or len(captures) > 1:
            raise EvidenceError("Unexpected PayPal capture count")
        if not captures:
            if response.get("status") not in ("CREATED", "PAYER_ACTION_REQUIRED", "APPROVED", "VOIDED"):
                raise EvidenceError("PayPal order has no corresponding capture evidence")
            return "approved" if response["status"] == "APPROVED" else "ready", ""
        capture = captures[0]
        self._amount(capture.get("amount", {}), order)
        if not provider_id(capture.get("id")) or capture.get("final_capture") is not True:
            raise EvidenceError("PayPal capture identity or finality is missing")
        status = capture.get("status")
        if status == "COMPLETED" and response.get("status") == "COMPLETED":
            return "completed", capture["id"]
        if status == "PENDING":
            return "pending", capture["id"]
        if status in ("DECLINED", "FAILED", "REFUNDED", "PARTIALLY_REFUNDED"):
            return "review", capture["id"]
        raise EvidenceError("PayPal capture is not confirmed")

    @staticmethod
    def _amount(amount, order):
        value = amount.get("value")
        if (amount.get("currency_code") != order["currency"] or not isinstance(value, str)
                or not re.fullmatch(r"[0-9]{1,9}(?:\.[0-9]{1,2})?", value)
                or Decimal(value) != Decimal(order["cents"]) / 100):
            raise EvidenceError("PayPal amount or currency does not match")
