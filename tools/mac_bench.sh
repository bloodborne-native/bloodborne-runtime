#!/usr/bin/env bash
# tools/mac_bench.sh NAME [ENV=VALUE...]: macOS counterpart of tools/bench.sh. Starts the game
# with frame stats and a pad file, enters the level from the title (cross, cross), stands still
# for ${HOLD:-40} s, closes the game and prints a summary of the in-level frame stats.
# Log: out/bench_NAME_<time>.log, per-frame CSV: out/bench_NAME_<time>.frames.csv.
set -u
cd -- "$(dirname -- "$0")/.."
name=${1:?usage: tools/mac_bench.sh NAME [ENV=VALUE...]}
shift
# The game these tests run is the one the sources make: run.sh only runs it.
bash build.sh
base=out/bench_${name}_$(date +%m%d_%H%M%S)
pgrep -x bb-probe > /dev/null && { echo "A game is already running: not starting another." >&2; exit 1; }
: > out/pad; : > out/test.clock; echo "${BASE_MASK:-0}" > out/toggles
# The game is a normal app: it comes to the front and takes the keyboard, so a test can be played
# by hand. BB_BENCH_BACKGROUND=1: a background-only app instead (no focus taken from the desktop,
# and no keyboard: macOS never makes it the active app). The pad file works either way.
[[ ${BB_BENCH_BACKGROUND:-0} == 1 ]] && export SDL_MAC_BACKGROUND_APP=1
env BB_GAME_DIR="${BB_GAME_DIR:-$HOME/Downloads/PS4/Bloodborne/CUSA03173}" \
    BB_PAD_FILE="$PWD/out/pad" BB_TEST_CLOCK="$PWD/out/test.clock" BB_TOGGLE_FILE="$PWD/out/toggles" BB_FRAME_STATS=1 \
    BB_FRAME_LOG="$PWD/$base.frames.csv" BB_FPS_LIMIT="${BB_FPS_LIMIT:-0}" \
    BB_PRESENT_DUMP_TRIGGER="$PWD/out/grab.trigger" BB_SCREEN_DUMP_TRIGGER="$PWD/out/screen.trigger" \
    BB_MENU_TRIGGER="$PWD/out/menu.trigger" BB_DUMP_DIR="$PWD/out/grab" "$@" \
    bash run.sh > "$base.log" 2>&1 < /dev/null &
launcher=$!
stop_owned_run() {
    # run.sh normally execs bb-probe; with a mounted mod view it owns a bb-probe child.
    local child
    child=$(pgrep -P "$launcher" -x bb-probe || true)
    kill -TERM "$launcher" 2>/dev/null || true
    sleep 2
    [[ -z $child ]] || kill -KILL "$child" 2>/dev/null || true
    kill -KILL "$launcher" 2>/dev/null || true
    wait "$launcher" 2>/dev/null || true
}
fail() { echo "FAILED: $*  ($base.log)"; tail -20 "$base.log"; stop_owned_run; exit 1; }
# run.sh may still be building (sources changed): it counts as alive until it exits.
alive() { kill -0 "$launcher" 2>/dev/null; }
require_alive() {
    alive && return
    local status=0
    wait "$launcher" || status=$?
    fail "the game exited $1 (launcher exit status $status)"
}
# Title: the pad is open and a few frame stats windows went by.
for i in $(seq 1 ${BOOT_WAIT:-240}); do
    sleep 1
    [[ $(grep -c '^Frame stats' "$base.log") -ge ${BOOT_WINDOWS:-3} ]] && grep -q 'pad opened' "$base.log" && break
    (( i <= 20 )) || require_alive "before the title"
done
sleep "${TITLE_WAIT:-6}"
tools/press.sh cross; sleep "${PRESS_GAP:-6}"; tools/press.sh cross
echo 'title input complete'
# Level: two frame-stat windows in a row with more than LEVEL_DRAWS draws a frame (the title and
# loading screens draw under 50; the Hunter's Dream about 540, Central Yharnam about 1,270). The
# old threshold of 600 missed the Hunter's Dream, so runs there sat out the whole LEVEL_WAIT.
start=$(date +%s)
for i in $(seq 1 $(( ${LEVEL_WAIT:-240} / 2 ))); do
    sleep 2
    require_alive "while loading"
    read -r prev last < <(grep '^Frame stats' "$base.log" | tail -2 |
        sed -E 's/.* ([0-9]+) draws\/frame.*/\1/' | tr '\n' ' ')
    [[ $prev =~ ^[0-9]+$ && $last =~ ^[0-9]+$ && $prev -gt ${LEVEL_DRAWS:-200} &&
       $last -gt ${LEVEL_DRAWS:-200} ]] && break
