"""Monitor's closed stdin leaves the receive-only bus stream asleep and connected."""
import os
import resource
import select
import socket
import subprocess
import sys
import tempfile
from pathlib import Path


def test(wrapper):
    with tempfile.TemporaryDirectory(dir="/tmp") as temp, socket.socket(socket.AF_UNIX) as server:
        path = str(Path(temp) / "listen")
        server.bind(path)
        server.listen(1)
        server.settimeout(3)
        env = dict(os.environ, NEXUS_BUS_FRIEND="friend", NEXUS_BUS_LISTENER=path)
        cpu_before = resource.getrusage(resource.RUSAGE_CHILDREN)
        child = subprocess.Popen([wrapper, "hook", "--as", "friend"], stdin=subprocess.DEVNULL,
                                 stdout=subprocess.PIPE, env=env)
        try:
            conn, _ = server.accept()
            with conn:
                # A half-close would tell the Node broker to tear down its listener. During
                # this interval an idle bridge also has no reason to spend CPU time.
                assert not select.select([conn], [], [], 0.7)[0], "bridge wrote or half-closed the socket"
                message = b"fixture\n"
                conn.sendall(message)
                assert select.select([child.stdout], [], [], 3)[0], "bridge buffered a live message"
                assert child.stdout.read(len(message)) == message
            assert child.wait(timeout=3) == 0, "bridge must exit on broker EOF"
        finally:
            if child.poll() is None:
                child.kill()
                child.wait()
            child.stdout.close()
        cpu_after = resource.getrusage(resource.RUSAGE_CHILDREN)
        used = cpu_after.ru_utime + cpu_after.ru_stime - cpu_before.ru_utime - cpu_before.ru_stime
        assert used < 0.2, f"idle bridge used {used:.3f}s CPU during 0.7s quiet"


if __name__ == "__main__":
    test(sys.argv[1])
