#!/usr/bin/env bash
#
# tests/run_brotato_watchdog.sh -- one integration-watchdog cycle.
#
# A cycle is:
#   1. the full Brotato suite          (tests/run_brotato_tests.sh)
#   2. the Demon CPU replay gate       (primary, once a Demon trace exists --
#      ocean/brotato/bridge/replay_cpu.py; reports status + first diverging tick)
#   3. the Bull CPU replay gate        (informational only; Bull's known
#      contact-explosion frame gap diverges by design, see known_issues.txt)
# and it records everything in build/brotato_watchdog/:
#   cycles.tsv          one row per cycle (totals, failing tests, demon gate verdict,
#                       tripwire, host pressure)
#   cycle_<label>_*.log full output of every step
#   known_issues.txt    standing failures + their owners (so they are not
#                       re-routed unless their signature moves)
#
# Demon trace lookup order: $BROTATO_WATCHDOG_DEMON_TRACE, then
# <repo|~>/rl_trace.demon_s1.jsonl, then any <repo|~/brotato_run/rl_out>/rl_trace.*demon*.jsonl.
# Skipped inside cycles via BROTATO_TEST_SKIP (run standalone instead): the
# full-engine weapon oracle (minutes) and the five .cu CUDA tests (each nvcc
# build is minutes of ptxas on this host, and the host is oversubscribed --
# nvcc is serialized through build/.nvcc.lock when they are run).
#
# Usage: tests/run_brotato_watchdog.sh [label]
#
# Exit status: 0 only when the suite is green and the Demon gate (if a trace
# exists) matched; the Bull gate never affects it.

set -uo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd -- "$SCRIPT_DIR/.." && pwd)
LABEL=${1:-cycle}

OUT=${BROTATO_WATCHDOG_DIR:-"$REPO/build/brotato_watchdog"}
mkdir -p "$OUT"
# Serialize cycles (a chained/late invocation must not interleave the replay
# outputs or the timeline with a running one).
if command -v flock >/dev/null 2>&1; then
    exec 9>"$OUT/cycle.lock"
    flock -w 3600 9 || true
fi
STAMP=$(date '+%Y-%m-%dT%H:%M:%S')
LOG="$OUT/cycle_${LABEL}_$(date '+%H%M%S').log"

# ------------------------------------------------------- 0. host pressure
# Main asked for these every cycle while the host is oversubscribed.
pressure() {
    _nv=$(ps -eo comm= 2>/dev/null | grep -cE '^(nvcc|ptxas|cicc)$')
    _mem=$(awk '/^MemAvailable:/{printf "%.1f", $2/1048576}' /proc/meminfo 2>/dev/null)
    printf 'nvcc/ptxas/cicc=%s MemAvailable=%sG' "${_nv:-?}" "${_mem:-?}"
}
PRESSURE=$(pressure)

# ------------------------------------------------------- 0. compile tripwire
# Cheapest possible "is the shared tree compiling" signal, so a broken shared
# header is reported in seconds rather than after the whole suite.
TRIP="$OUT/.tripwire"
if timeout 120 g++ -std=c++17 -O0 -fsyntax-only -Wno-narrowing -ffp-contract=off \
        -I"$REPO" -I"$REPO/ocean/brotato" "$REPO/tests/test_brotato_core.cpp" \
        > "$TRIP" 2>&1; then
    tripwire=OK
else
    tripwire="BROKEN[$(grep -m1 -E 'error:' "$TRIP" | sed "s|$REPO/||" | cut -c1-120)]"
fi

