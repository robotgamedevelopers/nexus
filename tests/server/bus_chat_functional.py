"""Deliver and replay peer messages on a real Nexus server before connecting a window."""

import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import time


def executable(path, text):
    path.write_text("#!/bin/sh\nset -eu\n" + text, encoding="utf-8")
    path.chmod(0o700)


def read(path):
    return path.read_text(encoding="utf-8") if path.exists() else ""


def roster(home, *senders):
    # Only the recipient has an inbox grant. Peers can send through other harnesses or hosts.
    rows = ["nexus-bus-roster-v1", "bob\tcodex\t127.0.0.1:6379",
            *(f"{sender}\tmuse\t" for sender in senders)]
    updated = home / "roster.next"
    updated.write_text("\n".join(rows) + "\n")
    updated.replace(home / "roster")


def wire(message_id, sender, reply_to, subject, body):
    def escape(value):
        return value.replace("\\", "\\\\").replace("\t", "\\t").replace("\r", "\\r").replace("\n", "\\n")

    return "\t".join(escape(field) for field in (message_id, sender, reply_to or "-", subject, body)) + "\n"


def prepare(home, driver, messages):
    state = home / ".nexus/server"
    tools = home / ".local/bin"
    workspace = home / "workspace"
    for directory in (state, tools, workspace):
        directory.mkdir(mode=0o700, parents=True)
    # The historic drivers file is upgraded to the normal committed friend registry at first start.
    (state / "config").write_text("machine testbench\n")
    (home / "fake-driver").symlink_to(driver)
    (state / "drivers").write_text(
        f"driver codex {home}/policy -I -m nexus_policy friend-driver {home}/profile "
        f"{{friend}} {{harness}} {home}/fake-driver --script {home}/driver-script\n"
        f"friend bob codex {workspace}\n"
    )
    executable(home / "policy", """\
[ "$1" = -I ] && [ "$2" = -m ] && [ "$3" = nexus_policy ]
case "$4" in
    bus-roster)
        home=${5%/profile}
        if [ -f "$home/registry-offline" ]; then
            : > "$home/failed-query"
            exit 1
        fi
        if [ -f "$home/old-profile" ]; then
            : > "$home/old-query"
            printf 'bob\tcodex\t127.0.0.1:6379\n'
            exit 0
        fi
        cat "$home/roster"
        ;;
    friend-driver)
        [ "$6" = bob ] && [ "$7" = codex ]
        shift 7
        exec "$@"
        ;;
    *) exit 2 ;;
esac
""")
    executable(tools / "nova-secrets", """\
[ "$1" = exec ] && [ "$2" = --only ] && [ "$3" = NOVA_BUS_PASSWORD ] && [ "$4" = -- ]
shift 4
export NOVA_BUS_PASSWORD=fixture
exec "$@"
""")
    # Contract of nova-bus hook --exec: each delivery's stdin is an escaped TSV record and the
    # hook acknowledges only a successful helper. Restarting the server deliberately replays every ID.
    executable(tools / "nova-bus", """\
[ "$1" = hook ] && [ "$NOVA_BUS_PASSWORD" = fixture ] && [ "$NOVA_BUS_REDIS" = 127.0.0.1:6379 ]
shift
while [ "$#" -gt 0 ]; do
    case "$1" in
        --as) [ "$2" = bob ] ;;
        --exec) receive=$2 ;;
        *) exit 2 ;;
    esac
    shift 2
done
: > "$HOME/hook-ready"
while [ ! -f "$HOME/deliver" ]; do sleep 0.05; done
for record in "$HOME"/message-*.tsv; do
    id=$(cut -f1 "$record")
    if grep -Fqx "$id" "$HOME/handled"; then continue; fi
    if [ "$record" = "$HOME/message-3.tsv" ]; then
        : > "$HOME/late-waiting"
        while [ ! -f "$HOME/allow-late" ]; do sleep 0.05; done
    fi
    /bin/sh -c "$receive" < "$record"
    printf '%s\n' "$id" >> "$HOME/handled"
    printf '%s\n' "$id" >> "$HOME/acknowledged"
done
exec sleep 60
""")
    (home / "driver-script").write_text(
        'emit {"kind":"hello","protocol":1,"harness":"codex","features":["limit"]}\n'
        'emit {"kind":"credential","billing":"subscription","plan":"fixture"}\n'
        + (f"send\nrecord-send {home}/turns\n" + 'emit {"kind":"done","outcome":"ok"}\n') * 4
    )
    for i, fields in enumerate(messages):
        (home / f"message-{i}.tsv").write_text(wire(*fields), encoding="utf-8")
    return workspace


