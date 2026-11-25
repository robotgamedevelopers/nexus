import asyncio
import errno
import json
import os
import subprocess
import tempfile
import threading
import unittest
from unittest import mock
from contextlib import asynccontextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from nexus_rpc.account_runtime import process_env
from nexus_rpc.backends import Failure, uuid7
from nexus_rpc.driver import Rpc
from nexus_rpc.muse_account import chat_command, grant_stamp, login_command, staged_login, subscription_tier


class AccountTests(unittest.TestCase):
    def test_rejects_unsafe_room(self):
        with tempfile.TemporaryDirectory() as d:
            home = Path(d)
            (home / "room").symlink_to(home)
            with self.assertRaises(Failure):
                chat_command("muse", home, d)

    def test_stamp_accepts_only_private_oauth_files(self):
        with tempfile.TemporaryDirectory() as d:
            home = Path(d)
            self.assertIsNone(grant_stamp(home))
            config = home / ".config" / "muse"
            config.mkdir(parents=True)
            auth = config / "auth.json"
            auth.write_text('{"providers":{"meta":{"mechanism":"api_key","access_token":"fixture"}}}')
            auth.chmod(0o600)
            self.assertIsNone(grant_stamp(home))
            auth.write_text('{"providers":{"meta":{"mechanism":"oauth","access_token":"fixture"}}}')
            info = auth.stat()
            self.assertEqual(grant_stamp(home), f"{info.st_dev}:{info.st_ino}:{info.st_size}:{info.st_mtime_ns}")
            auth.chmod(0o644)
            self.assertIsNone(grant_stamp(home))
            auth.rename(config / "previous")
            auth.symlink_to(config / "previous")
            self.assertIsNone(grant_stamp(home))

    def test_uncertain_subscription_preserves_login(self):
        class MissingStatus:
            async def call(self, method, params):
                if method == "account/read":
                    return {"state": "accountLogin", "credentialRequired": True}
                return {"tierName": "old observation"}

        with self.assertRaises(Failure) as refused:
            asyncio.run(subscription_tier(MissingStatus()))
        self.assertFalse(refused.exception.revoked)


BINARY = os.environ.get("NEXUS_TEST_MUSE_BINARY")