# ---------------------------------------------------------------- 1. the suite
# Private results dir: other agents run the same runner concurrently, and a
# shared status/ dir made cycle totals read peers' half-written rows.
SUITE_RESULTS="$OUT/results_$LABEL"
SUITE_LOG="$OUT/.suite_stdout"
# Per-test cap: a mid-edit can make a core loop hang, and a 15-minute stall
# hides the whole cycle.  C++ tests are sub-second, pure-python ~1-10 s, but the
# Godot-backed ones drive the real engine and can take minutes.
BROTATO_TEST_TIMEOUT=${BROTATO_WATCHDOG_TEST_TIMEOUT:-300} \
BROTATO_TEST_TIMEOUT_LONG=${BROTATO_WATCHDOG_TEST_TIMEOUT_LONG:-600} \
BROTATO_TEST_SKIP=${BROTATO_WATCHDOG_TEST_SKIP:-test_brotato_(weapon_godot_oracles|cuda|gpu_)} \
BROTATO_TEST_RESULTS="$SUITE_RESULTS" \
    bash "$SCRIPT_DIR/run_brotato_tests.sh" > "$SUITE_LOG" 2>&1
suite_rc=$?

total=0; npass=0; nfail=0; nbuild=0; nskip=0
totals=$(grep -E '^total=' "$SUITE_LOG" | tail -1)
if [ -n "$totals" ]; then
    suite_field() { printf '%s' "$totals" | tr ' ' '\n' | grep -E "^$1=" | head -1 | cut -d= -f2; }
    total=$(suite_field total); npass=$(suite_field pass); nfail=$(suite_field fail)
    nbuild=$(suite_field 'build-fail'); nskip=$(suite_field skip)
    total=${total:-0}; npass=${npass:-0}; nfail=${nfail:-0}
    nbuild=${nbuild:-0}; nskip=${nskip:-0}
fi

failing=""
if [ -d "$SUITE_RESULTS/status" ]; then
    for f in "$SUITE_RESULTS"/status/*.tsv; do
        IFS=$'\t' read -r name status checks fails skips detail < "$f"
        case "$status" in
            PASS|SKIP) continue ;;
            *) failing="${failing:+$failing,}${name}:${status}:${fails}/${checks}" ;;
        esac
    done
fi

# ------------------------------------------------------------- 2. replay gates
# replay_cpu.py resolves character/weapon tables from the external root.
export BROTATO_EXTERNAL_ROOT=${BROTATO_EXTERNAL_ROOT:-"$REPO/migration/game/external"}
# $1 trace  $2 outdir  $3 stdout capture file [extra args...]; sets REPLAY_STATUS/REPLAY_DIV/REPLAY_RC.
replay_trace() {
    REPLAY_STATUS="n/a"; REPLAY_DIV="n/a"; REPLAY_RC=0
    [ -f "$1" ] || { REPLAY_STATUS="no trace"; return; }
    local trace=$1 outdir=$2 capture=$3
    shift 3
    timeout 600 python3 "$REPO/ocean/brotato/bridge/replay_cpu.py" "$trace" \
        --output-dir "$outdir" "$@" > "$capture" 2>&1
    REPLAY_RC=$?
    REPLAY_STATUS=$(grep -m1 '^status:' "$capture" | sed 's/^status: *//')
    REPLAY_DIV=$(grep -m1 '^first_divergence:' "$capture" | sed 's/^first_divergence: *//')
    [ -z "$REPLAY_STATUS" ] && REPLAY_STATUS="INVALID(rc=$REPLAY_RC)"
    [ -z "$REPLAY_DIV" ] && REPLAY_DIV="none"
}

# Demon (PRIMARY).  Every VALIDATED demon capture (newest first, up to 3), each
# replayed at its own starting wave.  A capture only qualifies when it has a
# .meta.json sibling and every row parses as JSON with obs_complete == 1 -- the
# host rewrites these files while recording, so mtime alone can pick up a
# half-written capture (observed: two v4 takes with truncated rows).  Rejected
# captures are named in the cycle note instead of being replayed.
validate_trace() {
    python3 -c '
import json, sys
path = sys.argv[1]
try:
    n = 0
    with open(path) as fh:
        for line in fh:
            if not line.strip():
                continue
            row = json.loads(line)
            if row.get("obs_complete", 1) != 1:
                print("row %d incomplete" % n); sys.exit(1)
            n += 1
    if n == 0:
        print("empty"); sys.exit(1)
    print("ok %d rows" % n)
except Exception as exc:
    print("invalid: %s" % exc); sys.exit(1)
' "$1" 2>&1
}

