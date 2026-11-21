"""Exercise the installed host through its CLI. Uses only a temporary profile."""
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


class PersonalPolicy(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.profile = Path(tmp.name) / "profile"
        shutil.copytree(Path(__file__).parent / "example", self.profile)
        self.command = [sys.executable, "-I", "-m", "nexus_policy"]

    def check(self, *extra):
        return subprocess.run([*self.command, "check", str(self.profile), "review", *extra],
                              text=True, capture_output=True)

    def write(self, source):
        (self.profile / "policy.star").write_text(source)

    def test_live_changes_and_explicit_settings(self):
        capture = self.profile.parent / "capture.py"
        capture.write_text("import json,sys\nprint(json.dumps(sys.argv[1:]))\n")
        def start():
            return json.loads(subprocess.check_output([
                *self.command, "driver", str(self.profile), "review", sys.executable, str(capture),
                "--model", "chosen", "--account", "work", "--limit", "cwd", "--cwd", "/tmp/a b",
                "--keep-out", "/tmp/keep", "--resume", "session"], text=True))
        original = ["--model", "chosen", "--account", "work", "--limit", "cwd", "--cwd", "/tmp/a b",
                    "--keep-out", "/tmp/keep", "--resume", "session"]
        self.assertEqual(start(), original)
        (self.profile / "config.json").write_text('{"friends":{"review":{"model":"other","effort":"high"}}}')
        self.assertEqual(start(), ["--effort", "high", *original])
        self.write('def driver_start(ctx, config):\n    return {"effort": "low" if ctx["resuming"] else "medium"}\n')
        self.assertEqual(start(), ["--effort", "low", *original])
        self.assertEqual(json.loads(self.check().stdout), {"effort": "medium"})

    def test_refusals(self):
        sources = ['load("other.star", "x")', 'print("unframed output")',
                   'def driver_start(x, y): return {"command": "other"}',
                   'def driver_start(x, y): return {"model": "--account"}',
                   'def driver_start(x, y): return {"effort": "unbounded"}',
                   'def driver_start(x, y): return {"effort": str([i for i in range(1000000)][-1])}']
        for source in sources:
            with self.subTest(source=source):
                self.write(source)
                result = self.check()
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, "")
                self.assertTrue(result.stderr)

    def test_bad_policy_never_runs_driver(self):
        self.write('def driver_start(x, y): return {"limit": False}')
        program = self.profile.parent / "would_run.py"
        program.write_text("raise SystemExit(42)")
        result = subprocess.run([*self.command, "driver", str(self.profile), "review",
                                 sys.executable, str(program)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 1)
        hello, error = map(json.loads, result.stdout.splitlines())
        self.assertEqual(hello["kind"], "hello")
        self.assertIn("only model and effort", error["cause"])

    def test_profile_file_links_refused(self):
        config = self.profile / "config.json"
        saved = self.profile.parent / "elsewhere"
        config.rename(saved)
        config.symlink_to(saved)
        self.assertNotEqual(self.check().returncode, 0)


if __name__ == "__main__":
    unittest.main()
