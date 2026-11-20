"""Run with python3 -m support_service.app --config /private/support.json."""
import argparse
import hashlib
import hmac
import html
import http.cookies
import ipaddress
import json
import logging
import re
import secrets
import signal
import socketserver
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

from .paypal import EvidenceError, PayPal, ProviderError, amount_text
from .store import CheckoutError, Store

LOG = logging.getLogger("hubstack-support")
STATIC = Path(__file__).with_name("static")
COOKIE = "__Host-hubstack-support"
ID_PATTERN = r"[a-f0-9]{32}"


def escape(value):
    return html.escape(str(value), quote=True)


def cents_from_input(value):
    if not re.fullmatch(r"(?:0|[1-9][0-9]{0,2})(?:\.[0-9]{1,2})?", value):
        raise CheckoutError("Enter an amount from $1 to $500 with at most two decimal places.", 400)
    whole, _, fraction = value.partition(".")
    cents = int(whole) * 100 + int((fraction + "00")[:2])
    if not 100 <= cents <= 50000:
        raise CheckoutError("Choose an amount between $1 and $500.", 400)
    return cents


def load_config(path):
    config = json.loads(Path(path).read_text())
    origin = urllib.parse.urlsplit(config["public_url"])
    if (origin.scheme != "https" or not origin.hostname or origin.path or origin.query
            or origin.fragment or origin.username or origin.password):
        raise ValueError("public_url must be an HTTPS origin without a trailing slash")
    if not re.fullmatch(r"[a-f0-9]{64}", config["cookie_secret"]):
        raise ValueError("cookie_secret must contain 32 random bytes encoded as hex")
    projects = config["projects"]
    if not 1 <= len(projects) <= 100:
        raise ValueError("Configure between 1 and 100 projects")
    seen = set()
    for project in projects:
        slug = project["id"]
        if not re.fullmatch(r"[a-z0-9]+(?:-[a-z0-9]+)*", slug) or len(slug) > 64 or slug in seen:
            raise ValueError("Each project needs a unique lowercase URL slug")
        seen.add(slug)
        if not project["title"] or len(project["title"]) > 80 or len(project["summary"]) > 500:
            raise ValueError("Project title/summary is empty or too long")
        source = urllib.parse.urlsplit(project["source_url"])
        if source.scheme != "https" or not source.hostname or source.username or source.password:
            raise ValueError("Project source URL must use HTTPS")
    return config