DEMON_TRACES=()
_DEMON_NOTE=""
if [ -n "${BROTATO_WATCHDOG_DEMON_TRACE:-}" ]; then
    DEMON_TRACES=( "$BROTATO_WATCHDOG_DEMON_TRACE" )
else
    shopt -s nullglob
    _cands=( "$REPO"/migration/game/rl_trace.*demon*.jsonl              "$HOME"/brotato_run/rl_out/rl_trace.*demon*.jsonl )
    shopt -u nullglob
    _bad=""
    for _c in $(ls -t "${_cands[@]}" 2>/dev/null); do
        [ "${#DEMON_TRACES[@]}" -ge 3 ] && break
        if [ ! -f "$_c.meta.json" ]; then
            _bad="$_bad $(basename "$_c")(no .meta.json)"; continue
        fi
        _v=$(validate_trace "$_c")
        case "$_v" in
            ok*) DEMON_TRACES+=( "$_c" ) ;;
            *)   _bad="$_bad $(basename "$_c")($_v)" ;;
        esac
    done
    [ -n "$_bad" ] && _DEMON_NOTE="rejected unvalidated capture(s):$_bad"
    [ -n "${_DEMON_AUDIO_NOTE:-}" ] && _DEMON_NOTE="$_DEMON_NOTE${_DEMON_NOTE:+; }$_DEMON_AUDIO_NOTE"
fi

# Extra replay args for a capture: replay_cpu.py resolves an  field
# itself, but no trace records it yet (AuditRNG) -- a bridge-neutralized take
# replayed with audio_rng=1 carries the game-side audio draws (+3 global draws
# per birth/pop frame), which moves the stream.  When the trace or its meta
# declares audio_rng 0 we forward it explicitly; otherwise we leave the default
# and say so in the cycle note.
trace_audio_rng_args() {
    if [ -f "$1.meta.json" ] && grep -qE '"audio_rng"[[:space:]]*:[[:space:]]*0' "$1.meta.json"; then
        printf -- '--audio-rng 0'; return
    fi
    if grep -qE '"audio_rng"[[:space:]]*:[[:space:]]*0' "$1" 2>/dev/null; then
        printf -- '--audio-rng 0'; return
    fi
    printf ''
}

trace_start_wave() {
    python3 -c "
import json,sys
try:
    with open(sys.argv[1]) as fh: print(int(json.loads(fh.readline()).get('wave', 1)))
except Exception: print(1)" "$1" 2>/dev/null || printf '1'
}

demon_status="no trace"; demon_div="n/a"; demon_rc=0
demon_lines=""
DEMON_ANY_DIVERGED=0
if [ "${#DEMON_TRACES[@]}" -eq 0 ]; then
    demon_status="no demon trace"
else
    _n=0; _n_div=0; _n_invalid=0; _first_tick=""; _first_desc=""
    for _t in "${DEMON_TRACES[@]}"; do
        _n=$((_n + 1))
        _sw=$(trace_start_wave "$_t")
        _sw_args=()
        if [ "${_sw:-1}" -gt 1 ] 2>/dev/null; then _sw_args=( --start-wave "$_sw" ); fi
        _ar=$(trace_audio_rng_args "$_t")
        if [ -n "$_ar" ]; then _sw_args+=( $_ar ); else
            case "${_DEMON_AUDIO_NOTE:-}" in
                *audio_rng*) ;;
                *) _DEMON_AUDIO_NOTE="audio_rng not recorded in this capture: replayed with the default 1" ;;
            esac
        fi
        replay_trace "$_t" "$OUT/replay_demon_$_n" "$OUT/.replay_demon_$_n" "${_sw_args[@]}"
        demon_lines="${demon_lines}    $(basename "$_t") [wave $_sw]: $REPLAY_STATUS  first_divergence: $REPLAY_DIV
