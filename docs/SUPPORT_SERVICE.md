# Operate the open-source support service

## Purpose and boundary

The independent support page is **https://support.hubstack.cn/projects/agent-c**.
Its home page lists configured projects. Contributors select a one-time USD
amount, approve it on PayPal, and return for a private confirmation. They do not
need a Hubstack, LangLearn or Agent-C account. Contributions do not buy LangLearn
time, create subscriptions, grant product access or promise a feature.

The service runs on the owner's Arch host. It reuses LangLearn's active PayPal
merchant application, but owns its own process, configuration, SQLite database,
invoice namespace and return URLs. It neither calls LangLearn's credit-purchase
API nor reads its customer database. The existing Agent-C daemon, gateway and
PostgreSQL shutdown remains effective. The VPS runs only the exact support
reverse-proxy route; no support application or database is installed there.

This guide owns the support service's operation and limitations. Source is in
[`support_service/`](../support_service/); the independent systemd unit and
Traefik route are in [`deploy/arch/`](../deploy/arch/). Do not use the general
Agent-C production installer to deploy this service.

## Payment flow and evidence

1. `GET /projects/agent-c` creates an essential browser cookie and a signed form.
   No PayPal order is created by viewing the page or changing the amount.
2. `POST /checkout` requires that browser cookie, the exact site Origin, a valid
   form signature and a USD amount from $1 to $500 with at most two decimal
   places. Amounts become integer cents; for example, `$12.50` becomes `1250`.
   SQLite commits the project, title, amount, currency and browser binding before
   any provider mutation. Repeating a form preserves that snapshot; changing
   its amount or browser binding conflicts instead of creating another order.
3. The server creates a `CAPTURE` order through PayPal Orders v2. Its invoice is
   `support-<32-hex-local-id>`. Its custom ID is the configured project slug.
   The merchant ID and USD amount come from server configuration and the saved
   snapshot. PayPal's exact environment-specific approval host, `/checkoutnow`
   path and matching order token are checked before navigation.
4. A same-origin GET bridge navigates to PayPal. This avoids a cross-origin form
   redirect being blocked by the restrictive `form-action 'self'` policy. No
   PayPal JavaScript SDK, merchant secret or payment form runs on this site.
5. After PayPal approval, the return GET verifies the provider order token and
   browser binding. A local script submits the CSRF-protected capture POST; a
   visible completion button supports browsers with JavaScript disabled. The
   GET itself does not capture funds.
6. Before capture, a server-to-server GET must match the order, `CAPTURE` intent,
   reference, invoice, project, merchant, currency and exact amount. After a
   capture POST, the service reads the canonical order again: PayPal's capture
   response can omit fields needed for those checks. Success additionally needs
   exactly one final capture with matching amount and `COMPLETED` capture and
   order states. An order's top-level `COMPLETED` alone never proves payment.

