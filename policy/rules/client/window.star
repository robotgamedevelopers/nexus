"""The window application. The platform draws the panels and captures the declared keys."""

def _shortcuts():
    keys = [["Command+" + digit, "select_tab", pos] for pos, digit in enumerate("1234567890".elems())]
    keys.extend([
        ["Command+=", "zoom", 1],
        ["Command+Shift++", "zoom", 1],
        ["Command+-", "zoom", -1],
        ["Command+T", "open_terminal"],
        ["Command+W", "close_terminal"],
        ["Command+Escape", "stop"],
    ])
    return keys

def _bind(shortcuts, chord, command):
    return [k for k in shortcuts if k[0] != chord] + [[chord] + command]

def _panels(state):
    if state["tab"] == "terminal":
        return ["terminal"]
    if state["tab"] == "sprint":
        return ["board"]
    panels = ["transcript"]
    if state["tab"] == "friend":
        if state["prompt_open"]:
            panels.append("prompt")
        elif state["has_status"]:
            panels.append("status")
    return panels

def handle(event, state, config):
    if event == "window":
        shortcuts = _shortcuts()
        types = config.get("tab_types", [])
        panels = _panels(state)
        caption = ""
        for plugin in types:
            if plugin.get("open"):
                shortcuts = _bind(shortcuts, plugin["open"], ["open_tab", plugin["id"]])
            if state["tab"] == "extension" and state["type"] == plugin["id"]:
                panels = list(plugin.get("panels", ["native"] if plugin.get("native") else ["label"]))
                caption = plugin.get("label", plugin["title"])
                if plugin.get("close"):
                    shortcuts = _bind(shortcuts, plugin["close"], ["close_tab"])
                if plugin.get("invoke"):
                    shortcuts = _bind(shortcuts, plugin["invoke"], ["invoke_service"])
        if state["tab"] == "extension" and panels == ["transcript"]:
            panels = ["label"]
            caption = "This tab type is no longer registered."
            shortcuts = _bind(shortcuts, "Command+W", ["close_tab"])
        return {"shortcuts": shortcuts, "panels": panels, "tab_types": types, "label": caption}
    if event == "prompt_key":
        if state["key"] == "Enter":
            return [{"op": "send"}]
        if state["key"] == "Command+Enter":
            return [{"op": "queue"}]
    return []
