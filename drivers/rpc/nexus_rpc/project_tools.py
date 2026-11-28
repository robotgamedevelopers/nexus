"""Per-turn MCP connection to commands run inside the configured project wall."""
import asyncio
from contextlib import asynccontextmanager
import json
import os
from pathlib import Path
import signal
import tempfile

from . import bus_tool
from .antigravity_account import _AGENT

COMMAND = "project_shell"
TOOL = {"name": COMMAND, "description": "Run a shell command in the assigned project. Read or edit files, use git, build and test here.",
        "inputSchema": {"type": "object", "properties": {"command": {"type": "string"}},
                        "required": ["command"], "additionalProperties": False}}


def permission(params, session, tools):
    call = params.get("toolCall", {})
    args = call.get("args", {})
    name = call.get("name")
    # ACP uses a dispatcher for a fresh session and can name the tool directly on resume.
    registered = name in {f"mcp_nexus_project_{tool}" for tool in tools}
    if name == "call_mcp_tool":
        registered = (args.get("ServerName") == "nexus_project" and args.get("ToolName") in tools
                      and call.get("rawInput") == args)
    if session and params.get("sessionId") == session and registered:
        for option in params.get("options", []):
            if option.get("kind") == "allow_once" and isinstance(option.get("optionId"), str):
                return {"outcome": "selected", "optionId": option["optionId"]}
    return {"outcome": "cancelled"}


def workspace_root(cwd, keep_out):
    root = Path(cwd).resolve(strict=True)
    home = Path.home()
    protected = [home / p for p in (".nexus", ".ssh", ".config", ".codex", ".claude", ".gemini", "Library")]
    if not Path(cwd).is_absolute() or not root.is_dir() or home.is_relative_to(root):
        raise ValueError("Choose a dedicated project directory")
    for path in [*protected, *map(Path, keep_out)]:
        path = path.resolve()
        if root.is_relative_to(path) or path.is_relative_to(root):
            raise ValueError("Workspace overlaps a protected directory")
    return root


async def shell(wall, root, scratch, command):
    if not isinstance(command, str) or not command.strip() or len(command.encode()) > 131072:
        raise ValueError("Supply a nonempty project command")
    env = {"PATH": "/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin",
           "HOME": str(scratch), "TMPDIR": str(scratch), "LANG": "en_US.UTF-8"}
    proc = await asyncio.create_subprocess_exec(
        str(wall), "--write", str(root), "--write", str(scratch), "--env", "TMPDIR", "--",
        "/bin/sh", "-c", command, cwd=root, env=env, start_new_session=True,
        stdin=asyncio.subprocess.DEVNULL, stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT)
    output = bytearray()
    truncated = False
    try:
        async with asyncio.timeout(600):
            while chunk := await proc.stdout.read(32768):
                available = 131072 - len(output)
                output.extend(chunk[:available])
                truncated |= len(chunk) > available
            await proc.wait()
    finally:
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        await proc.wait()
    text = bytes(output).decode(errors="replace")
    if truncated:
        text += "\n[output truncated]"
    return {"content": [{"type": "text", "text": f"Exit {proc.returncode}\n{text}"}], "isError": proc.returncode != 0}


@asynccontextmanager
async def project_tools(home, cwd, keep_out, sender=None):
    root = workspace_root(cwd, keep_out)
    wall = Path.home() / ".local/bin/nova-sandbox"
    if not os.access(wall, os.X_OK):
        raise ValueError("Project work requires nova-sandbox on Studio")
    # The account room is read-only to the vendor. Only its trusted driver writes the MCP configuration.
    room_base = home / "room"
    room_base.mkdir(mode=0o700, exist_ok=True)
    with tempfile.TemporaryDirectory(dir="/tmp", prefix="nxm-") as ipc, tempfile.TemporaryDirectory(
        dir=room_base, prefix="project-") as room_name, tempfile.TemporaryDirectory(prefix="nx-work-") as scratch:
        room = Path(room_name)
        socket = Path(ipc).resolve() / "mcp"
        clients = set()

        async def serve(reader, writer):
            task = asyncio.current_task()
            clients.add(task)
            try:
                while line := await reader.readline():
                    msg = json.loads(line)
                    if "id" not in msg:
                        continue
                    reply = {"jsonrpc": "2.0", "id": msg["id"]}
                    try:
                        method, params = msg.get("method"), msg.get("params", {})
                        if method == "initialize":
                            result = {"protocolVersion": params["protocolVersion"], "capabilities": {"tools": {}},
                                      "serverInfo": {"name": "nexus_project", "version": "1"}}
                        elif method == "ping":
                            result = {}
                        elif method == "tools/list":
                            result = {"tools": [TOOL]}
                            if sender:
                                result["tools"] += bus_tool.respond(sender, msg)["tools"]
                        elif method == "tools/call" and params.get("name") == COMMAND:
                            result = await shell(wall, root, scratch, params["arguments"]["command"])
                        elif method == "tools/call" and sender and params.get("name") == bus_tool.TOOL:
                            result = await asyncio.to_thread(bus_tool.respond, sender, msg)
                        else:
                            reply["error"] = {"code": -32601, "message": "Unknown project tool or method"}
                        if "error" not in reply:
                            reply["result"] = result
                    except (KeyError, TypeError, OSError, ValueError, TimeoutError):
                        reply["error"] = {"code": -32603, "message": "Project tool failed or timed out"}
                    writer.write(json.dumps(reply).encode() + b"\n")
                    await writer.drain()
            except (ConnectionError, ValueError):
                pass
            finally:
                clients.discard(task)
                writer.close()

        server = await asyncio.start_unix_server(serve, str(socket), limit=2**20)
        async with server:
            config = room / ".agents"
            config.mkdir(mode=0o700)
            agent = config / "agents/nexus-project"
            agent.mkdir(parents=True)
            (agent / "agent.md").write_text(_AGENT.replace("nexus-dialogue", "nexus-project")
                                           .replace("inheritMcp: false", "inheritMcp: true"))
            spec = {"command": "/usr/bin/nc", "args": ["-U", str(socket)]}
            (config / "mcp_config.json").write_text(json.dumps({"mcpServers": {"nexus_project": spec}}))
            try:
                yield room, socket
            finally:
                server.close()
                for client in list(clients):
                    client.cancel()
                await asyncio.gather(*clients, return_exceptions=True)
