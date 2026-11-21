"""Vendor session protocols. Each invocation owns one provider process and one Nexus turn."""

import secrets
import time
import uuid


class Failure(Exception):
    def __init__(self, message, code="harness"):
        super().__init__(message)
        self.code = code


def subscription(plan):
    if (
        not isinstance(plan, str)
        or not plan.strip()
        or "free" in plan.lower()
        or plan.lower() in {"unknown", "none", "api", "api-key"}
        or "usage_based" in plan.lower()
    ):
        raise Failure("The harness did not confirm a subscription.", "no-credential")
    return plan


def uuid7():
    # MSP command IDs are millisecond-ordered UUIDv7, including on Python 3.11.
    return str(
        uuid.UUID(
            int=(int(time.time() * 1000) << 80)
            | (7 << 76)
            | (secrets.randbits(12) << 64)
            | (2 << 62)
            | secrets.randbits(62)
        )
    )


def text(value):
    return {"type": "text", "text": value}


async def session(call, fresh, resume, session_id, notify):
    if session_id:
        try:
            return await call(*resume)
        except Failure as error:
            if error.code != "session-not-found":
                raise
            notify()
    return await call(*fresh)


async def codex(rpc, turn):
    await rpc.call("initialize", {"clientInfo": {"name": "nexus", "version": "1"}})
    await rpc.notify("initialized", {})
    auth = (await rpc.call("account/read", {"refreshToken": False})).get("account") or {}
    if auth.get("type") != "chatgpt":
        raise Failure("Codex requires a ChatGPT subscription login.", "no-credential")
    turn.credential(subscription(auth.get("planType")))
    options = {
        "cwd": turn.cwd,
        "developerInstructions": turn.instructions,
        "modelProvider": "openai",
        "approvalPolicy": "on-request",
        "sandbox": "workspace-write",
    }
    if turn.model:
        options["model"] = turn.model
    result = await session(
        rpc.call,
        ("thread/start", options),
        ("thread/resume", {**options, "threadId": turn.resume}),
        turn.resume,
        turn.missing,
    )
    sid = turn.session(result["thread"]["id"])
    opts = {"threadId": sid, "input": [text(turn.prompt)]}
    if turn.effort:
        opts["effort"] = turn.effort
    result = await rpc.call("turn/start", opts)
    tid = result["turn"]["id"]
    rpc.interrupt = {"id": -1, "method": "turn/interrupt", "params": {"threadId": sid, "turnId": tid}}
    while True:
        method, params = await rpc.event()
        if params.get("threadId") != sid:
            continue
        if method == "item/completed" and params.get("turnId") == tid:
            item = params["item"]
            if item.get("type") == "agentMessage":
                turn.block(item["text"])
        if method == "turn/completed" and params["turn"]["id"] == tid:
            if params["turn"]["status"] != "completed":
                raise Failure("Codex did not complete the turn.")
            return


