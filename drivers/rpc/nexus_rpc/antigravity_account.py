"""Launch the Studio Antigravity harness with a private, file-backed Google login."""

import errno
import json
import os
import secrets
import stat
import sys
from contextlib import contextmanager
from pathlib import Path

from .account_runtime import DIRECTORY


_VENDOR_DIR = (".gemini", "antigravity-cli")
_TOKEN = "antigravity-oauth-token"
SUPPORTS_LIMIT = sys.platform == "darwin" and Path("/usr/bin/sandbox-exec").is_file()


# The harness needs its own state; its dialogue agent gets no filesystem, shell, or MCP tools.
_AGENT = """---
name: nexus-dialogue
description: Nexus text conversation
mainAgent: true
subagent: false
commandExecutionPolicy: "off"
inheritMcp: false
inheritCustomizations: false
tools:
  - send_message
---
Reply to the conversation using the AI's supplied standing instructions.
"""

# This private CLI settings file also guards file access if a saved session used another agent.
_SETTINGS = json.dumps({"permissions": {"deny": [
    "read_file(*)", "write_file(*)", "command(*)", "read_url(*)", "execute_url(*)",
    "mcp(*)",
]}}) + "\n"

# system.sb supplies dyld, system frameworks and certificate trust. No generic subprocess,
# Keychain service, shared user home, or sibling account is granted. --login prints its URL
# on the inherited tty; the desktop opens it outside this process. ACP uses local TCP IPC.
_PROFILE = """(version 1)
(deny default)
(import "system.sb")
(allow signal (target self))
(allow process-info* (target self))
(allow process-fork)
(allow process-exec file-read* file-map-executable (literal (param "BINARY")))
(allow file-read-metadata (path-ancestors (param "ACCOUNT")) (path-ancestors (param "BINARY")))
(allow file-read* (literal (param "ACCOUNT")) (subpath (param "ROOM"))
    (literal "/private/etc/hosts") (literal "/private/etc/resolv.conf")
    (literal "/private/var/run/resolv.conf"))
(allow file-read* file-write* (subpath (param "DATA")) (subpath (param "TMP")))
(allow file-ioctl)
(allow file-read* file-write-data (literal "/dev/tty"))
(allow mach-lookup (global-name "com.apple.SystemConfiguration.configd")
    (global-name "com.apple.dnssd.service"))
(deny mach-lookup (global-name-prefix "com.apple.securityd")
    (global-name-prefix "com.apple.security.") (global-name-prefix "com.apple.AppSSO"))
(allow network-outbound (literal "/private/var/run/mDNSResponder") (remote tcp "*:443")
    (remote tcp "localhost:*"))
(allow network-inbound network-bind (local tcp "localhost:*"))
"""


@contextmanager
def _directory(home, directories, *, create=True):
    """Pin each directory while replacing driver-owned configuration, including on retries."""
    fd = os.open(home, DIRECTORY)
    try:
        for part in directories:
            if create:
                try:
                    os.mkdir(part, 0o700, dir_fd=fd)
                except FileExistsError:
                    pass
            child = os.open(part, DIRECTORY, dir_fd=fd)
            os.close(fd)
            fd = child
            info = os.fstat(fd)
            if info.st_uid != os.getuid() or info.st_mode & 0o022:
                raise ValueError("Antigravity account directory has unsafe ownership or permissions.")
        yield fd
    finally:
        os.close(fd)


def _write_config(fd, name, content):
    tmp = ".nexus-" + secrets.token_hex(12)
    writer = os.open(tmp, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600, dir_fd=fd)
    try:
        with os.fdopen(writer, "w") as f:
            f.write(content)
        os.replace(tmp, name, src_dir_fd=fd, dst_dir_fd=fd)
    finally:
        try:
            os.unlink(tmp, dir_fd=fd)
        except FileNotFoundError:
            pass


