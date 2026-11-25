"""Codex's ChatGPT device login and named permission profile for Nexus turns."""

import base64
import errno
import json
import os
import stat
import sys
from contextlib import contextmanager
from pathlib import Path

PROFILE = "nexus-chat"
SUPPORTS_LIMIT = sys.platform == "darwin" and Path("/usr/bin/sandbox-exec").is_file()
_AUTH = (
    "-c",
    'forced_login_method="chatgpt"',
    "-c",
    'cli_auth_credentials_store="file"',
)


def login_command(binary, home):
    # The launcher supplies the account's private HOME and CODEX_HOME to both processes.
    return [binary, "login", "--device-auth", *_AUTH]


def chat_command(binary, home, cwd, *, keep_out=(), limited=False, write_dirs=(), bus_sender=None):
    if not SUPPORTS_LIMIT:
        raise ValueError("The Codex permission profile requires a verified macOS sandbox.")
    workspace = Path(cwd).resolve(strict=True)
    operator = Path.home()
    protected = [
        Path(home),
        *(
            operator / name
            for name in (
                ".nexus",
                ".codex",
                ".claude",
                ".gemini",
                ".grok",
                ".config/muse",
                ".local/share/muse",
                ".ssh",
                "Library/Keychains",
            )
        ),
    ]
    resolved = {form for path in protected for form in (path.absolute(), path.resolve())}
    if any(workspace.is_relative_to(path) or path.is_relative_to(workspace) for path in resolved):
        raise ValueError("Choose a working directory separate from the credential directories.")
    for extra in keep_out:
        path = Path(extra)
        if not path.is_absolute() or path == Path("/"):
            raise ValueError("Codex requires an absolute keep-out directory below the filesystem root.")
        resolved.add(path.resolve())
    if any(any(c in str(path) for c in "*?[]") for path in resolved):
        raise ValueError("Codex cannot represent wildcard characters in a private directory name.")
    access = {":minimal" if limited else ":root": "read", ":workspace_roots": "write"}
    for item in write_dirs:
        path = Path(item).resolve(strict=True)
        if not Path(item).is_absolute() or not path.is_dir() or any(c in str(path) for c in "*?[]") or any(
            path.is_relative_to(p) or p.is_relative_to(path) for p in resolved
        ):
            raise ValueError("Choose a writable directory separate from the protected directories.")
        access[str(path)] = "write"
    access.update({str(path): "none" for path in sorted(resolved)})
    filesystem = ", ".join(
        f"{json.dumps(path, ensure_ascii=False)}={json.dumps(mode)}" for path, mode in access.items()
    )
    # A fresh private CODEX_HOME has no trusted projects. Pin that decision for
    # each ancestor too, so a checkout cannot introduce an MCP server or hook.
    projects = ", ".join(
        f'{json.dumps(str(path), ensure_ascii=False)}={{trust_level="untrusted"}}'
        for path in (workspace, *workspace.parents)
    )
    settings = [
        f'default_permissions="{PROFILE}"',
        f"permissions.{PROFILE}.filesystem={{{filesystem}}}",
        f"permissions.{PROFILE}.network.enabled=false",
        'approval_policy="never"',
        "allow_login_shell=false",
        f"projects={{{projects}}}",
        "features.hooks=false",
        "features.plugins=false",
        "features.apps=false",
        "features.multi_agent=false",
        "features.skill_mcp_dependency_install=false",
    ]
    if bus_sender:
        import sys
        from .bus_tool import validate
        validate(bus_sender)
        args = ["-I", "-B", str(Path(__file__).with_name("bus_tool.py")), bus_sender]
        settings.extend([
            "mcp_servers.nexus.command=" + json.dumps(sys.executable),
            "mcp_servers.nexus.args=" + json.dumps(args),
            "mcp_servers.nexus.env.HOME=" + json.dumps(str(operator)),
            'mcp_servers.nexus.tools.nexus_bus_send.approval_mode="approve"',
        ])
    return [
        binary,
        "app-server",
        "--stdio",
        *_AUTH,
        *(item for setting in settings for item in ("-c", setting)),
    ]


_DIRECTORY = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW
_MAX_AUTH_BYTES = 1024 * 1024


@contextmanager
def _state_directory(home, *, create=False):
    parent = os.open(home, _DIRECTORY)
    try:
        info = os.fstat(parent)
        if info.st_uid != os.getuid() or info.st_mode & 0o022:
            raise ValueError("Codex account home has unsafe ownership or permissions.")
        if create:
            try:
                os.mkdir(".codex", mode=0o700, dir_fd=parent)
            except FileExistsError:
                pass
        directory = os.open(".codex", _DIRECTORY, dir_fd=parent)
        try:
            info = os.fstat(directory)
            if info.st_uid != os.getuid() or info.st_mode & 0o022:
                raise ValueError("Codex state directory has unsafe ownership or permissions.")
            yield directory, parent
        finally:
            os.close(directory)
    finally:
        os.close(parent)


