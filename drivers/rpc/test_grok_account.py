"""Grok's native ACP and device flow against a loopback-only fake provider."""

import asyncio
import json
import os
import subprocess
import shutil
import sys
import tempfile
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from unittest.mock import Mock, patch

from nexus_rpc import grok_account
from nexus_rpc.account_runtime import process_env
from nexus_rpc.backends import acp
from nexus_rpc.driver import Rpc


CLIENT_ID = "b1a00492-073a-47ea-816f-4c329264a828"


class GrokGrantStamp(unittest.TestCase):
    def setUp(self):
        self.home = Path(self.enterContext(tempfile.TemporaryDirectory()))
        self.data = self.home / ".grok"
        self.path = self.data / "auth.json"
        self.scope = "https://auth.x.ai::" + CLIENT_ID
        self.grant = {"key": "fixture-only", "auth_mode": "oidc", "user_id": "test-user",
                      "create_time": "2026-01-01T00:00:00Z", "expires_at": "2099-01-01T00:00:00Z",
                      "oidc_issuer": "https://auth.x.ai", "oidc_client_id": CLIENT_ID}

    def write(self, store):
        self.data.mkdir(mode=0o700, exist_ok=True)
        self.path.write_text(json.dumps(store))
        self.path.chmod(0o600)

    def test_read_only_private_file_stamp(self):
        self.assertIsNone(grok_account.grant_stamp(self.home))
        self.assertEqual(list(self.home.iterdir()), [])
        self.write({self.scope: self.grant})
        info = self.path.stat()
        expected = f"{info.st_dev}:{info.st_ino}:{info.st_size}:{info.st_mtime_ns}"
        self.assertEqual(grok_account.grant_stamp(self.home), expected)
        alternate = self.data / "moved"
        self.path.rename(alternate)
        alternate.rename(self.path)
        self.assertEqual(grok_account.grant_stamp(self.home), expected)
        self.path.chmod(0o644)
        self.assertIsNone(grok_account.grant_stamp(self.home))
        self.assertEqual(self.path.stat().st_mode & 0o777, 0o644)
        self.path.chmod(0o600)
        self.data.chmod(0o755)
        self.assertIsNone(grok_account.grant_stamp(self.home))
        self.data.chmod(0o700)
        os.link(self.path, alternate)
        self.assertIsNone(grok_account.grant_stamp(self.home))
        self.path.unlink()
        self.path.symlink_to(alternate)
        self.assertIsNone(grok_account.grant_stamp(self.home))

    def test_only_a_matching_usable_browser_grant_qualifies(self):
        for store in ({}, {"other": self.grant}, {self.scope: self.grant, "xai::api_key": {}},
                      *({self.scope: {**self.grant, **change}} for change in (
                          {"key": ""}, {"auth_mode": "api_key"}, {"oidc_issuer": "https://example.invalid"},
                          {"oidc_client_id": "other"}, {"expires_at": "2000-01-01T00:00:00Z"},
                          {"create_time": None}, {"expires_at": "tomorrow"}))):
            self.write(store)
            self.assertIsNone(grok_account.grant_stamp(self.home))
        self.write({self.scope: {**self.grant, "expires_at": "2000-01-01T00:00:00Z", "refresh_token": "renew"}})
        self.assertIsNotNone(grok_account.grant_stamp(self.home))
        self.path.write_text("{")
        self.assertIsNone(grok_account.grant_stamp(self.home))


