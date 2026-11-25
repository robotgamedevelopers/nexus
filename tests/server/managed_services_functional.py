"""Drive Nexus's service lifecycle with a temporary server, home and fixture children."""
import contextlib
import fcntl
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time


ARGUMENT = "a literal $HOME and a space"


def fixture(name, argument):
    work = Path.cwd()
    events = work / (name + ".events")

    def record(event):
        with events.open("a", encoding="utf-8") as out:
            print(event, os.getpid(), file=out)

    if argument != ARGUMENT or os.environ.get("SERVICE_MARKER") != "per row":
        record("bad-configuration")
        return 1
    if "NEXUS_SERVICE_TEST_PRIVATE" in os.environ:
        record("inherited-environment")
        return 1
    with (work / (name + ".lock")).open("a") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            record("overlap")
            return 1

        def stop(*_args):
            record("terminated")
            raise SystemExit(0)

        signal.signal(signal.SIGTERM, stop)
        record("started")
        print(f"running {name}", flush=True)
        if name == "agent" and not (work / "failed-once").exists():
            (work / "failed-once").touch()
            return 7
        # A failed test cannot leave the fixture alive indefinitely.
        deadline = time.monotonic() + 35
        while time.monotonic() < deadline:
            if name == "scheduled" and (work / "release-job").exists():
                record("finished")
                return 0
            time.sleep(0.04)
        return 1


def wait_until(label, condition, timeout=12):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = condition()
        if value:
            return value
        time.sleep(0.05)
    raise AssertionError(f"Timed out waiting for {label}")


def test_server(binary, box):
    server_dir = box / "server"
    work = box / "fixture"
    work.mkdir()
    env = {"HOME": str(box), "PATH": "/usr/bin:/bin", "NEXUS_SERVICE_TEST_PRIVATE": "parent-only"}
    log = box / "server.log"
    manifest = server_dir / "services.json"

    def cli(*command):
        return subprocess.run([binary, *command, "--dir", server_dir], env=env, cwd=box,
                              capture_output=True, text=True, timeout=5)

    def expect_success(*command):
        result = cli(*command)
        assert result.returncode == 0, (command, result.stdout, result.stderr)
        return result.stdout

    @contextlib.contextmanager
    def serving():
        with log.open("a") as output:
            proc = subprocess.Popen([binary, "serve", "--dir", server_dir], env=env, cwd=box,
                                    stdout=output, stderr=output)
            try:
                def ready():
                    assert proc.poll() is None, log.read_text()
                    return (server_dir / "server.sock").exists() and cli("status").returncode == 0
                wait_until("server startup", ready)
                yield proc
            finally:
                if proc.poll() is None:
                    try:
                        expect_success("stop")
                        assert proc.wait(timeout=8) == 0, log.read_text()
                    finally:
                        if proc.poll() is None:
                            proc.kill()
                            proc.wait(timeout=5)

    def row(name, **overrides):
        return {
            "name": name, "owner": "nexus-server", "cwd": str(work), "log": str(work / (name + ".log")),
            "argv": [sys.executable, "-I", str(Path(__file__).resolve()), "fixture", name, ARGUMENT],
            "env": {"HOME": str(box), "PATH": "/usr/bin:/bin", "SERVICE_MARKER": "per row"},
            **overrides,
        }

    def configure(rows):
        temp = manifest.with_suffix(".tmp")
        temp.write_text(json.dumps({"version": 1, "services": rows}))
        temp.replace(manifest)

    def pids(name, event):
        path = work / (name + ".events")
        lines = path.read_text().splitlines() if path.exists() else []
        return [int(line.split()[1]) for line in lines if line.startswith(event + " ")]

    def gone(pid):
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return True
        return False

    agent, timer = row("agent"), row("scheduled", every=1)
    external, disabled = row("external", owner="host"), row("disabled", enabled=False)
    rows = [agent, timer, external, disabled]
    try:
        with serving():
            assert not manifest.exists()
            assert "no managed services" in expect_success("services", "status")
            assert not list(work.glob("*.events"))
            configure(rows)
            expect_success("services", "check")
            expect_success("services", "reload")
            wait_until("failed agent to restart", lambda: len(pids("agent", "started")) == 2)
            wait_until("scheduled child", lambda: pids("scheduled", "started"))
            live_agent = pids("agent", "started")[1]
            assert not gone(live_agent)
            status = expect_success("services", "status")
            assert "service agent " in status and "service scheduled " in status, status
            assert "service external " not in status and "service disabled " not in status, status
            # A bad replacement is rejected as a whole; the previous children are still owned.
            configure([row("new"), row("invalid", argv=[])])
            assert cli("services", "check").returncode != 0
            assert cli("services", "reload").returncode != 0
            configure(rows)
            expect_success("services", "reload")
            assert not gone(live_agent)
            # The first scheduled process spans multiple periods, then releases the next run.
            time.sleep(2.1)
            assert len(pids("scheduled", "started")) == 1
            (work / "release-job").touch()
            wait_until("next scheduled run", lambda: len(pids("scheduled", "finished")) >= 2)
            assert not pids("scheduled", "overlap")
            # Replace one service and retain the other without restarting it.
            configure([agent, row("replacement"), external, disabled])
            expect_success("services", "reload")
            replacement = wait_until("replacement service", lambda: pids("replacement", "started"))[0]
            assert pids("agent", "started")[1:] == [live_agent] and not gone(live_agent)
        for name, pid in [("agent", live_agent), ("replacement", replacement)]:
            assert pid in pids(name, "terminated") and gone(pid), (name, pid)
            assert f"running {name}" in (work / (name + ".log")).read_text()
        # On the next boot the same manifest starts one copy of each enabled owned service.
        with serving():
            wait_until("agent after server restart", lambda: len(pids("agent", "started")) == 3)
            wait_until("replacement after server restart", lambda: len(pids("replacement", "started")) == 2)
        for name in ("external", "disabled", "new"):
            assert not (work / (name + ".events")).exists(), name
        for name in ("agent", "scheduled", "replacement"):
            assert not pids(name, "overlap") and all(gone(pid) for pid in pids(name, "started")), name
    except BaseException:
        if log.exists():
            print(log.read_text(), file=sys.stderr)
        for path in sorted(work.glob("*.events")):
            print(path.name, path.read_text(), file=sys.stderr)
        raise


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "fixture":
        sys.exit(fixture(*sys.argv[2:]))
    with tempfile.TemporaryDirectory(prefix="svc-", dir="/tmp") as temp_dir:
        test_server(Path(sys.argv[1]).resolve(), Path(temp_dir).resolve())
