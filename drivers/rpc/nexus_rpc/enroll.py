"""Human-only browser handoff for an account's local, unmodified login command."""

import argparse
import errno
import fcntl
import html
import importlib
import json
import os
import pty
import re
import secrets
import shutil
import signal
import socket
import stat
import struct
import subprocess
import tempfile
import termios
import time
from contextlib import contextmanager, nullcontext
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
from typing import NamedTuple
from urllib.parse import parse_qs, urlsplit

from .account_runtime import (
    PENDING,
    enrolled,
    home_for,
    locked_directory,
    process_env,
    read_pending,
    store_receipt,
)

# Only these HTTPS destinations become clickable, regardless of terminal output.
PROVIDERS = {
    "codex": ("Codex", {"auth.openai.com"}),
    "antigravity": ("Antigravity", {"accounts.google.com"}),
    "grok": ("Grok", {"accounts.x.ai"}),
    "muse": ("Muse", {"auth.meta.com"}),
}
ANSI = re.compile(r"\x1b(?:\[[0-?]*[ -/]*[@-~]|\][^\x07\x1b]*(?:\x07|\x1b\\))")
URL = re.compile(r"https://[^\s<>\"'\x00-\x1f\x7f]+")
DEVICE_CODE = re.compile(r"(?<![A-Za-z0-9-])[A-Z0-9]{4}-[A-Z0-9]{4}(?![A-Za-z0-9-])")
MAX_FORM = 8192


def notify_runner(url):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
        sock.settimeout(3)
        sock.connect(os.environ["NEXUS_SIGNIN_SOCKET"])
        sock.sendall(
            (os.environ["NEXUS_SIGNIN_FLOW"] + " " + url + "\n").encode("ascii")
        )


def link_from(text, provider):
    for match in URL.finditer(text):
        candidate = match.group().rstrip(".,;)")
        if "\\" in candidate or len(candidate) > 4096 or not candidate.isascii():
            continue
        try:
            url = urlsplit(candidate)
        except ValueError:
            continue
        if url.netloc in PROVIDERS[provider][1] and url.path.startswith("/"):
            return candidate
    return ""


@contextmanager
def enrollment_lock(home):
    # Receipt updates take a separate, short-lived directory lock shared with chat.
    fd = os.open(
        home / ".nexus-enrollment.lock",
        os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_NONBLOCK,
        0o600,
    )
    try:
        info = os.fstat(fd)
        if (
            not stat.S_ISREG(info.st_mode)
            or stat.S_IMODE(info.st_mode) != 0o600
            or info.st_uid != os.getuid()
            or info.st_nlink != 1
        ):
            raise ValueError("Unsafe enrollment lock")
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        yield
    finally:
        os.close(fd)


RETRY_DELAYS = (0.1, 0.25, 0.5, 1, 2, 4)
STAGE_NAME = re.compile(r"\.signin-[a-zA-Z0-9_-]{6,64}")
STAMP = re.compile(r"[a-zA-Z0-9._:-]{1,192}")


def _publish(home, encoded, *, replaces=None):
    with locked_directory(home, exclusive=True) as fd:
        old = read_pending(fd)
        if old != encoded:
            if old != replaces:
                raise ValueError("A different enrollment is pending")
            name = ".enroll-" + secrets.token_hex(12)
            file = os.open(name, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600, dir_fd=fd)
            try:
                with os.fdopen(file, "wb") as f:
                    f.write(encoded)
                    f.flush()
                    os.fsync(f.fileno())
                os.replace(name, PENDING, src_dir_fd=fd, dst_dir_fd=fd)
            finally:
                try:
                    os.unlink(name, dir_fd=fd)
                except FileNotFoundError:
                    pass
        os.fsync(fd)


def _forget(home, encoded):
    with locked_directory(home, exclusive=True) as fd:
        if read_pending(fd) == encoded:
            os.unlink(PENDING, dir_fd=fd)
            os.fsync(fd)