async def acp(rpc, turn):
    init = await rpc.call("initialize", {"protocolVersion": 1, "clientCapabilities": {}})
    options = {"cwd": turn.cwd, "mcpServers": []}
    caps = init["agentCapabilities"]
    if turn.harness == "antigravity":
        if not caps.get("_meta", {}).get("antigravity", {}).get("systemPrompt", {}).get("append"):
            raise Failure("This Antigravity version cannot append friend instructions.")
        options["_meta"] = {"antigravity": {"systemPrompt": {"append": turn.instructions}}}
    else:
        methods = {m["id"] for m in init["authMethods"]}
        if "cached_token" in methods:
            await rpc.call("authenticate", {"methodId": "cached_token", "_meta": {"headless": True}})
        elif init.get("_meta", {}).get("defaultAuthMethodId") not in (None, "grok.com"):
            raise Failure("Grok requires its cached Grok account login.", "no-credential")
        # This extension only accepts grok.com auth and reads current billing without a turn.
        billing = await rpc.call("_x.ai/billing", {})
        turn.credential(subscription(billing.get("subscription_tier")))
        if not caps.get("promptCapabilities", {}).get("embeddedContext"):
            raise Failure("This Grok version cannot accept friend context.")
    result = await session(
        rpc.call,
        ("session/new", options),
        ("session/load", {**options, "sessionId": turn.resume}),
        turn.resume,
        turn.missing,
    )
    # ACP load can replay old transcript updates before the response.
    rpc.events.clear()
    sid = turn.session(result.get("sessionId") or turn.resume)
    if turn.harness == "antigravity":
        tier = result.get("_meta", {}).get("antigravity", {}).get("tier", {})
        turn.credential(subscription(tier.get("name") or tier.get("id")))
    if turn.model:
        choice = next(
            (
                c
                for c in result.get("configOptions", [])
                if c.get("category") == "model" or c.get("id") == "model"
            ),
            None,
        )
        if choice:
            current = choice.get("currentValue")
            offered = {opt.get("value") for opt in choice.get("options", [])}
            uses_family_default = (
                turn.model.isalpha()
                and turn.model not in offered
                and isinstance(current, str)
                and current.startswith(turn.model + "-")
            )
            if current != turn.model and not uses_family_default:
                await rpc.call(
                    "session/set_config_option",
                    {"sessionId": sid, "configId": choice["id"], "value": turn.model},
                )
        else:
            await rpc.call("session/set_model", {"sessionId": sid, "modelId": turn.model})
    if turn.effort:
        config = next(
            (
                c
                for c in result.get("configOptions", [])
                if c.get("category") == "thought_level" or c.get("id") in {"reasoning_effort", "effort"}
            ),
            None,
        )
        if not config:
            raise Failure("This ACP session does not expose reasoning effort.")
        await rpc.call(
            "session/set_config_option", {"sessionId": sid, "configId": config["id"], "value": turn.effort}
        )
    parts = [text(turn.prompt)]
    if turn.harness == "grok":
        parts.insert(
            0,
            {
                "type": "resource",
                "resource": {
                    "uri": "nexus://friend/startup",
                    "mimeType": "text/plain",
                    "text": turn.instructions,
                },
            },
        )

    def update(method, params):
        u = params.get("update", {})
        if (
            method == "session/update"
            and params.get("sessionId") == sid
            and u.get("sessionUpdate") == "agent_message_chunk"
        ) and u["content"].get("type") == "text":
            turn.delta(u["content"]["text"])

    rpc.interrupt = {"method": "session/cancel", "params": {"sessionId": sid}}
    result = await rpc.call("session/prompt", {"sessionId": sid, "prompt": parts}, update, timeout=None)
    if result.get("stopReason") != "end_turn":
        raise Failure("The ACP harness ended the turn before completion.")


async def muse(rpc, turn):
    await rpc.call("initialize", {"clientInfo": {"name": "nexus", "version": "1"}})
    await rpc.notify("initialized", {})
    opts = {"commandId": uuid7(), "workspaceRoot": turn.cwd, "approvalMode": "denyUnmatched"}
    if turn.model:
        opts["modelId"] = turn.model
    result = await session(
        rpc.call,
        ("session/start", opts),
        ("session/resume", {"commandId": uuid7(), "sessionId": turn.resume, "excludeItems": True}),
        turn.resume,
        turn.missing,
    )
    sid = turn.session(result["session"]["sessionId"])
    usage = (await rpc.call("usage/read", {})).get("usage") or {}
    turn.credential(subscription(usage.get("tier")))
    if turn.model and result["session"].get("modelId") != turn.model:
        await rpc.call("session/setModel", {"commandId": uuid7(), "sessionId": sid, "model": turn.model})
    await rpc.call("view/subscribe", {"sessionId": sid, "after": result["viewCursor"]})
    opts = {
        "sessionId": sid,
        "commandId": uuid7(),
        "input": [text(turn.instructions + "\n\n"), text(turn.prompt)],
        "displayText": turn.prompt,
    }
    if turn.effort:
        opts["reasoningEffort"] = turn.effort
    result = await rpc.call("turn/start", opts)
    tid = result["turnId"]
    rpc.interrupt = {
        "id": -1,
        "method": "turn/interrupt",
        "params": {"commandId": uuid7(), "sessionId": sid, "turnId": tid},
    }
    while True:
        method, params = await rpc.event()
        if params.get("sessionId") != sid:
            continue
        if method == "item/completed":
            item = params["item"]
            if item.get("turnId") == tid and item.get("kind") == "agentMessage":
                turn.block(item["text"])
        if method == "turn/completed" and params["turnId"] == tid:
            if params["terminal"] != "completed":
                raise Failure("Muse did not complete the turn.")
            return


BACKENDS = {"codex": codex, "antigravity": acp, "grok": acp, "muse": muse}
