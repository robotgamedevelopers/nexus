"""Account-scoped Muse browser login, subscription checks, and chat-only sessions."""

import asyncio
import errno
import json
import os
import stat
from contextlib import ExitStack, contextmanager

from .account_runtime import DIRECTORY, process_env

from .backends import Failure

SUPPORTS_LIMIT = True


def login_command(binary, home):
    return [str(binary), "login"]


def chat_command(binary, home, cwd):
    room = home / "room"
    room.mkdir(mode=0o700, exist_ok=True)
    info = room.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
        raise Failure("Muse needs a private chat directory owned by the Nexus server.")
    # The runtime supplies a private HOME/XDG tree and intentionally omits API keys.
    # The empty preset removes built-in tools; the explicit roster also removes
    # reminder agents that would otherwise run their own model turns.
    return [
        str(binary), "serve", "--provider", "meta", "--preset", "empty",
        "--disable-shell", "--disable-write", "--sandbox-network", "restricted",
        "--disable-request-user-input", "--contributor-mode", "off",
        "-c", "run.toolset=[]",
        "-c", "run.reminder_roster=[]",
        "-c", "local_session_messaging.enabled=false",
        "-c", "feature_config.enabled=false",
        "-c", "telemetry.enabled=false",
    ]


async def subscription_tier(rpc, report_usage=None):
    account = await rpc.call("account/read", {})
    if account.get("state") == "loggedOut":
        raise Failure("Muse reports this account is signed out.", "no-credential", revoked=True)
    if account.get("state") in {"envKey", "apiKey"}:
        raise Failure("Muse requires a browser-linked account for subscription chat.", "no-credential", revoked=True)
    if account.get("state") != "accountLogin" or account.get("credentialRequired") is not True:
        raise Failure("Muse could not verify the browser-linked account.")

    # usage/read is only a cache and may be empty indefinitely on a fresh login.
    # usage/refresh performs a credential mint without opening a chat or using a model.
    result = await rpc.call("usage/refresh", {})
    if result.get("subscriptionActive") is False:
        raise Failure("Muse reports this account has no active subscription.", "no-credential", revoked=True)
    if result.get("subscriptionActive") is not True:
        raise Failure("Muse could not verify an active subscription.")
    if report_usage:
        report_usage(result.get("usage"))
    tier = result.get("tierName")
    return tier if isinstance(tier, str) and 0 < len(tier.strip()) <= 128 else "subscription"


@contextmanager
def _store(home, *, create=False):
    # Pin every directory so validation, rename and fsync operate on the same store.
    with ExitStack() as cleanup:
        directory = os.open(home, DIRECTORY)
        cleanup.callback(os.close, directory)
        root = os.fstat(directory)
        if root.st_uid != os.getuid() or root.st_mode & 0o077:
            raise ValueError("Muse needs an owner-only account home.")
        directories = [directory]
        for name in (".config", "muse"):
            if create:
                try:
                    os.mkdir(name, 0o700, dir_fd=directory)
                except FileExistsError:
                    pass
            directory = os.open(name, DIRECTORY, dir_fd=directory)
            cleanup.callback(os.close, directory)
            info = os.fstat(directory)
            if info.st_uid != os.getuid() or info.st_mode & 0o022:
                raise ValueError("Muse has an unsafe credential directory.")
            directories.append(directory)
        yield directories


def _grant(directory, *, sync=False):
    fd = os.open("auth.json", os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=directory)
    with os.fdopen(fd, "rb") as stream:
        info = os.fstat(fd)
        if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_nlink != 1
                or info.st_mode & 0o077 or not 0 < info.st_size <= (1 << 20)):
            raise ValueError("Muse needs a private browser grant.")
        # This is the store shape recognized by Meta's official Muse launcher.
        # Only the recognition decision and non-secret filesystem metadata leave here.
        try:
            raw = stream.read(info.st_size + 1)
            if len(raw) != info.st_size:
                raise ValueError
            grant = json.loads(raw)["providers"]["meta"]
            if (grant["mechanism"] != "oauth" or not isinstance(grant["access_token"], str)
                    or not grant["access_token"]):
                raise ValueError
        except (TypeError, ValueError, KeyError, RecursionError):
            raise ValueError("Muse did not persist a complete browser grant.") from None
        if sync:
            os.fsync(fd)
        return f"{info.st_dev}:{info.st_ino}:{info.st_size}:{info.st_mtime_ns}"


def grant_stamp(home):
    """Return a filesystem change stamp, only for a locally persisted Meta OAuth grant."""
    try:
        with _store(home) as directories:
            return _grant(directories[-1])
    except ValueError:
        return None
    except OSError as err:
        if err.errno in (errno.ENOENT, errno.ENOTDIR, errno.ELOOP, errno.ENXIO, errno.EISDIR):
            return None
        raise


@contextmanager
def staged_login(home, *, resume=None):
    from .enroll import staging_directory

    with staging_directory(home, resume) as staging:
        expected = None

        def promote():
            nonlocal expected
            with _store(staging) as source, _store(home, create=True) as destination:
                try:
                    stamp = _grant(source[-1], sync=True)
                except FileNotFoundError:
                    if expected is None:
                        raise
                else:
                    if expected is not None and stamp != expected:
                        raise ValueError("The approved Muse grant changed during installation.")
                    expected = stamp
                    os.replace("auth.json", "auth.json", src_dir_fd=source[-1], dst_dir_fd=destination[-1])
                if _grant(destination[-1], sync=True) != expected:
                    raise ValueError("The installed Muse grant differs from the approved grant.")
                for directory in reversed(destination):
                    os.fsync(directory)

        yield staging, promote


def check_grant(binary, stage):
    """Fresh, bounded entitlement probe. Only an active browser grant may replace the old one."""
    from .driver import Rpc

    if grant_stamp(stage) is None:
        return False

    async def probe():
        command = chat_command(binary, stage, stage / "room")
        proc = await asyncio.create_subprocess_exec(*command, cwd=stage / "room", env=process_env("muse", stage),
                                                    stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE,
                                                    stderr=asyncio.subprocess.DEVNULL)
        try:
            async with asyncio.timeout(15):
                rpc = Rpc(proc)
                await rpc.call("initialize", {"clientInfo": {"name": "nexus", "version": "1"},
                                              "capabilities": {"experimentalApi": True}})
                await rpc.notify("initialized", {})
                await subscription_tier(rpc)
                return True
        finally:
            if proc.returncode is None:
                try:
                    proc.kill()
                except ProcessLookupError:
                    pass
            await proc.communicate()

    try:
        return asyncio.run(probe())
    except Failure as error:
        if error.revoked:
            return False
    except (TimeoutError, OSError, ValueError, KeyError, TypeError):
        pass
    raise OSError(errno.EAGAIN, "Muse could not confirm the saved grant's subscription yet.")
