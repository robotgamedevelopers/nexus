"""Nexus's server-side driver_start policy hook. Run with python -I -m nexus_policy."""
import argparse
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import tempfile

from friend_identity import bus_grants, bus_roster, resolve

import starlark


MAX_SOURCE = 64 * 1024
EFFORTS = ("low", "medium", "high", "xhigh", "max")


def publish_friend_info(identity, cwd, model, effort):
    directory = Path.home() / ".nexus/server/friend-info"
    directory.mkdir(mode=0o700, parents=True, exist_ok=True)
    data = {"pronouns": identity.get("pronouns", ""), "repository": identity["repository"], "directories": list(dict.fromkeys(
        path for path in [cwd, *identity["writable_dirs"]] if path)), "model": model, "effort": effort}
    with tempfile.NamedTemporaryFile(mode="w", dir=directory, delete=False) as temp:
        try:
            json.dump(data, temp)
            temp.flush()
            os.replace(temp.name, directory / (identity["friend"] + ".json"))
        finally:
            Path(temp.name).unlink(missing_ok=True)


def read_profile_file(directory, name):
    fd = os.open(name, os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW, dir_fd=directory)
    with os.fdopen(fd, "rb") as stream:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
            raise ValueError(f"{name} must be a regular file with one link")
        data = stream.read(MAX_SOURCE + 1)
    if len(data) > MAX_SOURCE:
        raise ValueError(f"{name} exceeds {MAX_SOURCE} bytes")
    return data.decode("utf-8")


def settings(answer):
    if not isinstance(answer, dict) or answer.keys() - {"model", "effort"}:
        raise ValueError("driver_start must return a dictionary with only model and effort")
    if "model" in answer and (not isinstance(answer["model"], str) or
                              not re.fullmatch(r"[a-zA-Z0-9][a-zA-Z0-9._:/-]{0,63}", answer["model"])):
        raise ValueError("model must be a model identifier of 1 to 64 characters")
    if "effort" in answer and answer["effort"] not in EFFORTS:
        raise ValueError("effort must be one of " + ", ".join(EFFORTS))
    return answer


