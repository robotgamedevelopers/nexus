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
