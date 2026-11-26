#!/usr/bin/env bash
# Compile and run the Android sender's plain-JVM decision tests.
# Needs a JDK (javac/java) only. Does not need the Android SDK.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"

if [[ -z "${JAVA_HOME:-}" ]]; then
  if [[ -x /usr/libexec/java_home ]]; then
    JAVA_HOME="$(/usr/libexec/java_home -v 21 2>/dev/null || /usr/libexec/java_home 2>/dev/null || true)"
  fi
fi

JAVAC="${JAVA_HOME:+$JAVA_HOME/bin/}javac"
JAVA="${JAVA_HOME:+$JAVA_HOME/bin/}java"

LOGIC=(
  "$ROOT/src/dev/omarchy/steadysender/StreamTiming.java"
  "$ROOT/src/dev/omarchy/steadysender/StationaryDetector.java"
  "$ROOT/src/dev/omarchy/steadysender/DesktopProbe.java"
  "$ROOT/src/dev/omarchy/steadysender/HostCheck.java"
  "$ROOT/src/dev/omarchy/steadysender/DiscoveryChoice.java"
  "$ROOT/src/dev/omarchy/steadysender/DeepLinkRequest.java"
  "$ROOT/src/dev/omarchy/steadysender/ProbeReply.java"
  "$ROOT/src/dev/omarchy/steadysender/ProbeAccept.java"
  "$ROOT/src/dev/omarchy/steadysender/ReplyIntake.java"
  "$ROOT/src/dev/omarchy/steadysender/SampleJson.java"
  "$ROOT/src/dev/omarchy/steadysender/SensorClock.java"
  "$ROOT/src/dev/omarchy/steadysender/SensorBind.java"
  "$ROOT/src/dev/omarchy/steadysender/StreamOutput.java"
  "$ROOT/src/dev/omarchy/steadysender/TargetRefresh.java"
  "$ROOT/src/dev/omarchy/steadysender/TimeoutSource.java"
  "$ROOT/src/dev/omarchy/steadysender/WakeGate.java"
  "$ROOT/src/dev/omarchy/steadysender/DiscoveryCommit.java"
)

for f in "${LOGIC[@]}"; do
  if grep -n 'import android\.' "$f" >/dev/null; then
    echo "android import in $f" >&2
    exit 1
  fi
done

OUT="$(mktemp -d "${TMPDIR:-/tmp}/steady-android-tests.XXXXXX")"
trap 'rm -rf "$OUT"' EXIT

"$JAVAC" --release 11 -encoding UTF-8 -d "$OUT" \
  "${LOGIC[@]}" \
  "$ROOT/tests/dev/omarchy/steadysender/LogicTest.java"

"$JAVA" -cp "$OUT" dev.omarchy.steadysender.LogicTest
