"""Exercise the actual offline inventory translator without starting fleet services."""
import json
import subprocess
import sys

tool = sys.argv[1]
home = "/srv/fleet account"


def quoted(s):
    return {"__ansible_unsafe": s}


def service(name, argv, **fields):
    return dict(name=name, argv=[quoted(a) for a in argv], enabled=False, every=0,
                keepalive=True, env={}, keys=[], seat="", **fields)


worker = service("sprint-worker", ["nova-sprint", "worker", "--", "${not-a-shell}"], log=quoted("~/worker log"))
worker.update(env={"NOVA_LABEL": quoted("{{literal}}")}, seat="tools", keys=["MODEL_KEY"])
dispatch = service("scheduled", ["nova-bus", "who"])
dispatch.update(every=90, keepalive=False)
external = service("external", ["/bin/false"])
external["enabled"] = True


def project(rows, names):
    inv = {"_meta": {"hostvars": {"node": {"nova_services": rows, "nova_os": "darwin"},
                                   "remote": {"nova_services": [external]}}}}
    args = [sys.executable, tool, "--host", "node", "--home", home]
    for name in names:
        args += ["--take", name]
    return subprocess.run(args, input=json.dumps(inv), capture_output=True, text=True, timeout=5)


result = project([worker, dispatch, external], ["scheduled", "sprint-worker"])
assert result.returncode == 0, result.stderr
jobs = json.loads(result.stdout)["services"]
assert len(jobs) == 2 and jobs[0]["every"] == 90 and jobs[0]["owner"] == "nexus-server"
assert jobs[0]["argv"] == [home + "/.local/bin/nova-bus", "who"]
assert jobs[1]["env"]["NOVA_LABEL"] == "{{literal}}" and jobs[1]["log"] == home + "/worker log"
assert jobs[1]["argv"] == [home + "/.local/bin/nova-secrets", "exec", "--store", home + "/.local/share/nova/secrets",
                           "--as", "tools", "--key", home + "/.config/nova-secrets/tools.key", "--sops",
                           "/opt/homebrew/bin/sops", "--only", "MODEL_KEY", "--", "nova-sprint", "worker",
                           "--", "${not-a-shell}"]
for rows, names in (([external], ["external"]), ([worker], ["missing"]), ([worker, worker], ["sprint-worker"]),
                    ([dict(worker, seat="../other")], ["sprint-worker"]),
                    ([dict(worker, env={"MODEL_KEY": quoted("literal-secret")})], ["sprint-worker"]),
                    ([dict(worker, env={"PATH": quoted(":/bin")})], ["sprint-worker"])):
    result = project(rows, names)
    assert result.returncode != 0 and not result.stdout and "literal-secret" not in result.stderr, result
