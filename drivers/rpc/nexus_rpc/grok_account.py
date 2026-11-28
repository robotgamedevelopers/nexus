"""Account-private Grok device enrollment and text-only ACP on macOS."""

import errno
import json
import os
import secrets
import stat
import subprocess
import sys
from contextlib import ExitStack
from datetime import datetime, timedelta, timezone
from pathlib import Path

from .account_runtime import DIRECTORY, process_env


SUPPORTS_LIMIT = sys.platform == "darwin" and Path("/usr/bin/sandbox-exec").is_file()
_ISSUER = "https://auth.x.ai"
_CLIENT_ID = "b1a00492-073a-47ea-816f-4c329264a828"
_MAX_AUTH_BYTES = 1024 * 1024

# A curated profile requires one declared tool. Remove that same tool before
# registration. Grok's hosted searches run at xAI and have no local workspace.
_AGENT = """---
name: nexus-conversation
description: Nexus text conversation
discoverSkills: false
agentsMd: false
mcpInheritance: none
injectDefaultTools: false
toolConfig:
  tools:
    - id: GrokBuild:read_file
disallowedTools:
  - GrokBuild:read_file
---
"""

# Grok needs its own OAuth cache and persisted ACP sessions. Its executable
# can read those directly, while the dialogue profile has no local tools.
# The whole process is separately confined to this account's state. This
# includes native plugin discovery, subprocesses, and shared OS credentials.
_SANDBOX = """(version 1)
(deny default)
(import "system.sb")
(allow signal (target self))
(allow process-info* (target self))
(allow process-exec file-read* file-map-executable (literal (param "BINARY")))
(allow file-read-metadata (path-ancestors (param "ROOM")) (path-ancestors (param "BINARY")))
(allow file-read* (literal (param "ACCOUNT")) (subpath (param "ROOM"))
    (literal (param "AGENT")) (literal "/private/etc/hosts")
    (literal "/private/etc/resolv.conf") (literal "/private/var/run/resolv.conf"))
(allow file-read* file-write* (subpath (param "DATA")))
(allow file-ioctl file-read* file-write-data (literal "/dev/tty") (regex #"^/dev/ttys[0-9]+$"))
(allow mach-lookup (global-name "com.apple.SystemConfiguration.configd")
    (global-name "com.apple.dnssd.service"))
(allow network-outbound (literal "/private/var/run/mDNSResponder") (remote tcp "*:443"))
(deny mach-lookup (global-name-prefix "com.apple.securityd")
    (global-name-prefix "com.apple.security.") (global-name-prefix "com.apple.AppSSO"))
"""


def _prepare(home: Path, *, bus=False):
    fd = os.open(home, DIRECTORY)
    try:
        for name in (".grok", "room"):
            try:
                os.mkdir(name, 0o700, dir_fd=fd)
            except FileExistsError:
                pass
            child = os.open(name, DIRECTORY, dir_fd=fd)
            os.close(child)
        name = ".nexus-agent-" + secrets.token_hex(12)
        writer = os.open(name, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600, dir_fd=fd)
        try:
            with os.fdopen(writer, "w") as f:
                f.write(_AGENT.replace("mcpInheritance: none", "mcpInheritance:\n  named: [nexus_project]") if bus else _AGENT)
            os.replace(name, "nexus-bus-agent.md" if bus else "nexus-agent.md", src_dir_fd=fd, dst_dir_fd=fd)
        finally:
            try:
                os.unlink(name, dir_fd=fd)
            except FileNotFoundError:
                pass
    finally:
        os.close(fd)


def _command(binary, home, project_socket=None):
    sandbox = Path("/usr/bin/sandbox-exec")
    if sys.platform != "darwin" or not sandbox.is_file():
        raise ValueError("Isolated Grok accounts require the macOS sandbox.")
    _prepare(home, bus=bool(project_socket))
    home = home.resolve(strict=True)
    executable = Path(binary).resolve(strict=True)
    paths = {"BINARY": executable, "ACCOUNT": home, "ROOM": home / "room",
             "DATA": home / ".grok", "AGENT": home / ("nexus-bus-agent.md" if project_socket else "nexus-agent.md")}
    profile = _SANDBOX
    if project_socket:
        paths["SOCKET"] = project_socket
        profile += '\n(allow process-exec file-read* file-map-executable (literal "/usr/bin/nc"))\n'
        profile += '(allow network-outbound (literal (param "SOCKET")))\n'
    cmd = [str(sandbox)]
    for key, path in paths.items():
        cmd.extend(["-D", f"{key}={path}"])
    return [*cmd, "-p", profile, str(executable)]


