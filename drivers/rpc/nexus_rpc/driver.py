"""Bridge a single Nexus session to a provider's stdio RPC server."""

import argparse
import asyncio
import importlib
import json
import os
import re
import signal
import subprocess
import sys
from collections import deque
from pathlib import Path

from .account_runtime import enrolled, home_for, process_env, remove_receipt_if_current
from .backends import BACKENDS, Failure, turn_quota, usage_limit

LIMIT = 1 << 20
SESSION_ID = re.compile(r"(?!-)[!-~]{1,256}")
IDENTIFIER = re.compile(r"[A-Za-z0-9][A-Za-z0-9._:/-]{0,127}")


def emit(kind, **fields):
    print(json.dumps({"kind": kind, **fields}, ensure_ascii=False), flush=True)


def error(code, cause, *, account=None, **fields):
    fix = {
        "protocol": "Check the Nexus driver protocol.",
        "no-credential": f"Complete the sign-in prompt in Nexus. To open it for this account, run nexus-server config sign-in {account}.",
        "usage-limit": "Wait for the included usage to reset or select another signed-in account.",
        "session-not-found": "The next prompt uses a new harness session.",
    }
    emit(
        "error",
        code=code,
        cause=cause,
        fix=fix.get(code, "Check the harness installation and session settings."),
        **fields,
    )


class Rpc:
    def __init__(self, proc):
        self.proc = proc
        self.seq = 0
        self.interrupt = None
        self.events = deque()

    async def write(self, obj):
        self.proc.stdin.write(json.dumps({"jsonrpc": "2.0", **obj}).encode() + b"\n")
        await self.proc.stdin.drain()

    async def notify(self, method, params):
        await self.write({"method": method, "params": params})

    async def read(self):
        while True:
            line = await self.proc.stdout.readline()
            if not line:
                raise Failure("The harness closed its RPC stream.")
            msg = json.loads(line)
            if "id" in msg and "method" in msg:
                # No interactive tool approval or filesystem delegation on this transport.
                if msg["method"] == "session/request_permission":
                    reply = {"result": {"outcome": {"outcome": "cancelled"}}}
                else:
                    reply = {"error": {"code": -32601, "message": "Client capability unavailable"}}
                await self.write({"id": msg["id"], **reply})
                continue
            return msg

    async def event(self):
        if self.events:
            return self.events.popleft()
        msg = await self.read()
        return msg.get("method"), msg.get("params") or {}

    async def call(self, method, params, handle=None, timeout=45, *, quota_provider=None, optional=False):
        self.seq += 1
        await self.write({"id": self.seq, "method": method, "params": params})
        async with asyncio.timeout(timeout):
            while True:
                msg = await self.read()
                if "method" in msg:
                    event = (msg["method"], msg.get("params") or {})
                    if handle:
                        handle(*event)
                    elif len(self.events) < 4096:
                        self.events.append(event)
                    else:
                        raise Failure("The harness sent too many pending notifications.")
                    continue
                if msg.get("id") != self.seq:
                    raise Failure("The harness returned an unexpected response ID.")
                if "error" in msg:
                    if optional:
                        return None
                    err = msg["error"]
                    detail = str(err.get("message", "")).lower()
                    # Only retry a missing session before any user input was sent.
                    if method in {"session/load", "thread/resume", "session/resume"} and any(
                        s in detail
                        for s in ("not found", "unknown session", "no rollout found", "does not exist")
                    ):
                        code = "session-not-found"
                    elif (
                        method in {"authenticate", "account/read", "_x.ai/billing", "usage/refresh",
                                   "session/new", "session/load", "session/start", "session/resume"}
                        and err.get("code") in {-32000, -32001}
                        and detail.strip(". ") in {"auth required", "authentication required", "unauthorized",
                                                  "invalid token", "invalid_grant", "not authenticated",
                                                  "not logged in"}
                    ):
                        code = "no-credential"
                    elif quota_provider and turn_quota(quota_provider, err):
                        raise usage_limit()
                    else:
                        code = "harness"
                    raise Failure(f"The harness refused {method}.", code, revoked=code == "no-credential")
                return msg["result"]


