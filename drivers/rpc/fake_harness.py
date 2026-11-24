"""Offline functional-test peer. Refuses a wrong prompt, identity, route or session."""

import json
import os
import sys
import time
from pathlib import Path

mode = Path(sys.argv[0]).name
home = Path(os.environ["HOME"])
assert home.name in {"sample", "second"} and home.parent.name == mode
assert not {"META_API_KEY", "OPENAI_API_KEY", "GEMINI_API_KEY", "XAI_API_KEY", "SSH_AUTH_SOCK",
            "OPENAI_BASE_URL", "NEXUS_SIGNIN_SOCKET", "NEXUS_TEST_SECRET"}.intersection(os.environ)
assert os.environ["XDG_CONFIG_HOME"] == str(home / ".config")
if mode == "codex":
    assert os.environ["CODEX_HOME"] == str(home / ".codex")
elif mode == "grok":
    assert os.environ["GROK_HOME"] == str(home / ".grok") and os.environ["GROK_AGENT"] == "nexus-conversation"
elif mode == "muse":
    assert os.environ["TBH_CREDENTIAL_BACKEND"] == "file"
elif mode == "antigravity":
    assert os.environ["TMPDIR"] == str(home / "tmp")
session = "provider-session" if home.name == "sample" else "provider-second-session"
pending = []
session_open = False
bad = (home / "signed-out").exists()


def check_context(instructions):
    assert instructions.startswith("Standing instructions from a synthetic home.\n\n")
    assert "If they choose Nova Seed" in instructions
    assert "https://github.com/mas-bandwidth/nova and its docs/SEED-CORE.md" in instructions


def event(method, **params):
    print(json.dumps({"method": method, "params": params}), flush=True)


for line in sys.stdin:
    msg = json.loads(line)
    m = msg["method"]
    p = msg.get("params", {})
    if "id" not in msg:
        continue
    result = {}
    if m == "initialize":
        result = {
            "authMethods": [{"id": "cached_token"}] if not bad else [],
            "agentCapabilities": {
                "promptCapabilities": {"embeddedContext": True},
                "_meta": {"antigravity": {"systemPrompt": {"append": True}}},
            },
        }
    elif m == "model/list":
        result = {"models": [{"modelId": "one", "variants": ["high"], "isDefault": True}]} if mode == "muse" else {
            "data": [{"id": "one", "supportedReasoningEfforts": [{"reasoningEffort": "high"}], "isDefault": True}]}
    elif m == "account/read":
        if (home / "temporary-auth-error").exists():
            print(json.dumps({"id": msg["id"], "error": {"code": -32000, "message": "auth server timed out"}}), flush=True)
            continue
        result = {"account": {"type": "apiKey" if bad else "chatgpt", "planType": "plus"}}
        if mode == "muse":
            result = {"state": "loggedOut" if bad else "accountLogin", "credentialRequired": True}
    elif m == "_x.ai/billing":
        result = {"subscription_tier": "free" if bad else "SuperGrok"} if session_open else {"config": {}}
    elif m == "usage/refresh":
        result = {"subscriptionActive": not bad, "tierName": "test-subscription"}
    elif m in {
        "thread/start",
        "thread/resume",
        "session/new",
        "session/start",
        "session/load",
        "session/resume",
    }:
        resume = p.get("threadId") or p.get("sessionId")
        if resume == "missing":
            print(json.dumps({"id": msg["id"], "error": {"message": "session not found"}}), flush=True)
            continue
        assert resume in (None, session)
        session_open = True
        if mode == "codex":
            assert p["approvalPolicy"] == "never" and p["permissions"] == "nexus-chat"
            assert "sandbox" not in p
            check_context(p["developerInstructions"])
            assert p["modelProvider"] == "openai"
        if mode in {"antigravity", "grok"}:
            assert Path.cwd() == (home / "room").resolve() and Path(p["cwd"]) == home / "room"
        if mode == "muse":
            assert Path.cwd() == (home / "room").resolve()
            if m == "session/start":
                assert Path(p["workspaceRoot"]) == home / "room"
        if mode == "antigravity":
            check_context(p["_meta"]["antigravity"]["systemPrompt"]["append"])
        result = {
            "approvalPolicy": "never",
            "activePermissionProfile": {"id": "other" if (home / "wrong-profile").exists() else "nexus-chat"},
            "cwd": p.get("cwd"),
            "runtimeWorkspaceRoots": p.get("runtimeWorkspaceRoots"),
            "sessionId": session,
            "thread": {"id": session},
            "session": {"sessionId": session},
            "viewCursor": "0",
            "_meta": {"antigravity": {"tier": {"name": "free" if bad else "Google AI Pro"}}},
        }
    elif m in {"session/prompt", "turn/start"}:
        parts = p.get("input", p.get("prompt"))
        if mode == "grok":
            check_context(parts[0]["resource"]["text"])
        if mode == "muse":
            check_context(parts[0]["text"])
        prompt = parts[-1]["text"]
        assert prompt in ("Example?", "hold")
        (home / "sent").touch()
        scenario = home / "prompt-errors.json"
        if scenario.exists():
            fault = json.loads(scenario.read_text())
            for notification in fault.get("events", []):
                event(notification["method"], **notification["params"])
            print(json.dumps({"id": msg["id"], **fault["response"]}), flush=True)
            continue
        if prompt == "hold":
            time.sleep(50)
        if m == "session/prompt":
            event(
                "session/update",
                sessionId=session,
                update={
                    "sessionUpdate": "agent_message_chunk",
                    "content": {"type": "text", "text": "Reply ✅"},
                },
            )
            result = {"stopReason": "end_turn"}
        elif mode == "codex":
            event(
                "item/completed",
                threadId=session,
                turnId="t",
                item={"type": "agentMessage", "text": "Reply ✅"},
            )
            pending = [("turn/completed", {"threadId": session, "turn": {"id": "t", "status": "completed"}})]
            result = {"turn": {"id": "t"}}
        else:
            event(
                "item/completed",
                sessionId=session,
                item={"turnId": "t", "kind": "agentMessage", "text": "Reply ✅"},
            )
            pending = [("turn/completed", {"sessionId": session, "turnId": "t", "terminal": "completed"})]
            result = {"turnId": "t"}
    print(json.dumps({"id": msg["id"], "result": result}), flush=True)
    for method, params in pending:
        event(method, **params)
    pending = []
