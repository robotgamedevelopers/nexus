"""Server application for a submitted conversation message."""

load("server/sessions.star", "new_session")

def _command(state):
    words = state["text"].split()
    if not words or words[0] not in ["/clear", "/compact", "/model", "/effort"]:
        return None
    cmd = words[0]
    if cmd in ["/clear", "/compact"]:
        if len(words) == 1:
            return [{"op": "clear_context" if cmd == "/clear" else "compact_context"}]
        return [{"op": "notice", "text": "Use " + cmd + " on its own."}]
    catalog = state.get("catalog", {})
    models = catalog.get("models", {})
    model = state.get("model") or catalog.get("default", "")
    choices = models.keys() if cmd == "/model" else models.get(model, [])
    current = model if cmd == "/model" else state.get("effort", "")
    if not choices:
        return [{"op": "notice", "text": "No " + cmd[1:] + " choices reported by " + (state.get("harness") or "this harness") + "."}]
    if len(words) == 1:
        return [{"op": "notice", "text": "\n".join([v + (" (current)" if v == current else "") for v in choices])}]
    if len(words) != 2 or words[1] not in choices:
        return [{"op": "notice", "text": "Unknown " + cmd[1:] + ". Use " + cmd + " to list choices."}]
    return [{"op": "set_" + cmd[1:], "text": words[1]}]

def handle(event, state, config):
    if event == "new_session":
        return new_session(state, config)
    if event == "message":
        command = _command(state)
        if command != None:
            return command
        service = config.get("services", {}).get(state["friend"])
        if service:
            if "native" in service:
                return [{"op": "native", "name": service["native"]}]
            fail("Unknown service handler for " + state["friend"])
        # The text argument defaults to the submitted text. A fork can inspect it,
        # transform it, attach notices, or choose to queue against live session state.
        return [{"op": state["intent"]}]
    return []
