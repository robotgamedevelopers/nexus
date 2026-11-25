import contextlib
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from nexus_rpc import account_runtime

HERE = Path(__file__).resolve().parent
HARNESSES = ("codex", "antigravity", "grok", "muse")


def run_fixture():
    """Exercise the production bridge; substitute only the native executable launch policy."""
    import importlib
    from nexus_rpc.driver import main

    provider = importlib.import_module("nexus_rpc." + sys.argv[1] + "_account")

    def fake_command(binary, home, cwd, **kwargs):
        if sys.argv[1] == "codex":
            assert kwargs["limited"] == ("--limit" in sys.argv)
            if "--keep-out" in sys.argv:
                assert sys.argv[sys.argv.index("--keep-out") + 1] in kwargs["keep_out"]
        (home / "room").mkdir(mode=0o700, exist_ok=True)
        return [binary]

    with patch.object(provider, "chat_command", fake_command):
        sys.exit(main())


class Protocol(unittest.TestCase):
    @contextlib.contextmanager
    def driver(self, harness, *args, rejected=False, receipt=True, root=None, name="sample", primary=False):
        directory = contextlib.nullcontext(root) if root else tempfile.TemporaryDirectory()
        with directory as root:
            account = Path(root) / ".nexus/server/harness" / harness / name
            account.mkdir(parents=True, mode=0o700, exist_ok=True)
            workspace = Path(root) / "workspace"
            workspace.mkdir(exist_ok=True)
            if receipt:
                account_runtime.store_receipt(account, harness)
            if rejected:
                (account / "signed-out").touch()
            fake = Path(root) / harness
            fake.write_text("#!" + sys.executable + "\n" + (HERE / "fake_harness.py").read_text())
            fake.chmod(0o700)
            with tempfile.TemporaryFile() as instructions:
                content = b"Primary session context." if primary else b"Standing instructions from a synthetic home."
                instructions.write(content)
                if primary:
                    (account / "primary-instructions").write_bytes(content)
                instructions.seek(0)
                cmd = [
                    sys.executable,
                    "-c",
                    "from test_driver import run_fixture; run_fixture()",
                    harness,
                    "--binary",
                    str(fake),
                    "--cwd",
                    str(workspace),
                    "--account",
                    name,
                    "--primary-context-fd" if primary else "--startup-context-fd",
                    str(instructions.fileno()),
                    *args,
                ]
                p = subprocess.Popen(
                    cmd,
                    cwd=HERE,
                    pass_fds=[instructions.fileno()],
                    stdin=-1,
                    stdout=-1,
                    stderr=-1,
                    text=True,
                    env={
                        **os.environ, "HOME": root,
                        **dict.fromkeys(("META_API_KEY", "OPENAI_API_KEY", "GEMINI_API_KEY", "XAI_API_KEY",
                                         "SSH_AUTH_SOCK", "OPENAI_BASE_URL", "NEXUS_TEST_SECRET",
                                         "NEXUS_SIGNIN_SOCKET"), "synthetic-test-value"),
                    },
                )
                try:
                    hello = self.read(p)
                    self.assertEqual(hello["harness"], harness)
                    p.driver_features = hello["features"]
                    yield p, account
                finally:
                    p.stdin.close()
                    try:
                        p.wait(timeout=5)
                    finally:
                        if p.poll() is None:
                            p.kill()
                            p.wait()
                        p.stdout.close()
                        p.stderr.close()

    @staticmethod
    def read(p):
        return json.loads(p.stdout.readline())

    @staticmethod
    def send(p, kind, **fields):
        p.stdin.write(json.dumps({"kind": kind, **fields}) + "\n")
        p.stdin.flush()

    def reply(self, p, turn):
        self.send(p, "send", turn=turn, prompt="Example?")
        events = []
        while not events or events[-1]["kind"] != "done":
            events.append(self.read(p))
        return events

    def test_roundtrip(self):
        for h in HARNESSES:
            for args in ([], ["--resume", "missing"], ["--resume", "provider-session"]):
                with self.subTest(h=h, args=args), self.driver(h, *args) as (p, _):
                    for n in (10, 11):
                        events = self.reply(p, n)
                        self.assertEqual(events[-1]["outcome"], "ok", events)
                        self.assertEqual([e["delta"] for e in events if e["kind"] == "text"], ["Reply ✅"])
                        self.assertEqual(
                            [e["id"] for e in events if e["kind"] == "session"], ["provider-session"]
                        )
                        self.assertEqual(
                            [e["code"] for e in events if e["kind"] == "error"],
                            ["session-not-found"] if n == 10 and "missing" in args else [],
                        )

    def test_primary_conversation_and_driver_restart(self):
        for harness in HARNESSES:
            with self.subTest(harness=harness), tempfile.TemporaryDirectory() as home:
                with self.driver(harness, root=home, primary=True) as (p, _):
                    events = self.reply(p, 1)
                    self.assertEqual(events[-1]["outcome"], "ok", events)
                    sid, = [e["id"] for e in events if e["kind"] == "session"]
                with self.driver(harness, "--resume", sid, root=home, primary=True) as (p, _):
                    events = self.reply(p, 2)
                    self.assertEqual(events[-1]["outcome"], "ok", events)
                    self.assertFalse(any(e["kind"] == "error" for e in events))
                    self.assertEqual([e["id"] for e in events if e["kind"] == "session"], [sid])

    def test_reject_credentials(self):
        for h in HARNESSES:
            with self.subTest(h=h), self.driver(h, rejected=True) as (p, account):
                events = self.reply(p, 1)
                self.assertEqual(events[-2]["code"], "no-credential", events)
                self.assertEqual(p.wait(timeout=3), 1)
                self.assertFalse((account / "sent").exists())
                self.assertFalse((account / account_runtime.RECEIPT).exists())
            with self.driver(h, receipt=False) as (p, account):
                error = self.read(p)
                self.assertEqual(error["code"], "no-credential")
                self.assertIn("config sign-in sample", error["fix"])
                self.assertEqual(p.wait(timeout=3), 1)
                self.assertFalse((account / "sent").exists())

    def test_keep_out_requires_scope_and_valid_path(self):
        for extra in (["--keep-out", "/data"], ["--limit", "cwd", "--keep-out", "/"]):
            with self.driver("codex", *extra) as (p, account):
                self.assertEqual(self.read(p)["code"], "other")
                self.assertEqual(p.wait(timeout=3), 1)
                self.assertFalse((account / "sent").exists())

    def test_advertised_scope(self):
        for provider in HARNESSES:
            with self.subTest(provider=provider), self.driver(provider, "--limit", "cwd", "--keep-out", "/extra") as (p, account):
                if "limit" in p.driver_features:
                    self.assertEqual(self.reply(p, 1)[-1]["outcome"], "ok")
                else:
                    self.assertEqual(self.read(p)["code"], "other")
                    self.assertFalse((account / "sent").exists())

    def test_codex_rejects_unexpected_profile_before_turn(self):
        for options in ([], ["--resume", "provider-session"]):
            with self.subTest(options=options), self.driver("codex", *options) as (p, home):
                (home / "wrong-profile").touch()
                events = self.reply(p, 1)
                self.assertIn("other", [e.get("code") for e in events])
                self.assertFalse((home / "sent").exists())
                self.assertTrue((home / account_runtime.RECEIPT).exists())

    def test_temporary_provider_failure_keeps_enrollment(self):
        with self.driver("codex") as (p, home):
            (home / "temporary-auth-error").touch()
            events = self.reply(p, 1)
            self.assertEqual(events[-2]["code"], "harness")
            self.assertTrue((home / account_runtime.RECEIPT).exists())
            self.assertFalse((home / "sent").exists())
            (home / "temporary-auth-error").unlink()
            self.assertEqual(self.reply(p, 2)[-1]["outcome"], "ok")

    def test_turn_failure_routes_only_on_confirmed_exhaustion(self):
        cases = {
            "codex": (
                ({"codexErrorInfo": "usageLimitExceeded"}, "usage-limit"),
                ({"codexErrorInfo": "rateLimitExceeded", "message": "Usage limit reached"}, "harness"),
                ({"codexErrorInfo": "sessionBudgetExceeded"}, "harness"),
            ),
            "grok": (
                ({"kind": "usage_pool_exhausted"}, "usage-limit"),
                ({"kind": "global_rate_limit"}, "harness"),
                ({"kind": "concurrency_limit"}, "harness"),
            ),
            "antigravity": (
                ({"reason": "QUOTA_EXHAUSTED", "message": "Out of credits."}, "usage-limit"),
                ({"reason": "QUOTA_EXHAUSTED", "message": "You have exhausted your quota on this model."}, "harness"),
                ({"reason": "QUOTA_EXHAUSTED"}, "harness"),
            ),
            "muse": (
                ({"quota": {"limitKind": "tierLimit"}}, "usage-limit"),
                ({"message": "step limit reached", "retryable": True}, "harness"),
            ),
        }
        for provider, details in cases.items():
            with self.driver(provider) as (p, home):
                for turn, (detail, wanted) in enumerate(details, start=1):
                    with self.subTest(provider=provider, detail=detail):
                        error = {"code": -32000, "data": detail}
                        (home / "prompt-errors.json").write_text(json.dumps({"response": {"error": error}}))
                        events = self.reply(p, turn)
                        self.assertEqual(events[-2]["code"], wanted, events)
                        self.assertEqual(events[-1]["outcome"], "error")
                        self.assertTrue((home / account_runtime.RECEIPT).exists())

    def test_acp_transcript_and_success_message_cannot_request_rotation(self):
        for provider in ("antigravity", "grok"):
            with self.subTest(provider=provider), self.driver(provider) as (p, home):
                message = "Out of credits."
                events = [{"method": "session/update", "params": {
                    "sessionId": "provider-session",
                    "update": {"sessionUpdate": "agent_message_chunk", "content": {"type": "text", "text": message}},
                }}]
                result = {"stopReason": "end_turn", "_meta": {"message": message}}
                (home / "prompt-errors.json").write_text(json.dumps({"events": events, "response": {"result": result}}))
                reply = self.reply(p, 1)
                self.assertEqual(reply[-1]["outcome"], "ok")
                self.assertEqual([e["delta"] for e in reply if e["kind"] == "text"], [message])

    def test_provider_terminal_quota_and_unrelated_errors(self):
        for provider in ("codex", "muse", "grok"):
            with self.subTest(provider=provider), self.driver(provider) as (p, home):
                for turn in range(1, 3):
                    sid = "provider-session"
                    event_session = sid if turn == 1 else "unrelated-session"
                    if provider == "codex":
                        response = {"turn": {"id": "t"}}
                        events = [
                            {"method": "error", "params": {"threadId": event_session, "turnId": "t", "willRetry": False,
                                                          "error": {"codexErrorInfo": "usageLimitExceeded"}}},
                            {"method": "turn/completed", "params": {"threadId": sid, "turn": {"id": "t", "status": "failed"}}},
                        ]
                    elif provider == "muse":
                        response = {"turnId": "t"}
                        events = [
                            {"method": "turn/completed", "params": {"sessionId": event_session, "turnId": "t", "terminal": "failed",
                                                                   "error": {"quota": {"limitKind": "tierLimit"}}}},
                            {"method": "turn/completed", "params": {"sessionId": sid, "turnId": "t", "terminal": "failed"}},
                        ]
                    else:
                        response = {"stopReason": "cancelled"}
                        events = [{"method": "_x.ai/session/prompt_complete",
                                   "params": {"sessionId": event_session, "errorKind": "usage_limit_reached"}}]
                    (home / "prompt-errors.json").write_text(json.dumps({"events": events, "response": {"result": response}}))
                    reply = self.reply(p, turn)
                    self.assertEqual(reply[-1]["outcome"], "error")
                    self.assertEqual(reply[-2]["code"], "usage-limit" if turn == 1 else "harness")

    def test_two_accounts_of_same_provider(self):
        for provider in HARNESSES:
            with self.subTest(provider=provider), tempfile.TemporaryDirectory() as root:
                with self.driver(provider, root=root) as (first, first_home):
                    with self.driver(provider, root=root, name="second") as (second, second_home):
                        first_events = self.reply(first, 1)
                        self.assertEqual(first_events[-1]["outcome"], "ok", first_events)
                        self.assertTrue((first_home / "sent").exists())
                        self.assertFalse((second_home / "sent").exists())
                        second_events = self.reply(second, 1)
                        self.assertEqual(second_events[-1]["outcome"], "ok", second_events)
                        self.assertIn("provider-second-session", [e.get("id") for e in second_events])
                        (first_home / ".nexus-enrolled").unlink()
                        self.assertEqual(self.reply(first, 2)[-2]["code"], "no-credential")
                        self.assertEqual(self.reply(second, 2)[-1]["outcome"], "ok")

    def test_protocol_refusals(self):
        for message in (
            {"kind": "send", "turn": 1},
            {"kind": "stop", "turn": 0},
            {"kind": "settings", "effort": 2},
            {"kind": "server", "features": [True]},
        ):
            with self.subTest(message=message), self.driver("codex") as (p, account):
                p.stdin.write(json.dumps(message) + "\n")
                p.stdin.flush()
                self.assertEqual(self.read(p)["code"], "protocol")
                self.assertEqual(p.wait(timeout=3), 1)
                self.assertFalse((account / "sent").exists())

    def test_stop_and_continue(self):
        for h in HARNESSES:
            with self.subTest(h=h), self.driver(h) as (p, _):
                self.send(p, "send", turn=1, prompt="hold")
                while (event := self.read(p))["kind"] != "session":
                    self.assertNotIn(event["kind"], {"done", "error"}, event)
                self.send(p, "stop", turn=1)
                while (event := self.read(p))["kind"] != "done":
                    pass
                self.assertEqual(event["outcome"], "stopped")
                self.assertEqual(self.reply(p, 2)[-1]["outcome"], "ok")