class Pending(NamedTuple):
    provider: str
    prior: str | None
    stamp: str
    stage: str | None
    stage_id: str | None
    phase: str

    def encode(self):
        if self.provider not in PROVIDERS or self.phase not in ("approved", "checking"):
            raise ValueError("Invalid pending provider or phase")
        for value in [self.stamp] if self.prior is None else [self.stamp, self.prior]:
            if not isinstance(value, str) or not STAMP.fullmatch(value):
                raise ValueError("Invalid provider grant stamp")
        if self.stage is None:
            if self.stage_id is not None or self.phase == "checking":
                raise ValueError("Missing enrollment stage")
        elif (
            not isinstance(self.stage, str)
            or not STAGE_NAME.fullmatch(self.stage)
            or not isinstance(self.stage_id, str)
            or not re.fullmatch(r"[0-9]+:[0-9]+", self.stage_id)
        ):
            raise ValueError("Invalid enrollment stage")
        return json.dumps(self, separators=(",", ":")).encode("ascii")

    @classmethod
    def decode(cls, encoded):
        data = json.loads(encoded)
        if not isinstance(data, list) or len(data) != 6:
            raise ValueError("Invalid pending enrollment")
        record = cls(*data)
        if record.encode() != encoded:
            raise ValueError("Invalid pending enrollment")
        return record


def _identity(path):
    info = path.lstat()
    return f"{info.st_dev}:{info.st_ino}"


def _stage_exists(home, path):
    if path.parent != home or not STAGE_NAME.fullmatch(path.name):
        raise ValueError("Invalid enrollment staging directory")
    try:
        info = path.lstat()
    except FileNotFoundError:
        return False
    if (
        not stat.S_ISDIR(info.st_mode)
        or info.st_uid != os.getuid()
        or stat.S_IMODE(info.st_mode) != 0o700
    ):
        raise ValueError("Unsafe enrollment staging directory")
    return True


@contextmanager
def staging_directory(home, resume=None):
    """Preserve an approved grant across an interrupted commit; discard ordinary canceled logins."""
    path = (
        Path(tempfile.mkdtemp(prefix=".signin-", dir=home))
        if resume is None
        else resume
    )
    if not _stage_exists(home, path):
        raise FileNotFoundError("No staging directory")
    try:
        yield path
    finally:
        with locked_directory(home) as fd:
            pending = read_pending(fd)
        if (
            pending is None or Pending.decode(pending).stage != path.name
        ) and _stage_exists(home, path):
            shutil.rmtree(path)


def _retryable(error):
    return error.errno in {
        errno.EAGAIN,
        errno.EBUSY,
        errno.EDQUOT,
        errno.EINTR,
        errno.EIO,
        errno.EMFILE,
        errno.ENFILE,
        errno.ENOSPC,
        errno.ESTALE,
        errno.ETIMEDOUT,
        errno.EROFS,
    }


def recover(home, provider, module, binary, *, promote=None, expected_pending=None):
    """Commit a grant only when the journal and the provider's private state agree."""
    with locked_directory(home) as fd:
        encoded = read_pending(fd)
    if encoded is None:
        if expected_pending is not None and enrolled(home, provider):
            store_receipt(home, provider, pending=expected_pending)
            return True
        return False
    record = Pending.decode(encoded)
    if record.provider != provider:
        raise ValueError("Different provider in pending enrollment")
    stage = home / record.stage if record.stage else None
    staged = (
        stage is not None
        and _stage_exists(home, stage)
        and _identity(stage) == record.stage_id
    )

    def abandon():
        _forget(home, encoded)
        if staged:
            shutil.rmtree(stage)
        return False

    current = module.grant_stamp(home)
    if record.phase == "checking":
        # Native Muse can obtain a browser grant before its subscription mint returns. A positive
        # mint rotates its private auth file. Resume that check only inside the *same* staging directory.
        if not staged or current != record.prior or module.grant_stamp(stage) is None:
            return abandon()
        if not module.check_grant(binary, stage):
            return abandon()
        approved = record._replace(stamp=module.grant_stamp(stage), phase="approved")
        _publish(home, approved.encode(), replaces=encoded)
        record, encoded = approved, approved.encode()
    if current != record.stamp:
        if (
            not staged
            or current != record.prior
            or module.grant_stamp(stage) != record.stamp
        ):
            return abandon()
        try:
            if promote is None:
                with module.staged_login(home, resume=stage) as (_, recovered_promote):
                    recovered_promote()
            else:
                promote()
        except (ValueError, FileNotFoundError):
            return abandon()
    if module.grant_stamp(home) != record.stamp:
        return abandon()
    store_receipt(home, provider, pending=encoded)
    if staged and _stage_exists(home, stage):
        shutil.rmtree(stage)
    return True


