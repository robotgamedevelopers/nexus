"""Exercise the private sign-in page without a browser or an external provider."""

import http.cookiejar
import json
import sys
import time
import urllib.request
from pathlib import Path


def visit(url):
    opener = urllib.request.build_opener(
        urllib.request.ProxyHandler({}),
        urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()),
    )
    with opener.open(url, timeout=3) as response:
        assert b"fixture-account" in response.read()
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        with opener.open(url + "/status", timeout=3) as response:
            phase = json.load(response)["phase"]
        if phase != "pending":
            return 0 if phase == "signed-in" else 1
        time.sleep(0.02)
    return 1


if __name__ == "__main__":
    if sys.argv[1] == "visit":
        sys.exit(visit(sys.argv[2]))
    # The server spawns this fixture with the same provider, binary and account arguments as its installed entry point.
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "drivers/rpc"))
    from nexus_rpc.enroll import main

    sys.exit(main())
