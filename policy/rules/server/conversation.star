"""Server application for a submitted conversation message."""

load("server/sessions.star", "new_session")

def handle(event, state, config):
    if event == "new_session":
        return new_session(state)
    if event == "message":
        service = config.get("services", {}).get(state["friend"])
        if service:
            if "native" in service:
                return [{"op": "native", "name": service["native"]}]
            if service.get("handler") == "word_count":
                words = len(state["text"].split())
                return [{"op": "notice", "text": str(words) + (" word" if words == 1 else " words")}]
            fail("Unknown service handler for " + state["friend"])
        # The text argument defaults to the submitted text. A fork can inspect it,
        # transform it, attach notices, or choose to queue against live session state.
        return [{"op": state["intent"]}]
    return []
