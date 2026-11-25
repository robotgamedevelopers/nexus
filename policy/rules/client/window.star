load("client/accounts.star", account_window = "describe")
"""The window application. The platform draws the panels and captures the declared keys."""

# Curves are [fraction of cycle, level] pairs. The native renderer interpolates them.
def _prompt_style(config):
    style = dict(config.get("prompt_style", {}))
    if "caret_curve" not in style:
        seconds = style.get("caret_seconds", 1.5)
        top = min(style.get("caret_hold", 0.1) / seconds, 0.45)
        bottom = min(style.get("caret_bottom_hold", style.get("caret_hold", 0.1)) / seconds, 0.45)
        rise = (1.0 - top - bottom) / (1.0 + style.get("caret_fall_ratio", 0.5))
        points = [[0, 0], [rise, 1]]
        if top > 0:
            points.append([rise + top, 1])
        if bottom > 0:
            points.append([1.0 - bottom, 0])
        style["caret_curve"] = points + [[1, 0]]
    return style

def _transcript_style(config):
    style = dict(config.get("transcript_style", {}))
    if "scanner_curve" not in style:
        style["scanner_curve"] = [[0, 0], [0.08, 0], [0.5, 1], [0.58, 1], [1, 0]]
    return style

# Forks can extend this ordered command reference in their client config.
_COMMAND_HELP = {
    "/help": "Show this command list.",
    "/model": "Open the model selector. Use /model <model> [effort] to switch directly.",
    "/effort": "Open the effort selector. Use /effort <level> to switch directly.",
    "/info": "Show the AI, account, model, effort, repository and working directories.",
    "/clear": "Clear the conversation and its context. When signed out, clear the local conversation.",
    "/compact": "Compact the current conversation context.",
    "/login": "Open the account sign-in window.",
    "/logout": "Sign out of the selected account.",
    "/quiet": "Mute nexus sounds.",
    "/quiet off": "Turn sounds back on.",
    "/loud": "Turn sounds back on (same as /quiet off).",
}

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
    elif state["has_status"] and state.get("tab_count", 0) == 0:
        # No tab yet: the link's state (connecting, or why it failed) is all the window has to show.
        panels.append("status")
    return panels

def _titles(tabs, config):
    return [(t["label"][:1].upper() + t["label"][1:]) if t["kind"] == "friend" else config.get("tab_labels", {}).get(t["kind"], t["label"]) for t in tabs]

# The iPhone's form (docs/voice.md), chosen by the device's idiom (ui.idiom: "phone", "pad" or "desktop"). The app opens
# on the list of AIs (ui.menu), centred, with a microphone at the bottom: say an AI's name and what to ask, and the
# reply comes back on the list, spoken. An AI's tab opens full screen with the way back, its
# transcript and the prompt, whose microphone is plain dictation into the prompt; its replies are text. A fork turns
# both microphones off with "phone": {"voice": false}, and adds spoken forms of an AI's name with
# "phone": {"voice_names": {"zhi": ["zhi ya wen"]}}.
# The list's rows, by tab index: by kind in phone.menu_kinds' order (the dashboard, the AIs, then terminals), each kind
# in the strip's order; tabs of a kind not listed come last.
def _menu_tabs(tabs, phone):
    kinds = phone.get("menu_kinds", ["sprint", "friend", "terminal"])
    rank = {kind: i for i, kind in enumerate(kinds)}
    order = sorted(range(len(tabs)), key = lambda i: (rank.get(tabs[i]["kind"], len(kinds)), i))
    return order

