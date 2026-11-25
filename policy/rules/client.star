load("client/window.star", handle_event = "handle")

# A local UI decision. The renderer owns the set of supported scale steps.
def decide(event, context, config):
    if event == "text_zoom":
        return max(context["min"], min(context["max"], context["current"] + context["delta"]))
    return None

def handle(event, state, config):
    return handle_event(event, state, config)
