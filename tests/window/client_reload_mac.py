"""Exercise a real window across signed app replacements. Run with a Ninja build directory on macOS."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
BUILD = Path(sys.argv[1]).resolve()
SOURCE = BUILD / "nexus.app"
INSTALL = ROOT / "tools/nexus-update-client"
MARKER = Path("Contents/Resources/client-module.txt")


def run(*cmd):
    subprocess.run(cmd, check=True)


def fixture(directory, number):
    directory.mkdir()
    app = directory / "nexus.app"
    run("/usr/bin/ditto", SOURCE, app)
    abi, old = (app / MARKER).read_text().splitlines()
    revision = str(number) * 64
    plugins = app / "Contents/PlugIns"
    (plugins / f"nexus-client-{old}.bundle").unlink()
    module = plugins / f"nexus-client-{revision}.bundle"
    shutil.copy2(BUILD / "tests/window" / f"nexus_reload_fixture_{number}.bundle", module)
    (app / MARKER).write_text(f"{abi}\n{revision}\n")
    run("/usr/bin/codesign", "--force", "--sign", "-", module)
    run("/usr/bin/codesign", "--force", "--sign", "-", app)
    return app


def wait_for(client, log, message):
    deadline = time.monotonic() + 18
    while time.monotonic() < deadline and client.poll() is None:
        if message in log.read_text():
            return
        time.sleep(0.1)
    raise RuntimeError(f"missing {message!r} in client output\n{log.read_text()}")


run("cmake", "--build", BUILD, "--target", "nexus", "nexus_reload_fixture_1", "nexus_reload_fixture_2")
with tempfile.TemporaryDirectory(prefix="nexus-window-update-") as temp:
    root = Path(temp)
    first, second = (fixture(root / str(i), i) for i in (1, 2))
    installed = root / "nexus.app"
    run(INSTALL, first, installed)
    home = root / "home"
    home.mkdir()
    log = root / "window.log"
    with log.open("w") as output:
        client = subprocess.Popen([installed / "Contents/MacOS/nexus", "--windowed", "--demo"],
                                  env=dict(os.environ, HOME=str(home)), stdout=output, stderr=output)
        try:
            wait_for(client, log, "fixture draft ready")
            before = (installed / MARKER).read_bytes()
            damaged = root / "damaged.app"
            run("/usr/bin/ditto", second, damaged)
            (damaged / MARKER).write_text("damaged")
            assert subprocess.run([INSTALL, damaged, installed], capture_output=True).returncode != 0
            assert before == (installed / MARKER).read_bytes()
            incompatible = root / "incompatible.app"
            run("/usr/bin/ditto", second, incompatible)
            _, revision = (incompatible / MARKER).read_text().splitlines()
            (incompatible / MARKER).write_text("0" * 64 + "\n" + revision + "\n")
            run("/usr/bin/codesign", "--force", "--sign", "-", incompatible)
            run(INSTALL, incompatible, installed)
            wait_for(client, log, "needs a new client host")
            run(INSTALL, second, installed)
            wait_for(client, log, "fixture tab and draft preserved")
            run(INSTALL, SOURCE, installed)
            original = (SOURCE / MARKER).read_text().splitlines()[1][:12]
            wait_for(client, log, f"applied native client module {original}")
            print(f"Passed. Client PID {client.pid} kept its selected tab and unsent draft through the update.")
        finally:
            if client.poll() is None:
                client.terminate()
            client.wait(timeout=10)
