"""Private named accounts shared by the provider sign-in and chat processes."""

import contextlib
import fcntl
import os
import re
import secrets
import stat
from pathlib import Path

PROVIDERS = frozenset(("codex", "antigravity", "grok", "muse"))
ACCOUNT = re.compile(r"[A-Za-z0-9_][A-Za-z0-9._-]{0,63}")
RECEIPT = ".nexus-enrolled"
PENDING = ".nexus-enrollment-pending"
DIRECTORY = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW


class EnrollmentRefused(ValueError):
    """A safe, public failure category for a provider enrollment."""

    def __init__(self, code, reason):
        super().__init__(reason)
        self.code = code


def _open_exact(parent, name, flags):
    # Case-folding filesystems can otherwise open another registered account or receipt.
    fd = os.open(name, flags, dir_fd=parent)
    try:
        if name not in os.listdir(parent):
            raise ValueError("The account path contains a differently spelled directory or receipt.")
        return fd
    except BaseException:
        os.close(fd)
        raise


def home_for(provider, account, create=False):
    if provider not in PROVIDERS or not ACCOUNT.fullmatch(account):
        raise ValueError("Unknown provider or invalid account name.")
    root = Path.home()
    if not root.is_absolute():
        raise ValueError("The server home must be absolute.")
    parts = (".nexus", "server", "harness", provider, account)
    fd = os.open(root, DIRECTORY)
    try:
        for i, part in enumerate(parts):
            if create:
                try:
                    os.mkdir(part, mode=0o700, dir_fd=fd)
                except FileExistsError:
                    pass
            child = _open_exact(fd, part, DIRECTORY)
            os.close(fd)
            fd = child
            info = os.fstat(fd)
            mask = 0o077 if i == len(parts) - 1 else 0o022
            if info.st_uid != os.getuid() or info.st_mode & mask:
                raise ValueError("Unsafe ownership or permissions on the named account path.")
    finally:
        os.close(fd)
    return root.joinpath(*parts)


def process_env(provider, home):
    if provider not in PROVIDERS:
        raise ValueError("Unknown provider.")
    env = {
        "HOME": str(home),
        "PATH": os.defpath,
        "LANG": "en_US.UTF-8",
        "XDG_CONFIG_HOME": str(home / ".config"),
        "XDG_DATA_HOME": str(home / ".local/share"),
        "XDG_CACHE_HOME": str(home / ".cache"),
        "XDG_STATE_HOME": str(home / ".local/state"),
        "BROWSER": "/usr/bin/false",
    }
    if provider == "codex":
        env["CODEX_HOME"] = str(home / ".codex")
    if provider == "antigravity":
        env["TMPDIR"] = str(home / "tmp")
    if provider == "muse":
        env["TBH_CREDENTIAL_BACKEND"] = "file"
    if provider == "grok":
        env["GROK_HOME"] = str(home / ".grok")
    return env


@contextlib.contextmanager
def locked_directory(home, exclusive=False):
    fd = os.open(home, DIRECTORY)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX if exclusive else fcntl.LOCK_SH)
        yield fd
    finally:
        os.close(fd)


def _private_marker(fd, name, maximum):
    try:
        marker = _open_exact(fd, name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    except FileNotFoundError:
        return None, None
    with os.fdopen(marker, "rb") as stream:
        info = os.fstat(marker)
        if (
            not stat.S_ISREG(info.st_mode)
            or stat.S_IMODE(info.st_mode) != 0o600
            or info.st_uid != os.getuid()
            or info.st_nlink != 1
        ):
            raise ValueError("Invalid enrollment marker.")
        data = stream.read(maximum + 1)
        if len(data) > maximum:
            raise ValueError("Invalid enrollment marker size.")
        return info, data


def read_pending(fd):
    return _private_marker(fd, PENDING, 1024)[1]


def _receipt(fd, provider):
    expected = (provider + "\n").encode()
    info, data = _private_marker(fd, RECEIPT, len(expected))
    if info and data != expected:
        raise ValueError("Invalid provider enrollment receipt.")
    return info


def enrolled(home, provider):
    with locked_directory(home) as fd:
        return _receipt(fd, provider)


def store_receipt(home, provider, *, pending=None):
    if provider not in PROVIDERS:
        raise ValueError("Unknown provider.")
    with locked_directory(home, exclusive=True) as fd:
        if pending is not None:
            current = read_pending(fd)
            if current is None and _receipt(fd, provider):
                os.fsync(fd)
                return
            if current != pending:
                raise ValueError("The pending enrollment changed or was revoked.")
        name = ".nexus-enroll-" + secrets.token_hex(12)
        marker = os.open(name, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600, dir_fd=fd)
        try:
            with os.fdopen(marker, "wb") as stream:
                stream.write((provider + "\n").encode())
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(name, RECEIPT, src_dir_fd=fd, dst_dir_fd=fd)
            os.fsync(fd)
            if pending is not None:
                os.unlink(PENDING, dir_fd=fd)
                os.fsync(fd)
        finally:
            try:
                os.unlink(name, dir_fd=fd)
            except FileNotFoundError:
                pass


def remove_receipt_if_current(home, provider, observed):
    """A refusal from an older chat must never invalidate a newer sign-in."""
    with locked_directory(home, exclusive=True) as fd:
        current = _receipt(fd, provider)
        if current and os.path.samestat(current, observed) and current.st_ctime_ns == observed.st_ctime_ns:
            # A crash recovering a successful login must not restore a subsequently refused grant.
            try:
                os.unlink(PENDING, dir_fd=fd)
            except FileNotFoundError:
                pass
            else:
                os.fsync(fd)
            os.unlink(RECEIPT, dir_fd=fd)
            os.fsync(fd)
            return True
        return False
