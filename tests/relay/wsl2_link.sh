#!/bin/bash
# wsl2_link.sh <nexus_wsl2_link_tests.exe> <nexus-link-relay> <nexus-server> <nexus-fake-driver> <dir>: the window's link
# across the WSL2 boundary (tests/relay/wsl2_link.cpp), run from inside WSL2 with interop to Windows. <dir> is the test
# server's, under /tmp, made fresh: its HOME (<dir>/home), its drivers file (ada, on the fake driver, answering nine
# prompts), and the server's log, which is printed at the end. The server is stopped and <dir> removed
# afterwards, whatever the test did. Exits as the test did.
#
# The relay is started as the window starts it by default (relay/relay_words.h: a plain `sh -c` finding
# nexus-link-relay on the PATH, "$@" its arguments): the test's nexus-link-relay is a wrapper in <dir>/bin, put on that
# PATH through WSLENV (PATH/l), never in ~/.local/bin. It writes its process id to <dir>/relay.pid (the test kills that
# relay, and only it) and its arguments to <dir>/relay.args, then execs the relay. BASH_ENV, through WSLENV, names a
# file that prints a line, standing in for a profile that prints (WSL sets HOME from the user's entry, so a test cannot
# give the relay a ~/.profile of its own): a login bash, or any bash, would put that line ahead of the relay's Hello.
# One argument of the server's command has spaces, quotes, a $ and a backslash: it must reach the relay as one word.
# WSL2_LINK_STATE_OUT, when set, is a Windows path the test writes the state it read across the boundary to (its
# --state-out), for `nexus --screenshot PNG --state FILE`.
set -uo pipefail
if [ $# -ne 5 ]; then
    echo "usage: wsl2_link.sh <nexus_wsl2_link_tests.exe> <nexus-link-relay> <nexus-server> <nexus-fake-driver> <dir>" >&2
    exit 2
fi
test_exe=$1 relay=$2 server=$3 fake=$4 dir=$5
stop() {
    if [ -S "$dir/server.sock" ]; then
        HOME="$dir/home" "$server" stop --dir "$dir" || echo "wsl2_link.sh: stopping the server in $dir failed" >&2
    fi
}
stop
rm -rf "$dir"
mkdir -p "$dir/home" || exit 1
chmod 700 "$dir"
reply='send
emit {"kind":"session","id":"s-1"}
emit {"kind":"text","delta":"hello "}
emit {"kind":"text","delta":"back"}
emit {"kind":"text-end"}
emit {"kind":"done","outcome":"ok"}'
# One reply for the prompt, one for each of the six queued prompts the AI takes after it, and one for each of the two
# prompts around the relay's end.
{
    printf '%s\n%s\n' 'emit {"kind":"hello","protocol":1,"harness":"fake","features":["limit"]}' \
        'emit {"kind":"credential","billing":"subscription","plan":"max"}'
    for _ in 1 2 3 4 5 6 7 8 9; do printf '%s\n' "$reply"; done
} > "$dir/ada.script"
printf 'driver fake %s --script %s\nai ada fake %s\n' "$fake" "$dir/ada.script" "$dir" > "$dir/drivers"
mkdir -p "$dir/bin" || exit 1
printf '#!/bin/sh\necho $$ > "%s/relay.pid"\nprintf "%%s\\n" "$@" > "%s/relay.args"\nexec "%s" "$@"\n' \
    "$dir" "$dir" "$relay" > "$dir/bin/nexus-link-relay"
chmod 700 "$dir/bin/nexus-link-relay"
printf 'echo "a startup file printing ahead of the relay"\n' > "$dir/startup.sh"
probe='NEXUS_QUOTE_PROBE=two  spaces "quoted" $HOME back\slash'
# The PATH goes to the Windows side too: System32 stays on it, for the wsl.exe the test runs to kill the relay.
PATH="$dir/bin:$PATH:/mnt/c/Windows/System32" BASH_ENV="$dir/startup.sh" WSLENV="${WSLENV:+$WSLENV:}PATH/l:BASH_ENV" \
    "$test_exe" ${WSL2_LINK_STATE_OUT:+--state-out "$WSL2_LINK_STATE_OUT"} --relay-pid "$dir/relay.pid" - -- \
    /usr/bin/env "HOME=$dir/home" "$probe" "$server" new --dir "$dir"
code=$?
if [ $code -eq 0 ]; then
    if [ ! -f "$dir/relay.args" ]; then
        echo "wsl2_link.sh: the window's default launch did not run the test's nexus-link-relay ($dir/bin)" >&2
        code=1
    elif ! grep -Fxq -- "$probe" "$dir/relay.args"; then
        echo "wsl2_link.sh: the argument [$probe] did not reach the relay as one word; it got:" >&2
        sed 's/^/  [/; s/$/]/' "$dir/relay.args" >&2
        code=1
    else
        echo "wsl2_link.sh: the window's default launch ran the relay, and [$probe] reached it as one word"
    fi
fi
stop
if [ -f "$dir/server.log" ]; then
    echo "--- server log, $dir ---"
    cat "$dir/server.log"
    echo "------------------"
fi
rm -rf "$dir"
exit $code