The provider contract follows LangLearn's server-side implementation at Mac
`~/work/lang_learn/cloud/internal/paypal/client.go`, inspected at `7c7b623`, and
PayPal's current [Orders v2 reference](https://developer.paypal.com/api/orders/v2),
[capture endpoint](https://developer.paypal.com/api/orders/v2/orders-capture),
[order statuses](https://developer.paypal.com/api/orders/v2/definitions/order_status/)
and [idempotency guidance](https://developer.paypal.com/api/rest/reference/idempotency/),
consulted on 2026-11-20. This service implements that small contract independently
in Python rather than copying LangLearn's credit ledger and subscription engine.

### Durable retries and uncertain results

[`store.py`](../support_service/store.py) uses SQLite WAL, `synchronous=FULL` and
short `BEGIN IMMEDIATE` transactions. Network calls hold no database transaction.
Each operation claims a 60-second lease with a random fencing token. A late
response cannot overwrite a newer worker's result.

Create and capture use different deterministic UUID `PayPal-Request-Id` values,
derived from the local order and operation. Each provider mutation is admitted
at most three times, within two hours of its first attempt and three hours of
local order creation. These conservative local bounds are not a promise of
unlimited provider idempotency. Attempts and first-attempt time commit before
network calls and survive restarts. After the limit, the request enters manual
review; the service must not reset it or create a replacement automatically.

The recovery worker runs every 30 seconds, checks at most 20 old `capturing` or
`pending` requests, and performs provider GETs only. It never initiates a charge.
A lost capture response can therefore become a confirmed receipt after restart
without another capture POST. User-selected confirmation retries retain the
original request ID and limits. Pending payments remain visibly pending.
Mismatched evidence enters `review` and cannot be retried into success. The lease
and mutation window are checked again immediately before the provider POST,
after token acquisition, so a worker delayed by authentication cannot send a
stale operation.

Receipts are accessible only to the browser that started the contribution;
clearing or losing its seven-day cookie removes that browser's access. Payment
references and timestamps are retained for bookkeeping. No card data, raw payer
profile or raw provider response is stored. A keyed hash of the network address
supports abuse limits. The service trusts forwarded addresses only from the
configured VPS ZeroTier peer. It does not log request URLs, query tokens or
browser cookies.

There is no webhook receiver, refund API, subscription engine, entitlement
delivery or public donation total. Receipts record the original verified
contribution, not current net revenue after refunds. Handle refunds, disputes
and accounting through PayPal. LangLearn's inspected webhook code ignores
unknown provider orders rather than granting time to them
(`cloud/internal/paypal/webhook_payments.go`). No existing webhook registration
is changed by this deployment.

## Host prerequisites and private preparation

The implementation uses Python's standard library, including `sqlite3`; it adds
no Python package dependency. The tested Arch interpreter is Python **3.14.8**.
The browser checks use the existing `ui/` Node/Playwright installation and an
installed Chromium binary. Required infrastructure is the existing systemd user
manager, persistent user services, ZeroTier link and Traefik ingress. DNS for
`support.hubstack.cn` must point to the VPS; its existing `hubstack` certificate
resolver must be functional.

| Component | Exact reference deployment |
| --- | --- |
| Python process | Arch user `bruce`, `hubstack-support.service` |
| Listener | `10.147.16.2:21960`, free at preflight |
| Trusted proxy | VPS ZeroTier address `10.147.16.1` |
| Public origin | `https://support.hubstack.cn` |
| Private source configuration | `~/.config/hubstack-support/config.json`, owner-only mode 0600 |
| Installed private configuration | `~/.config/hubstack-support/runtime.json`, mode 0600 |
| Database | `~/.local/share/hubstack-support/state/support.sqlite3` |
| Releases and manifests | `~/.local/share/hubstack-support/releases/<revision>-<content-hash>/` |
| VPS route | `/etc/traefik/hubstack-support.yml` only |

For the existing host, import only the active merchant values from LangLearn's
private Compose configuration. Run from the Arch checkout:

```sh
cd /home/bruce/work/agent-c
sudo -n python3 tools/deploy/import_support_paypal.py \
  --owner bruce \
  --compose-file /opt/langlearn-cloud/runtime/compose.private.json
```

The importer resolves the mounted `LANGLEARN_PAYPAL_CONFIG_FILE`, reads
`LIVE_CLIENT_ID` and `LIVE_SECRET`, and takes the merchant ID from the selected
Compose service's environment. It generates a separate random cookie secret and
writes the destination without printing any credential. It refuses an existing
destination, a non-live/disabled merchant or a permissive credential file. An
existing destination means preparation has already occurred; preserve it rather
than repeatedly importing or rotating it. Neither LangLearn's files nor its
container are modified. Both services share credential-rotation consequences.

On another host, privately populate
[`config.example.json`](../support_service/config.example.json) with that host's
merchant values, a new `secrets.token_hex(32)` cookie secret, its own database
path and correct addresses. Do not commit the populated file, put secrets in
shell arguments, print it, or switch an existing database from sandbox to live.
The database pins environment, merchant, currency and public origin. Preserve
existing payment records and use a separate database for a different binding.

Check the live merchant without creating an order:

```sh
umask 077
python3 -m support_service.app \
  --config /home/bruce/.config/hubstack-support/config.json --check-provider
```

Expected output includes `"oauth": "ok"`, `"mode": "live"` and
`"orders_created": 0`. This proves OAuth access with that configuration; it does
not prove a donor can settle a payment in every account or country. The command
initializes/checks the private database but does not create a checkout order.

## Verify source, then install

Run on Arch, in `/home/bruce/work/agent-c`:

```sh
python3 -m unittest discover -s support_service/tests -v
systemd-analyze --user verify deploy/arch/hubstack-support.service
git diff --check
```

The tests use temporary databases, loopback HTTP transport fixtures and dummy
credentials. They cannot reach a real PayPal merchant. They exercise immutable
prices, provider evidence, lost-response recovery, leases, duplicate capture
prevention, private receipts, CSRF and stable actual HTTP idempotency headers.

For browser qualification, run a disposable fixture in one terminal:

```sh
python3 -m support_service.tests.browser_server
```

In a second terminal in the same checkout:

```sh
CHROMIUM_PATH=/home/bruce/.cache/ms-playwright/chromium-1223/chrome-linux64/chrome \
  node support_service/tests/browser.mjs
```

If dependencies are absent, first run `npm ci --prefix ui` and install a supported
Playwright Chromium or set `CHROMIUM_PATH` to an existing binary. The path above
is host-specific. The fixture browser completes an artificial PayPal approval
and receipt, then checks desktop, tablet and phone layouts. Stop the fixture
terminal with Ctrl-C when finished. Its database is temporary.

Commit the reviewed support source, then install its immutable release:

```sh
python3 tools/deploy/install_support.py \
  --config /home/bruce/.config/hubstack-support/config.json --publish-ingress
```

This operation enables/restarts only `hubstack-support.service` and publishes
only its Traefik file. It never starts `agent-c-daemon.service`,
`agent-c-gateway.service` or `agent-c-postgres`. It requires support runtime and
deployment files to match their committed source; unrelated pre-existing edits
are not staged or discarded. Release manifests record the exact source revision
and SHA-256 of every installed runtime file. `current` switches atomically;
the previous release link and private runtime config are retained for rollback.

The service has a 192 MiB memory limit, 50% of one CPU, 40 tasks, 256 file
descriptors, at most 16 HTTP workers and four concurrent provider operations.
Provider responses are bounded to 1 MiB. POST bodies are at most 4096 bytes;
chunked uploads are refused. Durable checkout limits admit three requests per
browser per minute, 12 per browser per day, 30 per hashed client per hour and
200 globally per hour. These limits bound resource abuse; they do not guarantee
that every legitimate donor behind a shared network will be admitted.

## Public acceptance and operation

To add another project, edit the `projects` array in the private source
configuration. Each entry needs a unique lowercase hyphen-separated `id`, a
`title`, a short `summary` and an HTTPS `source_url`. For example, an illustrative
`id` of `my-library` would create `/projects/my-library`; this example is not a
claim that such a repository or public page exists. Run the same installer to
apply the configuration, then check that exact page and its directory entry.
Project metadata never chooses a different recipient: all configured projects
use this service's single pinned merchant and USD currency. Existing orders keep
their original project, title and price snapshots after registry edits.

```sh
curl -fsS https://support.hubstack.cn/readyz
curl -fsS https://support.hubstack.cn/projects/agent-c -o /tmp/agent-c-support.html
SUPPORT_LIVE=1 SUPPORT_TEST_ORIGIN=https://support.hubstack.cn \
  CHROMIUM_PATH=/home/bruce/.cache/ms-playwright/chromium-1223/chrome-linux64/chrome \
  node support_service/tests/browser.mjs
systemctl --user show hubstack-support.service -p ActiveState -p MainPID -p MemoryCurrent
systemctl --user show agent-c-daemon.service agent-c-gateway.service \
  -p ActiveState -p UnitFileState
```

The public readiness response must name `hubstack-support`, the installed source
revision and `live` mode. It proves process/database access; it does not perform
a provider request on every probe. Startup separately requires successful OAuth.
The browser probe checks four widths (1440, 768, 390 and 320 pixels), amount
selection and horizontal overflow. In live mode it blocks all browser POSTs;
it cannot create orders or prove a real payment. Screenshots and JSON receipts
are written under ignored `build/support-browser-{fixture,live}/`.

For a delayed payment, use its receipt's **Check payment** action. That action
only reads the provider. If a receipt requests manual review, do not reset its
attempt counters, change its price or blindly create another payment. Match its
local reference, provider order and capture against PayPal's records. Logs can
be inspected with `journalctl --user -u hubstack-support.service -n 50`; logs
contain event codes and local references, not raw provider bodies or credentials.

To stop only this service:

```sh
systemctl --user disable --now hubstack-support.service
```

To return to the previous installed release, first stop the service, then restore
both its link and corresponding private runtime config. Run as `bruce` on Arch:

```sh
systemctl --user stop hubstack-support.service
python3 - <<'PY'
import os
from pathlib import Path
data = Path.home() / '.local/share/hubstack-support'
private = Path.home() / '.config/hubstack-support'
previous = (data / 'previous').resolve(strict=True)
config = (private / 'runtime.previous.json').read_bytes()
candidate = data / 'current.rollback'
candidate.unlink(missing_ok=True)
candidate.symlink_to(previous, target_is_directory=True)
os.replace(candidate, data / 'current')
target = private / 'runtime.rollback.json'
fd = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
with os.fdopen(fd, 'wb') as output:
    output.write(config)
    output.flush()
    os.fsync(output.fileno())
os.replace(target, private / 'runtime.json')
PY
systemctl --user start hubstack-support.service
```

Rollback does not erase, copy over or roll back the payment database. Schema
changes require a separately reviewed procedure. For a consistent backup, use
SQLite's backup API rather than copying a live `.sqlite3` file without its WAL:

```sh
umask 077
python3 - <<'PY'
import sqlite3
from pathlib import Path
state = Path.home() / '.local/share/hubstack-support/state'
target = state / 'support.backup.sqlite3'
if target.exists():
    raise SystemExit('Preserve the existing backup before taking another')
with sqlite3.connect(state / 'support.sqlite3') as source:
    with sqlite3.connect(target) as destination:
        source.backup(destination)
PY
```

Keep the backup private. Restore only with the service stopped and after
reconciling any provider operations newer than the backup; a database restore
does not undo PayPal transactions. Do not silently replace payment history.

## Recorded qualification

On 2026-11-20, the imported live merchant passed a non-charging OAuth preflight
on Arch Python 3.14.8. Twenty-one isolated Python checks passed. The loopback
browser fixture completed approval, server confirmation and its private receipt;
it also passed layout checks at 1440, 768, 390 and 320 pixels. Source inspection
and fixtures do not establish real donor settlement. Public release acceptance
and exact deployed artifact identity are recorded after installation. No real
order, charge or refund is part of routine release acceptance.
