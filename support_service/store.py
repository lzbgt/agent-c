"""Durable immutable price snapshots, operation leases and private receipts."""
import contextlib
import json
import secrets
import sqlite3
import time
from pathlib import Path


class CheckoutError(Exception):
    def __init__(self, message, status=409):
        self.status = status
        super().__init__(message)


class Store:
    def __init__(self, path, binding):
        self.path = str(path)
        Path(path).parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        with self.connect() as db:
            db.execute("PRAGMA journal_mode=WAL")
            version = db.execute("PRAGMA user_version").fetchone()[0]
            if version not in (0, 1):
                raise ValueError("Unsupported support database schema; preserve it for review")
            db.executescript("""
                CREATE TABLE IF NOT EXISTS metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL);
                CREATE TABLE IF NOT EXISTS orders (
                    id TEXT PRIMARY KEY, owner TEXT NOT NULL, client TEXT NOT NULL,
                    project TEXT NOT NULL, title TEXT NOT NULL, cents INTEGER NOT NULL,
                    currency TEXT NOT NULL, state TEXT NOT NULL DEFAULT 'new',
                    provider_id TEXT UNIQUE, approval_url TEXT NOT NULL DEFAULT '',
                    capture_id TEXT UNIQUE, created REAL NOT NULL, updated REAL NOT NULL,
                    create_attempts INTEGER NOT NULL DEFAULT 0, capture_attempts INTEGER NOT NULL DEFAULT 0,
                    create_started REAL NOT NULL DEFAULT 0, capture_started REAL NOT NULL DEFAULT 0,
                    lease TEXT NOT NULL DEFAULT '', lease_until REAL NOT NULL DEFAULT 0,
                    next_try REAL NOT NULL DEFAULT 0, error TEXT NOT NULL DEFAULT '');
                CREATE INDEX IF NOT EXISTS orders_owner ON orders(owner, created);
                CREATE INDEX IF NOT EXISTS orders_client ON orders(client, created);
                CREATE INDEX IF NOT EXISTS orders_recovery ON orders(state, updated);
                PRAGMA user_version=1;
            """)
        with self.transaction() as db:
            encoded = json.dumps(binding, sort_keys=True)
            row = db.execute("SELECT value FROM metadata WHERE key='provider'").fetchone()
            if row and row[0] != encoded:
                raise ValueError("Database is bound to a different provider environment or merchant")
            db.execute("INSERT OR IGNORE INTO metadata VALUES ('provider', ?)", (encoded,))

    @contextlib.contextmanager
    def connect(self):
        db = sqlite3.connect(self.path, timeout=5, isolation_level=None)
        db.row_factory = sqlite3.Row
        db.execute("PRAGMA synchronous=FULL")
        try:
            yield db
        finally:
            db.close()

    @contextlib.contextmanager
    def transaction(self):
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            try:
                yield db
                db.commit()
            except BaseException:
                db.rollback()
                raise

    def get(self, identifier, owner=None):
        with self.connect() as db:
            row = db.execute("SELECT * FROM orders WHERE id=?", (identifier,)).fetchone()
        if not row or (owner is not None and row["owner"] != owner):
            raise CheckoutError("Support request not found in this browser.", 404)
        return dict(row)

    def create(self, identifier, owner, client, project, cents):
        now = time.time()
        with self.transaction() as db:
            row = db.execute("SELECT * FROM orders WHERE id=?", (identifier,)).fetchone()
            if row:
                if (row["owner"], row["project"], row["cents"]) != (owner, project["id"], cents):
                    raise CheckoutError("This support request already has a different amount. Open the project page to start again.")
                return dict(row)
            # Persisted rate limits survive process restarts. Never store raw addresses.
            for column, value, window, maximum in (("owner", owner, 60, 3), ("owner", owner, 86400, 12),
                                                    ("client", client, 3600, 30)):
                count = db.execute(f"SELECT count(*) FROM orders WHERE {column}=? AND created>?",
                                   (value, now - window)).fetchone()[0]
                if count >= maximum:
                    raise CheckoutError("Too many support requests. Please try again later.", 429)
            if db.execute("SELECT count(*) FROM orders WHERE created>?", (now - 3600,)).fetchone()[0] >= 200:
                raise CheckoutError("Checkout is busy. Please try again later.", 429)
            db.execute("""INSERT INTO orders(id,owner,client,project,title,cents,currency,created,updated)
                          VALUES (?,?,?,?,?,?,'USD',?,?)""",
                       (identifier, owner, client, project["id"], project["title"], cents, now, now))
        return self.get(identifier, owner)

    def claim(self, identifier, operation):
        now = time.time()
        if operation not in ("create", "capture", "reconcile"):
            raise ValueError("Unknown operation")
        with self.transaction() as db:
            row = dict(db.execute("SELECT * FROM orders WHERE id=?", (identifier,)).fetchone())
            if row["state"] in ("completed", "review"):
                return row, None
            if row["lease_until"] > now or row["next_try"] > now:
                raise CheckoutError("This request is being checked. Please wait a few seconds.", 409)
            if operation == "create" and row["provider_id"]:
                return row, None
            if operation != "create" and not row["provider_id"]:
                raise CheckoutError("Continue to PayPal before confirming support.")
            token = secrets.token_hex(16)
            if operation != "reconcile":
                if operation == "capture" and row["state"] == "pending":
                    raise CheckoutError("PayPal is still processing this payment. Use Check payment.")
                first = row[operation + "_started"] or now
                # Stay well inside PayPal's documented idempotency retention window.
                if row[operation + "_attempts"] >= 3 or now - first > 7200 or now - row["created"] > 10800:
                    db.execute("UPDATE orders SET state='review',error='retry_limit',updated=? WHERE id=?",
                               (now, identifier))
                    row.update(state="review", error="retry_limit")
                    return row, None
                state = "creating" if operation == "create" else "capturing"
                db.execute(f"""UPDATE orders SET {operation}_attempts={operation}_attempts+1,
                    {operation}_started=?,state=?,lease=?,lease_until=?,updated=? WHERE id=?""",
                           (first, state, token, now + 60, now, identifier))
            else:
                db.execute("UPDATE orders SET lease=?,lease_until=?,updated=? WHERE id=?",
                           (token, now + 60, now, identifier))
            row = dict(db.execute("SELECT * FROM orders WHERE id=?", (identifier,)).fetchone())
        return row, token

    def finish(self, identifier, token, **changes):
        allowed = {"state", "provider_id", "approval_url", "capture_id", "error", "next_try"}
        if not changes.keys() <= allowed:
            raise ValueError("Unexpected payment update")
        changes.update(lease="", lease_until=0, updated=time.time())
        with self.transaction() as db:
            count = db.execute("UPDATE orders SET " + ",".join(f"{key}=?" for key in changes)
                               + " WHERE id=? AND lease=?", (*changes.values(), identifier, token)).rowcount
        if count != 1:
            raise CheckoutError("Another check has taken over this payment. Refresh its receipt.")
        return self.get(identifier)

    def authorize_mutation(self, identifier, token, operation):
        row = self.get(identifier)
        now = time.time()
        if (row["lease"] != token or row["lease_until"] <= now
                or now - row[operation + "_started"] > 7200 or now - row["created"] > 10800):
            raise CheckoutError("This operation expired before contacting PayPal. Refresh its receipt.")

    def check(self):
        with self.connect() as db:
            if not db.execute("SELECT value FROM metadata WHERE key='provider'").fetchone():
                raise ValueError("Payment database binding is missing")
            db.execute("SELECT id FROM orders LIMIT 1").fetchone()

    def recoverable(self):
        with self.connect() as db:
            return [row[0] for row in db.execute("""SELECT id FROM orders WHERE
                state IN ('capturing','pending') AND provider_id IS NOT NULL
                AND lease_until<? AND updated<? AND next_try<? ORDER BY updated LIMIT 20""",
                (time.time(), time.time() - 30, time.time()))]
