"""Exercise the actual private HTTP, Unix socket and terminal with synthetic sign-in programs."""

import errno
import http.client
import json
import os
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest.mock import patch
from urllib.parse import urlencode, urlsplit

from nexus_rpc import codex_account, muse_account
from nexus_rpc.account_runtime import (
    PENDING,
    enrolled,
    process_env,
    remove_receipt_if_current,
)
from nexus_rpc.enroll import (
    CODE_ASK,
    PASTE_CODE,
    Login,
    enrollment_lock,
    link_from,
    recover,
    run,
    run_paste,
)

GOOGLE = "https://accounts.google.com/o/oauth2/auth?test=one&scope=two"


class BrowserFlow(unittest.TestCase):
    def test_private_authorization_code_handoff(self):
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory).resolve()
            program = """
import os, sys
from pathlib import Path
assert os.isatty(0) and os.isatty(1)
assert os.environ['HOME'] == os.getcwd()
assert 'NEXUS_SIGNIN_FLOW' not in os.environ
assert 'NEXUS_SIGNIN_SOCKET' not in os.environ
print('private terminal transcript')
print('https://accounts.google.com.evil.invalid/path')
print('https://accounts.google.com/o/oauth2/auth?test=one&scope=two')
print('Paste the authorization code:', end='', flush=True)
if input() != 'synthetic-answer': sys.exit(1)
store = Path.home() / '.gemini/antigravity-cli'
store.mkdir(parents=True)
(store / 'antigravity-oauth-token').write_text('{"access_token":"test-fixture"}')
"""
            login = Login(
                "antigravity",
                "local-fixture",
                home,
                [sys.executable, "-c", program],
                process_env("antigravity", home),
            )
            result = []
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as unix:
                path = str(home / "flow")
                unix.bind(path)
                unix.listen(1)
                unix.settimeout(4)
                with patch.dict(
                    os.environ, NEXUS_SIGNIN_SOCKET=path, NEXUS_SIGNIN_FLOW="42-test"
                ):
                    worker = threading.Thread(
                        target=lambda: result.append(run(login)), daemon=True
                    )
                    worker.start()
                    self.addCleanup(login.close)
                    with unix.accept()[0] as connection:
                        notice = connection.makefile("rb").readline().decode().rstrip()
            nonce, url = notice.split()
            self.assertEqual(nonce, "42-test")
            dest = urlsplit(url)
            self.assertEqual(dest.hostname, "127.0.0.1")
            self.assertRegex(dest.path, r"^/nexus-signin/[0-9a-f]{64}$")
            origin = "http://" + dest.netloc

            def request(method, suffix="", body=None, **headers):
                conn = http.client.HTTPConnection(dest.hostname, dest.port, timeout=3)
                try:
                    conn.request(method, dest.path + suffix, body, headers)
                    res = conn.getresponse()
                    return res.status, dict(res.getheaders()), res.read().decode()
                finally:
                    conn.close()

            self.assertEqual(request("GET", "/bad")[0], 404)
            self.assertEqual(request("GET", Host="example.invalid")[0], 404)
            self.assertEqual(request("GET", Origin="https://example.invalid")[0], 404)
            self.assertEqual(request("GET", **{"Sec-Fetch-Site": "cross-site"})[0], 404)
            self.assertEqual(request("GET", "/status")[0], 403)
            self.assertIsNone(login.process)
            status, headers, page = request("GET")
            self.assertEqual(status, 200)
            self.assertIn("local-fixture", page)
            self.assertIn("frame-ancestors 'none'", headers["Content-Security-Policy"])
            self.assertEqual(headers["Cache-Control"], "no-store")
            self.assertEqual(headers["Referrer-Policy"], "no-referrer")
            self.assertIn("HttpOnly", headers["Set-Cookie"])
            cookie = headers["Set-Cookie"].split(";")[0]
            csrf = re.search(r"csrf:'([0-9a-f]{64})'", page)[1]
            self.assertEqual(request("GET")[0], 403)
            self.assertEqual(
                request("GET", "/status", Cookie="nexus_signin=wrong")[0], 403
            )

            def state():
                status, _, body = request("GET", "/status", Cookie=cookie)
                self.assertEqual(status, 200)
                self.assertNotIn("private terminal transcript", body)
                self.assertNotIn("evil.invalid", body)
                self.assertNotIn("synthetic-answer", body)
                return json.loads(body)

            def until(predicate):
                deadline = time.monotonic() + 4
                while time.monotonic() < deadline:
                    status = state()
                    if predicate(status):
                        return status
                    time.sleep(0.03)
                self.fail("The fixture sign-in did not reach its expected state")

            current = until(lambda s: s["input"])
            self.assertEqual(current["url"], GOOGLE)
            self.assertEqual(current["code"], "")
            form_headers = {
                "Origin": origin,
                "Cookie": cookie,
                "Content-Type": "application/x-www-form-urlencoded",
            }
            form = urlencode({"csrf": csrf, "code": "synthetic-answer"})
            self.assertEqual(
                request(
                    "POST",
                    body=form,
                    **{**form_headers, "Origin": "http://example.invalid"},
                )[0],
                403,
            )
            self.assertEqual(
                request("POST", body=form, **{**form_headers, "Cookie": ""})[0], 403
            )
            self.assertEqual(
                request("POST", body=form.replace(csrf, "invalid"), **form_headers)[0],
                403,
            )
            self.assertEqual(request("POST", body=form + "%0A", **form_headers)[0], 400)
            self.assertEqual(request("POST", body=form, **form_headers)[0], 204)
            current = until(lambda s: s["phase"] == "signed-in")
            self.assertEqual(
                current, {"phase": "signed-in", "url": "", "code": "", "input": False}
            )
            worker.join(4)
            self.assertEqual(result, [0])
            self.assertIsNotNone(enrolled(home, "antigravity"))

    def test_device_prompts_publish_only_the_provider_url_and_code(self):
        for provider, url in [
            ("muse", "https://auth.meta.com/fixture/link?code=WXYZ-7890"),
            ("codex", "https://auth.openai.com/codex/device"),
            ("grok", "https://accounts.x.ai/device?user_code=WXYZ-7890"),
        ]:
            with (
                self.subTest(provider=provider),
                tempfile.TemporaryDirectory() as directory,
            ):
                home = Path(directory)
                text = f"CLI details\n\x1b[92m{url}\x1b[0m\nEnter this one-time code:\n\x1b[91mWXYZ-7890\x1b[0m\n"
                cmd = [
                    sys.executable,
                    "-c",
                    "import sys,time; print(sys.argv[1],flush=True); time.sleep(10)",
                    text,
                ]
                login = Login(provider, "test", home, cmd, process_env(provider, home))
                try:
                    login.start()
                    deadline = time.monotonic() + 3
                    while not login.state()["code"] and time.monotonic() < deadline:
                        time.sleep(0.02)
                    self.assertEqual(
                        login.state(),
                        {
                            "phase": "pending",
                            "input": False,
                            "code": "WXYZ-7890",
                            "url": url,
                        },
                    )
                    self.assertFalse(login.input("value"))
                finally:
                    login.close()
                self.assertIsNone(enrolled(home, provider))
                self.assertIsNotNone(login.process.returncode)

    def test_codex_entry_point_promotes_private_login_and_preserves_it_on_repeat(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            binary = root / "vendor"
            binary.write_text(
                "#!"
                + sys.executable
                + """
import json, os, sys
from pathlib import Path
assert sys.argv[1:3] == ['login', '--device-auth']
assert 'OPENAI_API_KEY' not in os.environ
assert 'NEXUS_SIGNIN_FLOW' not in os.environ
assert Path.cwd() == Path.home()
target = Path(os.environ['CODEX_HOME']) / 'auth.json'
target.write_text(json.dumps({'auth_mode':'chatgpt','tokens':dict.fromkeys(
    ['id_token','access_token','refresh_token'],'synthetic')}))
print('private-native-transcript', flush=True)
"""
            )
            binary.chmod(0o700)
            env = {
                "HOME": str(root),
                "PATH": os.defpath,
                "NEXUS_SIGNIN_SOCKET": str(root / "socket"),
                "NEXUS_SIGNIN_FLOW": "test",
                "OPENAI_API_KEY": "fixture",
            }
            args = [
                sys.executable,
                "-m",
                "nexus_rpc.enroll",
                "codex",
                "--binary",
                str(binary),
                "--account",
                "local",
            ]
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
                listener.bind(env["NEXUS_SIGNIN_SOCKET"])
                listener.listen(1)
                listener.settimeout(4)
                cwd = Path(__file__).resolve().parent
                with subprocess.Popen(
                    args,
                    cwd=cwd,
                    env=env,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                ) as proc:
                    try:
                        with (
                            listener.accept()[0] as channel,
                            channel.makefile("rb") as notice,
                        ):
                            url = urlsplit(
                                notice.readline().decode().strip().split()[1]
                            )

                        def fetch(suffix, headers):
                            conn = http.client.HTTPConnection(
                                url.hostname, url.port, timeout=3
                            )
                            try:
                                conn.request("GET", url.path + suffix, headers=headers)
                                response = conn.getresponse()
                                self.assertEqual(response.status, 200)
                                return response.read(), response.getheader("Set-Cookie")
                            finally:
                                conn.close()

                        page, cookie = fetch("", {})
                        self.assertNotIn(b"private-native-transcript", page)
                        deadline = time.monotonic() + 3
                        while time.monotonic() < deadline:
                            body, _ = fetch("/status", {"Cookie": cookie.split(";")[0]})
                            phase = json.loads(body)["phase"]
                            if phase != "pending":
                                break
                            time.sleep(0.02)
                        self.assertEqual(phase, "signed-in")
                        self.assertEqual(proc.communicate(timeout=3), (b"", b""))
                        self.assertEqual(proc.returncode, 0)
                    finally:
                        if proc.poll() is None:
                            proc.kill()
            home = root / ".nexus/server/harness/codex/local"
            self.assertIsNotNone(enrolled(home, "codex"))
            credential = home / ".codex/auth.json"
            stamp = credential.stat().st_mtime_ns
            binary.unlink()
            repeated = subprocess.run(
                args, cwd=cwd, env=env, capture_output=True, timeout=3, check=False
            )
            self.assertEqual(
                (repeated.returncode, repeated.stdout, repeated.stderr), (0, b"", b"")
            )
            self.assertEqual(credential.stat().st_mtime_ns, stamp)

    def test_cancel_unwinds_staging_without_enrolling(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
                sock = str(root / "flow")
                listener.bind(sock)
                listener.listen(1)
                listener.settimeout(4)
                cmd = [
                    sys.executable,
                    "-m",
                    "nexus_rpc.enroll",
                    "codex",
                    "--binary",
                    sys.executable,
                    "--account",
                    "fixture",
                ]
                env = {
                    "HOME": str(root),
                    "NEXUS_SIGNIN_SOCKET": sock,
                    "NEXUS_SIGNIN_FLOW": "test",
                    "PATH": os.defpath,
                }
                with subprocess.Popen(
                    cmd,
                    cwd=Path(__file__).resolve().parent,
                    env=env,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                ) as proc:
                    try:
                        with listener.accept()[0]:
                            pass
                        proc.terminate()
                        self.assertEqual(proc.communicate(timeout=3), (b"", b""))
                        self.assertNotEqual(proc.returncode, 0)
                    finally:
                        if proc.poll() is None:
                            proc.kill()
            home = root / ".nexus/server/harness/codex/fixture"
            self.assertIsNone(enrolled(home, "codex"))
            self.assertFalse(list(home.glob(".signin-*")))
            self.assertFalse(list(home.glob(".login-bin-*")))

    def test_unsafe_links_never_enter_browser_state(self):
        for candidate in [
            "http://accounts.google.com/auth",
            "https://accounts.google.com.example.invalid/auth",
            "https://accounts.google.com@other.invalid/auth",
            "https://other.invalid@accounts.google.com/auth",
            "https://accounts.google.com:443/auth",
            "https://accounts.google.com\\@other.invalid/auth",
            "https://auth.openai.com/device",
        ]:
            self.assertEqual(link_from(candidate + "\n", "antigravity"), "")

    def test_enrollment_lock_excludes_other_helpers_and_releases_on_exit(self):
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory)
            with (
                enrollment_lock(home),
                self.assertRaises(BlockingIOError),
                enrollment_lock(home),
            ):
                self.fail("Overlapping sign-in unexpectedly acquired an account")
            with enrollment_lock(home):
                pass


# A scripted Antigravity CLI: its terminal prompt is the one the real `agy acp --login` prints (2026-11 tracer), and
# it takes the code on its terminal. A second prompt after a wrong code is what a vendor that refuses it does.
PASTE_PEER = """
import os, sys
from pathlib import Path
assert os.isatty(0) and os.isatty(1)
print('Open this link in a browser to log in with Google:')
print()
print('  https://accounts.google.com/o/oauth2/auth?test=one&scope=two')
print()
while True:
    print('Then paste the authorization code, or the full URL of the page you land on, and press Enter:', end='',
          flush=True)
    if input() == 'synthetic-answer':
        break
    print('That code was not accepted.')
store = Path.home() / '.gemini/antigravity-cli'
store.mkdir(parents=True)
(store / 'antigravity-oauth-token').write_text('{"access_token":"test-fixture"}')
"""


class PastedCodeFlow(unittest.TestCase):
    """run_paste: the vendor's URL and the ask for its code go to the runner; the runner's one line is the code."""

    def start(self, home):
        login = Login(
            "antigravity",
            "remote-fixture",
            home,
            [sys.executable, "-c", PASTE_PEER],
            process_env("antigravity", home),
        )
        self.addCleanup(login.close)
        notes, result = [], []
        noted = threading.Condition()

        def notify(text):
            with noted:
                notes.append(text)
                noted.notify_all()

        reader, writer = os.pipe()
        self.addCleanup(os.close, reader)
        self.addCleanup(os.close, writer)
        worker = threading.Thread(
            target=lambda: result.append(run_paste(login, reader, notify=notify)), daemon=True
        )
        worker.start()

        def until(count):
            with noted:
                self.assertTrue(noted.wait_for(lambda: len(notes) >= count, timeout=4), notes)
            return list(notes)

        return login, writer, worker, result, until

    def test_antigravity_signs_in_by_a_pasted_code(self):
        self.assertIn("antigravity", PASTE_CODE)
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory).resolve()
            login, writer, worker, result, until = self.start(home)
            # The vendor's own URL, then the ask: nothing of the terminal besides.
            self.assertEqual(until(2), [GOOGLE, CODE_ASK])
            os.write(writer, b"synthetic-answer\r\n")
            worker.join(4)
            self.assertEqual(result, [0])
            self.assertIsNotNone(enrolled(home, "antigravity"))
            self.assertNotIn(b"synthetic-answer", bytes(login.tail))

    def test_a_refused_code_ends_the_enrollment_and_no_second_is_taken(self):
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory).resolve()
            _, writer, worker, result, until = self.start(home)
            self.assertEqual(until(2), [GOOGLE, CODE_ASK])
            os.write(writer, b"wrong-answer\n")
            worker.join(4)
            self.assertEqual(result, [1])
            self.assertEqual(until(2), [GOOGLE, CODE_ASK])
            self.assertIsNone(enrolled(home, "antigravity"))

    def test_the_runner_ending_its_terminal_ends_the_enrollment(self):
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory).resolve()
            _, writer, worker, result, until = self.start(home)
            until(2)
            os.dup2(os.open(os.devnull, os.O_RDONLY), writer)  # the write end gone: end of input
            worker.join(4)
            self.assertEqual(result, [1])
            self.assertIsNone(enrolled(home, "antigravity"))


