"""Nexus's server-side driver_start policy hook. Run with python -I -m nexus_policy."""
import argparse
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys

import starlark


MAX_SOURCE = 64 * 1024
EFFORTS = ("low", "medium", "high", "xhigh", "max")


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
    args = parser.parse_args()
    try:
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
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        cause = "Nexus driver policy: " + " ".join(str(error).splitlines())[:500]
        if args.command == "driver":
            print(json.dumps({"kind": "hello", "protocol": 1, "harness": "claude", "features": ["limit"]}))
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