done
echo "level after $(( $(date +%s) - start )) s"
mark=$(grep -c '^Frame stats' "$base.log")
# TRACE=1: a Metal System Trace of ${TRACE_SECONDS:-5} s after the level settled (Xcode's xctrace;
# tools/mst_summary.py reads it).
if [[ ${TRACE:-0} == 1 ]]; then
    sleep 8
    DEVELOPER_DIR=${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer} xcrun xctrace record \
        --template 'Metal System Trace' --attach "$(pgrep -x bb-probe | head -1)" \
        --time-limit "${TRACE_SECONDS:-5}s" \
        --no-prompt --output "$base.trace" > "$base.xctrace.log" 2>&1 &&
        echo "trace: $base.trace" || { echo "trace failed: $base.xctrace.log"; tail -5 "$base.xctrace.log"; }
fi
# SAMPLE=1: call stacks of every thread (macOS sample), $base.sample.txt: SAMPLE_SECONDS (5) long,
# SAMPLE_DELAY (5) s after the level loaded.
if [[ ${SAMPLE:-0} == 1 ]]; then
    sleep "${SAMPLE_DELAY:-5}"
    sample "$(pgrep -x bb-probe | head -1)" "${SAMPLE_SECONDS:-5}" -file "$PWD/$base.sample.txt" > /dev/null 2>&1 &&
        echo "sample: $base.sample.txt" || echo "sample failed"
fi
# GPU utilization (IOAccelerator, no sudo needed) and the game's CPU use, once a second. The test
# banner counts down to the end of the hold (BB_TEST_CLOCK).
echo $(( $(date +%s) + ${HOLD:-40} )) > out/test.clock
: > "$base.load"
for ((t = 0; t < ${HOLD:-40}; ++t)); do
    require_alive "in the level at hold sample $t"
    gpu=$(ioreg -r -d 1 -c IOAccelerator | grep -oE '"Device Utilization %"=[0-9]+' | head -1 | cut -d= -f2)
    cpu=$(ps -o pcpu= -p "$(pgrep -x bb-probe | head -1)" 2>/dev/null | tr -d ' ')
    echo "$t ${gpu:-?} ${cpu:-?}" >> "$base.load"
    sleep 1
done
require_alive "in the level at the end of the hold"
awk 'NR > 3 {g += $2; c += $3; n++} END {if (n) printf "GPU utilization %.0f%%, game CPU %.0f%% (of one core)\n", g/n, c/n}' "$base.load"
stop_owned_run
# Summary of the windows after the level settled.
tail -n +1 "$base.log" | grep '^Frame stats' | tail -n +$((mark + 2)) |
    sed -E 's/Frame stats: ([0-9.]+) FPS, worst frame ([0-9.]+) ms.*GPU thread ([0-9.]+) us\/draw, ([0-9]+) draws\/frame, idle ([0-9.]+)%.*/\1 \2 \4 \5/' |
    awk '{n++; f+=$1; w+=$2; d+=$3; id+=$4} END {if (n) printf "%s: %d windows, %.1f FPS, worst %.0f ms, %d draws/frame, GPU thread idle %.0f%%\n", "'"$name"'", n, f/n, w/n, d/n, id/n}'
# Frame times of the hold (the CSV's last ${HOLD} seconds): median, p90, share of time in frames
# over 150 ms (stalls).
python3 - "$base.frames.csv" "${HOLD:-40}" <<'PY'
import csv, sys
rows = [(float(r['t_s']), float(r['frame_ms'])) for r in csv.DictReader(open(sys.argv[1])) if r.get('frame_ms')]
end = rows[-1][0] if rows else 0
f = sorted(ms for t, ms in rows if t >= end - float(sys.argv[2]) + 2)
if f:
    total = sum(f)
    print(f"frames: {len(f)}, median {f[len(f) // 2]:.1f} ms, p90 {f[int(len(f) * 0.9)]:.1f} ms, "
          f"mean {1000 * len(f) / total:.1f} FPS, {100 * sum(x for x in f if x > 150) / total:.0f}% of time in frames over 150 ms")
PY
echo "$base.log"
