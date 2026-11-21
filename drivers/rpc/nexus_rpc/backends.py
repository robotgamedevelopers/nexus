"""Vendor session protocols. Each invocation owns one provider process and one Nexus turn."""

import secrets
import time
import uuid
from pathlib import Path


class Failure(Exception):
    def __init__(self, message, code="harness", *, revoked=False):
        super().__init__(message)
        self.code = code
        self.revoked = revoked


# Keep unknown or newly introduced plans out of the prompt path until their billing is known.
PAID_PLANS = {
    "codex": {"go", "plus", "pro", "prolite", "team", "selfservebusinessprolite", "business",
              "enterprise", "edu", "eduplus", "edupro"},
    "antigravity": {"googleaipro", "googleaiultra"},
    "grok": {"supergrok", "supergrokheavy", "supergrokpro", "premium", "xpremium+"},
}


def subscription(provider, *candidates):
    plans = {
        "".join(c for c in plan.lower() if c not in " _-"): plan
        for plan in candidates if isinstance(plan, str)
    }
    disallowed = {"free", "freetier", "googleaifree", "none", "api", "apikey",
                  "selfservebusinessusagebased", "enterprisecbpusagebased"}
    if disallowed.intersection(plans):
        raise Failure("This account does not report subscription billing for chat.", "no-credential", revoked=True)
    for name, plan in plans.items():
        if name in PAID_PLANS[provider]:
            return plan
    raise Failure("The provider did not identify a supported paid subscription. Check the account's plan.")


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
    await rpc.call("initialize", {
        "clientInfo": {"name": "nexus", "version": "1"}, "capabilities": {"experimentalApi": True},
    })
    await rpc.notify("initialized", {})
    auth = (await rpc.call("account/read", {"refreshToken": False}))["account"]
    if auth is None or auth.get("type") == "apiKey":
        raise Failure("Codex requires a ChatGPT subscription login.", "no-credential", revoked=True)
    if auth.get("type") != "chatgpt":
        raise Failure("Codex did not confirm a ChatGPT account.")
    turn.credential(subscription("codex", auth.get("planType")))
    cwd = str(Path(turn.cwd).resolve(strict=True))
    options = {
        "cwd": cwd,
        "runtimeWorkspaceRoots": [cwd],
        "developerInstructions": turn.instructions,
        "modelProvider": "openai",
        "approvalPolicy": "never",
        "permissions": turn.provider.PROFILE,
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
    if (
        (result.get("activePermissionProfile") or {}).get("id") != turn.provider.PROFILE
        or result.get("approvalPolicy") != "never"
        or result.get("cwd") != options["cwd"]
        or result.get("runtimeWorkspaceRoots") != options["runtimeWorkspaceRoots"]
    ):
        raise Failure("Codex did not confirm the named permission profile and workspace.", "other")
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
            raise Failure("Grok did not confirm use of its cached Grok account login.")
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
    if turn.harness == "antigravity":
        tier = result.get("_meta", {}).get("antigravity", {}).get("tier", {})
        turn.credential(subscription("antigravity", tier.get("name"), tier.get("id")))
    else:
        # Grok loads this account's subscription during session/new or session/load.
        billing = await rpc.call("_x.ai/billing", {})
        turn.credential(subscription("grok", billing.get("subscription_tier")))
    sid = turn.session(result.get("sessionId") or turn.resume)
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
    await rpc.call("initialize", {
        "clientInfo": {"name": "nexus", "version": "1"}, "capabilities": {"experimentalApi": True},
    })
    await rpc.notify("initialized", {})
    turn.credential(await turn.provider.subscription_tier(rpc))
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