def login_command(binary, home: Path) -> list[str]:
    return [*_command(binary, home), "login", "--device-auth"]


def _unique_fields(pairs):
    fields = dict(pairs)
    if len(fields) != len(pairs):
        raise ValueError("Duplicate field in Grok auth data.")
    return fields


def _device_grant(raw):
    store = json.loads(raw.decode("utf-8"), object_pairs_hook=_unique_fields)
    scope = _ISSUER + "::" + _CLIENT_ID
    if not isinstance(store, dict) or set(store) != {scope}:
        return False
    grant = store[scope]
    if (not isinstance(grant, dict) or grant.get("auth_mode") != "oidc"
            or grant.get("oidc_issuer") != _ISSUER or grant.get("oidc_client_id") != _CLIENT_ID
            or not isinstance(grant.get("user_id"), str)
            or not isinstance(grant.get("key"), str) or not grant["key"].strip()):
        return False
    created = datetime.fromisoformat(grant["create_time"])
    expires = (datetime.fromisoformat(grant["expires_at"]) if grant.get("expires_at") is not None
               else created + timedelta(days=30))  # Grok's TTL when the issuer omits expires_in.
    if created.tzinfo is None or expires.tzinfo is None:
        return False
    refresh = grant.get("refresh_token")
    if refresh is not None and not isinstance(refresh, str):
        return False
    return bool(refresh and refresh.strip()) or expires > datetime.now(timezone.utc)


def grant_stamp(home: Path) -> str | None:
    """Identify an existing private xAI device grant using only filesystem metadata."""
    try:
        with ExitStack() as stack:
            account = os.open(home, DIRECTORY)
            stack.callback(os.close, account)
            data = os.open(".grok", DIRECTORY, dir_fd=account)
            stack.callback(os.close, data)
            for directory in (account, data):
                info = os.fstat(directory)
                if info.st_uid != os.getuid() or info.st_mode & 0o077:
                    return None
            fd = os.open("auth.json", os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=data)
            stack.callback(os.close, fd)
            info = os.fstat(fd)
            if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077
                    or info.st_nlink != 1 or not 0 < info.st_size <= _MAX_AUTH_BYTES):
                return None
            with os.fdopen(fd, "rb", closefd=False) as stream:
                raw = stream.read(_MAX_AUTH_BYTES + 1)
            current = os.stat("auth.json", dir_fd=data, follow_symlinks=False)
            if not os.path.samestat(info, current) or current.st_ctime_ns != info.st_ctime_ns:
                raise BlockingIOError(errno.EAGAIN, "Grok is replacing its auth file.")
            if len(raw) != info.st_size or not _device_grant(raw):
                return None
            return f"{info.st_dev}:{info.st_ino}:{info.st_size}:{info.st_mtime_ns}"
    except (ValueError, TypeError, KeyError, OverflowError, RecursionError):
        return None
    except OSError as exc:
        if exc.errno in (errno.ENOENT, errno.ENOTDIR, errno.ELOOP, errno.ENXIO, errno.EISDIR):
            return None
        raise


def chat_command(binary, home: Path, cwd: str, *, project_socket=None) -> list[str]:
    cmd = _command(binary, home, project_socket)
    # Grok can fall back to its built-in coding tools on unrecognized profile
    # fields. Permit releases whose native model-facing schema has been checked.
    version = subprocess.run(
        [*cmd, "--version"], env=process_env("grok", home), cwd=home / "room",
        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
        timeout=5, check=False,
    )
    audited = b"grok 1.0.88 (2fe3608f7398)"
    if version.returncode or version.stdout.strip() not in (audited, audited + b" [stable]"):
        raise ValueError("This Grok build has not been verified for isolated conversations.")
    return [*cmd, "agent", "--no-leader", "--agent-profile", str(home / ("nexus-bus-agent.md" if project_socket else "nexus-agent.md")), "stdio"]
