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

    def test_friend_identity_reaches_the_selected_driver(self):
        root = self.profile.parent
        checkout = root / "home"
        checkout.mkdir()
        def git(*args):
            subprocess.run(["git", "-C", str(checkout), *args], check=True, capture_output=True)
        git("init")
        (checkout / "standing.md").write_text("read my original instructions")
        git("add", "standing.md")
        git("-c", "user.name=Test", "-c", "user.email=test@example.invalid", "commit", "-m", "home")
        registry = root / "registry"
        fixture = {"result": {"status": "ok"}, "items": [{"kind": "friend", "fields": {
            "name": "review", "harness": "codex", "model": "selected", "enabled": True}}]}
        startup = {"home": str(checkout), "prefix": "open the home", "files": ["standing.md"]}
        fixture["items"][0]["fields"]["startup"] = startup
        def publish():
            registry.write_text("#!" + sys.executable + "\nprint(" + repr(json.dumps(fixture)) + ")\n")
        publish()
        registry.chmod(0o700)
        config = {"nova_config": [str(registry)]}
        (self.profile / "identities.json").write_text(json.dumps(config))
        capture = root / "driver.py"
        capture.write_text("import json,sys,os\nargs=sys.argv[1:]\n"
                           "print(json.dumps([args,os.fdopen(int(args[-1])).read()]))\n")
        command = [*self.command, "friend-driver", str(self.profile), "review", "codex", sys.executable,
                   str(capture), "--cwd", str(root)]
        for args in ([], ["--resume", "existing"]):
            flags, prompt = json.loads(subprocess.check_output([*command, *args], text=True))
            self.assertIn("read my original instructions", prompt)
            self.assertIn("open the home", prompt)
            self.assertIn('"harness": "codex"', prompt)
            self.assertEqual(flags[flags.index("--model") + 1], "selected")
        self.assertNotIn("--write-dir", flags)
        startup["writable_dirs"] = ["/shared/cache", "/shared/build"]
        publish()
        flags, _ = json.loads(subprocess.check_output(command, text=True))
        self.assertEqual([flags[i+1] for i, flag in enumerate(flags) if flag == "--write-dir"], startup["writable_dirs"])
        startup["bus_listener"] = "127.0.0.1:6379"
        startup["bus_owner"] = "nexus-server"
        publish()
        flags, _ = json.loads(subprocess.check_output(command, text=True))
        self.assertNotIn("--bus-listener", flags)
        self.assertNotIn("127.0.0.1:6379", flags)
        # Existing Claude listener grants remain driver-owned until explicitly transferred.
        startup.pop("bus_owner")
        fixture["items"][0]["fields"]["harness"] = "claude"
        publish()
        claude = list(command)
        claude[claude.index("codex")] = "claude"
        flags, _ = json.loads(subprocess.check_output(claude, text=True))
        self.assertEqual(flags[flags.index("--bus-listener") + 1], "review@127.0.0.1:6379")
        startup.pop("bus_listener")
        fixture["items"][0]["fields"]["harness"] = "codex"
        publish()
        different = subprocess.run([*self.command, "identity", str(self.profile), "review", "--harness", "claude"],
                                   capture_output=True, text=True)
        self.assertNotEqual(different.returncode, 0)
        startup["files"] = ["../elsewhere"]
        publish()
        refused = subprocess.run(command, capture_output=True, text=True)
        self.assertNotEqual(refused.returncode, 0)
        self.assertEqual(json.loads(refused.stdout.splitlines()[1])["kind"], "error")

    def test_bus_roster_and_subscription_grants(self):
        root = self.profile.parent
        registry = root / "registry"
        registry.write_text("#!" + sys.executable + "\nimport pathlib,sys\n"
                            "assert sys.argv[1:] == ['friend','list','--json']\n"
                            "with pathlib.Path(__file__).with_name('reads').open('a') as counter: counter.write('1')\n"
                            "print(pathlib.Path(__file__).with_name('rows.json').read_text())\n")
        registry.chmod(0o700)
        (self.profile / "identities.json").write_text(json.dumps({"nova_config": [str(registry)]}))

        def row(name, harness, startup, enabled=True):
            return {"kind": "friend", "fields": {"name": name, "harness": harness,
                    "enabled": enabled, "startup": startup}}

        def query(command, rows):
            (root / "rows.json").write_text(json.dumps({"result": {"status": "ok"}, "items": rows}))
            reads = root / "reads"
            reads.write_text("")
            result = subprocess.run([*self.command, command, str(self.profile)], capture_output=True, text=True)
            self.assertEqual(reads.read_text(), "1")
            return result

        grant = {"bus_owner": "nexus-server", "bus_listener": "127.0.0.1:6379"}
        without_startup = row("five", "new-harness", None)
        del without_startup["fields"]["startup"]
        remote_name = "external-" + "x" * 119
        remote_harness = "custom:Runtime/v2.1"
        remote = row(remote_name, remote_harness, {"bus_owner": "remote-node", "bus_listener": "remote.example:6380"})
        rows = [row("one", "codex", grant), row("two", "claude", {"bus_listener": "127.0.0.1:6379"}),
                row("three", "claude", grant, False), row("four", "muse", {}), without_startup,
                row("six", "other", grant), remote, row("seven", "other", {"bus_listener": "remote.example:6380"}),
                {"kind": "machine", "fields": {"name": "elsewhere"}}]
        marker = "nexus-bus-roster-v1\n"
        expected = {"bus-grants": "one\tcodex\t127.0.0.1:6379\nsix\tother\t127.0.0.1:6379\n",
                    "bus-roster": marker + "one\tcodex\t127.0.0.1:6379\ntwo\tclaude\t\nfour\tmuse\t\n"
                                           "five\tnew-harness\t\nsix\tother\t127.0.0.1:6379\n"
                                           f"{remote_name}\t{remote_harness}\t\nseven\tother\t\n"}
        invalid_rows = [row("bad", "claude", dict(grant, bus_listener=address))
                        for address in ("::1:6379", "127.0.0.1:65536", "127.0.0.1:0", "db:6379", "", None)]
        invalid_rows += [row("two", "other", {}), row("bad\tname", "codex", {}), row("bad", "bad route", {}),
                         row("bad", "codex", []), row(remote_name + "x", remote_harness, {}),
                         row("bad", "codex", {}, enabled="true")]
        for command, output in expected.items():
            with self.subTest(command=command):
                got = query(command, rows)
                self.assertEqual(got.returncode, 0, got.stderr)
                self.assertEqual(got.stdout, output)
                empty = query(command, [])
                self.assertEqual(empty.returncode, 0, empty.stderr)
                self.assertEqual(empty.stdout, marker if command == "bus-roster" else "")
                for invalid in invalid_rows:
                    with self.subTest(row=invalid):
                        refused = query(command, [*rows, invalid])
                        self.assertNotEqual(refused.returncode, 0)
                        self.assertEqual(refused.stdout, "")

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