"
        case "$REPLAY_STATUS" in
            MATCH) ;;
            INVALID*) _n_invalid=$((_n_invalid + 1)); DEMON_ANY_DIVERGED=1 ;;
            *) _n_div=$((_n_div + 1)); DEMON_ANY_DIVERGED=1
               _tk=$(printf '%s' "$REPLAY_DIV" | sed -n 's/^tick=\([0-9]*\).*/\1/p')
               if [ -n "$_tk" ] && { [ -z "$_first_tick" ] || [ "$_tk" -lt "$_first_tick" ]; }; then
                   _first_tick=$_tk; _first_desc="$REPLAY_DIV"
               fi ;;
        esac
    done
    if [ "$_n_div" = 0 ] && [ "$_n_invalid" = 0 ]; then
        demon_status="MATCH ($_n/$_n)"
    else
        demon_status="DIVERGED ($_n_div/$_n)"
        [ "$_n_invalid" != 0 ] && demon_status="${demon_status/-/invalid: $_n_invalid -}"
        demon_div="${_first_desc:-unknown}"
    fi
fi

# Bull replay gate: removed (Demon-first directive).  Bull coverage lives in the
# unchanged test suite; there is no Bull replay in cycles at all.
bull_status="-"; bull_div="-"; bull_rc=0

# ------------------------------------------------------- 3. recent source edits
RECENT="$OUT/.recent"
: > "$RECENT"
for d in "$REPO/ocean/brotato" "$REPO/tests"; do
    find "$d" -maxdepth 1 -type f -mmin -40 -printf '%TY-%Tm-%TdT%TH:%TM:%TS %p\n' 2>/dev/null
done | sort >> "$RECENT"

# ------------------------------------------------------------------- record
{
    echo "=== watchdog $LABEL @ $STAMP"
    echo "--- host pressure: $PRESSURE"
    echo "--- tripwire (host compile of test_brotato_core.cpp): $tripwire"
    echo "--- suite (rc=$suite_rc): $totals"
    echo "--- failing: ${failing:-none}"
    echo "--- demon replay [PRIMARY] ($demon_status): $demon_div"
    printf '%s' "$demon_lines"
    [ -n "${_DEMON_NOTE:-}" ] && echo "--- note: $_DEMON_NOTE"
    echo
    echo "########## files modified in the last 40 min ##########"
    cat "$RECENT"
    echo
    echo "########## suite output ##########"
    cat "$SUITE_LOG"
    echo
    echo "########## demon replay output ##########"
    cat "$OUT/.replay_demon" 2>/dev/null
} > "$LOG"

TIMELINE="$OUT/cycles.tsv"
[ -f "$TIMELINE" ] || printf '#ts\ttotal\tpass\tfail\tbuildfail\tskip\tdemon\tdemon_div\tbull\tbull_div\tfailing\ttripwire\tpressure\n' > "$TIMELINE"
printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$STAMP" "$total" "$npass" "$nfail" "$nbuild" "$nskip" \
    "$demon_status" "$demon_div" "$bull_status" "$bull_div" "${failing:-none}" "$tripwire" \
    "$PRESSURE" >> "$TIMELINE"

printf 'WATCHDOG %s @ %s\n' "$LABEL" "$STAMP"
printf '  host pressure : %s\n' "$PRESSURE"
printf '  tripwire: %s\n' "$tripwire"
printf '  suite : total=%s pass=%s fail=%s build-fail=%s skip=%s\n' \
    "$total" "$npass" "$nfail" "$nbuild" "$nskip"
printf '  failing: %s\n' "${failing:-none}"
printf '  demon replay [PRIMARY] : %s  first_divergence: %s\n' "$demon_status" "$demon_div"
printf '%s' "$demon_lines"
[ -n "${_DEMON_NOTE:-}" ] && printf "  note: %s
" "$_DEMON_NOTE"
if [ -s "$RECENT" ]; then
    printf '  recent edits (40 min):\n'
    sed -E "s|^[0-9-]+T([0-9:]+)[.0-9]* $REPO/|    \1  |" "$RECENT" | head -12
fi
printf '  log: %s\n' "$LOG"

if [ "$nfail" != 0 ] || [ "$nbuild" != 0 ]; then
    exit 1
fi
if [ "$DEMON_ANY_DIVERGED" = 1 ]; then
    exit 1
fi
exit 0