class Login:
    def __init__(
        self,
        provider,
        account,
        home,
        command,
        env,
        *,
        login_home=None,
        promote=None,
        module=None,
        binary=None,
        prior=None,
    ):
        self.provider = provider
        self.account = account
        self.home = home
        self.login_home = login_home or home
        self.promote = promote
        self.module = module or importlib.import_module(
            f".{provider}_account", __package__
        )
        self.binary = binary
        self.prior = prior
        self.pending = None
        self.published = False
        self.retry_at = 0
        self.retries = 0
        self.command = command
        self.env = env
        self.process = None
        self.master = None
        self.tail = bytearray()
        self.url = ""
        self.code = ""
        self.prompt = False
        self.finished = None
        self.result = 1
        self.last_seen = False

    def start(self):
        if self.process is not None or self.finished is not None:
            return
        master, slave = pty.openpty()
        try:
            tty = termios.tcgetattr(slave)
            tty[3] &= ~termios.ECHO
            termios.tcsetattr(slave, termios.TCSANOW, tty)
            fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 512, 0, 0))
            os.set_blocking(master, False)
            # Keep the vendor in the runner's process group, including on an abrupt server shutdown.
            self.process = subprocess.Popen(
                self.command,
                stdin=slave,
                stdout=slave,
                stderr=slave,
                cwd=self.login_home,
                env=self.env,
                close_fds=True,
                umask=0o077,
            )
            self.master = master
        except OSError:
            os.close(master)
            self.finished = time.monotonic()
        finally:
            os.close(slave)

    def drain(self):
        if self.master is None:
            return
        changed = False
        for _ in range(16):
            try:
                chunk = os.read(self.master, 8192)
            except BlockingIOError:
                break
            except OSError as err:
                if err.errno != errno.EIO:
                    raise
                chunk = b""
            if not chunk:
                os.close(self.master)
                self.master = None
                break
            changed = True
            self.tail.extend(chunk)
            del self.tail[:-32768]
        if not changed:
            return
        # Keep partial lines private until a delimiter proves the URL complete.
        terminal = ANSI.sub("", self.tail.decode("utf-8", "replace"))
        complete = re.sub(r"\S+$", "", terminal)
        self.url = link_from(complete, self.provider) or self.url
        if self.provider == "antigravity":
            self.prompt |= bool(
                re.search(
                    r"(?i)(?:paste|enter)[^\r\n]{0,100}(?:auth(?:orization)? )?code",
                    terminal,
                )
            )
        else:
            code = DEVICE_CODE.search(complete)
            if code:
                self.code = code[0]

    def input(self, value):
        if (
            self.master is None
            or not self.prompt
            or not re.fullmatch(r"[!-~]{1,2048}", value)
        ):
            return False
        # The PTY is not a shell. Only one printable authorization code and its newline may be submitted.
        data = (value + "\n").encode("ascii")
        if os.write(self.master, data) != len(data):
            raise OSError("incomplete terminal input")
        self.prompt = False
        self.tail.clear()
        return True

    def commit(self):
        if not self.published:
            if self.pending is None:
                stage = self.login_home if self.login_home != self.home else None
                checking = hasattr(self.module, "check_grant")
                self.pending = Pending(
                    self.provider,
                    self.prior,
                    self.module.grant_stamp(self.login_home),
                    stage.name if stage else None,
                    _identity(stage) if stage else None,
                    "checking" if checking else "approved",
                ).encode()
            _publish(self.home, self.pending)
            self.published = True
        if not recover(
            self.home,
            self.provider,
            self.module,
            self.binary,
            promote=self.promote,
            expected_pending=self.pending,
        ):
            raise ValueError("The provider grant did not qualify for enrollment")

    def poll(self):
        if (
            self.process is None
            or self.finished is not None
            or time.monotonic() < self.retry_at
        ):
            return
        self.drain()
        status = self.process.poll()
        if status is None:
            return
        self.drain()
        self.url = self.code = ""
        self.prompt = False
        self.tail.clear()
        if status == 0 or (
            self.login_home != self.home and hasattr(self.module, "check_grant")
        ):
            try:
                self.commit()
                self.result = 0
            except OSError as error:
                if _retryable(error) and self.retries < len(RETRY_DELAYS):
                    self.retry_at = time.monotonic() + RETRY_DELAYS[self.retries]
                    self.retries += 1
                    return
            except (ValueError, RuntimeError):
                pass
        self.finished = time.monotonic()

    def close(self):
        if self.master is not None:
            os.close(self.master)
            self.master = None
        if self.process is not None and self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=1)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        self.url = self.code = ""
        self.tail.clear()

    def state(self):
        self.poll()
        phase = (
            ("saving" if self.retries else "pending")
            if self.finished is None
            else ("signed-in" if self.result == 0 else "failed")
        )
        return {
            "phase": phase,
            "url": self.url,
            "code": self.code,
            "input": self.prompt,
        }