def _phone(window, state, config):
    ui = state.get("ui", {})
    phone = config.get("phone", {})
    menu = ui.get("menu", True)
    window["show_tabs"] = False
    window["reorder_tabs"] = False
    window["focus_prompt"] = False
    window["voice_mic"] = phone.get("voice", True)
    window["tab_titles"] = window.get("tab_titles") or _titles(ui.get("tabs", []), config)
    window["voice_names"] = phone.get("voice_names", {})
    window["menu_tabs"] = _menu_tabs(ui.get("tabs", []), phone)
    if menu:
        window["panels"] = ["menu"]
        return window
    panels = [p for p in window["panels"] if p != "status"]
    if state["tab"] == "friend" and state["has_status"]:
        panels.append("status")
    window["panels"] = (["back"] + panels)[:6]
    return window

def handle(event, state, config):
    if event == "overlay":
        return config.get("overlay_hotkey", {"modifiers": "Command", "code": "Backquote"})
    if event == "window":
        shortcuts = _shortcuts()
        count = state.get("tab_count", 0)
        if count:
            for chord, direction in config.get("tab_navigation", {"Command+ArrowLeft": -1, "Command+ArrowRight": 1}).items():
                target = state["active_tab"] + direction
                target = target % count if config.get("tab_navigation_wrap", False) else max(0, min(target, count - 1))
                shortcuts = _bind(shortcuts, chord, ["select_tab", target])
            for chord, direction in config.get("tab_reorder", {"Command+Shift+ArrowLeft": -1, "Command+Shift+ArrowRight": 1}).items():
                shortcuts = _bind(shortcuts, chord, ["move_tab", direction])
        if state["tab"] == "friend":
            for chord, command in config.get("account_shortcuts", {}).items():
                shortcuts = _bind(shortcuts, chord, ["account_command", command])
            default_picker = {} if "account_shortcuts" in config else {"Command+ArrowUp": "previous", "Command+ArrowDown": "next"}
            for chord, command in config.get("account_picker_shortcuts", default_picker).items():
                if config.get("centered_account_menu", False) and not state.get("account_picker_open") and not state.get("needs_sign_in") and command in ["previous", "next"]:
                    command = "open"
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
        account_button = ""
        model_button = ""
        offer = state.get("limit_offer")
        if state["tab"] == "friend" and offer:
            panels = ["transcript", "limit_choice"]
            account = offer["account"]
            model = offer["model"]
            model_name = "Opus" if model == "opus" else model
            if not offer["connected"]:
                caption = "Reconnecting to resume the Fable choice…"
            elif offer["waiting"]:
                caption = "Waiting for the server to resume the conversation…"
            else:
                if account and model:
                    caption = "Out of Fable. Switch to another account or drop to " + model_name + "?"
                elif account:
                    caption = "Out of Fable. Switch to another account?"
                elif model:
                    caption = "Out of Fable. Drop to " + model_name + "?"
                else:
                    caption = "Out of Fable. No other account or model is available."
                keys = config.get("limit_choice_keys", {})
                if account:
                    chord = keys.get("account", "Command+Shift+A")
                    account_button = "Switch to " + account + "  (" + chord + ")"
                    if not state.get("account_picker_open"):
                        shortcuts = _bind(shortcuts, chord, ["resolve_limit", "account"])
                if model:
                    chord = keys.get("model", "Command+Shift+O")
                    model_button = "Use " + model_name + "  (" + chord + ")"
                    if not state.get("account_picker_open"):
                        shortcuts = _bind(shortcuts, chord, ["resolve_limit", "model"])
        if config.get("centered_account_menu", False) and state.get("needs_sign_in") and "account" in panels:
            shortcuts = _bind(shortcuts, "Escape", ["account_picker", "cancel"])
        if state.get("account_picker_open", False):
            if config.get("centered_account_menu", False):
                panels = ["transcript", "account", "prompt"]
            for chord, command in config.get("account_picker_keys", {"ArrowUp": "previous", "ArrowDown": "next", "Enter": "confirm", "Escape": "cancel"}).items():
                shortcuts = _bind(shortcuts, chord, ["account_picker", command])
        account = state.get("sign_in_account") or {}
        provider = account.get("provider", "")
        directory = config.get("nova_friends", {})
        friend = directory.get("friends", {}).get(account.get("friend", ""), {})
        provider = provider or friend.get("harness", "")
        display = directory.get("providers", {}).get(provider, provider)
        window = {
            "sign_in_heading": "Please sign in to " + display if display else "Please sign in",
            "sounds": config.get("sounds", {}),
            "message_card": config.get("message_card", {"fill":"#303030", "ink":"#ffffff", "email_ink":"#477fba", "text_size":34, "radius":20}),
            "transcript_style": _transcript_style(config),
            "centered_account_menu": config.get("centered_account_menu", False),
            "sign_in_on_select": config.get("sign_in_on_select", False),
            "account_auto_label": config.get("account_auto_label", "auto"),
            "show_tabs": config.get("show_tabs", True),
            "reorder_tabs": config.get("reorder_tabs", True),
            "prompt_command": False,
            "prompt_style": _prompt_style(config),
            "tab_style": config.get("tab_style", {}),
            "overlay_opacity": config.get("overlay_opacity", 0.60),
            "tab_groups": groups,
            "closable": closable,
            "shortcuts": shortcuts,
            "panels": panels,
            "tab_types": types,
            "label": caption,
            "limit_account_label": account_button,
            "limit_model_label": model_button,
        }
        words = state.get("ui", {}).get("draft", "").split()
        window["prompt_command"] = bool(words) and (words[0] == "/help" or words[0] in config.get("prompt_commands", _COMMAND_HELP.keys()))
        tabs = state.get("ui", {}).get("tabs", [])
        if config.get("centered_account_menu", False) or config.get("tab_labels"):
            window["tab_titles"] = _titles(tabs, config)
        if config.get("centered_account_menu", False):
            window = account_window(window, state, config)
        if state["tab"] == "friend" and state["prompt_open"]:
            ui = state.get("ui", {})
            draft = ui.get("draft", "")
            catalog = ui.get("catalog", {})
            models = catalog.get("models", {})
            order = [m for m in catalog.get("order", models.keys()) if m in models and m != "default"]
            model = ui.get("model")
            if not model or model == "default":
                model = order[0] if order else ""
            efforts = models.get(model, [])
            kind = ui.get("choice_kind", "")
            lists = {"model": order, "effort": efforts}
            if not kind and len(words) == 1 and words[0] in ["/model", "/effort"] and not state.get("account_picker_open"):
                window["shortcuts"] = _bind(window["shortcuts"], "Enter", ["account_action", "picker:" + words[0][1:]])
            if kind in lists:
                values = lists[kind]
                current_effort = ui.get("effort") or catalog.get("default_efforts", {}).get(model)
                selected = ui.get("choice_value") or (model if kind == "model" else current_effort)
                index = values.index(selected) if selected in values else 0
                targets = catalog.get("resolved", {}) if kind == "model" else {}
                items = [{"text": "Model" if kind == "model" else "Effort"}]
                if not values:
                    items.append({"text": "No choices reported yet"})
                for i, value in enumerate(values):
                    target = targets.get(value)
                    items.append({"text": value + (" → " + target if target and target != value else ""),
                                  "action": "choose:" + value, "selected": i == index})
                window["account_items"] = items
                window["panels"] = ["transcript", "account", "prompt"]
                keys = {"Escape": "picker:close"}
                if values:
                    keys.update({"ArrowUp": "point:" + values[(index-1) % len(values)],
                                 "ArrowDown": "point:" + values[(index+1) % len(values)], "Enter": "choose:" + values[index]})
                for chord, operation in keys.items():
                    window["shortcuts"] = _bind(window["shortcuts"], chord, ["account_action", operation])
            completions = [("/model ", order), ("/effort ", efforts)]
            if len(words) >= 2 and words[0] == "/model" and words[1] in models:
                completions.append(("/model " + words[1] + " ", models[words[1]]))
            for command, choices in completions:
                if draft.startswith(command) and choices:
                    value = draft[len(command):]
                    matches = [v for v in choices if v.startswith(value)]
                    if value in choices:
                        next_value = choices[(choices.index(value) + 1) % len(choices)]
                    elif matches:
                        next_value = matches[0]
                    else:
                        continue
                    window["shortcuts"] = _bind(window["shortcuts"], "Tab", ["replace_draft", command + next_value])
        if state["tab"] == "friend" and state["prompt_open"] and not window.get("account_items"):
            for chord, direction in config.get("transcript_scroll_keys", {
                    "Control+ArrowUp": "top", "Control+ArrowDown": "bottom",
                    "PageUp": "page_up", "PageDown": "page_down"}).items():
                window["shortcuts"] = _bind(window["shortcuts"], chord, ["scroll_transcript", direction])
        if state["tab"] == "friend" and state["prompt_open"] and not state.get("account_picker_open") and not state.get("ui", {}).get("choice_kind"):
            if state.get("ui", {}).get("draft") == "/login":
                for chord in ["Enter", "Command+Enter"]:
                    window["shortcuts"] = _bind(window["shortcuts"], chord, ["account_picker", "open"])
            for chord in config.get("clear_draft_keys", ["Command+Backspace", "Command+Delete"]):
                window["shortcuts"] = _bind(window["shortcuts"], chord, ["clear_composer"])
        if state["tab"] == "friend" and state.get("needs_sign_in"):
            if state.get("ui", {}).get("help_visible") and state.get("ui", {}).get("dismissed") and not state.get("account_picker_open"):
                window["panels"] = ["transcript", "prompt"]
                window["account_items"] = []
            else:
                window["panels"] = ["spacer" if p == "transcript" else p for p in window["panels"]]
        if state.get("ui", {}).get("idiom") == "phone":
            window = _phone(window, state, config)
        return window
    if event == "prompt_key":
        if state["key"] in ["Enter", "Command+Enter"] and state["text"].strip() == "/help":
            commands = dict(_COMMAND_HELP)
            commands.update(config.get("command_help", {}))
            shortcuts = [
                "Keyboard shortcuts",
                "⌘ + backtick (the ~ key)   Open the Nexus overlay",
                "⌘T / ⌘W   Open a terminal tab / close the current terminal",
                "⌘+ / ⌘−   Increase / decrease font size (⌘= also increases it).",
                "⌘1, ⌘2, ⌘3, ⌘4 …   Select a tab by position",
                "⌘← / ⌘→   Move to the previous / next tab",
                "⌘↑ / ⌘↓   Choose an account for the current AI, including among multiple Claude accounts",
                "Esc   Hide the overlay and return to the previous app. In a picker, Esc closes the picker first.",
            ]
            return [{"op": "draft", "text": ""}, {"op": "notice", "text": "\n".join(shortcuts)}, {"op": "notice", "text": "\nSlash commands"}] + [
                {"op": "notice", "text": cmd + "  " + description} for cmd, description in commands.items()
            ]
        if state["key"] in ["Enter", "Command+Enter"] and state["text"].strip() in ["/quiet", "/quiet off", "/loud"]:
            return [{"op": "quiet" if state["text"].strip() == "/quiet" else "loud"}]
        if state["key"] == "Enter" and state.get("signed_out") and state["text"].strip() == "/clear":
            return [{"op": "clear_local"}]
        if state["text"] == "/logout":
            if state["key"] == "Enter":
                return [{"op": "sign_out"}]
            return [{"op": "notice", "text": "Press Enter to sign out of the selected account."}]
        if config.get("hold_on_enter", False) and not state["text"].lstrip().startswith("/"):
            if state["key"] == "Command+Enter" or (state["key"] == "Enter" and not state["text"].strip()):
                return [{"op": "release"}]
            if state["key"] == "Enter":
                return [{"op": "hold"}]
        if state["key"] == "Enter":
            return [{"op": "send"}]
        if state["key"] == "Command+Enter":
            return [{"op": "queue"}]
    return []
