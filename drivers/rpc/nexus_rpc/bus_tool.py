"""Host-side MCP tool for the registry-selected AI. The provider receives only the receipt."""
import json
from pathlib import Path
import re
import subprocess
import sys

NAME = r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}"
TOOL = "nexus_bus_send"
SCHEMA = {"type": "object", "properties": {
    "to": {"type": "string", "pattern": "^" + NAME + "$"},
    "subject": {"type": "string", "minLength": 1, "maxLength": 4096},
    "body": {"type": "string", "maxLength": 1048576},
    "reply_to": {"type": "string", "pattern": "^[0-9]+-[0-9]+$"}},
    "required": ["to", "subject", "body"], "additionalProperties": False}


def validate(grant):
    ai, _, endpoint = grant.partition("@")
    if not re.fullmatch(NAME, ai) or not re.fullmatch(r"127\.0\.0\.1:[1-9][0-9]{0,4}", endpoint) or int(endpoint.split(":")[1]) > 65535:
        raise ValueError("Invalid bus grant")
    return ai, endpoint


def send(grant, args):
    ai, endpoint = validate(grant)
    if not isinstance(args, dict) or set(args) - SCHEMA["properties"].keys():
        raise ValueError("Invalid arguments")
    to, subject, body = (args.get(k) for k in ("to", "subject", "body"))
    if not isinstance(to, str) or not re.fullmatch(NAME, to) or not isinstance(subject, str) or not 0 < len(subject) <= 4096 or not isinstance(body, str) or len(body.encode()) > 1048576:
        raise ValueError("Invalid message")
    reply = args.get("reply_to")
    if reply is not None and (not isinstance(reply, str) or not re.fullmatch("[0-9]+-[0-9]+", reply)):
        raise ValueError("Invalid reply ID")
    home = Path.home()
    bin_dir = home / ".local/bin"
    result = subprocess.run([bin_dir / "nova-secrets", "exec", "--only", "NOVA_BUS_PASSWORD", "--",
        bin_dir / "nova-bus", "send", "--as", ai, "--to", to, *(["--re", reply] if reply else []), "--", subject, "-"],
        input=body + "\n", text=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, cwd=home, timeout=15,
        env={"HOME": str(home), "PATH": "/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin", "NOVA_BUS_USER": "bus", "NOVA_BUS_REDIS": endpoint},
        check=True)
    return result.stdout.strip()


def respond(grant, msg):
    method = msg.get("method")
    params = msg.get("params", {})
    if method == "initialize":
        return {"protocolVersion": params["protocolVersion"], "capabilities": {"tools": {}}, "serverInfo": {"name": "nexus", "version": "1"}}
    if method == "ping":
        return {}
    if method == "tools/list":
        return {"tools": [{"name": TOOL, "description": "Send a message or reply as yourself to an AI.", "inputSchema": SCHEMA}]}
    if method == "tools/call" and params.get("name") == TOOL:
        try:
            return {"content": [{"type": "text", "text": send(grant, params.get("arguments"))}]}
        except (ValueError, OSError, subprocess.SubprocessError):
            return {"isError": True, "content": [{"type": "text", "text": "Send returned no receipt. Check the recipient and bus connection."}]}
    raise ValueError("Unknown method")


if __name__ == "__main__":
    grant = sys.argv[1]
    validate(grant)
    for line in sys.stdin:
        msg = json.loads(line)
        if "id" not in msg:
            continue
        response = {"id": msg["id"], "jsonrpc": "2.0"}
        try:
            response["result"] = respond(grant, msg)
        except (KeyError, ValueError, TypeError):
            response["error"] = {"code": -32601, "message": "Unsupported request"}
        print(json.dumps(response), flush=True)