class MetaFixture(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_POST(self):
        self.rfile.read(int(self.headers.get("Content-Length", "0")))
        self.server.requests.append(self.path)
        status = 200
        if self.path == "/oidc/device/authorization/":
            result = {"device_code": "fixture", "user_code": "TEST-CODE", "interval": 1,
                      "expires_in": 60, "verification_uri": "https://auth.meta.com/oauth/device/"}
        elif self.path == "/oidc/device/token/" and self.server.deny_login:
            status, result = 400, {"error": "access_denied"}
        elif self.path == "/oidc/device/token/":
            result = {"access_token": "fixture-access", "refresh_token": "fixture-refresh",
                      "token_type": "Bearer", "expires_in": 3600}
        elif self.path == "/muse-code/key" and self.server.mint_unavailable:
            status, result = 503, {"error": "temporarily_unavailable"}
        elif self.path == "/muse-code/key":
            result = {"api_key": "fixture-key", "base_url": self.server.origin,
                      "is_subs_active": self.server.subscribed, "subs_tier_name": "Fixture"}
        else:
            status, result = 404, {}
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(json.dumps(result).encode())


@unittest.skipUnless(BINARY, "set NEXUS_TEST_MUSE_BINARY to a native Muse with MSP usage/refresh")
class NativeMuseTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.home = Path(tmp.name).resolve()
        self.env = process_env("muse", self.home)
        self.env.update({"http_proxy": "http://127.0.0.1:1", "https_proxy": "http://127.0.0.1:1",
                         "NO_PROXY": "127.0.0.1,localhost"})

    @asynccontextmanager
    async def host(self, *, echo=False, original_settings=False):
        cmd = chat_command(BINARY, self.home, self.home / "room")
        if echo:
            cmd[cmd.index("meta")] = "echo"
        if original_settings:
            cmd = [BINARY, "serve", "--provider", "echo"]
        p = await asyncio.create_subprocess_exec(*cmd, env=self.env, cwd=self.home / "room",
                                                 stdin=-1, stdout=-1, stderr=subprocess.DEVNULL)
        try:
            rpc = Rpc(p)
            await rpc.call("initialize", {"clientInfo": {"name": "nexus", "version": "1"},
                                          "capabilities": {"experimentalApi": True}})
            await rpc.notify("initialized", {})
            yield rpc
        finally:
            p.stdin.close()
            try:
                await asyncio.wait_for(p.wait(), 3)
            except asyncio.TimeoutError:
                p.kill()
                await p.wait()

    def issuer(self):
        fixture = ThreadingHTTPServer(("127.0.0.1", 0), MetaFixture)
        fixture.origin = "http://127.0.0.1:" + str(fixture.server_port)
        fixture.requests = []
        fixture.subscribed = True
        fixture.deny_login = False
        fixture.mint_unavailable = False
        threading.Thread(target=fixture.serve_forever, daemon=True).start()
        self.addCleanup(fixture.server_close)
        self.addCleanup(fixture.shutdown)
        self.env.update({"TBH_AUTH_BASE_URL": fixture.origin, "TBH_MINT_BASE_URL": fixture.origin})
        return fixture

    async def test_real_login_and_cold_subscription_without_model_turn(self):
        fixture = self.issuer()
        async with self.host() as rpc:
            with self.assertRaises(Failure) as signed_out:
                await subscription_tier(rpc)
            self.assertTrue(signed_out.exception.revoked)
        login = await asyncio.create_subprocess_exec(*login_command(BINARY, self.home),
                                                    env=self.env, cwd=self.home,
                                                    stdin=-3, stdout=-3, stderr=-3)
        self.assertEqual(await asyncio.wait_for(login.wait(), 10), 0)
        async with self.host() as rpc:
            self.assertEqual(await subscription_tier(rpc), "Fixture")
        fixture.deny_login = True
        denied = await asyncio.create_subprocess_exec(*login_command(BINARY, self.home),
                                                     env=self.env, cwd=self.home, stdin=-3, stdout=-3, stderr=-3)
        self.assertNotEqual(await asyncio.wait_for(denied.wait(), 10), 0)
        async with self.host() as rpc:
            self.assertEqual(await subscription_tier(rpc), "Fixture")
        fixture.subscribed = False
        async with self.host() as rpc:
            with self.assertRaisesRegex(Failure, "no active subscription"):
                await subscription_tier(rpc)
        self.assertEqual(fixture.requests, ["/oidc/device/authorization/", "/oidc/device/token/",
                                            "/muse-code/key", "/muse-code/key", "/oidc/device/authorization/",
                                            "/oidc/device/token/", "/muse-code/key", "/muse-code/key"])

    async def test_approved_login_survives_mint_outage_and_missing_receipt(self):
        from nexus_rpc import enroll, muse_account
        from nexus_rpc.account_runtime import enrolled

        fixture = self.issuer()
        fixture.mint_unavailable = True
        with staged_login(self.home) as (stage, promote):
            env = {**self.env, **process_env("muse", stage)}
            login = await asyncio.create_subprocess_exec(*login_command(BINARY, stage),
                                                        env=env, cwd=stage, stdin=-3, stdout=-3, stderr=-3)
            self.assertNotEqual(await asyncio.wait_for(login.wait(), 12), 0)
            self.assertIsNotNone(grant_stamp(stage))
            runner = enroll.Login("muse", "fixture", self.home, login_command(BINARY, stage), env,
                                  login_home=stage, promote=promote, module=muse_account, binary=BINARY)
            with mock.patch.object(muse_account, "process_env", return_value=env):
                with self.assertRaises(OSError) as unavailable:
                    await asyncio.to_thread(runner.commit)
                self.assertEqual(unavailable.exception.errno, errno.EAGAIN)
        self.assertFalse(enrolled(self.home, "muse"))
        self.assertIsNotNone(grant_stamp(stage))
        fixture.mint_unavailable = False
        with mock.patch.object(muse_account, "process_env", return_value=env):
            # Crash after native validation and promotion but before receipt publication.
            with mock.patch.object(enroll, "store_receipt", side_effect=OSError(errno.EIO, "interrupted")):
                with self.assertRaises(OSError):
                    await asyncio.to_thread(enroll.recover, self.home, "muse", muse_account, BINARY)
        self.assertIsNotNone(grant_stamp(self.home))
        self.assertFalse(enrolled(self.home, "muse"))
        self.assertTrue(enroll.recover(self.home, "muse", muse_account, BINARY))
        self.assertTrue(enrolled(self.home, "muse"))
        self.assertEqual(fixture.requests.count("/oidc/device/authorization/"), 1)
        self.assertEqual(fixture.requests.count("/oidc/device/token/"), 1)

    async def test_unsubscribed_replacement_keeps_original_grant(self):
        from nexus_rpc import enroll, muse_account

        fixture = self.issuer()
        fixture.subscribed = False
        config = self.home / ".config" / "muse"
        config.mkdir(parents=True)
        original = config / "auth.json"
        original.write_text('{"providers":{"meta":{"mechanism":"oauth","access_token":"old-fixture"}}}')
        original.chmod(0o600)
        prior = grant_stamp(self.home)
        with staged_login(self.home) as (stage, promote):
            env = {**self.env, **process_env("muse", stage)}
            command = login_command(BINARY, stage)
            login = await asyncio.create_subprocess_exec(*command, env=env, cwd=stage, stdin=-3, stdout=-3, stderr=-3)
            self.assertEqual(await asyncio.wait_for(login.wait(), 10), 0)
            runner = enroll.Login("muse", "fixture", self.home, command, env, login_home=stage,
                                  promote=promote, module=muse_account, binary=BINARY, prior=prior)
            with mock.patch.object(muse_account, "process_env", return_value=env):
                with self.assertRaisesRegex(ValueError, "did not qualify"):
                    await asyncio.to_thread(runner.commit)
        self.assertEqual(grant_stamp(self.home), prior)
        self.assertFalse(stage.exists())

    async def test_empty_toolset_replaces_old_session_toolset_on_resume(self):
        session_id = None
        for original in (True, False):
            async with self.host(echo=True, original_settings=original) as rpc:
                if original:
                    session = await rpc.call("session/start", {"workspaceRoot": str(self.home / "room"),
                                                               "commandId": uuid7(), "approvalMode": "denyUnmatched"})
                else:
                    session = await rpc.call("session/resume", {"sessionId": session_id, "commandId": uuid7()})
                session_id = session["session"]["sessionId"]
                await rpc.call("view/subscribe", {"sessionId": session_id, "after": session["viewCursor"]})
                await rpc.call("turn/start", {"sessionId": session_id, "commandId": uuid7(),
                                              "input": [{"type": "text", "text": "Fixture echo."}]})
                while True:
                    method, params = await asyncio.wait_for(rpc.event(), 10)
                    if method == "turn/completed" and params["sessionId"] == session_id:
                        self.assertEqual(params["terminal"], "completed")
                        break
        session_log, = self.home.glob(f".local/share/muse/sessions/*/*/*/{session_id}/session.jsonl")
        events = [json.loads(line).get("payload", {}).get("event", {}) for line in session_log.read_text().splitlines()]
        rosters = [event for event in events if isinstance(event, dict) and "toolset" in event]
        self.assertTrue(rosters[0]["toolset"]["active_tools"])
        self.assertEqual(rosters[-1]["toolset"], {"source": "settings", "mode": "named", "active_tools": []})
        self.assertEqual(rosters[-1]["reminder_roster"]["agents"], [])