def _file_stamp(info):
    return f"{info.st_dev}:{info.st_ino}:{info.st_size}:{info.st_mtime_ns}"


def personal_email(account):
    server = Path.home() / ".nexus/server"
    profile = server / "profile/config.json"
    if not profile.exists() or account not in json.loads(profile.read_text()).get("personal_chatgpt_accounts", []):
        return None
    rows = json.loads((server / "nexus-config/accounts.json").read_text())["accounts"]
    for row in rows:
        if row.get("provider") == "codex" and row.get("name") == account and row.get("email"):
            return row["email"]
    raise ValueError("Configure an email for this personal ChatGPT account.")


def matches_personal(expected, email, plan):
    return (isinstance(email, str) and email.casefold() == expected.casefold()
            and isinstance(plan, str) and plan.lower() in {"go", "plus", "pro", "prolite"})


def _grant(directory, *, sync=False, expected_email=None):
    fd = os.open("auth.json", os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=directory)
    try:
        info = os.fstat(fd)
        if (
            not stat.S_ISREG(info.st_mode)
            or info.st_uid != os.getuid()
            or info.st_mode & 0o077
            or info.st_nlink != 1
            or not 0 < info.st_size <= _MAX_AUTH_BYTES
        ):
            raise ValueError("Codex did not create a private credential file.")
        with os.fdopen(fd, "rb", closefd=False) as stream:
            raw = stream.read(_MAX_AUTH_BYTES + 1)
        if len(raw) != info.st_size:
            raise ValueError("Codex did not finish writing the ChatGPT login.")
        try:
            record = json.loads(raw)
        except (ValueError, UnicodeError, RecursionError):
            raise ValueError("Codex did not finish writing the ChatGPT login.") from None
        tokens = record.get("tokens") if isinstance(record, dict) else None
        if (
            not isinstance(tokens, dict)
            or record.get("auth_mode") != "chatgpt"
            or record.get("OPENAI_API_KEY")
            or not all(
                isinstance(tokens.get(k), str) and tokens[k]
                for k in ("id_token", "access_token", "refresh_token")
            )
            or _file_stamp(os.fstat(fd)) != _file_stamp(info)
        ):
            raise ValueError("Codex did not finish writing the ChatGPT login.")
        if expected_email:
            # This is the private grant produced by the device login. The running harness
            # independently reports its account and workspace before any turn is submitted.
            try:
                payload = tokens["id_token"].split(".")[1]
                claims = json.loads(base64.urlsafe_b64decode(payload + "=" * (-len(payload) % 4)))
                workspace = claims["https://api.openai.com/auth"]
                if (not matches_personal(expected_email, claims.get("email"), workspace.get("chatgpt_plan_type"))
                        or not workspace.get("chatgpt_account_id")
                        or workspace["chatgpt_account_id"] != tokens.get("account_id")):
                    raise ValueError()
            except (ValueError, IndexError, AttributeError, KeyError, TypeError):
                raise ValueError("Choose the Personal workspace for the configured ChatGPT email.") from None
        if sync:
            os.fsync(fd)
        return _file_stamp(info)
    finally:
        os.close(fd)


def grant_stamp(home):
    """Identify a durable private ChatGPT login without exposing its contents."""
    try:
        with _state_directory(home) as (directory, parent):
            stamp = _grant(directory, sync=True)
            os.fsync(directory)
            os.fsync(parent)
            return stamp
    except ValueError:
        return None
    except OSError as err:
        if err.errno in (errno.ENOENT, errno.ELOOP, errno.ENOTDIR, errno.ENXIO, errno.ENODEV):
            return None
        raise


@contextmanager
def staged_login(home, *, resume=None):
    """Keep the previous login until this account's new grant has been made durable."""
    from .enroll import staging_directory

    home = Path(home)
    with staging_directory(home, resume) as staging:
        if resume is None:
            (staging / ".codex").mkdir(mode=0o700)
        expected = None

        def promote():
            nonlocal expected
            with _state_directory(staging) as (src, _), _state_directory(home, create=True) as (dst, parent):
                try:
                    staged = _grant(src, sync=True, expected_email=personal_email(home.name))
                except FileNotFoundError:
                    # A previous attempt may have renamed the file before a directory sync or receipt write failed.
                    if expected is None:
                        raise
                else:
                    if expected is not None and expected != staged:
                        raise ValueError("The staged Codex login changed during promotion.")
                    expected = staged
                    os.replace("auth.json", "auth.json", src_dir_fd=src, dst_dir_fd=dst)
                if _grant(dst, sync=True) != expected:
                    raise ValueError("The installed Codex login differs from the approved login.")
                os.fsync(dst)
                os.fsync(parent)

        yield staging, promote
