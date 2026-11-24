"""Normalize provider discovery responses into the Nexus keyboard picker catalog."""

import json
import os
import re
import tempfile
from pathlib import Path

IDENTIFIER = re.compile(r"[A-Za-z0-9][A-Za-z0-9._:/-]{0,127}")


def save(provider, rows):
    models = {}
    default = ""
    for name, efforts, selected in rows:
        if not isinstance(name, str) or not IDENTIFIER.fullmatch(name):
            continue
        models[name] = list(dict.fromkeys(v for v in efforts if isinstance(v, str) and IDENTIFIER.fullmatch(v))) if isinstance(efforts, list) else []
        if selected:
            default = name
    if not models:
        return
    data = json.dumps({"default": default, "models": models}, separators=(",", ":"))
    if len(data) > 16384:
        return
    root = Path.home() / ".nexus/server/catalogs"
    root.mkdir(mode=0o700, parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(mode="w", dir=root, delete=False) as temp:
        temp.write(data)
    os.replace(temp.name, root / (provider + ".json"))


async def discover(rpc, provider):
    rows = []
    params = {}
    seen = set()
    while True:
        page = await rpc.call("model/list", params)
        if provider == "muse":
            rows = [(r["modelId"], r.get("variants"), r.get("isDefault")) for r in page["models"]]
            break
        rows.extend((r["id"], [e["reasoningEffort"] for e in r.get("supportedReasoningEfforts", [])], r.get("isDefault"))
                    for r in page["data"] if not r.get("hidden"))
        cursor = page.get("nextCursor")
        if not cursor:
            break
        if cursor in seen or len(seen) >= 32:
            return
        seen.add(cursor)
        params = {"cursor": cursor}
    save(provider, rows)


def acp_catalog(provider, session):
    def values(options):
        for item in options:
            if "options" in item:
                yield from values(item["options"])
            elif "value" in item:
                yield item["value"]

    configs = session.get("configOptions", [])
    model = next((c for c in configs if c.get("category") == "model" or c.get("id") == "model"), {})
    effort = next((c for c in configs if c.get("category") == "thought_level" or c.get("id") in ("reasoning_effort", "effort")), {})
    legacy = session.get("models") or {}
    offered = list(values(model.get("options", []))) or [r["modelId"] for r in legacy.get("availableModels", [])]
    current = model.get("currentValue") or legacy.get("currentModelId")
    efforts = list(values(effort.get("options", [])))
    save(provider, [(value, efforts if value == current else [], value == current) for value in offered])
