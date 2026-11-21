# A local UI decision. The renderer owns the set of supported scale steps.
def decide(event, context, config):
    if event == "text_zoom":
        return max(context["min"], min(context["max"], context["current"] + context["delta"]))
    return None
