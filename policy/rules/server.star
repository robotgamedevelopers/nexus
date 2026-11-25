load("server/conversation.star", handle_event = "handle")

# The server supplies linked accounts in configuration order and current eligibility.
def decide(event, context, config):
    if event == "model_after_limit":
        return config.get("fable_fallback_model", "opus")
    if event == "account_command":
        linked = context["linked"]
        current = context["current"]
        navigation = config.get("account_navigation", {})
        steps = navigation.get("steps", {"previous": -1, "next": 1})
        command = context["command"]
        if command not in steps:
            fail("No account command named " + command)
        step = steps[command]
        index = linked.index(current) if current in linked else 0
        if navigation.get("wrap", True):
            return linked[(index + step) % len(linked)]
        return linked[max(0, min(index + step, len(linked) - 1))]
    if event == "account_after_limit":
        linked = context["linked"]
        current = context["current"]
        start = linked.index(current) + 1 if current in linked else 0
        for account in linked[start:] + linked[:start]:
            if account in context["eligible"]:
                return account
        return None
    return None

def handle(event, state, config):
    return handle_event(event, state, config)
