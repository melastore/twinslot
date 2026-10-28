#!/usr/bin/env bash
# Survey a rooted phone over adb for twinslot. Read-only: on the phone it only
# writes its own scratch files under /data/local/tmp and removes them after.
#   tools/survey.sh [OUT_DIR]
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-survey-$(date +%Y%m%d-%H%M%S)}
TMP=/data/local/tmp/twinslot-survey

[ -x "$ROOT/out/twinslot" ] || make -C "$ROOT" device
adb get-state > /dev/null

# The scratch files hold raw partition headers: removed even when a step below fails.
trap "adb shell \"su -c 'rm -rf $TMP /data/local/tmp/twinslot-survey.sh /data/local/tmp/twinslot'\"" EXIT
adb push "$ROOT/out/twinslot" /data/local/tmp/twinslot > /dev/null
adb push "$ROOT/device/survey.sh" /data/local/tmp/twinslot-survey.sh > /dev/null
adb shell "su -c 'sh /data/local/tmp/twinslot-survey.sh $TMP /data/local/tmp/twinslot'"
adb pull "$TMP" "$OUT" > /dev/null

echo "survey saved in $OUT"
echo "it contains the phone's serial number and partition GUIDs: keep it private"