class EnrollmentRecovery(unittest.TestCase):
    def write_grant(self, home, provider, value):
        if provider == "codex":
            dest = home / ".codex/auth.json"
            record = {
                "auth_mode": "chatgpt",
                "tokens": dict.fromkeys(
                    ("id_token", "access_token", "refresh_token"), value
                ),
            }
        else:
            dest = home / ".config/muse/auth.json"
            record = {
                "providers": {"meta": {"mechanism": "oauth", "access_token": value}}
            }
        dest.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        fd, name = tempfile.mkstemp(dir=dest.parent)
        with os.fdopen(fd, "w") as f:
            json.dump(record, f)
        os.replace(name, dest)

    def finished_login(self, home, provider, stage, promote, status=0):
        module = {"codex": codex_account, "muse": muse_account}[provider]
        login = Login(
            provider,
            "fixture",
            home,
            [sys.executable, "-c", f"raise SystemExit({status})"],
            process_env(provider, stage),
            login_home=stage,
            module=module,
            binary="/fixture",
            prior=module.grant_stamp(home),
            promote=promote,
        )
        login.start()
        self.addCleanup(login.close)
        login.process.wait(timeout=4)
        return login

    def test_invalid_staged_promotion_can_be_retried_with_a_fresh_login(self):
        def failed():
            raise ValueError("invalid fixture")

        with tempfile.TemporaryDirectory() as root:
            home = Path(root)
            with codex_account.staged_login(home) as (stage, _):
                self.write_grant(stage, "codex", "new-fixture")
                login = self.finished_login(home, "codex", stage, failed)
                self.assertEqual(login.state()["phase"], "failed")
            self.assertIsNone(enrolled(home, "codex"))
            self.assertIsNone(codex_account.grant_stamp(home))
            self.assertFalse((home / PENDING).exists())

    def test_transient_receipt_error_retries_without_repeating_promotion(self):
        with tempfile.TemporaryDirectory() as root:
            home = Path(root)
            with codex_account.staged_login(home) as (stage, promote):
                self.write_grant(stage, "codex", "new-fixture")
                login = self.finished_login(home, "codex", stage, promote)
                with patch(
                    "nexus_rpc.enroll.store_receipt",
                    side_effect=OSError(errno.ENOSPC, "fixture"),
                ) as install:
                    login.poll()
                    self.assertEqual(login.state()["phase"], "saving")
                    self.assertEqual(install.call_count, 1)
                self.assertIsNone(enrolled(home, "codex"))
                self.assertIsNotNone(codex_account.grant_stamp(home))
                self.assertNotIn(b"new-fixture", (home / PENDING).read_bytes())
                login.retry_at = 0
                login.poll()
                self.assertEqual(login.state()["phase"], "signed-in")
                self.assertIsNotNone(enrolled(home, "codex"))
                self.assertFalse((home / PENDING).exists())

    def test_final_directory_sync_retries_without_reviving_a_revoked_receipt(self):
        for revoked in (False, True):
            with self.subTest(revoked=revoked), tempfile.TemporaryDirectory() as root:
                home = Path(root)
                account_stat = home.stat()
                fsync = os.fsync

                def interrupt_final_sync(
                    fd, account_stat=account_stat, home=home, fsync=fsync
                ):
                    if (
                        os.path.samestat(account_stat, os.fstat(fd))
                        and (home / ".nexus-enrolled").exists()
                        and not (home / PENDING).exists()
                    ):
                        raise OSError(errno.EIO, "simulated directory sync failure")
                    fsync(fd)

                with codex_account.staged_login(home) as (stage, promote):
                    self.write_grant(stage, "codex", "new-fixture")
                    login = self.finished_login(home, "codex", stage, promote)
                    with patch("os.fsync", side_effect=interrupt_final_sync):
                        login.poll()
                    self.assertEqual(login.state()["phase"], "saving")
                    receipt = enrolled(home, "codex")
                    self.assertIsNotNone(receipt)
                    self.assertFalse((home / PENDING).exists())
                    if revoked:
                        self.assertTrue(
                            remove_receipt_if_current(home, "codex", receipt)
                        )
                    login.retry_at = 0
                    self.assertEqual(
                        login.state()["phase"], "failed" if revoked else "signed-in"
                    )
                self.assertEqual(enrolled(home, "codex") is None, revoked)

    def test_helper_restart_resumes_on_either_side_of_credential_rename(self):
        for rename_completed in (False, True):
            with (
                self.subTest(rename_completed=rename_completed),
                tempfile.TemporaryDirectory() as root,
            ):
                home = Path(root)
                self.write_grant(home, "codex", "old-fixture")
                previous = codex_account.grant_stamp(home)
                with codex_account.staged_login(home) as (stage, promote):
                    self.write_grant(stage, "codex", "new-fixture")
                    expected = codex_account.grant_stamp(stage)

                    def interrupted(rename_completed=rename_completed):
                        if rename_completed:
                            promote()
                        raise OSError(errno.EIO, "fixture")

                    login = self.finished_login(home, "codex", stage, interrupted)
                    login.poll()
                    self.assertEqual(login.state()["phase"], "saving")
                self.assertTrue(stage.exists())
                self.assertIsNone(enrolled(home, "codex"))
                self.assertEqual(
                    codex_account.grant_stamp(home),
                    expected if rename_completed else previous,
                )
                self.assertTrue(
                    recover(home, "codex", codex_account, "/no-vendor-needed")
                )
                self.assertEqual(codex_account.grant_stamp(home), expected)
                self.assertFalse(stage.exists())
                self.assertIsNotNone(enrolled(home, "codex"))

    def test_muse_rechecks_rotated_staged_grant_without_overwriting_an_active_grant(
        self,
    ):
        for active in (False, True):
            with self.subTest(active=active), tempfile.TemporaryDirectory() as root:
                home = Path(root)
                self.write_grant(home, "muse", "old-fixture")
                old = muse_account.grant_stamp(home)
                with muse_account.staged_login(home) as (stage, promote):
                    self.write_grant(stage, "muse", "new-fixture")
                    login = self.finished_login(home, "muse", stage, promote, status=1)

                    def transient(binary, stage):
                        self.write_grant(stage, "muse", "rotated-fixture")
                        raise OSError(errno.EAGAIN, "temporary mint failure")

                    with patch.object(
                        muse_account, "check_grant", side_effect=transient
                    ):
                        login.poll()
                    self.assertEqual(login.state()["phase"], "saving")
                self.assertEqual(muse_account.grant_stamp(home), old)

                def recheck(binary, stage, active=active):
                    if active:
                        self.write_grant(stage, "muse", "checked-fixture")
                    return active

                with patch.object(muse_account, "check_grant", side_effect=recheck):
                    self.assertEqual(
                        recover(home, "muse", muse_account, "/fixture"), active
                    )
                self.assertEqual(enrolled(home, "muse") is not None, active)
                self.assertEqual(muse_account.grant_stamp(home) == old, not active)
                self.assertFalse(stage.exists())

    def test_fresh_or_revoked_account_does_not_infer_a_receipt_from_provider_files(
        self,
    ):
        with tempfile.TemporaryDirectory() as root:
            home = Path(root)
            self.write_grant(home, "codex", "previous-fixture")
            self.assertFalse(recover(home, "codex", codex_account, "/no-vendor-needed"))
            self.assertIsNone(enrolled(home, "codex"))


if __name__ == "__main__":
    unittest.main()