class FixtureProvider(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def reply(self, status, kind, body):
        self.send_response(status)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/settings":
            self.reply(200, "application/json", b'{"subscription_tier":"supergrok","subscription_tier_display":"SuperGrok"}')
            return
        if self.path == "/billing?format=credits":
            self.reply(200, "application/json", b'{"config":{}}')
            return
        self.reply(404, "application/json", b"{}")

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        if self.path == "/oauth2/device/code":
            self.reply(200, "application/json", json.dumps({
                "user_code": "TEST-ONLY", "device_code": "private-fixture-code",
                "verification_uri": "https://accounts.x.ai/device",
                "verification_uri_complete": "https://accounts.x.ai/device?user_code=TEST-ONLY",
                "expires_in": 300, "interval": 1,
            }).encode())
            return
        if self.path == "/oauth2/token":
            if self.server.approve_login:
                self.reply(200, "application/json", b'{"access_token":"issued-fixture","refresh_token":"refresh-fixture","expires_in":3600}')
            else:
                self.reply(400, "application/json", b'{"error":"access_denied"}')
            return
        if self.path != "/responses":
            self.reply(404, "application/json", b"{}")
            return
        request = json.loads(body)
        # Grok also asks the fixture to label/summarize turns. Record the actual
        # dialogue whose final user input contains the ACP embedded resource.
        inputs = request.get("input", [])
        if inputs and str(inputs[-1].get("content", "")).endswith("</attached_files>\n\n</system-reminder>"):
            self.server.turns.append((self.headers.get("Authorization"), request))
        result = {"id": "resp_fixture", "object": "response", "created_at": 1,
                  "model": request["model"], "status": "completed",
                  "output": [{"type": "message", "id": "msg_fixture", "role": "assistant",
                              "status": "completed", "content": [{"type": "output_text", "text": "Fixture reply.", "annotations": []}]}],
                  "usage": {"input_tokens": 3, "output_tokens": 3, "total_tokens": 6, "input_tokens_details": {"cached_tokens": 0},
                            "output_tokens_details": {"reasoning_tokens": 0}}}
        events = [
            {"type": "response.created", "sequence_number": 0,
             "response": {**result, "status": "in_progress", "output": []}},
            {"type": "response.output_text.delta", "sequence_number": 1, "output_index": 0,
             "item_id": "msg_fixture", "content_index": 0, "delta": "Fixture reply."},
            {"type": "response.completed", "sequence_number": 2, "response": result},
        ]
        stream = "".join("data: " + json.dumps(event) + "\n\n" for event in events)
        self.reply(200, "text/event-stream", (stream + "data: [DONE]\n\n").encode())


@unittest.skipUnless(sys.platform == "darwin" and shutil.which("grok"), "requires the native macOS Grok CLI")
class NativeGrok(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name).resolve()
        self.enterContext(patch.dict(os.environ, {"HOME": str(self.root)}))
        self.binary = shutil.which("grok")
        self.http = ThreadingHTTPServer(("127.0.0.1", 0), FixtureProvider)
        self.http.turns = []
        self.http.approve_login = False
        self.url = f"http://127.0.0.1:{self.http.server_port}"
        thread = threading.Thread(target=self.http.serve_forever, daemon=True)
        thread.start()
        self.addCleanup(thread.join)
        self.addCleanup(self.http.server_close)
        self.addCleanup(self.http.shutdown)
        # Preserve every filesystem rule; replace the native Internet grant with loopback alone.
        local = grok_account._SANDBOX.replace('remote tcp "*:443"', 'remote tcp "localhost:*"')
        self.enterContext(patch.object(grok_account, "_SANDBOX", local))

    def account(self, name):
        home = self.root / name
        home.mkdir(mode=0o700)
        data = home / ".grok"
        data.mkdir(mode=0o700)
        (data / "auth.json").write_text(json.dumps({"https://auth.x.ai::" + CLIENT_ID: {
            "key": name + "-test-token", "auth_mode": "oidc", "user_id": name,
            "create_time": "2026-01-01T00:00:00Z", "expires_at": "2099-01-01T00:00:00Z",
            "oidc_issuer": "https://auth.x.ai", "oidc_client_id": CLIENT_ID,
        }}))
        return home

    async def turn(self, home, resume=None):
        command = await asyncio.to_thread(grok_account.chat_command, self.binary, home, str(self.root))
        command[-1:-1] = ["--cli-chat-proxy-base-url", self.url, "--xai-api-base-url", self.url]
        env = {**process_env("grok", home), "GROK_AGENT": "nexus-conversation"}
        room = home / "room"
        proc = await asyncio.create_subprocess_exec(
            *command, cwd=room, env=env, stdin=-1, stdout=-1, stderr=asyncio.subprocess.DEVNULL,
        )
        try:
            turn = Mock(harness="grok", cwd=str(room), resume=resume, model="grok", effort=None,
                        instructions="AI context fixture.", prompt="Reply to the fixture.")
            turn.session.side_effect = lambda session_id: session_id
            await asyncio.wait_for(acp(Rpc(proc), turn), 12)
            turn.credential.assert_called_once_with("SuperGrok")
            turn.missing.assert_not_called()
            self.assertEqual("".join(call.args[0] for call in turn.delta.call_args_list), "Fixture reply.")
            return turn.session.call_args.args[0]
        finally:
            proc.stdin.close()
            try:
                await asyncio.wait_for(proc.wait(), 2)
            except TimeoutError:
                proc.kill()
                await proc.wait()

    async def test_new_and_resumed_dialogues_have_no_local_tools(self):
        first = self.account("first")
        second = self.account("second")
        sid = await self.turn(first)
        await self.turn(first, sid)
        await self.turn(second)
        self.assertEqual([auth for auth, _ in self.http.turns], [
            "Bearer first-test-token", "Bearer first-test-token", "Bearer second-test-token"])
        for _, request in self.http.turns:
            self.assertIn("AI context fixture.", json.dumps(request["input"]))
            for tool in request.get("tools", []):
                self.assertIn(tool["type"], {"web_search", "x_search"}, tool)

    def test_native_sandbox_denies_other_accounts(self):
        own = self.account("own")
        sibling = self.account("sibling")
        # /bin/cat is a harmless stand-in subject to the identical Seatbelt rules.
        command = grok_account._command("/bin/cat", own)

        def read_auth(account):
            return subprocess.run([*command, str(account / ".grok/auth.json")], cwd=own / "room",
                                  stdout=subprocess.PIPE, stderr=subprocess.DEVNULL).returncode

        self.assertEqual(read_auth(own), 0)
        self.assertNotEqual(read_auth(sibling), 0)

    async def test_native_device_flow(self):
        home = self.account("new")
        auth = home / ".grok/auth.json"
        credential = next(iter(json.loads(auth.read_text()).values()))
        credential["oidc_issuer"] = self.url
        auth.write_text(json.dumps({self.url + "::" + CLIENT_ID: credential}))
        original = auth.read_bytes()
        self.assertNotEqual(await self.login(home), 0)
        self.assertEqual(auth.read_bytes(), original)
        self.http.approve_login = True
        self.assertEqual(await self.login(home), 0)
        persisted = json.loads(auth.read_text())[self.url + "::" + CLIENT_ID]
        self.assertEqual(persisted["key"], "issued-fixture")
        self.assertEqual(persisted["refresh_token"], "refresh-fixture")
        self.assertIsNone(grok_account.grant_stamp(home))
        # The real CLI issued this grant against our private loopback test issuer.
        with patch.object(grok_account, "_ISSUER", self.url):
            self.assertIsNotNone(grok_account.grant_stamp(home))

    async def login(self, home):
        command = grok_account.login_command(self.binary, home)
        env = {**process_env("grok", home), "GROK_OAUTH2_ISSUER": self.url, "GROK_OAUTH2_CLIENT_ID": CLIENT_ID}
        proc = await asyncio.create_subprocess_exec(*command, cwd=home, env=env, stdin=asyncio.subprocess.DEVNULL,
                                                    stdout=-1, stderr=asyncio.subprocess.STDOUT)
        try:
            output, _ = await asyncio.wait_for(proc.communicate(), 12)
            self.assertIn(b"https://accounts.x.ai/device", output)
            self.assertIn(b"TEST-ONLY", output)
            self.assertNotIn(b"private-fixture-code", output)
            self.assertNotIn(b"issued-fixture", output)
            return proc.returncode
        finally:
            if proc.returncode is None:
                proc.kill()
                await proc.wait()


if __name__ == "__main__":
    unittest.main()
