# Keys in an AI's settings override this person's defaults.
def driver_start(context, config):
    settings = dict(config.get("defaults", {}))
    settings.update(config.get("ais", {}).get(context["ai"], {}))
    return settings