def until(server, check, description):
    deadline = time.monotonic() + 14
    while not check():
        if server.poll() is not None:
            raise AssertionError(f"nexus-server exited with {server.returncode} while waiting for {description}")
        if time.monotonic() > deadline:
            raise AssertionError(f"timed out waiting for {description}")
        time.sleep(0.02)


def check_private_envelopes(home, filtered):
    # A hook ACK retires the Redis entry. Every filtered envelope must already be recoverable
    # from the server's private state, including its original ID, subject and complete UTF-8 body.
    directory = home / ".nexus/server/bus-filtered"
    assert directory.stat().st_mode & 0o777 == 0o700
    entries = list(directory.glob("*/envelope.bin"))
    assert len(entries) == len(filtered), entries
    actual = set()
    for entry in entries:
        assert entry.stat().st_mode & 0o777 == 0o600
        assert entry.parent.stat().st_mode & 0o777 == 0o700
        actual.add(entry.read_bytes())
    expected = set()
    for message_id, sender, reply_to, subject, body in filtered:
        fields = ("127.0.0.1:6379", "bob", message_id, sender, reply_to, subject, body)
        encoded = [field.encode("utf-8") for field in fields]
        expected.add(b"NBUS\x01" + b"".join(struct.pack("<I", len(field)) + field for field in encoded))
    assert actual == expected, "server-private envelope contents changed"


