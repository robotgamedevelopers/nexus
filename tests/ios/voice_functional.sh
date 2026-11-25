#!/bin/sh
# The voice's recognizer end to end on a simulator (tests/ios/tests.cmake): installs the app, runs
#   nexus --recognize AUDIO --ais NAMES
# in it, which recognizes the recorded audio on the device through Apple's Speech (src/voice/speech_apple.mm) and routes
# the transcript with the matcher (src/voice/name_match.h), and checks the route line it prints.
#
#   voice_functional.sh DEVICE APP AUDIO NAMES EXPECTED [planted]
#
# With `planted` (the reversed witness) it passes only when the app printed a route and it is not EXPECTED: a refusal
# or no route still fails, so a witness never passes by the recognizer not running.
#
# DEVICE is a simulator's UDID or "booted". Speech recognition needs the app allowed once on that simulator (simctl
# privacy has no switch for it): launch the app, tap Talk, and Allow. Until then this test fails and says so.
set -u
device=$1 app=$2 audio=$3 names=$4 expected=$5 mode=${6:-}
xcrun simctl install "$device" "$app" || { echo "FAIL: could not install $app on simulator $device (is one booted?)"; exit 1; }
bundle=$(/usr/libexec/PlistBuddy -c "Print CFBundleIdentifier" "$app/Info.plist") || { echo "FAIL: no bundle id in $app"; exit 1; }
out=$(xcrun simctl launch --console-pty --terminate-running-process "$device" "$bundle" --recognize "$audio" --ais "$names" 2>&1)
printf '%s\n' "$out" | grep -E '^(heard|route|failed|refused):'
case $out in
    *"refused:"*) echo "FAIL: speech recognition is not allowed for nexus on this simulator: launch the app, tap Talk, and Allow once."; exit 1 ;;
esac
route=$(printf '%s\n' "$out" | grep '^route:' | tail -1 | tr -d '\r')
[ -n "$route" ] || { echo "FAIL: the app printed no route line"; exit 1; }
if [ "$mode" = planted ]; then
    [ "$route" != "$expected" ] || { echo "FAIL: the planted case was routed as the real one: $route"; exit 1; }
    echo "PASS (planted): $route"
    exit 0
fi
[ "$route" = "$expected" ] || { echo "FAIL: expected '$expected', got '$route'"; exit 1; }
echo "PASS: $route"