class App:
    def __init__(self, config, provider=None):
        self.config = config
        self.origin = config["public_url"]
        self.secret = bytes.fromhex(config["cookie_secret"])
        self.projects = {p["id"]: p for p in config["projects"]}
        self.provider = provider or PayPal(config["paypal"])
        self.store = Store(config["database"], {
            "mode": self.provider.mode, "merchant_id": self.provider.merchant_id,
            "currency": "USD", "public_url": self.origin})

    def digest(self, value):
        return hmac.new(self.secret, value.encode(), hashlib.sha256).hexdigest()

    def operate(self, identifier, operation):
        order, lease = self.store.claim(identifier, operation)
        if lease is None:
            return order
        try:
            if operation == "create":
                provider_id, approval = self.provider.create(order, self.origin,
                    lambda: self.store.authorize_mutation(identifier, lease, "create"))
                return self.store.finish(identifier, lease, state="ready", provider_id=provider_id,
                                         approval_url=approval, error="", next_try=0)
            response = self.provider.get(order["provider_id"])
            state, capture = self.provider.evidence(response, order)
            if operation == "capture" and state == "approved":
                captured = self.provider.capture(order,
                    lambda: self.store.authorize_mutation(identifier, lease, "capture"))
                if captured.get("id") != order["provider_id"]:
                    raise EvidenceError("Capture response names another order")
                # Capture responses may omit intent, amount and payee even with
                # Prefer: return=representation. Validate a canonical GET rather
                # than treating that sparse POST response as a full order.
                response = self.provider.get(order["provider_id"])
                state, capture = self.provider.evidence(response, order)
                if state in ("approved", "ready"):
                    raise ProviderError("PayPal capture is not confirmed")
            # After an attempted capture, a missing capture remains ambiguous. A
            # read-only recovery never turns that observation into a new charge.
            if state in ("ready", "approved"):
                state = "capturing" if order["capture_attempts"] else "ready"
            return self.store.finish(identifier, lease, state=state, capture_id=capture or None,
                                     error="", next_try=time.time() + 10)
        except EvidenceError:
            LOG.warning("payment_evidence_mismatch order=%s", identifier)
            return self.store.finish(identifier, lease, state="review", error="evidence_mismatch")
        except ProviderError:
            LOG.warning("payment_provider_unavailable operation=%s order=%s", operation, identifier)
            return self.store.finish(identifier, lease, error="provider_unavailable", next_try=time.time() + 10)

    def recover(self, stop):
        while not stop.wait(30):
            try:
                for identifier in self.store.recoverable():
                    if stop.is_set():
                        break
                    try:
                        self.operate(identifier, "reconcile")
                    except CheckoutError:
                        pass
            except Exception:
                # Never log configuration, raw provider JSON or buyer data.
                LOG.error("payment_recovery_failed")

    def page(self, title, content):
        return f'''<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="color-scheme" content="light"><meta name="description" content="Support independent open-source projects with a one-time contribution.">
<title>{escape(title)} · Open-source support</title><link rel="icon" href="/static/icon.svg" type="image/svg+xml">
<link rel="stylesheet" href="/static/site.css"><script defer src="/static/site.js"></script></head>
<body><header><a class="brand" href="/" aria-label="Open-source support home"><span class="mark" aria-hidden="true">↗</span> Open-source support</a>
<a class="header-link" href="/help">Payment help <span aria-hidden="true">↗</span></a></header>
<main>{content}</main><footer><span>Independent work. Shared progress.</span><nav aria-label="Footer"><a href="/">Projects</a><a href="/privacy">Privacy</a><a href="mailto:lzbgt@icloud.com">Contact</a></nav></footer></body></html>'''

    def project_page(self, project, owner):
        intent, stamp = secrets.token_hex(16), str(int(time.time()))
        proof = self.digest(f"form:{owner}:{project['id']}:{intent}:{stamp}")
        csrf = self.digest("csrf:" + owner)
        sandbox = '<p class="notice">Sandbox checkout — no real payment.</p>' if self.provider.mode == "sandbox" else ""
        content = f'''<section class="support-layout"><div class="intro"><p class="eyebrow">A little support goes a long way</p>
<h1>Keep {escape(project['title'])}<br><span>moving forward.</span></h1>
<p class="lede">{escape(project['summary'])}</p><a class="source-link" href="{escape(project['source_url'])}" rel="noreferrer">Explore the project <span aria-hidden="true">↗</span></a>
<div class="purpose"><span class="purpose-icon" aria-hidden="true">✦</span><p>Your contribution supports maintenance, thoughtful improvements, and the next useful idea.</p></div></div>
<article class="checkout-card">{sandbox}<p class="eyebrow">One-time contribution</p><h2>Support {escape(project['title'])}</h2>
<form action="/checkout" method="post" data-checkout>
<input type="hidden" name="project" value="{escape(project['id'])}"><input type="hidden" name="intent" value="{intent}">
<input type="hidden" name="stamp" value="{stamp}"><input type="hidden" name="proof" value="{proof}"><input type="hidden" name="csrf" value="{csrf}">
<div class="presets" role="group" aria-label="Suggested contribution amounts">
<button type="button" data-amount="5" aria-pressed="false">$5</button><button type="button" data-amount="15" aria-pressed="true">$15</button><button type="button" data-amount="30" aria-pressed="false">$30</button></div>
<label for="amount">Amount <span>USD</span></label><div class="amount-field"><span aria-hidden="true">$</span><input id="amount" name="amount" inputmode="decimal" type="number" min="1" max="500" step="0.01" value="15.00" required aria-describedby="amount-hint"></div>
<p class="field-hint" id="amount-hint">Choose a suggestion or enter your own amount.</p>
<button class="primary paypal" type="submit">Continue with <strong>PayPal</strong><span aria-hidden="true">↗</span></button>
<p class="reassurance">No account here. No subscription.</p></form>
<div class="card-note"><svg width="15" height="17" viewBox="0 0 18 20" aria-hidden="true"><path d="M4 8V5a5 5 0 0 1 10 0v3M2 8h14v10H2z" fill="none" stroke="currentColor" stroke-width="1.5"/></svg><span>Payment details stay with PayPal.</span></div></article></section>'''
        return self.page("Support " + project["title"], content)

    def receipt_page(self, order, owner, returning=False, cancelled=False, continuing=False):
        state = order["state"]
        completed = state == "completed"
        title = "Thank you for your support." if completed else "Your contribution"
        message = {
            "new": "Your support request is ready to continue.",
            "creating": "We could not finish connecting to PayPal. Resume this same request below.",
            "ready": "Continue securely with PayPal to support this project.",
            "capturing": "We are checking this payment with PayPal. Keep this page; please do not start another payment.",
            "pending": "PayPal is processing your payment. This is not a completed contribution yet.",
            "review": "This payment needs a manual check. Please contact us with the reference below before making another payment.",
            "completed": "Your one-time contribution has been confirmed by PayPal. Thank you for helping keep this project moving."}[state]
        if cancelled and state in ("ready", "new", "creating"):
            message = "You left PayPal. You can resume this same contribution whenever you are ready."
        csrf = self.digest("csrf:" + owner)
        action = ""
        if not completed and state != "review":
            prefix = f'<form method="post" action="/orders/{order["id"]}/'
            hidden = f'<input type="hidden" name="csrf" value="{csrf}">'
            if state in ("new", "creating"):
                action = prefix + 'resume">' + hidden + '<button class="primary" type="submit">Resume with PayPal</button></form>'
            elif state == "ready" and not returning:
                auto = ' data-paypal-redirect' if continuing else ''
                action = f'<a class="primary"{auto} href="{escape(order["approval_url"])}">Continue with PayPal <span aria-hidden="true">↗</span></a>'
            elif returning and state == "ready":
                message = "Confirming your contribution with PayPal…"
                action = prefix + 'capture" data-return-confirm>' + hidden + '<button class="primary" type="submit">Complete contribution</button></form>'
            else:
                action = prefix + 'reconcile">' + hidden + '<button class="primary" type="submit">Check payment</button></form>'
                if state == "capturing":
                    action += prefix + 'capture">' + hidden + '<button class="secondary" type="submit">Retry confirmation safely</button></form>'
        icon = "✓" if completed else "↗"
        content = f'''<section class="receipt"><div class="receipt-icon{' success' if completed else ''}" aria-hidden="true">{icon}</div>
<p class="eyebrow">{escape(order['title'])}</p><h1>{title}</h1><p class="lede">{message}</p>
<div class="receipt-amount">${amount_text(order['cents'])} <span>USD · One time</span></div>{action}
<details><summary>Payment reference</summary><dl><dt>Support reference</dt><dd>{order['id']}</dd>'''
        if completed:
            content += f'<dt>PayPal capture</dt><dd>{escape(order["capture_id"])}</dd>'
        content += '</dl><p class="field-hint">Keep this reference for payment questions. This page records the original confirmation; later refunds are handled through PayPal.</p></details>'
        content += f'<a class="source-link" href="/projects/{escape(order["project"])}">Back to the project <span aria-hidden="true">↗</span></a></section>'
        return self.page("Contribution to " + order["title"], content)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "Support"
    sys_version = ""

    @property
    def app(self):
        return self.server.app

    def log_message(self, *args):
        pass  # Receipt IDs, PayPal query tokens and network addresses are private.

    def setup(self):
        super().setup()
        self.connection.settimeout(15)

    def send(self, status, body="", content_type="text/html; charset=utf-8", headers=None):
        data = body.encode() if isinstance(body, str) else body
        self.send_response(status)
        for name, value in {
            "Content-Type": content_type, "Content-Length": str(len(data)),
            "Cache-Control": "no-store", "X-Content-Type-Options": "nosniff",
            "Referrer-Policy": "same-origin", "X-Frame-Options": "DENY",
            "Content-Security-Policy": "default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self'; connect-src 'self'; form-action 'self'; base-uri 'none'; frame-ancestors 'none'",
            "Permissions-Policy": "camera=(), microphone=(), geolocation=()",
            "Strict-Transport-Security": "max-age=31536000", "Connection": "close",
        }.items():
            self.send_header(name, value)
        for name, value in headers or []:
            self.send_header(name, value)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(data)
        self.close_connection = True

    def redirect(self, destination):
        self.send(303, headers=[("Location", destination)])

    def browser(self, create=False):
        jar = http.cookies.SimpleCookie()
        try:
            jar.load(self.headers.get("Cookie", ""))
        except http.cookies.CookieError:
            pass
        token = jar[COOKIE].value if COOKIE in jar else ""
        if not re.fullmatch(r"[a-f0-9]{64}", token):
            if not create:
                raise CheckoutError("Open the project page in this browser to begin. Cookies must be enabled.", 403)
            token = secrets.token_hex(32)
        return token, self.app.digest("owner:" + token)

    def require_post(self, owner):
        if (self.headers.get("Origin") != self.app.origin or self.headers.get("Sec-Fetch-Site", "same-origin") not in ("same-origin", "none")):
            raise CheckoutError("Open this page directly before continuing.", 403)
        if self.headers.get("Transfer-Encoding") or self.headers.get_content_type() != "application/x-www-form-urlencoded":
            raise CheckoutError("Unsupported request format.", 415)
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if not 0 < length <= 4096:
                raise ValueError
            raw = self.rfile.read(length)
            if len(raw) != length:
                raise ValueError
            parsed = urllib.parse.parse_qs(raw.decode(), strict_parsing=True, max_num_fields=12)
            if any(len(v) != 1 for v in parsed.values()):
                raise ValueError
            fields = {k: v[0] for k, v in parsed.items()}
        except (ValueError, UnicodeError):
            raise CheckoutError("Invalid form. Reload the page and try again.", 400) from None
        if not hmac.compare_digest(fields.get("csrf", ""), self.app.digest("csrf:" + owner)):
            raise CheckoutError("This form belongs to another browser. Reload the project page.", 403)
        return fields

    def handle_route(self):
        try:
            if len(self.path) > 2048:
                raise CheckoutError("Address is too long.", 414)
            url = urllib.parse.urlsplit(self.path)
            path = url.path
            if self.headers.get("Host") != urllib.parse.urlsplit(self.app.origin).netloc:
                raise CheckoutError("Unknown site.", 421)
            if self.command in ("GET", "HEAD"):
                self.get_route(path, urllib.parse.parse_qs(url.query))
            elif self.command == "POST":
                self.post_route(path)
            else:
                raise CheckoutError("Method not allowed.", 405)
        except CheckoutError as error:
            self.send(error.status, self.app.page("Please check", f'<section class="receipt"><h1>Let’s check that.</h1><p class="lede">{escape(error)}</p><a class="source-link" href="/">View projects ↗</a></section>'))
        except (BrokenPipeError, ConnectionResetError, TimeoutError):
            self.close_connection = True
        except Exception:
            LOG.error("request_failed method=%s", self.command)
            self.send(503, self.app.page("Please retry", '<section class="receipt"><h1>Please try again shortly.</h1><p class="lede">Keep your payment reference and refresh its page. Please avoid starting a second payment while the first is being checked.</p></section>'))

    def get_route(self, path, query):
        if path in ("/healthz", "/readyz"):
            self.app.store.check()
            self.send(200, json.dumps({"status": "ok", "service": "hubstack-support",
                "revision": self.app.config.get("source_revision", "development"),
                "mode": self.app.provider.mode}), "application/json")
            return
        if path.startswith("/static/"):
            name = path.removeprefix("/static/")
            types = {"site.css": "text/css; charset=utf-8", "site.js": "text/javascript; charset=utf-8", "icon.svg": "image/svg+xml"}
            if name not in types:
                raise CheckoutError("Page not found.", 404)
            self.send(200, (STATIC / name).read_bytes(), types[name])
            return
        if path == "/":
            cards = "".join(f'<a class="project-link" href="/projects/{escape(p["id"])}"><div><h2>{escape(p["title"])}</h2><p>{escape(p["summary"])}</p></div><span aria-hidden="true">↗</span></a>' for p in self.app.projects.values())
            self.send(200, self.app.page("Support open source", '<section class="directory"><p class="eyebrow">Built in the open</p><h1>Help good ideas<br><span>keep growing.</span></h1><p class="lede">Choose a project. Make a one-time contribution. Help independent open-source work continue.</p><div class="project-list">' + cards + '</div></section>'))
            return
        match = re.fullmatch(r"/projects/([a-z0-9-]+)", path)
        if match and match[1] in self.app.projects:
            token, owner = self.browser(create=True)
            self.send(200, self.app.project_page(self.app.projects[match[1]], owner), headers=[(
                "Set-Cookie", f"{COOKIE}={token}; Path=/; Secure; HttpOnly; SameSite=Lax; Max-Age=604800")])
            return
        order_match = re.fullmatch(r"/orders/(" + ID_PATTERN + r")", path)
        if order_match or path in ("/return", "/cancel"):
            _, owner = self.browser()
            identifier = order_match[1] if order_match else query.get("order", [""])[0]
            order = self.app.store.get(identifier, owner)
            returning = path == "/return"
            if returning and query.get("token") != [order["provider_id"]]:
                raise CheckoutError("PayPal return does not match this browser’s support request.", 403)
            self.send(200, self.app.receipt_page(order, owner, returning, path == "/cancel", query.get("continue") == ["1"]))
            return
        if path in ("/privacy", "/help"):
            if path == "/privacy":
                title = "Your payment. Your privacy."
                text = '<p>PayPal handles your payment details. This service does not collect card numbers, PayPal passwords, or payer names and emails.</p><p>We retain the project, amount, payment references, payment state and timestamps to confirm contributions and answer payment questions. An essential browser cookie gives this browser access to its receipt. Only a hash of that browser token and a keyed hash of the network address are stored; the latter limits checkout abuse. There are no analytics cookies.</p><p>Payment records are retained for bookkeeping and payment support. Contact <a href="mailto:lzbgt@icloud.com">lzbgt@icloud.com</a> with questions or privacy requests. PayPal applies its own <a href="https://www.paypal.com/privacy" rel="noreferrer">privacy policy</a> during checkout.</p>'
            else:
                title = "Payment help"
                text = '<p>Contributions are voluntary, one-time support for the selected open-source project. They do not create a subscription or buy LangLearn time, product access, or a guaranteed feature.</p><p>Choose an amount in USD, then continue to PayPal. PayPal shows payment options available to your account and location. You do not need a LangLearn or Hubstack account. After approval, return here for confirmation.</p><p>If confirmation is delayed, keep the receipt page and use Check payment. Avoid making another payment for the same contribution. Contact <a href="mailto:lzbgt@icloud.com">lzbgt@icloud.com</a> with the support reference for payment or refund questions. Refunds and disputes are handled through PayPal; this site does not issue a charitable tax receipt.</p>'
            self.send(200, self.app.page(title, f'<section class="prose"><h1>{title}</h1>{text}</section>'))
            return
        raise CheckoutError("Page not found.", 404)

    def post_route(self, path):
        _, owner = self.browser()
        fields = self.require_post(owner)
        if path == "/checkout":
            slug, intent, stamp = (fields.get(k, "") for k in ("project", "intent", "stamp"))
            if slug not in self.app.projects or not re.fullmatch(ID_PATTERN, intent) or not re.fullmatch(r"[0-9]{10}", stamp):
                raise CheckoutError("Invalid support request.", 400)
            if not 0 <= time.time() - int(stamp) <= 3600:
                raise CheckoutError("This form has expired. Reload the project page.", 403)
            if not hmac.compare_digest(fields.get("proof", ""), self.app.digest(f"form:{owner}:{slug}:{intent}:{stamp}")):
                raise CheckoutError("Invalid support form.", 403)
            cents = cents_from_input(fields.get("amount", ""))
            peer = self.client_address[0]
            address = peer
            if peer == self.app.config.get("trusted_proxy"):
                forwarded = self.headers.get("X-Forwarded-For", "").split(",")[-1].strip()
                if forwarded:
                    try:
                        address = str(ipaddress.ip_address(forwarded))
                    except ValueError:
                        raise CheckoutError("Invalid client address.", 400) from None
            self.app.store.create(intent, owner, self.app.digest("client:" + address), self.app.projects[slug], cents)
            order = self.app.operate(intent, "create")
            self.redirect("/orders/" + intent + ("?continue=1" if order["state"] == "ready" else ""))
            return
        match = re.fullmatch(r"/orders/(" + ID_PATTERN + r")/(resume|capture|reconcile)", path)
        if match:
            identifier, operation = match.groups()
            self.app.store.get(identifier, owner)
            order = self.app.operate(identifier, "create" if operation == "resume" else operation)
            self.redirect("/orders/" + identifier + ("?continue=1" if operation == "resume" and order["state"] == "ready" else ""))
            return
        raise CheckoutError("Page not found.", 404)

    do_GET = do_HEAD = do_POST = do_PUT = do_DELETE = handle_route