class AccountFiles(unittest.TestCase):
    def test_unsafe_path_and_marker(self):
        with tempfile.TemporaryDirectory() as root, patch.dict(os.environ, HOME=root):
            home = account_runtime.home_for("codex", "first", create=True)
            parent = home.parent
            home.rmdir()
            home.symlink_to(parent, target_is_directory=True)
            with self.assertRaises(OSError):
                account_runtime.home_for("codex", "first")
            home.unlink()
            home.mkdir(mode=0o700)
            marker = home / account_runtime.RECEIPT
            for mode, contents in ((0o644, b"codex\n"), (0o600, b"grok\n"), (0o600, b"codex\njunk")):
                marker.write_bytes(contents)
                marker.chmod(mode)
                with self.assertRaises(ValueError):
                    account_runtime.enrolled(home, "codex")
            marker.unlink()
            marker.symlink_to("/dev/null")
            with self.assertRaises(OSError):
                account_runtime.enrolled(home, "codex")

    def test_case_aliases_never_share_a_login(self):
        with tempfile.TemporaryDirectory() as root, patch.dict(os.environ, HOME=root):
            home = account_runtime.home_for("grok", "_example", create=True)
            account_runtime.store_receipt(home, "grok")
            if (home.parent / "_Example").exists():
                with self.assertRaises(ValueError):
                    account_runtime.home_for("grok", "_Example", create=True)
            else:
                second = account_runtime.home_for("grok", "_Example", create=True)
                self.assertIsNone(account_runtime.enrolled(second, "grok"))
            self.assertTrue(account_runtime.enrolled(home, "grok"))

    def test_case_aliases_of_provider_and_receipt_are_refused(self):
        with tempfile.TemporaryDirectory() as root, patch.dict(os.environ, HOME=root):
            home = account_runtime.home_for("grok", "first", create=True)
            account_runtime.store_receipt(home, "grok")
            uppercase = home / ".NEXUS-ENROLLED"
            if not uppercase.exists():
                self.skipTest("filesystem distinguishes case")
            marker = home / account_runtime.RECEIPT
            marker.rename(uppercase)
            with self.assertRaises(ValueError):
                account_runtime.enrolled(home, "grok")
            home.parent.rename(home.parent.with_name("Grok"))
            with self.assertRaises(ValueError):
                account_runtime.home_for("grok", "first")

    def test_receipt_commit_cannot_revive_revoked_pending_enrollment(self):
        with tempfile.TemporaryDirectory() as root, patch.dict(os.environ, HOME=root):
            home = account_runtime.home_for("muse", "work", create=True)
            ticket = home / account_runtime.PENDING
            ticket.write_bytes(b"one")
            ticket.chmod(0o600)
            with self.assertRaises(ValueError):
                account_runtime.store_receipt(home, "muse", pending=b"other")
            self.assertIsNone(account_runtime.enrolled(home, "muse"))
            account_runtime.store_receipt(home, "muse", pending=b"one")
            self.assertFalse(ticket.exists())
            account_runtime.store_receipt(home, "muse", pending=b"one")
            observed = account_runtime.enrolled(home, "muse")
            self.assertTrue(account_runtime.remove_receipt_if_current(home, "muse", observed))
            with self.assertRaises(ValueError):
                account_runtime.store_receipt(home, "muse", pending=b"one")
            self.assertIsNone(account_runtime.enrolled(home, "muse"))

    def test_concurrent_reenrollment_survives_refusal(self):
        with tempfile.TemporaryDirectory() as root, patch.dict(os.environ, HOME=root):
            home = account_runtime.home_for("grok", "example", create=True)
            account_runtime.store_receipt(home, "grok")
            first = account_runtime.enrolled(home, "grok")
            account_runtime.store_receipt(home, "grok")
            second = account_runtime.enrolled(home, "grok")
            pending = home / account_runtime.PENDING
            pending.touch(mode=0o600)
            self.assertFalse(account_runtime.remove_receipt_if_current(home, "grok", first))
            self.assertTrue(pending.exists())
            self.assertTrue(account_runtime.remove_receipt_if_current(home, "grok", second))
            self.assertFalse(pending.exists())
            self.assertIsNone(account_runtime.enrolled(home, "grok"))


if __name__ == "__main__":
    unittest.main()