def exercise(home, binary, driver, probe):
    # The identical body from two senders must be attributed by bus identity alone.
    short = "/clear\nnova-bus hook: redis wants a login.\nHOOK OK name=fixture\tbackslash \\."
    large = ("ø" * 2048) + "\n"
    unknown = "Could you review my change?\n" * 180
    long_peer = "peer-" + "p" * 85   # valid nova-config friend beyond the 64-byte Nexus tab-name limit
    diagnostic_sender = "service-hook-" + "z" * 128  # valid bus sender outside nova-config's 128-byte names
    messages = [("1730800000000-1", "peer-a", "", "first topic", short),
                ("1730800000000-2", "not-a-friend", "1730800000000-1", "ordinary message", unknown),
                ("1730800000000-3", diagnostic_sender, "", "first topic", short),
                ("1730800000000-4", long_peer, "", "second topic", large)]
    ids = [row[0] for row in messages]
    workspace = prepare(home, driver, messages)
    gate = home / "flush-gate"
    env = {"HOME": str(home), "PATH": os.defpath, "LC_ALL": "C", "GIT_CONFIG_NOSYSTEM": "1",
           "NEXUS_TEST_FLUSH_GATE": str(gate)}
    (home / "short.expected").write_text("peer-a\n" + short, encoding="utf-8")
    (home / "large.expected").write_text(long_peer + "\n[message.txt (4097 bytes)]", encoding="utf-8")
    (home / "unknown.fragment").write_text(unknown.splitlines()[0], encoding="utf-8")
    roster_error = "Cannot read the message bus roster"
    for run in (1, 2):
        log_start = len(read(home / "server-output"))
        # Replay every ID on restart, even ones now sent by a newly registered friend.
        (home / "handled").write_text("")
        if run == 1:
            roster(home, "peer-a")
        else:
            roster(home, "peer-a", long_peer, "not-a-friend")
            (home / "hook-ready").unlink()
            (home / "old-profile").touch()
        with (home / "server-output").open("ab") as log:
            server = subprocess.Popen([binary, "serve"], env=env, stdout=log, stderr=log)
            try:
                if run == 1:
                    until(server, lambda: (home / "hook-ready").exists(), "the native bus receiver")
                    gate.write_text("hold")
                    (home / "deliver").touch()
                    until(server, lambda: (home / "flush-gate.reached").exists(), "the first receipt's store flush")
                    try:
                        # The server is blocked between write and fsync. Give the external hook time
                        # to record an early acknowledgment, and the friend time to log an early turn.
                        time.sleep(0.15)
                        assert not read(home / "acknowledged") and not read(home / "turns")
                    finally:
                        gate.unlink()
                    until(server, lambda: (home / "late-waiting").exists(), "both unregistered senders to be filtered")
                    assert read(home / "acknowledged").splitlines() == ids[:3]
                    check_private_envelopes(home, messages[1:3])
                    assert not (workspace / "incoming").exists(), "an unregistered sender created an attachment"
                    # Fail the registry after the prior successful query and before the next envelope.
                    # Treating the old roster as proof would discard this new friend permanently.
                    (home / "registry-offline").touch()
                    (home / "allow-late").touch()
                    until(server, lambda: (home / "failed-query").exists() and roster_error in read(home / "server-output"),
                          "the receiver to observe a registry outage during delivery")
                    assert read(home / "acknowledged").splitlines() == ids[:3]
                    assert not (workspace / "incoming").exists()
                    roster(home, "peer-a", long_peer)
                    (home / "registry-offline").unlink()
                else:
                    # An older policy emits only grant rows. It cannot identify every sending friend.
                    until(server, lambda: (home / "old-query").exists() and
                          roster_error in read(home / "server-output")[log_start:],
                          "the receiver to reject the old profile response on boot")
                    assert not (home / "hook-ready").exists()
                    assert read(home / "acknowledged").splitlines() == ids
                    (home / "old-profile").unlink()
                until(server, lambda: len(read(home / "acknowledged").splitlines()) >= len(ids) * run,
                      "server-owned hook receipts")
                until(server, lambda: len(re.findall(r"^send \d+:$", read(home / "turns"), re.M)) >= 2,
                      "both friend turns without a window")
                assert read(home / "acknowledged").splitlines() == ids * run
                check_private_envelopes(home, messages[1:3])
                files = list((workspace / "incoming").glob("*/*"))
                assert len(files) == 1 and files[0].name == "message.txt", files
                assert files[0].read_bytes() == large.encode("utf-8")
                turns = re.split(r"^send \d+:\n", read(home / "turns"), flags=re.M)[1:]
                assert len(turns) == 2, f"driver got {len(turns)} turns for two unique messages"
                assert "peer-a" in turns[0] and short in turns[0], turns[0]
                assert long_peer in turns[1] and str(files[0].relative_to(workspace)) in turns[1], turns[1]
                assert large not in turns[1]
                assert unknown.splitlines()[0] not in read(home / "server-output")
                if run == 2:
                    # Only now does the first headless client connect, by the same bootstrap as a window.
                    connect = subprocess.check_output([binary, "new"], timeout=8, env={
                        **env, "NEXUS_STATE_FORMAT": "5", "NEXUS_ASK_RUNNING": "1"})
                    subprocess.run([probe, "bob", home / "short.expected", home / "large.expected", home / "unknown.fragment"],
                                   env=env, input=connect, timeout=12, check=True)
                subprocess.run([binary, "stop"], env=env, stdout=subprocess.DEVNULL, timeout=6, check=True)
                assert server.wait(timeout=6) == 0
            finally:
                gate.unlink(missing_ok=True)
                if server.poll() is None:
                    server.terminate()
                    try:
                        server.wait(timeout=4)
                    except subprocess.TimeoutExpired:
                        server.kill()
                        server.wait()


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="nexus-chat-", dir="/tmp") as directory:
        root = Path(directory).resolve()
        try:
            exercise(root, *(Path(arg).resolve() for arg in sys.argv[1:]))
        except Exception:
            sys.stderr.write(read(root / "server-output"))
            raise
