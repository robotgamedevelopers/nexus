import tempfile
import unittest
from pathlib import Path

from nexus_rpc.project_tools import COMMAND, permission, workspace_root


class ProjectTests(unittest.TestCase):
    def test_only_the_registered_session_and_tool_get_one_time_permission(self):
        args = {"ServerName": "nexus_project", "ToolName": COMMAND, "Arguments": {"command": "pwd"}}
        call = {"name": "call_mcp_tool", "args": args, "rawInput": args.copy()}
        params = {"sessionId": "one", "toolCall": call, "options": [
            {"kind": "allow_always", "optionId": "always"}, {"kind": "allow_once", "optionId": "once"}]}
        self.assertEqual(permission(params, "one", {COMMAND}), {"outcome": "selected", "optionId": "once"})
        for session, tools in (("two", {COMMAND}), (None, {COMMAND}), ("one", set())):
            self.assertEqual(permission(params, session, tools), {"outcome": "cancelled"})
        call["rawInput"] = {"ServerName": "different"}
        self.assertEqual(permission(params, "one", {COMMAND}), {"outcome": "cancelled"})

        call["name"] = "mcp_nexus_project_project_shell"
        self.assertEqual(permission(params, "one", {COMMAND})["optionId"], "once")
        self.assertEqual(permission(params, "two", {COMMAND}), {"outcome": "cancelled"})
        call["name"] = "mcp_other_project_shell"
        self.assertEqual(permission(params, "one", {COMMAND}), {"outcome": "cancelled"})

    def test_overlapping_keep_out_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve()
            self.assertEqual(workspace_root(str(root), []), root)
            for excluded in (root, root.parent, root / "private"):
                with self.assertRaises(ValueError):
                    workspace_root(str(root), [excluded])


class NativeBus(unittest.IsolatedAsyncioTestCase):
    async def test_conversation_mcp_lists_only_its_mail_tool_and_returns_the_receipt(self):
        import asyncio
        import json
        from nexus_rpc.project_tools import project_tools

        calls = []

        async def send(args):
            calls.append(args)
            return {"content": [{"type": "text", "text": "receipt"}]}

        with tempfile.TemporaryDirectory() as tmp:
            async with project_tools(Path(tmp), None, [], native_send=send) as (_, socket):
                reader, writer = await asyncio.open_unix_connection(str(socket))

                async def request(method, params):
                    writer.write(json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": params}).encode() + b"\n")
                    await writer.drain()
                    return json.loads(await asyncio.wait_for(reader.readline(), 3))

                try:
                    init = await request("initialize", {"protocolVersion": "2025-03-26"})
                    self.assertIn("tools", init["result"]["capabilities"])
                    tools = (await request("tools/list", {}))["result"]["tools"]
                    self.assertEqual([tool["name"] for tool in tools], ["nexus_bus_send"])
                    args = {"to": "emma", "subject": "Review", "body": "Ready?"}
                    answer = await request("tools/call", {"name": "nexus_bus_send", "arguments": args})
                    self.assertEqual(answer["result"]["content"][0]["text"], "receipt")
                    self.assertEqual(calls, [args])
                finally:
                    writer.close()
                    await writer.wait_closed()
            self.assertFalse(socket.exists())
