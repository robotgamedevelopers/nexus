# The server supplies linked accounts in configuration order and current eligibility.
def decide(event, context, config):
    if event == "account_after_limit":
        linked = context["linked"]
        current = context["current"]
        start = linked.index(current) + 1 if current in linked else 0
        for account in linked[start:] + linked[:start]:
            if account in context["eligible"]:
                return account
        return None
    return None
