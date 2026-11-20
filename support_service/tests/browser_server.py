"""Loopback-only fixture with no PayPal network calls or real merchant access."""
import argparse
import json
from pathlib import Path
import tempfile

from support_service.app import App, BoundedServer
from support_service.tests.test_support import FixturePayPal


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=21961)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="support-browser-") as directory:
        config = json.loads(Path("support_service/config.example.json").read_text())
        config.update(database=directory + "/fixture.sqlite3", public_url=f"http://127.0.0.1:{args.port}",
                      cookie_secret="11" * 32)
        app = App(config, FixturePayPal())
        server = BoundedServer(("127.0.0.1", args.port), app)
        print(config["public_url"], flush=True)
        try:
            server.serve_forever()
        finally:
            server.server_close()


if __name__ == "__main__":
    main()
