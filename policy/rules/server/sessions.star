"""Build a new conversation from the friend, driver defaults and optional source session."""

def new_session(state, config = {}):
    defaults = state["defaults"]
    result = {field: defaults[field] for field in ["account", "instance", "model", "effort"]}
    source = state["source"]
    if source != None:
        for field in ["account", "model", "effort"]:
            result[field] = source[field]
        if source["friend"] == state["friend"]:
            result["instance"] = source["instance"]
    roster = state["roster"]
    if roster != None:
        if source == None or (result["account"] and result["account"] not in roster["linked"]):
            mode = config.get("account_default", "preferred")
            if mode == "first":
                result["account"] = roster["linked"][0] if roster["linked"] else ""
            elif mode == "preferred":
                result["account"] = roster["preferred"]
            else:
                fail("account_default must be first or preferred")
        if result["instance"] not in roster["instances"]:
            result["instance"] = roster["instances"][0] if roster["instances"] else ""
    return result
