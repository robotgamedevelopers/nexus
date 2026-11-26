load("server/conversation.star", conversation = "handle")

def handle(event, state, config):
    if event == "message":
        service = config.get("services", {}).get(state["ai"], {})
        if service.get("handler") == "word_count":
            words = len(state["text"].split())
            return [{"op": "notice", "text": str(words) + (" word" if words == 1 else " words")}]
    return conversation(event, state, config)
