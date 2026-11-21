import contextlib
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
HARNESSES = ("codex", "antigravity", "grok", "muse")


class Protocol(unittest.TestCase):
    @contextlib.contextmanager
    def driver(self, harness, *args, rejected=False):
        with tempfile.TemporaryDirectory() as root:
            account = Path(root) / ".nexus/server/harness" / harness / "sample"
            account.mkdir(parents=True, mode=0o700)
            if rejected:
                (account / "signed-out").touch()
            fake = Path(root) / harness
            fake.write_text("#!" + sys.executable + "\n" + (HERE / "fake_harness.py").read_text())
            fake.chmod(0o700)
            with tempfile.TemporaryFile() as instructions:
                instructions.write(b"Standing instructions from a synthetic home.")
                instructions.seek(0)
                cmd = [
                    sys.executable,
                    "-m",
                    "nexus_rpc.driver",
                    harness,
                    "--binary",
                    str(fake),
                    "--cwd",
                    root,
                    "--account",
                    "sample",
                    "--startup-context-fd",
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
                    env={**os.environ, "HOME": root, "META_API_KEY": "fake", "OPENAI_API_KEY": "fake"},
                )
                try:
                    self.assertEqual(self.read(p)["harness"], harness)
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

    def test_reject_credentials_and_unsupported_scope(self):
        for h in HARNESSES:
            with self.subTest(h=h), self.driver(h, rejected=True) as (p, account):
                events = self.reply(p, 1)
                self.assertEqual(events[-2]["code"], "no-credential", events)
                self.assertEqual(p.wait(timeout=3), 1)
                self.assertFalse((account / "sent").exists())
            with self.driver(h, "--limit", "cwd") as (p, account):
                self.assertEqual(self.read(p)["kind"], "error")
                self.assertFalse((account / "sent").exists())

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
                while self.read(p)["kind"] != "session":
                    pass
                self.send(p, "stop", turn=1)
                while (event := self.read(p))["kind"] != "done":
                    pass
                self.assertEqual(event["outcome"], "stopped")
                self.assertEqual(self.reply(p, 2)[-1]["outcome"], "ok")


if __name__ == "__main__":
    unittest.main()