class Portal(HTTPServer):
    allow_reuse_address = False

    def __init__(self, login):
        super().__init__(("127.0.0.1", 0), Handler)
        self.login = login
        self.authority = "127.0.0.1:" + str(self.server_port)
        self.route = "/nexus-signin/" + secrets.token_hex(32)
        self.session = None
        self.csrf = secrets.token_hex(32)
        self.csp_nonce = secrets.token_hex(16)

    def get_request(self):
        sock, addr = super().get_request()
        sock.settimeout(2)
        return sock, addr

    def handle_error(self, request, client_address):
        # Browser disconnects and malformed input must never put a request line into a log.
        pass


PAGE = """<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<title>Sign in to {provider} · Nexus</title>
<style nonce="{nonce}">:root{{color-scheme:light dark;font:17px Inter,Roboto,system-ui}}
body{{max-width:32rem;margin:12vh auto;padding:0 1rem}}code{{font-size:1.4em}}input,button{{font:inherit}}
[hidden]{{display:none}}</style><h1>Sign in to {provider}</h1>
<p>Nexus account <strong>{account}</strong></p><p id="status" role="status">Waiting for {provider}…</p>
<p id="verify" hidden>Verification code <code id="code"></code></p>
<p><a id="link" target="_blank" rel="noreferrer noopener" hidden>Open {provider} sign-in</a></p>
<form id="reply" hidden><label>Authorization code <input id="input" type="password" maxlength="2048"
 autocomplete="off" spellcheck="false"></label> <button>Continue</button></form>
<script nonce="{nonce}">
const find = id => document.getElementById(id);
let complete = false;
let offeredLink = false;
function status(message) {{
 const output = find('status');
 if (output.textContent !== message) output.textContent = message;
}}
async function refresh() {{
 try {{
  const r = await fetch(location.pathname + '/status', {{cache:'no-store'}});
  if (!r.ok) throw Error();
  const s = await r.json();
  find('link').hidden = !s.url;
  if (s.url) {{
   find('link').href = s.url;
   if (!offeredLink && document.activeElement === document.body) find('link').focus();
   offeredLink = true;
  }}
  find('verify').hidden = !s.code;
  find('code').textContent = s.code;
  find('reply').hidden = !s.input;
  complete = s.phase === 'signed-in' || s.phase === 'failed';
  status(s.phase === 'signed-in' ? 'Signed in. Return to Nexus.' :
    s.phase === 'failed' ? 'Sign-in ended. Return to Nexus to try again.' :
    s.phase === 'saving' ? 'Finishing sign-in…' :
    s.url ? 'Complete sign-in with {provider}.' : 'Waiting for {provider}…');
 }} catch {{ complete = true; status('Sign-in closed. Return to Nexus.'); }}
 if (!complete) setTimeout(refresh, 600);
}}
find('reply').addEventListener('submit', async e => {{
 e.preventDefault();
 const value = find('input').value; find('input').value='';
 const result = await fetch(location.pathname, {{method:'POST',body:new URLSearchParams({{csrf:'{csrf}',code:value}})}});
 if (!result.ok) status('The code could not be submitted.');
}});
refresh();</script></html>"""