class BoundedServer(socketserver.ThreadingMixIn, HTTPServer):
    daemon_threads = True
    request_queue_size = 32

    def __init__(self, address, app):
        self.app = app
        self.workers = threading.BoundedSemaphore(16)
        super().__init__(address, Handler)

    def process_request(self, request, client_address):
        if not self.workers.acquire(blocking=False):
            request.close()
            return
        try:
            super().process_request(request, client_address)
        except BaseException:
            self.workers.release()
            raise

    def process_request_thread(self, request, client_address):
        try:
            super().process_request_thread(request, client_address)
        finally:
            self.workers.release()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True)
    parser.add_argument("--check-provider", action="store_true", help="Check OAuth only; creates no order")
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(levelname)s %(message)s")
    config = load_config(args.config)
    app = App(config)
    app.provider.access_token()  # Non-charging startup preflight; never creates an order.
    if args.check_provider:
        print(json.dumps({"oauth": "ok", "mode": app.provider.mode, "orders_created": 0}))
        return
    host, port = config["listen"].rsplit(":", 1)
    server = BoundedServer((host, int(port)), app)
    stop = threading.Event()
    recovery = threading.Thread(target=app.recover, args=(stop,), daemon=True)
    recovery.start()

    def shutdown(*_):
        stop.set()
        threading.Thread(target=server.shutdown, daemon=True).start()

    signal.signal(signal.SIGTERM, shutdown)
    signal.signal(signal.SIGINT, shutdown)
    LOG.info("support_started revision=%s mode=%s", config.get("source_revision", "development"), app.provider.mode)
    try:
        server.serve_forever(poll_interval=0.5)
    finally:
        stop.set()
        server.server_close()
        recovery.join(timeout=15)


if __name__ == "__main__":
    main()
