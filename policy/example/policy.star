# Keys in a friend's settings override this person's defaults.
def driver_start(context, config):
    settings = dict(config.get("defaults", {}))
    settings.update(config.get("friends", {}).get(context["friend"], {}))
    return settings