class Handler(BaseHTTPRequestHandler):
    server: Portal

    def log_message(self, *args):
        pass

    def reply(self, status, body=b"", mime="text/plain; charset=utf-8", *, claim=False):
        self.send_response_only(status)
        self.send_header("Content-Type", mime)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        if claim:
            self.send_header(
                "Set-Cookie",
                f"nexus_signin={self.server.session}; Path={self.server.route}; HttpOnly; SameSite=Strict",
            )
        self.send_header("Cache-Control", "no-store")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("X-Frame-Options", "DENY")
        nonce = self.server.csp_nonce
        self.send_header(
            "Content-Security-Policy",
            "default-src 'none'; frame-ancestors 'none'; base-uri 'none'; "
            f"connect-src 'self'; script-src 'nonce-{nonce}'; style-src 'nonce-{nonce}'; form-action 'none'",
        )
        self.end_headers()
        self.wfile.write(body)

    def send_error(self, code, *args):
        self.reply(code)

    def authorized(self):
        return (
            self.headers.get_all("Host") == [self.server.authority]
            and self.headers.get_all("Origin", [])
            in ([], ["http://" + self.server.authority])
            and self.headers.get("Sec-Fetch-Site", "") != "cross-site"
        )

    def claimed(self):
        tokens = [
            part.strip()[len("nexus_signin=") :]
            for header in self.headers.get_all("Cookie", [])
            for part in header.split(";")
            if part.strip().startswith("nexus_signin=")
        ]
        return self.server.session is not None and tokens == [self.server.session]

    def do_GET(self):
        app = self.server
        if not self.authorized() or self.path not in (app.route, app.route + "/status"):
            return self.reply(404)
        claim = self.path == app.route and app.session is None
        if not claim and not self.claimed():
            return self.reply(403)
        if self.path.endswith("/status"):
            state = app.login.state()
            self.reply(200, json.dumps(state).encode(), "application/json")
            app.login.last_seen = state["phase"] in {"signed-in", "failed"}
            return
        if claim:
            app.session = secrets.token_hex(32)
        app.login.start()
        page = PAGE.format(
            provider=PROVIDERS[app.login.provider][0],
            account=html.escape(app.login.account),
            csrf=app.csrf,
            nonce=app.csp_nonce,
        )
        self.reply(200, page.encode(), "text/html; charset=utf-8", claim=claim)

    def do_POST(self):
        app = self.server
        if (
            not self.authorized()
            or not self.claimed()
            or self.path != app.route
            or self.headers.get_all("Origin") != ["http://" + app.authority]
        ):
            return self.reply(403)
        length = self.headers.get("Content-Length", "")
        if (
            self.headers.get("Transfer-Encoding")
            or len(self.headers.get_all("Content-Length", [])) != 1
            or not re.fullmatch(r"[0-9]{1,4}", length)
            or int(length) > MAX_FORM
            or self.headers.get_content_type() != "application/x-www-form-urlencoded"
        ):
            return self.reply(400)
        try:
            fields = parse_qs(
                self.rfile.read(int(length)).decode("ascii"),
                strict_parsing=True,
                max_num_fields=2,
                errors="strict",
            )
        except (UnicodeError, ValueError):
            return self.reply(400)
        if fields.get("csrf") != [app.csrf] or len(fields.get("code", [])) != 1:
            return self.reply(403)
        self.reply(204 if app.login.input(fields["code"][0]) else 400)


def run(login):
    with Portal(login) as portal:
        notify_runner("http://" + portal.authority + portal.route)
        portal.timeout = 0.1
        deadline = time.monotonic() + 600
        while time.monotonic() < deadline:
            portal.handle_request()
            login.poll()
            if login.finished is not None and (
                login.last_seen or time.monotonic() - login.finished > 3
            ):
                return login.result
    return 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("provider", choices=PROVIDERS)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--account", required=True)
    args = parser.parse_args()
    if not args.binary.is_absolute():
        return 1
    os.umask(0o077)
    home = home_for(args.provider, args.account, create=True)
    module = importlib.import_module(f".{args.provider}_account", __package__)
    # The device flows print their verification page. A CLI opening the OS browser directly
    # would send it to whichever desktop happens to host the server. Keep that command local.
    with enrollment_lock(home):
        for attempt in range(len(RETRY_DELAYS) + 1):
            try:
                if recover(home, args.provider, module, str(args.binary)) or enrolled(
                    home, args.provider
                ):
                    return 0
                prior = module.grant_stamp(home)
                break
            except OSError as error:
                if not _retryable(error) or attempt == len(RETRY_DELAYS):
                    raise
                time.sleep(RETRY_DELAYS[attempt])
        staging = (
            module.staged_login(home)
            if hasattr(module, "staged_login")
            else nullcontext((home, None))
        )
        with (
            staging as (login_home, promote),
            tempfile.TemporaryDirectory(prefix=".login-bin-", dir=home) as directory,
        ):
            command = module.login_command(str(args.binary), login_home)
            env = process_env(args.provider, login_home)
            for binary in ("open", "xdg-open", "sensible-browser", "wslview"):
                (Path(directory) / binary).symlink_to("/usr/bin/false")
            env.update(PATH=directory + ":" + os.defpath, BROWSER="/usr/bin/false")
            login = Login(
                args.provider,
                args.account,
                home,
                command,
                env,
                login_home=login_home,
                promote=promote,
                module=module,
                binary=str(args.binary),
                prior=prior,
            )
            try:
                result = run(login)
                if result == 0:
                    from .refresh_catalog import after_login
                    after_login(args.provider, args.binary, home)
                return result
            finally:
                login.close()


def terminate(signum, frame):
    # The runner cancels the entire process group. Unwind staged files and reap the vendor.
    raise SystemExit(1)


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, terminate)
    try:
        raise SystemExit(main())
    except Exception:  # noqa: BLE001 -- this process is the private transport boundary.
        # Nothing from an exception raised while handling credentials belongs in a terminal or log.
        raise SystemExit(1) from None
