#!/usr/bin/env bash
# `ssh localhost` for the link's tests on a fleet Linux runner, with nothing written outside this job's own directory.
#
# The link's functional tests reach this machine over ssh (tests/link/CMakeLists.txt). A fleet Linux runner's login
# (ghrunner, or the fleet login inside captain's WSL2) has no ssh login of its own to this machine: the host's sshd
# refuses it (DenyUsers), and its ~/.ssh is the host's, not this job's to change. So this starts an sshd of the job's
# own, as the runner's login, and writes an ssh wrapper that reaches it as `localhost` (its own config: port, key,
# known host). The tests run the wrapper because NEXUS_E2E_SSH names it (tests/link/link_e2e.cpp); the product's own
# ssh is untouched. Ruled fine by the coordinator, 2026-11-26, on these terms, which this script keeps:
#
#   - it binds 127.0.0.1 only (ListenAddress 127.0.0.1:<free port>), so it adds no network access;
#   - its host key and its one client key are made per job, here, under DIR, and it accepts that key for the runner's
#     own login alone (AllowUsers; no passwords, no forwarding); it grants nothing the job's login does not already have;
#   - it is stopped by PID in an always() step (`--stop DIR`, below), so it dies with the job even when a test fails.
#
#   bash .github/ssh-localhost.sh DIR          starts it under DIR (made here, 0700) and writes NEXUS_E2E_SSH to
#                                              $GITHUB_ENV (or prints it when GITHUB_ENV is unset); its pid is DIR/sshd.pid
#   bash .github/ssh-localhost.sh --stop DIR   stops it by that PID and fails if it is still running (nothing to stop: a
#                                              line saying so); every job that starts it runs this in an `if: always()`
#                                              step
set -euo pipefail
if [ "${1-}" = --stop ]; then
  dir=${2:?usage: ssh-localhost.sh --stop DIR}
  if [ ! -f "$dir/sshd.pid" ]; then
    echo "no sshd.pid under $dir: this job's sshd never started, nothing to stop"
    exit 0
  fi
  pid=$(cat "$dir/sshd.pid")
  if ! kill -0 "$pid" 2>/dev/null; then
    echo "this job's sshd (pid $pid) has already ended"
    exit 0
  fi
  kill "$pid"
  for _ in $(seq 50); do
    if ! kill -0 "$pid" 2>/dev/null; then echo "stopped this job's sshd (pid $pid)"; exit 0; fi
    sleep 0.1
  done
  echo "::error::this job's sshd (pid $pid) is still running 5 s after SIGTERM"
  exit 1
fi
dir=${1:?usage: ssh-localhost.sh DIR}
sshd=/usr/sbin/sshd
if [ ! -x "$sshd" ]; then
  echo "::error::no $sshd on $(hostname -s): openssh-server is a runner package (nova-fleet runner-packages)"
  exit 1
fi
if [ -f "$dir/sshd.pid" ]; then
  kill "$(cat "$dir/sshd.pid")" 2>/dev/null || true
fi
rm -rf "$dir"
mkdir -p "$dir"
chmod 700 "$dir"
dir=$(cd "$dir" && pwd -P)
me=$(id -un)
ssh-keygen -q -t ed25519 -N '' -C "nexus-ci-host" -f "$dir/host_ed25519"
ssh-keygen -q -t ed25519 -N '' -C "nexus-ci-client" -f "$dir/id_ed25519"
cp "$dir/id_ed25519.pub" "$dir/authorized_keys"
chmod 600 "$dir/authorized_keys"
# A free port: the kernel's pick for a socket bound and closed at once (a race with another job is caught below: sshd
# then fails to bind and the check fails by name).
port=$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()')
cat > "$dir/sshd_config" <<EOF
ListenAddress 127.0.0.1:$port
HostKey $dir/host_ed25519
PidFile $dir/sshd.pid
AuthorizedKeysFile $dir/authorized_keys
AllowUsers $me
PubkeyAuthentication yes
PasswordAuthentication no
KbdInteractiveAuthentication no
UsePAM no
StrictModes no
PermitUserEnvironment no
X11Forwarding no
AllowTcpForwarding no
LogLevel ERROR
EOF
"$sshd" -t -f "$dir/sshd_config"
"$sshd" -f "$dir/sshd_config" -E "$dir/sshd.log"
printf '[127.0.0.1]:%s %s\n' "$port" "$(cut -d' ' -f1-2 "$dir/host_ed25519.pub")" > "$dir/known_hosts"
cat > "$dir/ssh_config" <<EOF
Host localhost
  HostName 127.0.0.1
  Port $port
  User $me
  IdentityFile $dir/id_ed25519
  IdentitiesOnly yes
  UserKnownHostsFile $dir/known_hosts
  GlobalKnownHostsFile /dev/null
  StrictHostKeyChecking yes
  CheckHostIP no
EOF
cat > "$dir/ssh" <<EOF
#!/bin/sh
exec /usr/bin/ssh -F "$dir/ssh_config" "\$@"
EOF
chmod 755 "$dir/ssh"
for _ in $(seq 50); do
  if "$dir/ssh" -o BatchMode=yes -o ConnectTimeout=5 localhost true 2>/dev/null; then
    echo "ssh localhost works for $me through this job's sshd on 127.0.0.1:$port ($dir/ssh)"
    if [ -n "${GITHUB_ENV:-}" ]; then echo "NEXUS_E2E_SSH=$dir/ssh" >> "$GITHUB_ENV"; else echo "NEXUS_E2E_SSH=$dir/ssh"; fi
    exit 0
  fi
  sleep 0.1
done
echo "::error::this job's sshd on 127.0.0.1:$port does not take $me's key"
cat "$dir/sshd.log" || true
"$dir/ssh" -v -o BatchMode=yes -o ConnectTimeout=5 localhost true || true
exit 1