def evaluate(profile, context):
    directory = os.open(profile, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        source = read_profile_file(directory, "policy.star")
        config = json.loads(read_profile_file(directory, "config.json"))
    finally:
        os.close(directory)
    if not isinstance(config, dict):
        raise ValueError("config.json must contain a JSON object")
    # The host supplies plain, frozen data. No loader or host callbacks. A script
    # cannot print a protocol line. Both module initialization and the hook are metered.
    module = starlark.exec_file(
        source + "\n_nexus_answer = driver_start(_nexus_context, _nexus_config)\n",
        filename="policy.star",
        predeclared={"_nexus_context": starlark.to_value(context), "_nexus_config": starlark.to_value(config)},
        universal={"print": None}, max_steps=50000, max_allocs=2 * 1024 * 1024)
    answer = module.get("_nexus_answer")
    if not isinstance(answer, starlark.Dict) or len(answer) > 2:
        raise ValueError("driver_start must return a dictionary with only model and effort")
    return settings(dict(answer.items()))


def decision(profile, context):
    # Bound evaluation independently of the real driver's lifetime and its process
    # limits. The worker receives only the public context on stdin.
    run = subprocess.run([sys.executable, "-I", "-m", "nexus_policy", "_evaluate", str(profile)],
                         input=json.dumps(context), capture_output=True, text=True, timeout=3,
                         cwd="/", env={})
    if run.returncode:
        raise ValueError(run.stderr.strip()[:500] or f"policy evaluator exited {run.returncode}")
    return settings(json.loads(run.stdout))


def cli():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    check = commands.add_parser("check", help="show one decision without starting a driver")
    check.add_argument("profile", type=Path)
    check.add_argument("friend")
    check.add_argument("--account", default="")
    check.add_argument("--resuming", action="store_true")
    driver = commands.add_parser("driver", help="apply driver_start, then run the configured Claude driver")
    driver.add_argument("profile", type=Path)
    driver.add_argument("friend")
    driver.add_argument("node")
    driver.add_argument("entry")
    driver.add_argument("arguments", nargs=argparse.REMAINDER)
    bus = commands.add_parser("bus-grants", help="list enabled friend, harness and loopback endpoint grants for nexus-server")
    bus.add_argument("profile", type=Path)
    roster = commands.add_parser("bus-roster", help="list enabled registry friends and their server-owned inboxes")
    roster.add_argument("profile", type=Path)
    inspect = commands.add_parser("identity", help="verify the friend definition and home without starting a model")
    inspect.add_argument("profile", type=Path)
    inspect.add_argument("friend")
    inspect.add_argument("--harness")
    launch = commands.add_parser("friend-driver", help="start a Nexus driver with its friend's standing instructions")
    launch.add_argument("profile", type=Path)
    launch.add_argument("friend")
    launch.add_argument("harness")
    launch.add_argument("program")
    launch.add_argument("arguments", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    try:
        if args.command in ("identity", "friend-driver", "bus-grants", "bus-roster"):
            directory = os.open(args.profile, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
            try:
                config = json.loads(read_profile_file(directory, "identities.json"))
            finally:
                os.close(directory)
            if args.command in ("bus-grants", "bus-roster"):
                if args.command == "bus-roster":
                    rows = bus_roster(config)
                    print("nexus-bus-roster-v1")
                else:
                    rows = bus_grants(config)
                for row in rows:
                    print("\t".join(row))
                return 0
            instructions, identity = resolve(config, args.friend, args.harness)
            if args.command == "identity":
                print(json.dumps(identity))
                return 0
            # Driver programs may have leading arguments (e.g. a Node entry point).
            def option(key):
                for pos in range(len(args.arguments) - 2, -1, -1):
                    if args.arguments[pos] == key:
                        return args.arguments[pos + 1]
                return ""
            context = {"friend": args.friend, "provider": args.harness, "account": option("--account"),
                       "resuming": bool(option("--resume")), "identity": identity}
            defaults = decision(args.profile.absolute(), context)
            if identity["model"] and identity["model"] != identity["harness"]:
                defaults.setdefault("model", identity["model"])
            extra = [item for key, value in defaults.items() if not option("--" + key)
                     for item in ("--" + key, value)]
            if args.harness == "claude" and not option("--limit"):
                extra.extend(["--limit", "cwd"])
            for path in identity["writable_dirs"]:
                extra.extend(["--write-dir", path])
            # Existing registry rows keep their driver listener until the inbox owner transfers it.
            if identity["bus_listener"] and not identity["bus_owner"]:
                if args.harness != "claude":
                    raise ValueError("a driver-owned bus_listener requires a Claude friend")
                extra.extend(["--bus-listener", args.friend + "@" + identity["bus_listener"]])
            if identity["bus_owner"] == "nexus-server" and args.harness == "claude":
                extra.extend(["--bus-sender", args.friend + "@" + identity["bus_listener"]])
            if identity["readable_dirs"] or identity["github_ssh_alias"]:
                if args.harness != "claude":
                    raise ValueError("this harness does not implement read-only directory and GitHub grants")
                for path in identity["readable_dirs"]:
                    extra.extend(["--read-dir", path])
                alias = identity["github_ssh_alias"]
                if alias:
                    repo = re.fullmatch(r"https://github[.]com/([A-Za-z0-9_-]+/[A-Za-z0-9_.-]+)", identity["repository"])
                    if not repo or not re.fullmatch(r"[a-z0-9][a-z0-9-]*", alias):
                        raise ValueError("invalid GitHub SSH grant")
                    extra.extend(["--git-remote", f"git@{alias}:{repo[1].removesuffix('.git')}.git"])
            publish_friend_info(identity, option("--cwd"), defaults.get("model", ""), defaults.get("effort", ""))
            with tempfile.TemporaryFile() as descriptor:
                descriptor.write(instructions.encode())
                descriptor.seek(0)
                os.set_inheritable(descriptor.fileno(), True)
                os.execv(args.program, [args.program, *args.arguments, *extra,
                                        "--startup-context-fd", str(descriptor.fileno())])
        if args.command == "check":
            context = {"friend": args.friend, "provider": "claude", "account": args.account, "resuming": args.resuming}
            print(json.dumps(decision(args.profile.absolute(), context), sort_keys=True))
            return 0
        # The driver takes pairs. Let its own parser validate them, unchanged.
        flags = dict(zip(args.arguments[::2], args.arguments[1::2]))
        context = {"friend": args.friend, "provider": "claude", "account": flags.get("--account", ""),
                   "resuming": "--resume" in flags}
        answer = decision(args.profile.absolute(), context)
        defaults = [item for key, value in answer.items() if "--" + key not in flags
                    for item in ("--" + key, value)]
        os.execv(args.node, [args.node, args.entry, *defaults, *args.arguments])
    except (OSError, ValueError, KeyError, TypeError, subprocess.TimeoutExpired) as error:
        cause = "Nexus driver policy: " + " ".join(str(error).splitlines())[:500]
        if args.command in ("driver", "friend-driver"):
            harness = getattr(args, "harness", "claude")
            print(json.dumps({"kind": "hello", "protocol": 1, "harness": harness,
                              "features": ["limit"] if harness == "claude" else []}))
            print(json.dumps({"kind": "error", "code": "other", "cause": cause,
                              "fix": "Check the server profile with nexus_policy check and retry."}))
        else:
            print(cause, file=sys.stderr)
        return 1


if __name__ == "__main__":
    if sys.argv[1:2] == ["_evaluate"] and len(sys.argv) == 3:
        import resource
        try:
            resource.setrlimit(resource.RLIMIT_CPU, (2, 2))
            if sys.platform.startswith("linux"):
                memory = 256 * 1024 * 1024
                resource.setrlimit(resource.RLIMIT_AS, (memory, memory))
            print(json.dumps(evaluate(sys.argv[2], json.load(sys.stdin))))
        except Exception as exc:
            print(" ".join(str(exc).splitlines())[:500], file=sys.stderr)
            sys.exit(1)
    else:
        sys.exit(cli())
