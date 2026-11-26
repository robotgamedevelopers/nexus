#!/bin/sh
# The server side of ios_ssh_functional (tests/ios/ssh_functional.cpp), on the building Mac: an sshd of the test's own,
# on 127.0.0.1 only, run as this user with its own host key, config and authorized_keys in DIR, and nothing of the
# machine's (no ~/.ssh, no PAM, no password). The one key it takes is one this script makes for the test's client
# (DIR/client/ssh-key, the device key's form), behind tools/nexus-ios-gate, whose login shell is a stand-in that runs
# a stand-in nexus-server (DIR/bin) printing a CONNECT line. So the test runs the in-app ssh end to end against a real
# sshd and the real gate, and nothing else runs.
#
#   sh tests/ios/sshd_fixture.sh start DIR GATE     writes DIR/port and DIR/user, and starts sshd
#   sh tests/ios/sshd_fixture.sh stop DIR           stops the sshd started in DIR (by its pid file), and removes DIR
set -eu
what=$1
dir=$2

if [ "$what" = stop ]; then
    if [ -f "$dir/sshd.pid" ]; then
        kill "$(cat "$dir/sshd.pid")" 2>/dev/null || true
    fi
    rm -rf "$dir"
    exit 0
fi
[ "$what" = start ] || { echo "usage: sshd_fixture.sh start DIR GATE | stop DIR" >&2; exit 2; }
gate=$3
if [ -f "$dir/sshd.pid" ]; then
    kill "$(cat "$dir/sshd.pid")" 2>/dev/null || true
fi
rm -rf "$dir"
mkdir -p "$dir/client" "$dir/bin"
chmod 700 "$dir" "$dir/client"

ssh-keygen -q -t ecdsa -b 256 -N '' -C fixture-host -f "$dir/host_key"
# The client's key in the form the app keeps it: SEC1 PEM (`EC PRIVATE KEY`) naming its curve, mode 0600. ssh-keygen's
# PEM spells the curve's parameters out, which the app's mbedTLS does not read, so openssl writes it again.
ssh-keygen -q -t ecdsa -b 256 -m PEM -N '' -C nexus-ios -f "$dir/client/ssh-key"
openssl ec -in "$dir/client/ssh-key" -param_enc named_curve -out "$dir/client/ssh-key.named" 2>/dev/null
mv "$dir/client/ssh-key.named" "$dir/client/ssh-key"
chmod 600 "$dir/client/ssh-key"

# The stand-ins: a login shell that runs its -c line with DIR/bin first on the PATH, and a nexus-server that prints
# the line nexus-server new prints, with what it was asked on standard error.
cat > "$dir/shell" <<EOF
#!/bin/sh
[ "\$1" = -l ] && [ "\$2" = -c ] || { echo "fixture shell: wanted -l -c, got \$*" >&2; exit 2; }
PATH="$dir/bin:\$PATH" exec /bin/sh -c "\$3"
EOF
cat > "$dir/bin/nexus-server" <<'EOF'
#!/bin/sh
echo "fixture nexus-server: $*" >&2
[ "${1:-}" = new ] || { echo "fixture nexus-server: only new here" >&2; exit 3; }
echo "NEXUS CONNECT 60001 AbCdEfGhIjKlMnOpQrSt+/ format ${NEXUS_STATE_FORMAT:-1}"
EOF
chmod 755 "$dir/shell" "$dir/bin/nexus-server"
printf 'restrict,command="env SHELL=%s %s" %s\n' "$dir/shell" "$gate" "$(cat "$dir/client/ssh-key.pub")" \
    > "$dir/authorized_keys"
chmod 600 "$dir/authorized_keys"

# A port no one holds: tried in turn from one picked by the process id.
port=$((20000 + $$ % 20000))
tries=0
while :; do
    cat > "$dir/sshd_config" <<EOF
Port $port
ListenAddress 127.0.0.1
HostKey $dir/host_key
PidFile $dir/sshd.pid
AuthorizedKeysFile $dir/authorized_keys
StrictModes no
UsePAM no
PasswordAuthentication no
KbdInteractiveAuthentication no
PubkeyAuthentication yes
AllowUsers $(id -un)
EOF
    /usr/sbin/sshd -f "$dir/sshd_config" -E "$dir/sshd.log" && break
    tries=$((tries + 1))
    [ $tries -lt 20 ] || { echo "sshd_fixture: sshd did not start" >&2; cat "$dir/sshd.log" >&2; exit 1; }
    port=$((port + 1))
done
# Started once the pid file is there and the port answers.
for i in 1 2 3 4 5 6 7 8 9 10; do
    if [ -f "$dir/sshd.pid" ] && nc -z 127.0.0.1 "$port" 2>/dev/null; then
        echo "$port" > "$dir/port"
        id -un > "$dir/user"
        echo "sshd_fixture: sshd on 127.0.0.1:$port"
        exit 0
    fi
    sleep 0.2
done
echo "sshd_fixture: sshd did not answer on $port" >&2
cat "$dir/sshd.log" >&2
exit 1
