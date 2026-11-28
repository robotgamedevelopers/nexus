import errno
import json
import os
import stat
import select
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from nexus_rpc import antigravity_account as agy
from nexus_rpc.account_runtime import process_env


@unittest.skipUnless(sys.platform == "darwin", "Antigravity account isolation uses macOS Seatbelt")
class AntigravityAccounts(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name).resolve()

    def home(self, name):
        home = self.root / name
        home.mkdir(mode=0o700)
        return home

    def test_same_provider_accounts_and_keychain_are_isolated(self):
        # This small native process stands in for agy. It asks launchd about the Keychain
        # service (no item lookup), checks its own fake token and attempts a sibling read.
        probe = self.root / "probe"
        source = r'''
#include <servers/bootstrap.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
int main(int argc, char **argv) {
    mach_port_t service;
    if (bootstrap_look_up(bootstrap_port, "com.apple.securityd", &service) == KERN_SUCCESS) return 1;
    int fd = open(argv[1], O_RDONLY);
    char value;
    if (fd < 0 || read(fd, &value, 1) != 1 || value != argv[2][0]) return 2;
    close(fd);
    if (open(argv[3], O_RDONLY) != -1 || errno != EPERM) return 3;
    if (open(argv[4], O_WRONLY) != -1 || errno != EPERM) return 4;
    if (open(argv[5], O_WRONLY) != -1 || errno != EPERM) return 5;
    if (execl("/usr/bin/security", "security", "help", NULL) != -1 || errno != EPERM) return 6;
    return 0;
}
'''
        subprocess.run(["/usr/bin/cc", "-x", "c", "-o", str(probe), "-"], input=source,
                       text=True, capture_output=True, check=True)
        accounts = [self.home(name) for name in ("one", "two")]
        for index, home in enumerate(accounts):
            agy.login_command(probe, home)
            (home / ".gemini/credential-fixture").write_text(str(index))
            (home / ".nexus-enrolled").touch()
        for i, home in enumerate(accounts):
            for command in (agy.login_command(probe, home), agy.chat_command(probe, home, str(self.root))):
                with self.subTest(account=i, login="--login" in command):
                    args = command[:command.index("acp")] + [home / ".gemini/credential-fixture", str(i),
                        accounts[1 - i] / ".gemini/credential-fixture", home / ".nexus-enrolled",
                        home / "room/.agents/agents/nexus-dialogue/agent.md"]
                    result = subprocess.run(args, cwd=home / "room", env=process_env("antigravity", home),
                                            capture_output=True)
                    self.assertEqual(result.returncode, 0)

    def test_dialogue_and_vendor_permissions(self):
        home = self.home("example")
        room = home / "room"
        command = agy.chat_command("/bin/cat", home, str(self.root))
        self.assertEqual(command[command.index("--agent") + 1], "nexus-dialogue")
        self.assertIn("--sandbox", command)
        agent = (room / ".agents/agents/nexus-dialogue/agent.md").read_text()
        self.assertEqual(agent.split("\ntools:\n")[1].split("---")[0].splitlines(), ["  - send_message"])
        self.assertIn("inheritMcp: false", agent)
        rules = json.loads((home / ".gemini/antigravity-cli/settings.json").read_text())["permissions"]
        self.assertTrue({"read_file(*)", "write_file(*)", "command(*)", "mcp(*)"} <= set(rules["deny"]))

    def test_symlinked_config_directory_is_refused(self):
        home = self.home("example")
        unrelated = self.home("unrelated")
        (home / ".gemini").symlink_to(unrelated, target_is_directory=True)
        with self.assertRaises(OSError):
            agy.login_command("/bin/cat", home)
        self.assertEqual(list(unrelated.iterdir()), [])

    def test_promotion_requires_a_persisted_login_and_preserves_previous_grant(self):
        home = self.home("example")
        agy.login_command("/bin/cat", home)
        rel = Path(".gemini/antigravity-cli/antigravity-oauth-token")
        prior = json.dumps({"access_token": "synthetic-first"})
        replacement = json.dumps({"access_token": "synthetic-second"})
        (home / rel).write_text(prior)
        with agy.staged_login(home) as (staging, promote):
            agy.login_command("/bin/cat", staging)
            with self.assertRaises(FileNotFoundError):
                promote()
            (staging / rel).write_text("{}")
            with self.assertRaises(ValueError):
                promote()
            self.assertEqual((home / rel).read_text(), prior)
            (staging / rel).write_text(replacement)
            promote()
        self.assertEqual((home / rel).read_text(), replacement)
        self.assertEqual((home / rel).stat().st_mode & 0o777, 0o600)

    def test_enveloped_grant_is_promoted_without_rewriting_vendor_metadata(self):
        home = self.home("wrapped")
        path = Path(".gemini/antigravity-cli/antigravity-oauth-token")
        with agy.staged_login(home) as (stage, promote):
            agy.login_command("/bin/cat", stage)
            for invalid in (None, {}, {"access_token": 42}):
                (stage / path).write_text(json.dumps({"token": invalid}))
                self.assertIsNone(agy.grant_stamp(stage))
            data = b'{"token":{"access_token":"fixture","refresh_token":"fixture"},"version":1}'
            (stage / path).write_bytes(data)
            expected = agy.grant_stamp(stage)
            self.assertTrue(expected)
            promote()
            self.assertEqual(agy.grant_stamp(home), expected)
            self.assertEqual((home / path).read_bytes(), data)

    def test_promotion_retry_after_rename_and_grant_stamp(self):
        home = self.home("example")
        rel = Path(".gemini/antigravity-cli/antigravity-oauth-token")
        self.assertIsNone(agy.grant_stamp(home))
        self.assertFalse((home / ".gemini").exists())
        with agy.staged_login(home) as (staging, promote):
            agy.login_command("/bin/cat", staging)
            (staging / rel).write_text(json.dumps({"access_token": "fixture"}))
            stamp = agy.grant_stamp(staging)
            self.assertTrue(stamp)
            fsync = os.fsync
            def full_disk(fd):
                if stat.S_ISDIR(os.fstat(fd).st_mode):
                    raise OSError(errno.EIO, "fixture directory sync failure")
                fsync(fd)
            with patch.object(os, "fsync", side_effect=full_disk), self.assertRaises(OSError):
                promote()
            self.assertFalse((staging / rel).exists())
            self.assertEqual(agy.grant_stamp(home), stamp)
            promote()
            (home / rel).write_text(json.dumps({"access_token": "different fixture"}))
            with self.assertRaises(FileNotFoundError):
                promote()
            (home / rel).write_text("[null]")
            self.assertIsNone(agy.grant_stamp(home))

    @unittest.skipUnless(shutil.which("agy"), "requires the installed Antigravity binary")
    def test_real_vendor_uses_private_file_and_preserves_permission_denies(self):
        home = self.home("example")
        cmd = agy.chat_command(shutil.which("agy"), home, str(self.root))
        data = home / ".gemini/antigravity-cli"
        # Invalid JSON distinguishes our exact file from a missing or shared Keychain login.
        (data / "antigravity-oauth-token").write_text("Z synthetic invalid token fixture")
        room = home / "room"
        with subprocess.Popen(cmd, cwd=room, env=process_env("antigravity", home), text=True,
                              stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL) as p:
            try:
                for i, (method, params) in enumerate([
                    ("initialize", {"protocolVersion": 1, "clientCapabilities": {}}),
                    ("session/new", {"cwd": str(room), "mcpServers": []}),
                ]):
                    p.stdin.write(json.dumps(dict(jsonrpc="2.0", id=i, method=method, params=params)) + "\n")
                    p.stdin.flush()
                    self.assertTrue(select.select([p.stdout], [], [], 12)[0])
                    result = json.loads(p.stdout.readline())
                self.assertIn("auth", result["error"]["message"].lower())
            finally:
                p.terminate()
        logs = "".join(f.read_text(errors="replace") for f in (data / "log").glob("*.log"))
        self.assertTrue("invalid character 'Z'" in logs)
        self.assertTrue("falling back to file" in logs)
        self.assertFalse("failed to load cli settings" in logs.lower())
        self.assertIn("read_file(*)", json.loads((data / "settings.json").read_text())["permissions"]["deny"])

    @unittest.skipUnless(shutil.which("agy"), "requires the installed Antigravity binary")
    def test_real_vendor_login_in_clean_home(self):
        home = self.home("fresh")
        result = subprocess.run(agy.login_command(shutil.which("agy"), home), cwd=home,
                                env=process_env("antigravity", home), input=b"", capture_output=True,
                                timeout=12)
        text = result.stdout + result.stderr
        # Never put provider URLs, OAuth state or challenge parameters in test output.
        self.assertTrue(b"https://accounts.google.com/" in text)
        self.assertTrue(b"paste the authorization code" in text.lower())
        self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
