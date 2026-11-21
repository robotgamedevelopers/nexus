"""Resolve a configured friend's registry row and tracked home documents at driver start."""
import hashlib
import json
import os
import stat
from pathlib import Path
import re
import subprocess

MAX_CONTEXT = 256 * 1024
IDENTIFIER = re.compile(r"[a-zA-Z0-9][a-zA-Z0-9._:/-]{0,127}")


def output(argv):
    run = subprocess.run(argv, stdin=subprocess.DEVNULL, capture_output=True, timeout=10)
    if run.returncode:
        raise ValueError(f"{Path(argv[0]).name} could not read the configured friend definition or home revision")
    return run.stdout


def read_home_file(home, filename):
    # Open every component beneath the home without following links, including the last.
    directory = os.open(home, os.O_RDONLY | os.O_DIRECTORY)
    parts = filename.split("/")
    try:
        for part in parts[:-1]:
            child = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=directory)
            os.close(directory)
            directory = child
        fd = os.open(parts[-1], os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=directory)
        with os.fdopen(fd, "rb") as file:
            info = os.fstat(file.fileno())
            if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
                raise ValueError(f"{filename} must be a regular home file with one link")
            data = file.read(MAX_CONTEXT + 1)
    finally:
        os.close(directory)
    if len(data) > MAX_CONTEXT:
        raise ValueError(f"{filename} exceeds the startup context limit")
    return data.decode("utf-8")


def resolve(config, name, harness=None):
    if not isinstance(config, dict) or not isinstance(config.get("friends"), dict):
        raise ValueError("identities.json needs a friends object")
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}", name):
        raise ValueError("invalid friend name")
    spec = config["friends"].get(name)
    if not isinstance(spec, dict):
        raise ValueError(f"no home and startup files configured for {name}")
    command = config.get("nova_config", ["nova-config"])
    if not isinstance(command, list) or not command or not all(isinstance(v, str) and v for v in command):
        raise ValueError("nova_config must be a program and its arguments")
    response = json.loads(output([command[0], "friend", "show", name, "--json", *command[1:]]))
    items = response.get("items", [])
    if response.get("result", {}).get("status") != "ok" or len(items) != 1 or items[0].get("kind") != "friend":
        raise ValueError(f"nova-config returned no unique definition for {name}")
    row = items[0]["fields"]
    if row.get("name") != name or row.get("enabled") is not True:
        raise ValueError(f"{name} is missing or disabled in nova-config")
    route = row.get("harness")
    model = row.get("model", "")
    if not isinstance(route, str) or not IDENTIFIER.fullmatch(route) or not isinstance(model, str) or (model and not IDENTIFIER.fullmatch(model)):
        raise ValueError(f"invalid harness or model in the nova-config definition for {name}")
    if harness is not None and harness != route:
        raise ValueError(f"{name} uses the {route} harness in nova-config; the configured driver is {harness}")
    home = spec.get("home")
    files = spec.get("files")
    prefix = spec.get("prefix")
    if not isinstance(home, str) or not Path(home).is_absolute() or not isinstance(prefix, str) or not prefix.strip():
        raise ValueError(f"{name} needs an absolute home checkout and startup prefix")
    if not isinstance(files, list) or not 1 <= len(files) <= 32:
        raise ValueError(f"{name} needs an ordered list of home files")
    git = ["git", "-C", home]
    if Path(output([*git, "rev-parse", "--show-toplevel"]).decode().strip()).resolve() != Path(home).resolve():
        raise ValueError(f"{name}'s home must be the root of its checkout")
    revision = output([*git, "rev-parse", "--verify", "HEAD^{commit}"]).decode().strip()
    definition = {k: row[k] for k in ("name", "harness", "model", "pronouns", "note") if k in row}
    pieces = [prefix, f"Home checkout: {home}", "nova-config friend: " + json.dumps(definition, ensure_ascii=False)]
    for filename in files:
        if not isinstance(filename, str) or not all(re.fullmatch(r"[\w.-]+", p) and p not in (".", "..")
                                                    for p in filename.split("/")):
            raise ValueError("home files must be relative paths inside the checkout")
        entry = output([*git, "ls-tree", "-z", revision, "--", filename])
        if not entry.startswith((b"100644 blob ", b"100755 blob ")):
            raise ValueError(f"{name}: {filename} is not a committed regular file")
        contents = read_home_file(home, filename)
        if not contents.strip() or "\x00" in contents:
            raise ValueError(f"{name}: {filename} must contain UTF-8 text")
        pieces.append(f"# {filename}\n\n{contents}")
        if sum(len(piece.encode()) + 2 for piece in pieces) > MAX_CONTEXT:
            raise ValueError(f"{name}'s startup context exceeds {MAX_CONTEXT} bytes")
    prompt = "\n\n".join(pieces)
    return prompt, {"friend": name, "harness": route, "model": model, "home": home,
                    "revision": revision, "files": files, "sha256": hashlib.sha256(prompt.encode()).hexdigest()}
