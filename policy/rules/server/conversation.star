"""Server application for a submitted conversation message."""

load("server/sessions.star", "new_session")

def _text(value):
    for char in ["\\", "`", "*", "_", "[", "]", "<", ">"]:
        value = value.replace(char, "\\" + char)
    return value

def _code(value):
    longest = 0
    run = 0
    for char in value.elems():
        run = run + 1 if char == "`" else 0
        longest = max(longest, run)
    fence = "`" * (longest + 1)
    return fence + " " + value + " " + fence

def _command(state):
    words = state["text"].split()
    if not words or words[0] not in ["/clear", "/compact", "/model", "/effort", "/info", "/goal"]:
        return None
    cmd = words[0]
    if cmd == "/goal":
        text = state["text"].strip()[len(cmd):].strip()
        if not text:
            return [{"op": "goal"}]
        return [{"op": "goal", "text": "" if text in ["off", "clear"] else text}]
    if cmd in ["/clear", "/compact"]:
        if len(words) == 1:
            return [{"op": "clear_context" if cmd == "/clear" else "compact_context"}]
        return [{"op": "notice", "text": "Use " + cmd + " on its own."}]
    catalog = state.get("catalog", {})
    models = catalog.get("models", {})
    order = [m for m in catalog.get("order", models.keys()) if m in models and m != "default"]
    model = state.get("model")
    if not model or model == "default":
        model = order[0] if order else ""
    if cmd == "/info":
        if len(words) != 1:
            return [{"op": "notice", "text": "Use /info on its own."}]
        identity = state.get("identity", {})
        model = model or identity.get("model") or "unreported"
        target = catalog.get("resolved", {}).get(model)
        email = state.get("account_email") or "email unavailable"
        repository = identity.get("repository") or "unreported"
        repo = _code(repository)
        if repository.startswith("https://") or repository.startswith("http://"):
            url = repository.replace(" ", "%20").replace("(", "%28").replace(")", "%29")
            repo = "[" + _text(repository) + "](" + url + ")"
        lines = [
            "- **AI:** " + _text(state["ai"]),
            "- **Harness:** " + _code(state.get("harness") or "unknown"),
            "- **Account:** " + _code(email) + " (" + _text(state.get("login_state") or "unknown") + ")",
            "- **Model:** " + _code(model) + (" → " + _code(target) if target and target != model else ""),
            "- **Effort:** " + _code(state.get("effort") or identity.get("effort") or catalog.get("default_efforts", {}).get(model) or "harness default"),
            "- **Repository:** " + repo,
        ]
        if identity.get("pronouns"):
            lines.insert(1, "- **Pronouns:** " + _text(identity["pronouns"]))
        dirs = identity.get("directories", [])
        lines += ["", "**Working directories**", ""]
        lines += ["- " + _code(path) for path in dirs] if dirs else ["Unreported"]
        return [{"op": "notice", "text": "**Session info**\n\n" + "\n".join(lines)}]
    choices = order if cmd == "/model" else models.get(model, [])
    current = model if cmd == "/model" else state.get("effort", "")
    if not choices:
        return [{"op": "notice", "text": "No " + cmd[1:] + " choices reported by " + (state.get("harness") or "this harness") + "."}]
    if len(words) == 1:
        if cmd == "/model":
            targets = catalog.get("resolved", {})
            labels = []
            for value in choices:
                target = targets.get(value)
                label = _code(value) + (" → " + _code(target) if target and target != value else "")
                labels.append("- " + label + (" **(current)**" if value == current else ""))
            return [{"op": "notice", "text": "**Models**\n\n" + "\n".join(labels)}]
        return [{"op": "notice", "text": "**Effort levels**\n\n" + "\n".join(["- " + _code(v) + (" **(current)**" if v == current else "") for v in choices])}]
    if cmd == "/model" and len(words) == 3 and words[1] in models:
        efforts = models[words[1]]
        if words[2] not in efforts:
            return [{"op": "notice", "text": "Available effort levels for " + words[1] + ": " + (", ".join(efforts) or "none reported") + "."}]
        return [{"op": "set_model", "text": words[1], "effort": words[2]}]
    if len(words) != 2 or words[1] not in choices:
        return [{"op": "notice", "text": "Unknown " + cmd[1:] + ". Use " + cmd + " to list choices."}]
    return [{"op": "set_" + cmd[1:], "text": words[1]}]

def handle(event, state, config):
    if event == "notice":
        kind = state["kind"]
        if kind == "setting":
            labels = config.get("setting_labels", {"model": "Model", "effort": "Effort", "account": "Account"})
            reply = "**" + _text(labels.get(state["setting"], state["setting"])) + ":** " + _code(state["value"])
            if state.get("effort"):
                reply += " · **" + _text(labels.get("effort", "Effort")) + ":** " + _code(state["effort"])
            return reply
        if kind == "goal_stopped":
            return config.get("goal_stopped_text", "Goal stopped.")
        if kind == "goal_status":
            return (config.get("goal_status_heading", "**Active goal** · every five minutes") + "\n\n" + _text(state["goal"])) if state.get("goal") else config.get("goal_empty_text", "No active goal. Use `/goal <text>` to send now and repeat every five minutes.")
        fail("Unknown notice kind: " + kind)
    if event == "new_session":
        return new_session(state, config)
    if event == "message":
        command = _command(state)
        if command != None:
            return command
        service = config.get("services", {}).get(state["ai"])
        if service:
            if "native" in service:
                return [{"op": "native", "name": service["native"]}]
            fail("Unknown service handler for " + state["ai"])
        # The text argument defaults to the submitted text. A fork can inspect it,
        # transform it, attach notices, or choose to queue against live session state.
        return [{"op": state["intent"]}]
    return []
