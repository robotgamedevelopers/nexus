"""Resolve a configured AI's registry row and tracked home documents at driver start."""
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
        raise ValueError(f"{Path(argv[0]).name} could not read the configured AI definition or home revision")
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


def registry_items(config, *arguments):
    if not isinstance(config, dict):
        raise ValueError("identities.json must be an object")
    command = config.get("nova_config", ["nova-config"])
    if not isinstance(command, list) or not command or not all(isinstance(v, str) and v for v in command):
        raise ValueError("nova_config must be a program and its arguments")
    # nova-config names its registry rows "friend"; the word stays only at this boundary.
    response = json.loads(output([command[0], "friend", *arguments, "--json", *command[1:]]))
    if (not isinstance(response, dict) or not isinstance(response.get("result"), dict)
            or response["result"].get("status") != "ok" or not isinstance(response.get("items"), list)):
        raise ValueError("nova-config could not read the AI registry")
    return response["items"]


def bus_endpoint(address):
    if address == "":
        return ""
    if (not isinstance(address, str) or not re.fullmatch(r"127[.]0[.]0[.]1:[1-9][0-9]{0,4}", address)
            or int(address.rsplit(":", 1)[1]) > 65535):
        raise ValueError("startup.bus_listener must be 127.0.0.1:<port> with port 1 through 65535")
    return address


def native_bus_endpoint(startup):
    address = bus_endpoint(startup.get("bus_listener", ""))
    if not address:
        raise ValueError("startup.bus_owner requires startup.bus_listener for that AI")
    return address


def bus_owner(startup):
    owner = startup.get("bus_owner", "")
    if owner not in ("", "nexus-server"):
        raise ValueError("startup.bus_owner must be nexus-server or omitted")
    if owner:
        native_bus_endpoint(startup)
    return owner


def bus_roster(config, include_policy=False):
    roster = []
    names = set()
    for item in registry_items(config, "list"):
        if not isinstance(item, dict) or not isinstance(item.get("kind"), str):
            raise ValueError("nova-config returned an invalid AI registry item")
        # nova-config's row kind is "friend"; the word stays only at this boundary.
        if item["kind"] != "friend":
            continue
        row = item.get("fields")
        if not isinstance(row, dict) or not isinstance(row.get("enabled"), bool):
            raise ValueError("nova-config returned invalid AI registry fields")
        if not row["enabled"]:
            continue
        name, harness = row.get("name"), row.get("harness")
        if (not isinstance(name, str) or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,127}", name)
                or not isinstance(harness, str) or not IDENTIFIER.fullmatch(harness)):
            raise ValueError("invalid AI name or harness in the message bus roster")
        if name in names:
            raise ValueError("nova-config returned duplicate enabled rows for one AI")
        names.add(name)
        startup = row.get("startup")
        if startup is None:
            startup = {}
        if not isinstance(startup, dict):
            raise ValueError("the AI registry startup field must be an object")
        address = native_bus_endpoint(startup) if startup.get("bus_owner") == "nexus-server" else ""
        row = (name, harness, address)
        if include_policy:
            mode = startup.get("no_sandbox", False)
            if type(mode) is not bool or (mode and harness != "claude"):
                raise ValueError("invalid no_sandbox registry setting")
            row += ("no-sandbox" if mode else "sandbox",)
        roster.append(row)
    return roster


def bus_grants(config):
    return [row for row in bus_roster(config) if row[2]]


def resolve(config, name, harness=None):
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}", name):
        raise ValueError("invalid AI name")
    items = registry_items(config, "show", name)
    # nova-config's row kind is "friend"; the word stays only at this boundary.
    if len(items) != 1 or items[0].get("kind") != "friend":
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
    spec = row.get("startup")
    if not isinstance(spec, dict) or not spec:
        raise ValueError(f"no startup definition for {name} in nova-config")
    no_sandbox = spec.get("no_sandbox", False)
    if type(no_sandbox) is not bool or (no_sandbox and route != "claude"):
        raise ValueError("no_sandbox must be boolean and requires a supported Claude driver when enabled")
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
    # Names nova-config's own record ("nova-config friend"), so the word stays at this boundary.
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
    def directories(key):
        roots = spec.get(key, [])
        if not isinstance(roots, list) or any(not isinstance(d, str) or not os.path.isabs(d) or
                os.path.normpath(d) != d or d == "/" or any(c in d for c in "\x00\r\n*?[]") for d in roots):
            raise ValueError(f"{name}: {key} must be a list of absolute directories")
        return roots
    prompt = "\n\n".join(pieces)
    return prompt, {"no_sandbox": no_sandbox, "ai": name, "harness": route, "model": model, "home": home,
                    "repository": spec.get("repository", ""), "pronouns": row.get("pronouns", ""),
                    "writable_dirs": directories("writable_dirs"), "readable_dirs": directories("readable_dirs"),
                    "bus_listener": bus_endpoint(spec.get("bus_listener", "")), "bus_owner": bus_owner(spec),
                    "github_ssh_alias": spec.get("github_ssh_alias", ""), "revision": revision, "files": files,
                    "sha256": hashlib.sha256(prompt.encode()).hexdigest()}
