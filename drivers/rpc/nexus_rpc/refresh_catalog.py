"""Refresh a local harness catalog without opening a conversation or submitting input."""

import argparse
import asyncio
import json
import importlib
import logging
import subprocess
import os
import tempfile
from pathlib import Path

from .account_runtime import process_env
from .catalog import acp_catalog, discover, save
from .driver import Rpc


async def account_catalog(provider, binary, home):
    module = importlib.import_module(f".{provider}_account", __package__)
    room = home / "room"
    command = await asyncio.to_thread(module.chat_command, binary, home, str(room))
    env = process_env(provider, home)
    if provider == "grok":
        env["GROK_AGENT"] = "nexus-conversation"
    proc = await asyncio.create_subprocess_exec(*command, cwd=room, env=env,
        stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.DEVNULL,
        limit=2**20)
    try:
        async with asyncio.timeout(10):
            rpc = Rpc(proc)
            if provider in ("grok", "antigravity"):
                init = await rpc.call("initialize", {"protocolVersion": 1, "clientCapabilities": {}})
                if provider == "grok" and any(a.get("id") == "cached_token" for a in init.get("authMethods", [])):
                    await rpc.call("authenticate", {"methodId":"cached_token", "_meta":{"headless":True}})
                result = await rpc.call("session/new", {"cwd":str(room), "mcpServers":[]})
                acp_catalog(provider, result)
            else:
                await rpc.call("initialize", {"clientInfo":{"name":"nexus","version":"1"}, "capabilities":{"experimentalApi":True}})
                await rpc.notify("initialized", {})
                await discover(rpc, provider)
    finally:
        if proc.returncode is None:
            proc.kill()
        await proc.wait()


def after_login(provider, binary, home):
    if provider == "codex":
        # Codex's workspace-scoped launch requires the actual friend workspace.
        return
    from .backends import Failure
    try:
        asyncio.run(account_catalog(provider, binary, home))
    except (Failure, OSError, TimeoutError, ValueError, TypeError, KeyError, subprocess.SubprocessError):
        logging.warning("%s discovery unavailable after sign-in; the next turn retries", provider)


async def refresh(provider, binary):
    with tempfile.TemporaryDirectory(prefix="nexus-discovery-") as home:
        env = {"HOME": home, "PATH": os.defpath, "LANG": "en_US.UTF-8", "CODEX_HOME": home,
               "CLAUDE_CONFIG_DIR": home, "XDG_CONFIG_HOME": home}
        commands = {
            "claude": ["--print", "--verbose", "--input-format", "stream-json", "--output-format", "stream-json"],
            "codex": ["app-server"],
            "muse": ["serve", "--provider", "meta"],
        }
        proc = await asyncio.create_subprocess_exec(binary, *commands[provider], cwd=home, env=env,
            stdin=asyncio.subprocess.PIPE, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.DEVNULL,
            limit=2**20)
        try:
            async with asyncio.timeout(25):
                if provider == "claude":
                    req = {"type": "control_request", "request_id": "catalog", "request": {"subtype": "initialize"}}
                    proc.stdin.write(json.dumps(req).encode() + b"\n")
                    await proc.stdin.drain()
                    while line := await proc.stdout.readline():
                        response = json.loads(line).get("response", {})
                        if response.get("request_id") == "catalog":
                            rows = response.get("response", {}).get("models", [])
                            if not rows:
                                raise ValueError("Claude did not advertise its models")
                            save(provider, [(r["value"], r.get("supportedEffortLevels"), r["value"] == "default") for r in rows])
                            return
                    raise ValueError("Claude closed the discovery stream")
                rpc = Rpc(proc)
                await rpc.call("initialize", {"clientInfo": {"name": "nexus", "version": "1"},
                                              "capabilities": {"experimentalApi": True}})
                await rpc.notify("initialized", {})
                await discover(rpc, provider)
        finally:
            if proc.returncode is None:
                proc.kill()
            await proc.wait()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("provider", choices=("codex", "claude", "muse"))
    parser.add_argument("binary", type=Path)
    args = parser.parse_args()
    asyncio.run(refresh(args.provider, args.binary.resolve(strict=True)))
