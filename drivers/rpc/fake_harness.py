"""Offline functional-test peer. Refuses a wrong prompt, identity, route or session."""

import json
import os
import sys
import time
from pathlib import Path

mode = Path(sys.argv[0]).name
home = Path(os.environ["HOME"])
assert home.name == "sample" and home.parent.name == mode
assert "META_API_KEY" not in os.environ and "OPENAI_API_KEY" not in os.environ
context = "Standing instructions from a synthetic home."
session = "provider-session"
pending = []
bad = (home / "signed-out").exists()


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
    elif m == "account/read":
        result = {"account": {"type": "apiKey" if bad else "chatgpt", "planType": "plus"}}
    elif m == "_x.ai/billing":
        result = {"subscription_tier": "free" if bad else "SuperGrok"}
    elif m == "usage/read":
        result = {} if bad else {"usage": {"tier": "test-subscription"}}
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
        if mode == "codex":
            assert p["developerInstructions"] == context and p["modelProvider"] == "openai"
        if mode == "antigravity":
            assert p["_meta"]["antigravity"]["systemPrompt"]["append"] == context
        result = {
            "sessionId": session,
            "thread": {"id": session},
            "session": {"sessionId": session},
            "viewCursor": "0",
            "_meta": {"antigravity": {"tier": {} if bad else {"id": "pro"}}},
        }
    elif m in {"session/prompt", "turn/start"}:
        parts = p.get("input", p.get("prompt"))
        if mode == "grok":
            assert parts[0]["resource"]["text"] == context
        if mode == "muse":
            assert context in parts[0]["text"]
        prompt = parts[-1]["text"]
        assert prompt in ("Example?", "hold")
        (home / "sent").touch()
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