class Turn:
    def __init__(self, args, prompt, number):
        self.__dict__.update(vars(args))
        self.args = args
        self.prompt = prompt
        self.number = number
        self.writing = False
        self.abandoned = False

    def credential(self, plan):
        emit("credential", billing="subscription", plan=plan)

    def session(self, sid):
        if not isinstance(sid, str) or not SESSION_ID.fullmatch(sid):
            raise Failure("The harness returned an invalid session ID.")
        self.args.resume = sid
        emit("session", id=sid)
        return sid

    def missing(self):
        self.args.resume = None
        error("session-not-found", "The harness could not find the saved conversation.", turn=self.number)

    def quota(self, minutes, percent):
        if (type(minutes) is int and 1 <= minutes <= 525600
                and type(percent) in (int, float) and 0 <= percent <= 2147483647):
            emit("quota", minutes=minutes, used_percent=round(percent))

    def child_agents(self, count):
        emit("child-agents", count=count)

    def delta(self, value):
        if not isinstance(value, str):
            raise Failure("The harness returned non-text output.")
        for pos in range(0, len(value), 32000):
            emit("text", turn=self.number, delta=value[pos : pos + 32000])
            self.writing = True

    def end(self):
        if self.writing:
            emit("text-end", turn=self.number)
            self.writing = False

    def block(self, value):
        self.delta(value)
        self.end()

    async def run(self):
        home = self.account_home
        env = process_env(self.harness, home)
        proc = None
        rpc = None
        outcome = "error"
        fatal = False
        try:
            try:
                receipt = enrolled(home_for(self.harness, self.account), self.harness)
            except FileNotFoundError:
                receipt = None
            if receipt is None:
                raise Failure("The named account needs a provider sign-in.", "no-credential")
            options = {}
            if self.harness == "codex":
                options = {"keep_out": self.keep_out, "limited": bool(self.limit)}
                if getattr(self, "bus_sender", None):
                    options["bus_sender"] = self.bus_sender
                if getattr(self, "write_dir", []):
                    options["write_dirs"] = self.write_dir
            try:
                command = await asyncio.to_thread(
                    self.provider.chat_command, self.binary, home, self.cwd, **options
                )
            except (OSError, ValueError, subprocess.SubprocessError) as err:
                raise Failure(
                    "The isolated harness could not start. Check its installed version and workspace path.",
                    "other",
                ) from err
            if self.harness in {"antigravity", "grok", "muse"}:
                self.cwd = str(home / "room")
            if self.harness == "grok":
                env["GROK_AGENT"] = "nexus-conversation"
            proc = await asyncio.create_subprocess_exec(
                *command,
                env=env,
                cwd=self.cwd,
                stdin=-1,
                stdout=-1,
                stderr=asyncio.subprocess.DEVNULL,
                start_new_session=True,
                umask=0o077,
                limit=4 * LIMIT,
            )
            rpc = Rpc(proc)
            await BACKENDS[self.harness](rpc, self)
            outcome = "ok"
        except asyncio.CancelledError:
            outcome = "stopped"
        except Failure as exc:
            self.end()
            code = exc.code
            cause = str(exc)
            if exc.revoked:
                try:
                    if not remove_receipt_if_current(home, self.harness, receipt):
                        code = "harness"
                        cause = "The account sign-in changed while this turn was starting. Retry the turn."
                except (OSError, ValueError):
                    code = "other"
                    cause = "The provider refused the login, and Nexus could not update its enrollment receipt."
            error(code, cause, account=self.account, turn=self.number)
            fatal = code == "no-credential"
        except (TimeoutError, OSError, ValueError, TypeError, KeyError, AttributeError):
            self.end()
            error(
                "harness",
                "The harness failed to start or returned an invalid or late RPC response.",
                turn=self.number,
            )
        finally:
            if proc is not None:
                # Allow the provider to persist a completed or interrupted turn before reaping it.
                try:
                    async with asyncio.timeout(1):
                        if outcome == "stopped" and rpc is not None and rpc.interrupt:
                            await rpc.write(rpc.interrupt)
                        proc.stdin.close()
                        await proc.communicate()
                except (OSError, TimeoutError):
                    try:
                        os.killpg(proc.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    await proc.communicate()
            if not self.abandoned:
                self.end()
                emit("done", turn=self.number, outcome=outcome)
        return fatal


def reject_constant(_):
    raise ValueError("Invalid JSON number.")


def turn_number(msg):
    n = msg.get("turn")
    if type(n) not in (int, float) or not 1 <= n < 2**53 or int(n) != n:
        raise ValueError("Invalid turn number.")
    return int(n)


async def serve(args):
    loop = asyncio.get_running_loop()
    reader = asyncio.StreamReader(limit=LIMIT + 1)
    transport, _ = await loop.connect_read_pipe(
        lambda: asyncio.StreamReaderProtocol(reader), sys.stdin.buffer
    )
    line = asyncio.create_task(reader.readline())
    active = None
    turn = None
    last = 0
    invalid_turn = {}
    try:
        while True:
            await asyncio.wait([line, active] if active else [line], return_when=asyncio.FIRST_COMPLETED)
            if active and active.done():
                if active.result():
                    return 1
                active = None
            if not line.done():
                continue
            invalid_turn = {}
            raw = line.result()
            if not raw:
                return 0
            if not raw.endswith(b"\n") or len(raw) - 1 > LIMIT:
                raise ValueError("Invalid protocol line length.")
            msg = json.loads(raw.decode("utf-8"), parse_constant=reject_constant)
            if isinstance(msg, dict):
                try:
                    invalid_turn = {"turn": turn_number(msg)}
                except ValueError:
                    pass
            if not isinstance(msg, dict) or not isinstance(msg.get("kind"), str) or not msg["kind"]:
                raise ValueError("A protocol object must have a kind.")
            json.dumps(msg, ensure_ascii=False).encode()
            kind = msg["kind"]
            if kind == "send":
                n = turn_number(msg)
                if active or n <= last or not isinstance(msg.get("prompt"), str):
                    raise ValueError("Unexpected send or invalid prompt.")
                last = n
                turn = Turn(args, msg["prompt"], n)
                active = asyncio.create_task(turn.run())
            elif kind == "stop":
                if turn_number(msg) == last and active:
                    active.cancel()
            elif kind == "settings":
                updates = {k: msg[k] for k in ("model", "effort") if msg.get(k) is not None}
                if not updates or not all(isinstance(v, str) and v for v in updates.values()):
                    raise ValueError("Invalid driver settings.")
                if all(IDENTIFIER.fullmatch(v) for v in updates.values()):
                    vars(args).update(updates)
                else:
                    error("other", "The setting is not a harness identifier.")
            elif kind == "server" and not (
                isinstance(msg.get("features"), list) and all(isinstance(f, str) for f in msg["features"])
            ):
                raise ValueError("Invalid server features.")
            line = asyncio.create_task(reader.readline())
    except (ValueError, RecursionError):
        error("protocol", "The server sent an invalid driver message.", **invalid_turn)
        return 1
    finally:
        line.cancel()
        if active and not active.done():
            turn.abandoned = True
            active.cancel()
            await asyncio.gather(active, return_exceptions=True)
        transport.close()


def main():
    harness = sys.argv[1] if len(sys.argv) > 1 and sys.argv[1] in BACKENDS else "unknown"
    provider = importlib.import_module(f".{harness}_account", __package__) if harness in BACKENDS else None
    supports_limit = getattr(provider, "SUPPORTS_LIMIT", False)
    emit("hello", protocol=1, harness=harness, features=["limit"] if supports_limit else [])
    parser = argparse.ArgumentParser(exit_on_error=False, allow_abbrev=False)
    parser.add_argument("harness", choices=BACKENDS)
    parser.add_argument("--binary", required=True)
    parser.add_argument("--account", required=True)
    parser.add_argument("--cwd", required=True)
    parser.add_argument("--limit", choices=["cwd"])
    parser.add_argument("--write-dir", action="append", default=[])
    parser.add_argument("--keep-out", action="append", default=[])
    parser.add_argument("--bus-sender")
    parser.add_argument("--resume")
    parser.add_argument("--model")
    parser.add_argument("--effort")
    context = parser.add_mutually_exclusive_group(required=True)
    context.add_argument("--startup-context-fd", type=int)
    context.add_argument("--primary-context-fd", type=int)
    try:
        args = parser.parse_args()
        args.provider = provider
        if args.limit and not supports_limit:
            raise Failure("This harness cannot enforce the requested workspace limit.", "other")
        if args.write_dir and args.harness != "codex":
            raise Failure("This conversation-only harness has no workspace file tools for additional directories.", "other")
        if args.keep_out and not args.limit:
            raise ValueError("Additional kept-out directories require a workspace limit.")
        for excluded in args.keep_out:
            if not Path(excluded).is_absolute() or Path(excluded).resolve() == Path("/"):
                raise ValueError("Invalid kept-out directory.")
        if not Path(args.binary).is_absolute() or not Path(args.cwd).is_absolute():
            raise ValueError("Supply absolute harness and workspace paths.")
        if any(v is not None and not IDENTIFIER.fullmatch(v) for v in (args.model, args.effort)):
            raise ValueError("Invalid driver settings.")
        if args.resume and not SESSION_ID.fullmatch(args.resume):
            raise ValueError("Invalid resume ID.")
        fd = args.primary_context_fd if args.primary_context_fd is not None else args.startup_context_fd
        if fd < 3:
            raise ValueError("Startup context must use a private file descriptor.")
        with os.fdopen(fd, "rb") as f:
            data = f.read(262145)
        if not data or len(data) > 262144:
            raise ValueError("Invalid AI context.")
        args.instructions = data.decode()
        if args.bus_sender:
            from .bus_tool import validate
            validate(args.bus_sender)
            args.instructions += "\nUse nexus_bus_send to send or reply as yourself. Keep routine sends and their receipts out of the conversation unless the person asks."
        if args.primary_context_fd is None:
            setup_guide = Path(__file__).with_name("ai-setup.txt").read_text(encoding="utf-8").strip()
            args.instructions += "\n\n" + setup_guide
        try:
            args.account_home = home_for(args.harness, args.account)
            ready = enrolled(args.account_home, args.harness)
        except FileNotFoundError:
            ready = None
        if not ready:
            raise Failure("The named account needs a provider sign-in.", "no-credential")
        return asyncio.run(serve(args))
    except Failure as exc:
        error(exc.code, str(exc), account=args.account)
    except (ValueError, OSError, argparse.ArgumentError, SystemExit):
        error("other", "Invalid startup arguments, account directory, scope or AI instructions.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
