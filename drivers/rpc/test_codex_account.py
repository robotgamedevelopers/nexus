import asyncio
import base64
import http.server
import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from unittest import mock

from nexus_rpc.account_runtime import process_env
from nexus_rpc.codex_account import PROFILE, chat_command, grant_stamp, login_command, staged_login
from nexus_rpc.driver import Rpc

BINARY = shutil.which("codex")


class Accounts(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name).resolve()
        self.operator = self.root / "operator"
        self.home = self.operator / ".nexus/server/harness/codex/one"
        self.home.mkdir(parents=True, mode=0o700)
        self.cwd = self.root / "workspace"
        self.cwd.mkdir()
        env = mock.patch.dict(os.environ, {"HOME": str(self.operator)})
        env.start()
        self.addCleanup(env.stop)

    def write_login(self, home, token="local-only"):
        file = home / ".codex/auth.json"
        record = {
            "auth_mode": "chatgpt",
            "tokens": dict.fromkeys(("id_token", "access_token", "refresh_token"), token),
        }
        file.write_text(json.dumps(record))
        file.chmod(0o600)
        return file

    def test_personal_workspace_and_email_before_promotion(self):
        server = self.operator / ".nexus/server"
        for directory, name, config in [
            ("profile", "config", {"personal_chatgpt_accounts": ["one"]}),
            ("nexus-config", "accounts", {"accounts": [{"provider": "codex", "name": "one", "email": "me@example.com"}]})
        ]:
            (server / directory).mkdir(exist_ok=True)
            (server / directory / (name + ".json")).write_text(json.dumps(config))
        for email, plan, accept in [("me@example.com", "plus", True), ("me@example.com", "business", False),
                                    ("someone@example.com", "pro", False)]:
            with staged_login(self.home) as (stage, promote):
                data = {"email": email, "https://api.openai.com/auth": {"chatgpt_plan_type": plan, "chatgpt_account_id": "example"}}
                token = "x." + base64.urlsafe_b64encode(json.dumps(data).encode()).decode() + ".x"
                file = self.write_login(stage, token)
                record = json.loads(file.read_text())
                record["tokens"]["account_id"] = "example"
                file.write_text(json.dumps(record))
                if accept:
                    promote()
                    before = grant_stamp(self.home)
                else:
                    with self.assertRaisesRegex(ValueError, "Personal workspace"):
                        promote()
                    self.assertEqual(grant_stamp(self.home), before)

    def test_grant_stamp_requires_a_complete_private_chatgpt_login(self):
        self.assertIsNone(grant_stamp(self.home))
        (self.home / ".codex").mkdir(mode=0o700)
        auth = self.write_login(self.home)
        self.assertIsInstance(grant_stamp(self.home), str)
        auth.parent.chmod(0o777)
        self.assertIsNone(grant_stamp(self.home))
        auth.parent.chmod(0o700)
        auth.chmod(0o644)
        self.assertIsNone(grant_stamp(self.home))
        auth.chmod(0o600)
        alias = self.root / "other-name"
        os.link(auth, alias)
        self.assertIsNone(grant_stamp(self.home))
        alias.unlink()
        record = json.loads(auth.read_text())
        record["OPENAI_API_KEY"] = "fixture-key"
        auth.write_text(json.dumps(record))
        self.assertIsNone(grant_stamp(self.home))
        auth.write_text("{")
        self.assertIsNone(grant_stamp(self.home))
        auth.unlink()
        auth.mkdir(mode=0o700)
        self.assertIsNone(grant_stamp(self.home))
        auth.rmdir()
        auth.symlink_to(self.root / "absent")
        self.assertIsNone(grant_stamp(self.home))

    def test_promotion_retries_sync_and_receipt_without_replacing_login(self):
        with staged_login(self.home) as (stage, promote):
            self.write_login(stage)
            expected = grant_stamp(stage)
            sync = os.fsync

            def fail_first_directory(fd):
                if stat.S_ISDIR(os.fstat(fd).st_mode):
                    raise OSError("injected directory sync error")
                sync(fd)

            with mock.patch("os.fsync", side_effect=fail_first_directory):
                with self.assertRaises(OSError):
                    promote()
            self.assertEqual(grant_stamp(self.home), expected)
            self.assertIsNone(grant_stamp(stage))
            promote()  # directory sync retry
            promote()  # receipt write retry
            self.assertEqual(grant_stamp(self.home), expected)
            self.assertNotIn("local-only", expected)
            auth = self.home / ".codex/auth.json"
            auth.rename(auth.with_suffix(".older"))
            self.write_login(self.home, "different-local-grant")
            with self.assertRaisesRegex(ValueError, "differs from"):
                promote()

    def test_cancel_and_incomplete_login_preserve_existing_auth(self):
        dest = self.home / ".codex"
        dest.mkdir(mode=0o700)
        (dest / "auth.json").write_bytes(b"old fixture")
        with staged_login(self.home) as (stage, promote):
            artifact = stage / ".codex/auth.json"
            artifact.write_bytes(b"{}")
            artifact.chmod(0o600)
            with self.assertRaisesRegex(ValueError, "finish writing"):
                promote()
        self.assertFalse(stage.exists())
        self.assertEqual((dest / "auth.json").read_bytes(), b"old fixture")

    def test_login_rejects_credential_symlink(self):
        with staged_login(self.home) as (stage, promote):
            file = self.root / "sibling"
            file.touch(mode=0o600)
            (stage / ".codex/auth.json").symlink_to(file)
            with self.assertRaises(OSError):
                promote()
        self.assertFalse((self.home / ".codex/auth.json").exists())

    @unittest.skipUnless(BINARY and sys.platform == "darwin", "requires native Codex on macOS")
    def test_real_sandbox(self):
        private = self.home / "private"
        sibling = self.root / "other"
        keep_out = self.cwd / "excluded"
        keep_out.mkdir()
        denied = keep_out / "private"
        for path in (private, denied, sibling):
            path.touch()
        alias = self.cwd / "alias"
        alias.symlink_to(private)
        (self.home / ".codex").mkdir()
        cache = self.root / "cache"
        cache.mkdir()
        for limited in (False, True):
            env = process_env("codex", self.home)
            command = chat_command(BINARY, self.home, self.cwd, keep_out=[keep_out], limited=limited, write_dirs=[str(cache)])
            opts = command[3:]
            for path, success in ((cache / "object", True), (self.root / "unlisted", False)):
                res = subprocess.run([BINARY, "sandbox", *opts, "-P", PROFILE, "--", "/usr/bin/touch", str(path)],
                                     cwd=self.cwd, env=env, capture_output=True, timeout=10)
                self.assertEqual(res.returncode == 0, success, res.stderr)
            for path in (private, denied, alias, sibling):
                with self.subTest(limited=limited, path=path.name):
                    res = subprocess.run(
                        [BINARY, "sandbox", *opts, "-P", PROFILE, "--", "/bin/cat", str(path)],
                        cwd=self.cwd,
                        env=env,
                        check=False,
                        capture_output=True,
                        timeout=10,
                    )
                    if path == sibling and not limited:
                        self.assertEqual(res.returncode, 0)
                    else:
                        self.assertNotEqual(res.returncode, 0)
                        self.assertIn(b"Operation not permitted", res.stderr)
            res = subprocess.run(
                [BINARY, "sandbox", *opts, "-P", PROFILE, "--", "/usr/bin/touch", "ok"],
                cwd=self.cwd,
                env=env,
                check=False,
                capture_output=True,
                timeout=10,
            )
            self.assertEqual(res.returncode, 0)
            self.assertTrue((self.cwd / "ok").is_file())

    @unittest.skipUnless(BINARY and sys.platform == "darwin", "requires native Codex on macOS")
    def test_real_app_server_selects_profile_for_new_and_resumed_threads(self):
        (self.home / ".codex").mkdir(mode=0o700)

        async def run():
            process = await asyncio.create_subprocess_exec(
                *chat_command(BINARY, self.home, self.cwd, limited=True),
                stdin=-1,
                stdout=-1,
                stderr=-3,
                cwd=self.cwd,
                env=process_env("codex", self.home),
            )
            try:
                rpc = Rpc(process)
                await rpc.call(
                    "initialize",
                    {
                        "clientInfo": {"name": "nexus-test", "version": "1"},
                        "capabilities": {"experimentalApi": True},
                    },
                )
                await rpc.notify("initialized", {})
                self.assertIsNone((await rpc.call("account/read", {"refreshToken": False}))["account"])
                options = {
                    "permissions": PROFILE,
                    "approvalPolicy": "never",
                    "cwd": str(self.cwd),
                    "runtimeWorkspaceRoots": [str(self.cwd)],
                }
                for method in ("thread/start", "thread/resume"):
                    out = await rpc.call(method, options)
                    self.assertEqual(out["activePermissionProfile"]["id"], PROFILE)
                    self.assertEqual(out["runtimeWorkspaceRoots"], [str(self.cwd)])
                    self.assertEqual(out["approvalPolicy"], "never")
                    options["threadId"] = out["thread"]["id"]
                    if method == "thread/start":
                        # An entirely empty thread has no rollout to resume.
                        await rpc.call(
                            "thread/shellCommand",
                            {"threadId": options["threadId"], "command": "/usr/bin/true"},
                        )
                        while True:
                            event, _ = await rpc.event()
                            if event == "turn/completed":
                                break
            finally:
                process.stdin.close()
                await process.communicate()

        asyncio.run(asyncio.wait_for(run(), 15))

    @unittest.skipUnless(BINARY, "requires native Codex")
    def test_real_device_login_with_local_issuer(self):
        paths = []

        def encode(obj):
            return base64.urlsafe_b64encode(json.dumps(obj).encode()).decode().rstrip("=")

        token = ".".join((encode({"alg": "none"}), encode({"sub": "fixture"}), "fixture"))

        class Issuer(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_POST(self):
                self.rfile.read(int(self.headers.get("Content-Length", "0")))
                paths.append(self.path)
                replies = {
                    "/api/accounts/deviceauth/usercode": {
                        "device_auth_id": "local",
                        "user_code": "TEST-ONLY",
                        "interval": "1",
                    },
                    "/api/accounts/deviceauth/token": {
                        "authorization_code": "local",
                        "code_challenge": "local",
                        "code_verifier": "local",
                    },
                    "/oauth/token": {
                        "id_token": token,
                        "access_token": "local-only",
                        "refresh_token": "local-only",
                    },
                }
                payload = json.dumps(replies.get(self.path, {})).encode()
                self.send_response(200 if self.path in replies else 404)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)

        with http.server.ThreadingHTTPServer(("127.0.0.1", 0), Issuer) as service:
            thread = threading.Thread(target=service.serve_forever, daemon=True)
            thread.start()
            try:
                with staged_login(self.home) as (stage, promote):
                    base = f"http://127.0.0.1:{service.server_port}"
                    run = subprocess.run(
                        [*login_command(BINARY, stage), "--experimental_issuer", base],
                        env=process_env("codex", stage),
                        cwd=stage,
                        check=False,
                        capture_output=True,
                        timeout=12,
                    )
                    self.assertEqual(run.returncode, 0)
                    self.assertIn(b"TEST-ONLY", run.stdout)
                    self.assertIn((base + "/codex/device").encode(), run.stdout)
                    approved = grant_stamp(stage)
                    self.assertIsNotNone(approved)
                    promote()
                    promote()
                    self.assertEqual(grant_stamp(self.home), approved)
            finally:
                service.shutdown()
                thread.join()
        self.assertEqual(
            paths, ["/api/accounts/deviceauth/usercode", "/api/accounts/deviceauth/token", "/oauth/token"]
        )
        credential = self.home / ".codex/auth.json"
        self.assertEqual(stat.S_IMODE(credential.stat().st_mode), 0o600)
        self.assertFalse(stage.exists())
        self.assertFalse((self.operator / ".codex/auth.json").exists())
