"""The personal sign-in surface, its account actions and its keyboard interaction."""

def describe(window, state, config):
    ui = state.get("ui", {})
    if state["tab"] != "friend":
        return window

    appearance = {
        "side_extension": 20,
        "row_height": 1.45,
        "row_gap": 0.15,
        "heading_gap": 0.8,
        "selected_fill": "#86c5ff",
        "selected_ink": "#ffffff",
        "email_weight": 700,
    }
    appearance.update(window["message_card"])
    window["message_card"] = appearance

    options = ui.get("accounts", [])
    names = [a["name"] for a in options]
    current = ui.get("current", "")
    highlighted = ui.get("highlighted", current)
    signed_out = state.get("needs_sign_in", False)
    opened = state.get("account_picker_open", False)
    dismissed = ui.get("dismissed", False)
    visible = bool(options) and (opened or (signed_out and not dismissed))
    submit = "sign_in:" if config.get("sign_in_on_select", False) else "select:"
    enter = "sign_in:" if highlighted == current and signed_out else submit
    items = []
    if visible:
        if signed_out:
            items.append({"text": window["sign_in_heading"]})
        for account in options:
            name = account["name"]
            action = "sign_in:" if signed_out and name == current else submit
            items.append({"text": account["label"], "action": action + name, "selected": name == highlighted})
        window["panels"] = ["account", "prompt"] if state["prompt_open"] else ["account"]
    elif signed_out and dismissed and state["prompt_open"]:
        window["panels"] = ["transcript", "prompt"]
    window["account_items"] = items

    # The native host validates the target of each primitive against the active server roster.
    bindings = {}
    if names:
        index = names.index(highlighted) if highlighted in names else 0
        steps = {"next": 1, "previous": -1, "open": 0}
        opening = config.get("account_picker_shortcuts", {"Command+ArrowUp": "previous", "Command+ArrowDown": "next"})
        def bind(chord, command):
            if command in steps:
                target = names[(index + (steps[command] if visible else 0)) % len(names)]
                bindings[chord] = ["account_action", "focus:" + target]
            elif command == "cancel":
                bindings[chord] = ["account_action", "dismiss"]
            elif command == "confirm":
                bindings[chord] = ["account_action", enter + highlighted]
        for chord, command in opening.items():
            bind(chord, command)
        if visible:
            for chord, command in config.get("account_picker_keys", {"ArrowUp": "previous", "ArrowDown": "next", "Enter": "confirm", "Escape": "cancel"}).items():
                if chord != "Enter" or not ui.get("logout") or opened:
                    bind(chord, command)
        elif signed_out and not ui.get("logout"):
            bindings["Enter"] = ["account_action", "reveal"]
    window["shortcuts"] = [b for b in window["shortcuts"] if b[1] != "account_picker" and b[0] not in bindings] + [[key] + action for key, action in bindings.items()]
    return window
