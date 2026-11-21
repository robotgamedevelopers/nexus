"""The window application. The platform draws the panels and captures the declared keys."""

def _shortcuts():
    keys = [["Command+" + digit, "select_tab", pos] for pos, digit in enumerate("1234567890".elems())]
    keys.extend([
        ["Command+=", "zoom", 1],
        ["Command+Shift++", "zoom", 1],
        ["Command+-", "zoom", -1],
        ["Command+T", "open_terminal"],
        ["Command+W", "close_tab"],
        ["Command+Escape", "stop"],
    ])
    return keys

def _bind(shortcuts, chord, command):
    return [k for k in shortcuts if k[0] != chord] + [[chord] + command]

# A fork orders groups, assigns tab types and grants each group's operations.
_BUILTINS = [
    {"id": "dashboards", "types": ["sprint"], "closable": False},
    {"id": "friends", "types": ["friend"], "closable": False},
    {"id": "terminals", "types": ["terminal"], "closable": True},
]

def _tab_groups(config, plugins):
    groups = []
    by_id = {}
    assigned = {}
    for row in config.get("tab_groups", _BUILTINS):
        group = {"id": row["id"], "types": list(row.get("types", [])),
                 "closable": row.get("closable", False), "accepts_from": row.get("accepts_from", [])}
        if group["id"] in by_id:
            fail("repeated tab group " + group["id"])
        groups.append(group)
        by_id[group["id"]] = group
        for t in group["types"]:
            assigned[t] = True
    declared = dict(by_id)
    # Keep existing profiles: a registered plugin names its group directly,
    # and an explicit per-type close key enables closing in its implicit group.
    for row in _BUILTINS + [{"id": p["group"], "types": [p["id"]], "closable": bool(p.get("close"))} for p in plugins]:
        t = row["types"][0]
        if t in assigned:
            continue
        if row["id"] not in by_id:
            by_id[row["id"]] = {"id": row["id"], "types": [], "closable": row["closable"], "accepts_from": []}
            groups.append(by_id[row["id"]])
        elif row["id"] not in declared and row["closable"]:
            by_id[row["id"]]["closable"] = True
        by_id[row["id"]]["types"].append(t)
        assigned[t] = True
    return groups

def _panels(state):
    if state["tab"] == "terminal":
        return ["terminal"]
    if state["tab"] == "sprint":
        return ["board"]
    if state["tab"] == "friend" and state["prompt_open"] and state.get("needs_sign_in", False):
        return ["account", "prompt"]
    panels = ["transcript"]
    if state["tab"] == "friend":
        if state["prompt_open"]:
            panels.extend(["account", "prompt"])
        elif state["has_status"]:
            panels.append("status")
    return panels

def handle(event, state, config):
    if event == "window":
        shortcuts = _shortcuts()
        count = state.get("tab_count", 0)
        if count:
            for chord, direction in config.get("tab_navigation", {"Command+ArrowLeft": -1, "Command+ArrowRight": 1}).items():
                shortcuts = _bind(shortcuts, chord, ["select_tab", (state["active_tab"] + direction) % count])
            for chord, direction in config.get("tab_reorder", {"Command+Shift+ArrowLeft": -1, "Command+Shift+ArrowRight": 1}).items():
                shortcuts = _bind(shortcuts, chord, ["move_tab", direction])
        if state["tab"] == "friend":
            for chord, command in config.get("account_shortcuts", {}).items():
                shortcuts = _bind(shortcuts, chord, ["account_command", command])
            default_picker = {} if "account_shortcuts" in config else {"Command+ArrowUp": "previous", "Command+ArrowDown": "next"}
            for chord, command in config.get("account_picker_shortcuts", default_picker).items():
                shortcuts = _bind(shortcuts, chord, ["account_picker", command])
        types = config.get("tab_types", [])
        groups = _tab_groups(config, types)
        tab_type = state.get("type", "") if state["tab"] == "extension" else state["tab"]
        group_id = state.get("group", "")
        group = None
        # Before its first layout a tab takes its type’s group. A permitted drag can change it.
        assigned_group = None
        for candidate in groups:
            if candidate["id"] == group_id:
                group = candidate
            if tab_type in candidate["types"]:
                assigned_group = candidate
        if group == None:
            group = assigned_group
        closable = group["closable"] if group != None else False
        panels = _panels(state)
        caption = ""
        for plugin in types:
            if plugin.get("open"):
                shortcuts = _bind(shortcuts, plugin["open"], ["open_tab", plugin["id"]])
            if state["tab"] == "extension" and state["type"] == plugin["id"]:
                panels = list(plugin.get("panels", ["native"] if plugin.get("native") else ["label"]))
                caption = plugin.get("label", plugin["title"])
                if type(plugin.get("closable", True)) != "bool":
                    fail("tab type closable must be a boolean")
                if not plugin.get("closable", True):
                    closable = False
                if plugin.get("close"):
                    shortcuts = _bind(shortcuts, plugin["close"], ["close_tab"])
                if plugin.get("invoke"):
                    shortcuts = _bind(shortcuts, plugin["invoke"], ["invoke_service"])
        if state["tab"] == "extension" and panels == ["transcript"]:
            panels = ["label"]
            caption = "This tab type is no longer registered."
            if group == None:
                closable = True
        if state.get("account_picker_open", False):
            for chord, command in config.get("account_picker_keys", {"ArrowUp": "previous", "ArrowDown": "next", "Enter": "confirm", "Escape": "cancel"}).items():
                shortcuts = _bind(shortcuts, chord, ["account_picker", command])
        return {
            "message_card": config.get("message_card", {"fill":"#ffffff", "ink":"#000000", "email_ink":"#477fba", "text_size":34, "radius":20}),
            "transcript_style": config.get("transcript_style", {}),
            "account_auto_label": config.get("account_auto_label", "auto"),
            "overlay_opacity": config.get("overlay_opacity", 0.60),
            "tab_groups": groups,
            "closable": closable,
            "shortcuts": shortcuts,
            "panels": panels,
            "tab_types": types,
            "label": caption,
        }
    if event == "prompt_key":
        if state["key"] == "Enter":
            return [{"op": "send"}]
        if state["key"] == "Command+Enter":
            return [{"op": "queue"}]
    return []