def _command(binary, home: Path, *, login, project_socket=None, cwd=None):
    if not SUPPORTS_LIMIT:
        raise ValueError("Isolated Antigravity accounts require the macOS sandbox on Studio.")
    executable = Path(binary).resolve(strict=True)
    with _directory(home, ("tmp",)):
        pass
    with _directory(home, ("room", ".agents", "agents", "nexus-dialogue")) as fd:
        _write_config(fd, "agent.md", _AGENT)
    with _directory(home, _VENDOR_DIR) as fd:
        _write_config(fd, "settings.json", _SETTINGS)
    home = home.resolve(strict=True)
    data = home / ".gemini"
    paths = {"ACCOUNT": home, "BINARY": executable, "DATA": data, "ROOM": home / "room",
             "TMP": home / "tmp"}
    profile = _PROFILE
    if project_socket:
        paths["ROOM"] = Path(cwd)
        paths["SOCKET"] = project_socket
        profile += '\n(allow process-exec file-read* file-map-executable (literal "/usr/bin/nc"))\n'
        profile += '(allow network-outbound (literal (param "SOCKET")))\n'
    command = ["/usr/bin/sandbox-exec"]
    for key, path in paths.items():
        command.extend(["-D", f"{key}={path}"])
    command.extend(["-p", profile, str(executable), "acp", "--disable-telemetry",
                    "--gemini-dir", str(data)])
    if login:
        return [*command, "--login"]
    return [*command, "--agent", "nexus-project" if project_socket else "nexus-dialogue", "--sandbox"]


def login_command(binary, home: Path) -> list[str]:
    return _command(binary, home, login=True)


def chat_command(binary, home: Path, cwd: str, *, project_socket=None) -> list[str]:
    return _command(binary, home, login=False, project_socket=project_socket, cwd=cwd)


_MAX_GRANT = 1024 * 1024


def _grant(directory, *, seal=False):
    fd = os.open(_TOKEN, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=directory)
    with os.fdopen(fd, "rb") as stream:
        info = os.fstat(fd)
        if (
            not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid()
            or info.st_nlink != 1 or info.st_mode & 0o022 or not 0 < info.st_size <= _MAX_GRANT
        ):
            raise ValueError("Antigravity did not persist a private Google login.")
        raw = stream.read(_MAX_GRANT + 1)
        if len(raw) != info.st_size:
            raise ValueError("Antigravity did not finish writing the Google login.")
        try:
            record = json.loads(raw)
        except (ValueError, UnicodeError, RecursionError):
            raise ValueError("Antigravity did not finish writing the Google login.") from None
        # Current agy stores the OAuth token inside an envelope. Older versions wrote it directly.
        if isinstance(record, dict) and "token" in record:
            record = record["token"]
        token = record.get("access_token") if isinstance(record, dict) else None
        if not isinstance(token, str) or not token:
            raise ValueError("Antigravity did not finish writing the Google login.")
        if seal:
            os.fchmod(fd, 0o600)
        os.fsync(fd)
        return f"{info.st_dev}:{info.st_ino}:{info.st_size}:{info.st_mtime_ns}"


def _sync_grant_directories(home, directory):
    os.fsync(directory)
    # A first login may have created both levels of .gemini/antigravity-cli.
    for parts in ((".gemini",), ()):
        with _directory(home, parts, create=False) as parent:
            os.fsync(parent)


def grant_stamp(home) -> str | None:
    """Validate and sync the grant through read-only descriptors; identify it by file metadata."""
    try:
        with _directory(home, _VENDOR_DIR, create=False) as directory:
            stamp = _grant(directory)
            _sync_grant_directories(home, directory)
            return stamp
    except ValueError:
        return None
    except OSError as error:
        if error.errno in (errno.ENOENT, errno.ELOOP, errno.ENOTDIR, errno.ENXIO, errno.ENODEV):
            return None
        raise


@contextmanager
def staged_login(home, *, resume=None):
    """Keep the existing login until agy has exited and persisted its new OAuth grant."""
    from .enroll import staging_directory

    with staging_directory(home, resume) as staging:
        expected = None

        def promote():
            nonlocal expected
            with _directory(staging, _VENDOR_DIR, create=False) as source, _directory(home, _VENDOR_DIR) as dest:
                try:
                    current = _grant(source, seal=True)
                except FileNotFoundError:
                    # rename may have completed immediately before the directory sync failed.
                    if expected is None or _grant(dest) != expected:
                        raise
                else:
                    if expected is not None and expected != current:
                        raise ValueError("The staged Google login changed during promotion.")
                    expected = current
                    os.replace(_TOKEN, _TOKEN, src_dir_fd=source, dst_dir_fd=dest)
                _sync_grant_directories(home, dest)

        yield staging, promote
