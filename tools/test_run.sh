#!/usr/bin/env bash
# tools/test_run.sh NAME [ENV=VALUE...]: a test run of the game that never touches the player's own
# save or settings: the save is copied into out/test-user/ first, the game uses that copy and
# out/test-user/bbport.ini, and tools/mac_bench.sh runs it in the background with short timers
# (BOOT_WINDOWS=1 TITLE_WAIT=3 PRESS_GAP=4; HOLD seconds in the level, default 45). Prints the
# frame summary and any STOP or fault line. Refuses to start while another game is running.
#   tools/test_run.sh base
#   tools/test_run.sh rec BB_RECOMP_LIB=$PWD/recomp/out/recomp/librecomp.dylib
# Which save the copy starts from:
#   SAVE_FROM=DIR   a folder with a savedata/ (a launcher backup in save-backups/<profile>/b-...)
#                   instead of the player's current save; it is copied, never written.
#   KEEP_SAVE=1     keep out/test-user/savedata from the previous test run (its progress).
# What it tests, shown at the top of the game's window (the overlay's test banner):
#   TEST_TITLE=...  the title (default: NAME);  TEST_DETAIL=...  a second line.
# The window comes to the front and takes the keyboard (it can be played by hand);
# BB_BENCH_BACKGROUND=1 keeps it a background app, which macOS never gives the keyboard.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
name=${1:?usage: tools/test_run.sh NAME [ENV=VALUE...]}
shift
pgrep -x bb-probe > /dev/null && { echo "A game is already running: not starting another." >&2; exit 1; }
data="$HOME/Library/Application Support/bloodborne_mac/user"
test_user=$PWD/out/test-user
mkdir -p "$test_user"
if [[ ${KEEP_SAVE:-0} == 1 && -d $test_user/savedata ]]; then
    echo "Save: the previous test run's copy (KEEP_SAVE=1)"
else
    source=${SAVE_FROM:-$data}
    [[ -z ${SAVE_FROM:-} || -d $source/savedata ]] || { echo "No savedata/ in $source" >&2; exit 1; }
    rm -rf -- out/test-user/savedata
    [[ -d $source/savedata ]] && cp -R "$source/savedata" "$test_user/" && echo "Save: a copy of $source/savedata"
fi
[[ -f $test_user/bbport.ini || ! -f $data/../bbport.ini ]] || cp "$data/../bbport.ini" "$test_user/bbport.ini"
BOOT_WINDOWS=1 TITLE_WAIT=3 PRESS_GAP=4 LEVEL_WAIT=${LEVEL_WAIT:-200} HOLD=${HOLD:-45} \
    tools/mac_bench.sh "$name" BB_USER_DIR="$test_user" BB_CONFIG="$test_user/bbport.ini" \
    "BB_GPU_USER_DIR=$test_user" BB_LANGUAGE=1 BB_UI_LANGUAGE=en \
    "BB_TEST_TITLE=${TEST_TITLE:-$name}" "BB_TEST_DETAIL=${TEST_DETAIL:-}" \
    "$@" > "out/test_$name.txt" 2>&1 || true
echo "$name: $(tail -2 "out/test_$name.txt" | head -1)"
log=$(ls -t out/bench_"$name"_*.log 2>/dev/null | head -1)
[[ -n $log ]] && grep -m3 "^Recompiled: [0-9]* of\|STOP\|Host fault\|Guest fault" "$log" || true
