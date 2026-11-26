#!/bin/sh
# tools/nexus-ios-gate, the forced command of an iPhone's or iPad's ssh key: the lines the app's link sends run, as
# their words; anything else is refused with status 126. SHELL is /bin/echo, so a line that runs prints its words and
# nothing is run. The lines are quoted as src/link/bootstrap.cpp remoteLine quotes them.
#
# Usage: sh tests/ios/gate_test.sh tools/nexus-ios-gate
set -u
gate=$1
failures=0

quote() {   # one word, as bootstrap.cpp quoted(): in single quotes, each ' as '\''
    printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"
}
line() {    # exec "$SHELL" -l -c <the quoted words, quoted again>
    inner=""
    for w in "$@"; do inner="$inner${inner:+ }$(quote "$w")"; done
    printf 'exec "$SHELL" -l -c %s' "$(quote "$inner")"
}
check() {   # NAME WANT(run|refuse) EXPECTED_OUTPUT LINE
    out=$(SHELL=/bin/echo SSH_ORIGINAL_COMMAND="$4" sh "$gate" 2>&1)
    status=$?
    if [ "$2" = run ] && { [ $status -ne 0 ] || [ "$out" != "$3" ]; }; then
        echo "FAIL $1: wanted it run as [$3], got status $status [$out]"
        failures=$((failures + 1))
    elif [ "$2" = refuse ] && { [ $status -ne 126 ] || [ "${out#nexus-ios-gate: }" = "$out" ]; }; then
        echo "FAIL $1: wanted it refused, got status $status [$out]"
        failures=$((failures + 1))
    else
        echo "ok   $1"
    fi
}

check new run "-l -c exec env NEXUS_STATE_FORMAT=5 NEXUS_HELLO=0a0b NEXUS_ASK_RUNNING=1 nexus-server new --client window-0123456789abcdef --next 12" \
    "$(line env NEXUS_STATE_FORMAT=5 NEXUS_HELLO=0a0b NEXUS_ASK_RUNNING=1 nexus-server new --client window-0123456789abcdef --next 12)"
check history run "-l -c exec env NEXUS_STATE_FORMAT=5 nexus-server history-page 3 1 40" \
    "$(line env NEXUS_STATE_FORMAT=5 nexus-server history-page 3 1 40)"
check bare run "-l -c exec nexus-server new" "$(line nexus-server new)"
check "another program" refuse "" "$(line rm -rf /tmp/x)"
check "a semicolon" refuse "" "$(line nexus-server 'new;id')"
check "a substitution" refuse "" "$(line nexus-server new '$(id)')"
check "a variable not nexus's" refuse "" "$(line env PATH=/tmp nexus-server new)"
check "another verb" refuse "" "$(line nexus-server stop)"
check "another program with the verb" refuse "" "$(line rm new)"
check "a shell" refuse "" "bash"
check "a quote in a word" refuse "" "$(line nexus-server new "a'b")"
check "more after the line" refuse "" "$(line nexus-server new); id"
check "nothing" refuse "" ""

[ $failures -eq 0 ] || { echo "gate_test: $failures failed"; exit 1; }
echo "gate_test: all passed"
