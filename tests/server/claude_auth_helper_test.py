"""Exercise the staged Nexus helper with only temporary synthetic credentials and a fake Claude."""
import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import time
import unittest

HELPER = Path(sys.argv.pop(1)).resolve()
EMAIL = "work@example.test"

CLI = '''#!/usr/bin/python3 -I
import json, os, sys, time
from pathlib import Path
cfg = Path(os.environ["CLAUDE_CONFIG_DIR"])
assert Path.home() == cfg.parent and cfg.parent.resolve() == Path.cwd()
assert not any(x in os.environ for x in ["ANTHROPIC_API_KEY", "CLAUDE_CODE_OAUTH_TOKEN", "HTTP_PROXY", "ANTHROPIC_BASE_URL"])
mode = Path(sys.argv[0]).name
credentials = cfg / ".credentials.json"
if sys.argv[1:3] == ["auth", "login"]:
    assert sys.argv[3:5] == ["--claudeai", "--email"] and "NEXUS_SIGNIN_FLOW" in os.environ
    assert not credentials.exists()
    with credentials.open("w") as f:
        json.dump({"claudeAiOauth":{"accessToken":"fake-access","refreshToken":"fake-refresh","expiresAt":int(time.time()*1000)+2400000,"scopes":["user:profile","user:inference"]}}, f)
    credentials.chmod(0o600)
    (cfg / "account").write_text(mode)
elif sys.argv[1:] == ["auth","status","--json"]:
    assert (cfg / "account").read_text() == mode
    profile = {"loggedIn":True,"authMethod":"claude.ai","apiProvider":"firstParty","email":"work@example.test"}
    if mode == "wrong": profile["email"] = "other@example.test"
    if mode == "absent": profile["email"] = None
    if mode == "bearer": profile["authMethod"] = "oauth_token"
    if mode == "external": profile["apiProvider"] = "bedrock"
    if mode == "badgrant": credentials.write_text("{}")
    print(json.dumps(profile))
else:
    events = [json.loads(x) for x in sys.stdin]
    assert [x["type"] for x in events] == ["control_request", "control_request"]
    assert [x["request"]["subtype"] for x in events] == ["initialize", "get_usage"]
    data = json.loads(credentials.read_text())
    grant = data["claudeAiOauth"]
    if mode != "outage":
        grant.update(accessToken="rotated-access",refreshToken="rotated-refresh", expiresAt=int(time.time()*1000)+2800000)
        credentials.write_text(json.dumps(data))
'''


class Helper(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.account = self.root / "accounts" / "work"
        self.scratch = self.account / ".signin"
        self.scratch.mkdir(parents=True, mode=0o700)
        self.account.chmod(0o700)
        self.path = self.account / "verified-claude.json"
        self.env = {"CLAUDE_CONFIG_DIR": str(self.scratch), "BROWSER": "/usr/bin/true",
                    "NEXUS_SIGNIN_SOCKET": str(self.root / "socket"), "NEXUS_SIGNIN_FLOW":"fixture",
                    "ANTHROPIC_API_KEY":"synthetic-no", "HTTP_PROXY":"synthetic-no"}
        self.env["TMPDIR"] = str(self.root)

    def call(self, op, mode="match", email=EMAIL, account=None):
        cli = self.root / mode
        cli.write_text(CLI.replace("/usr/bin/python3", sys.executable, 1))
        cli.chmod(0o700)
        return subprocess.run([str(HELPER),op,"--claude",str(cli),"--account-dir",str(account or self.account),
                               "--email",email], capture_output=True, env=self.env, timeout=30)

    def test_enroll(self):
        out = self.call("enroll",email="WORK@example.test")
        self.assertEqual((out.returncode,out.stdout,out.stderr),(0,b"",b""))
        self.assertEqual(stat.S_IMODE(self.path.stat().st_mode),0o600)
        self.assertEqual(self.call("access").stdout,b"fake-access\n")
        self.assertEqual(self.call("access",email="else@example.test").returncode,11)
        self.assertEqual(json.loads(self.path.read_bytes())["email"], EMAIL)

    def test_rejection_preserves_existing(self):
        self.assertEqual(self.call("enroll").returncode,0)
        prior = self.path.read_bytes()
        for mode,code in (("wrong",11),("absent",12),("bearer",12),("external",12),("badgrant",12)):
            with self.subTest(mode=mode):
                fail = self.call("enroll",mode)
                self.assertEqual((fail.returncode,fail.stdout,fail.stderr),(code,b"",b""))
                self.assertEqual(self.path.read_bytes(), prior)
                self.assertEqual(list(self.scratch.iterdir()), [])

    def test_legacy_file_is_not_evidence(self):
        (self.account / "oauth-token").write_text("legacy-opaque-token")
        self.assertEqual(self.call("access").returncode,12)
        self.assertEqual(self.call("enroll","wrong").returncode,11)
        self.assertFalse(self.path.exists())

    def test_access_renews_with_native_client(self):
        self.assertEqual(self.call("enroll").returncode,0)
        data = json.loads(self.path.read_text())
        data["credential"]["expiresAt"] = 1
        self.path.write_text(json.dumps(data))
        self.assertEqual(self.call("access","outage").returncode,13)
        self.assertEqual(self.call("access").stdout,b"rotated-access\n")
        self.assertEqual(json.loads(self.path.read_text())["credential"]["refreshToken"],"rotated-refresh")

    def test_can_recover_native_refresh_after_interruption(self):
        self.assertEqual(self.call("enroll").returncode,0)
        store = json.loads(self.path.read_text())
        store["credential"]["expiresAt"] = 1
        self.path.write_text(json.dumps(store))
        stage = self.account/".verified-claude-renew"
        stage.mkdir(mode=0o700)
        (stage/"config").mkdir(mode=0o700)
        signature = hashlib.sha256(json.dumps(store,sort_keys=True,separators=(",",":")).encode()).hexdigest()
        marker = stage/"source.json"
        marker.write_text(json.dumps({"sha256":signature}))
        marker.chmod(0o600)
        file = stage/"config"/".credentials.json"
        file.write_text(json.dumps({"claudeAiOauth":dict(store["credential"],accessToken="recovered",expiresAt=time.time()*1000+900000)}))
        file.chmod(0o600)
        self.assertEqual(self.call("access","outage").stdout,b"recovered\n")
        self.assertFalse(stage.exists())

    def test_different_account_and_hardlink(self):
        self.assertEqual(self.call("enroll").returncode,0)
        elsewhere = self.root/"accounts"/"other"
        elsewhere.mkdir(mode=0o700)
        self.assertEqual(self.call("access",account=elsewhere).returncode,12)
        os.link(self.path, elsewhere/self.path.name)
        self.assertEqual(self.call("access").returncode,13)


if __name__ == "__main__":
    unittest.main()
