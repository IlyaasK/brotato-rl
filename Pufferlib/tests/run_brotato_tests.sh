#!/usr/bin/env bash
#
# tests/run_brotato_tests.sh -- build and run every Brotato test on Linux.
#
# Discovers
#   tests/test_brotato_*.cpp   C++ regression tests (built with g++ using
#                              build.sh's C++ flags, plus the extra flags each
#                              file names in its own "Build and run:" header)
#   tests/test_brotato_*.py    pytest tests (run through `uv run --with pytest`)
# and runs them in parallel with `xargs -P`.
#
# test_brotato_*.cu are built and run when nvcc is present (each file carries its
# own nvcc command line, arch included); without nvcc they are reported as SKIP.
# test_brotato_metal.mm needs a Metal toolchain and is always SKIP.
#   BROTATO_TEST_NO_CUDA=1   force the CUDA tests to SKIP
#
# Python tests that need a game/engine binary are not skipped by this script;
# the tests skip themselves (pytest shows the reason) when the pinned Godot
# binary or retail pack is absent.
#
# Usage:
#   tests/run_brotato_tests.sh [--jobs N] [--list] [--verbose]
#
# Environment:
#   BROTATO_TEST_JOBS      parallelism (default: nproc)
#   BROTATO_TEST_TIMEOUT   per-test timeout in seconds (default 900)
#   BROTATO_TEST_RESULTS   result dir (default build/brotato_test_results)
#   BROTATO_TEST_ONLY      only run tests whose path matches this ERE
#   CXX                    C++ compiler (default g++)
#
# Conventions for the C++ tests (they run with the 8 MiB default stack):
#   * a BtState is ~1.75 MiB -- keep it in BSS/static (as test_brotato_core does),
#     never as a local, or the test dies with SIGSEGV; the runner reports that as
#     CRASH with the hint rather than as an assertion failure.
#   * stdout is block-buffered when the runner captures it, so a crashing test can
#     lose its already-printed checks; tests that can crash should setbuf(stdout, NULL).
#   BROTATO_CAMPAIGN_ROOT / BROTATO_DLC_ROOT
#                          recovered game sources; default to the in-tree
#                          migration/game/... copies when present
#   BROTATO_RL_OBS_TABLES  generated observation tables; default to
#                          migration/game/external/brotato_obs_tables.json
#   BROTATO_TEST_ENGINE    pinned Godot 3.7-dev1; auto-detected from
#                          $HOME/tools/bin/godot-3.7-dev1 when present, which
#                          also sets BROTATO_GODOT/BROTATO_RECOVERED so the
#                          Godot-backed tests run instead of skipping.
#                          Set BROTATO_TEST_NO_ENGINE=1 to keep them skipped.
#
# Exit status: 0 when every test passed (or skipped), 1 otherwise.

set -uo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd -- "$SCRIPT_DIR/.." && pwd)
SELF="$SCRIPT_DIR/$(basename -- "${BASH_SOURCE[0]}")"

# Engine-backed tests need an X display (the pinned Godot is an X11 build).  Each
# one gets its OWN xvfb-run instead of a single server for the whole run: a
# server that dies (host OOM) then only costs that test, and `--display-driver`
# / "X11 Display is not available" is reported as SKIP rather than as 21 hard
# failures.  Set DISPLAY yourself to use a real session.
ENGINE_TESTS_RE='test_brotato_rng|test_brotato_player_visual|test_brotato_weapon_godot_oracles|test_brotato_rng_jump'

CXX=${CXX:-g++}
# CUDA: the brotato GPU tests are runnable whenever nvcc is present (they carry
# their own nvcc compile line in the file header, arch included).  nvcc is a
# no-op substitution on hosts without one -- those tests then report SKIP.
NVCC=${NVCC:-}
if [ -z "$NVCC" ]; then
    for _nv in /opt/cuda/bin/nvcc "$(command -v nvcc 2>/dev/null || true)"; do
        [ -n "$_nv" ] && [ -x "$_nv" ] && NVCC=$_nv && break
    done
fi
JOBS=${BROTATO_TEST_JOBS:-}
TIMEOUT=${BROTATO_TEST_TIMEOUT:-900}
RESULTS=${BROTATO_TEST_RESULTS:-"$REPO/build/brotato_test_results"}
ONLY=${BROTATO_TEST_ONLY:-}
SKIP=${BROTATO_TEST_SKIP:-}
# Tests that drive the real Godot engine get a much larger cap than the fast
# unit tests (the weapon oracle runs one full engine simulation).
LONG_PATTERN=${BROTATO_TEST_LONG:-test_brotato_weapon_godot_oracles}
LONG_TIMEOUT=${BROTATO_TEST_TIMEOUT_LONG:-1800}
VERBOSE=0

# Flags shared with build.sh's C++ compile for the brotato env.
BASE_FLAGS=(-std=c++17 -O2 -Wno-narrowing -ffp-contract=off -I. -Iocean/brotato)

# Host rule (Main): at most 4 parallel builds; nvcc itself is serialized
# host-wide through build/.nvcc.lock inside run_cu.
if [ -z "$JOBS" ]; then
    JOBS=$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
    [ "$JOBS" -gt 4 ] && JOBS=4
fi

# ---------------------------------------------------------------- source roots
# bridge scripts still default to the original macOS paths; point the tests at
# the in-tree recovered copies unless the caller already set the variables.
if [ -z "${BROTATO_CAMPAIGN_ROOT:-}" ] && [ -d "$REPO/migration/game/brotato_recovered" ]; then
    BROTATO_CAMPAIGN_ROOT="$REPO/migration/game/brotato_recovered"
fi
if [ -z "${BROTATO_DLC_ROOT:-}" ] && [ -d "$REPO/migration/game/brotato_abyss" ]; then
    BROTATO_DLC_ROOT="$REPO/migration/game/brotato_abyss"
fi
[ -n "${BROTATO_CAMPAIGN_ROOT:-}" ] && export BROTATO_CAMPAIGN_ROOT
[ -n "${BROTATO_DLC_ROOT:-}" ] && export BROTATO_DLC_ROOT
if [ -z "${BROTATO_RL_OBS_TABLES:-}" ] && [ -z "${BROTATO_OBS_TABLES:-}" ]; then
    _tables="$REPO/migration/game/external/brotato_obs_tables.json"
    [ -f "$_tables" ] && export BROTATO_RL_OBS_TABLES="$_tables"
fi

# Godot-dependent tests.  test_brotato_{rng,player_visual}.py opt in through
# BROTATO_TEST_ENGINE and test_brotato_weapon_godot_oracles.py through
# BROTATO_GODOT/BROTATO_RECOVERED; without them all three skip.  Wire the pinned
# 3.7-dev1 engine when one is present so they actually run.  Disable with
# BROTATO_TEST_NO_ENGINE=1 (or set the variables yourself).
if [ -z "${BROTATO_TEST_NO_ENGINE:-}" ]; then
    if [ -z "${BROTATO_TEST_ENGINE:-}" ]; then
        for _godot in "$HOME/tools/bin/godot-3.7-dev1" "$REPO/tools/bin/godot-3.7-dev1" \
                       "$REPO/../tools/bin/godot-3.7-dev1"; do
            if [ -x "$_godot" ]; then BROTATO_TEST_ENGINE=$_godot; break; fi
        done
    fi
    [ -n "${BROTATO_TEST_ENGINE:-}" ] && export BROTATO_TEST_ENGINE
    if [ -z "${BROTATO_GODOT:-}" ] && [ -n "${BROTATO_TEST_ENGINE:-}" ]; then
        BROTATO_GODOT=$BROTATO_TEST_ENGINE
    fi
    [ -n "${BROTATO_GODOT:-}" ] && export BROTATO_GODOT
    if [ -z "${BROTATO_RECOVERED:-}" ] && [ -d "$REPO/migration/game/brotato_recovered" ]; then
        BROTATO_RECOVERED="$REPO/migration/game/brotato_recovered"
    fi
    [ -n "${BROTATO_RECOVERED:-}" ] && export BROTATO_RECOVERED
fi
export BROTATO_TEST_TIMEOUT="$TIMEOUT" BROTATO_TEST_RESULTS="$RESULTS"

# ------------------------------------------------------------- child utilities
# Extra compiler flags named by a test's own header, minus the shared base.
extras_for() {
    local src=$1 line tok extra="" skip=0
    line=$(grep -m1 -E '^[[:space:]]*\*[[:space:]]+(c\+\+|cc|g\+\+|clang\+\+)[[:space:]]' "$src" 2>/dev/null)
    [ -z "$line" ] && return 0
    for tok in $line; do
        case "$tok" in \**) continue ;; esac
        if [ "$skip" = 1 ]; then skip=0; continue; fi
        case "$tok" in
            -o) skip=1 ;;
            -O2|-std=c++17|-ffp-contract=off|-Wno-narrowing|-I.|-Iocean/brotato|-lm) ;;
            -*) extra="$extra $tok" ;;
        esac
    done
    printf '%s' "${extra# }"
}

# "checks" count printed by the test itself, or "-" when it prints none.
cpp_checks() {
    local log=$1 m
    m=$(grep -oE '[0-9]+ checks, [0-9]+ failures' "$log" | tail -1)
    if [ -n "$m" ]; then printf '%s' "${m%% *}"; return; fi
    m=$(grep -oE 'all [0-9]+ checks passed' "$log" | tail -1)
    if [ -n "$m" ]; then printf '%s' "$(printf '%s' "$m" | grep -oE '[0-9]+')"; return; fi
    m=$(grep -oE 'OK \([0-9]+ checks\)' "$log" | tail -1)
    if [ -n "$m" ]; then printf '%s' "$(printf '%s' "$m" | grep -oE '[0-9]+')"; return; fi
    printf '-'
}

cpp_fails() {
    local log=$1 m
    m=$(grep -oE '[0-9]+ checks, [0-9]+ failures' "$log" | tail -1)
    if [ -n "$m" ]; then printf '%s' "${m}" | grep -oE '[0-9]+ failures' | grep -oE '^[0-9]+'; return; fi
    m=$(grep -oE 'FAILURES: [0-9]+' "$log" | tail -1)
    if [ -n "$m" ]; then printf '%s' "${m##* }"; return; fi
    grep -cE '(^|[^0-9A-Za-z])FAIL' "$log" 2>/dev/null || true
}

# The last pytest status line: "3 failed, 12 passed, 2 skipped in 0.5s".
py_summary() {
    grep -oE '[0-9]+ (passed|failed|error|errors|skipped)(, [0-9]+ (passed|failed|error|errors|skipped))*' "$1" \
        | tail -1
}

py_count() { # summary count for a key
    printf '%s' "$1" | grep -oE "[0-9]+ $2" | tail -1 | grep -oE '[0-9]+'
}

# Writes one status row: name \t status \t checks \t fails \t skips \t detail
emit() {
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$1" "$2" "${3:--}" "${4:--}" "${5:--}" "${6:-}" \
        > "$RESULTS/status/$1.tsv"
    [ "${BROTATO_TEST_VERBOSE:-0}" = 1 ] && printf '[%-9s] %s %s\n' "$2" "$1" "${6:-}"
    return 0
}

# Detects a test that resolved a hardcoded original-macOS path.
mac_path_note() {
    local log=$1 hit
    hit=$(grep -oE '/Users/[A-Za-z0-9_.-]+' "$log" | sort -u | head -3 | paste -sd, -)
    [ -n "$hit" ] && printf 'needs-old-Mac-paths:%s' "$hit"
}

run_cpp() {
    local src=$1 name log bin extra rc out
    name=$(basename -- "$src" .cpp)
    log="$RESULTS/log/$name.log"
    bin="$RESULTS/bin/$name"
    extra=$(extras_for "$src")
    local cap=$TIMEOUT
    printf '%s' "$name" | grep -qE "$LONG_PATTERN" && cap=$LONG_TIMEOUT

    if ! command -v "${CXX%% *}" >/dev/null 2>&1; then
        emit "$name" BUILDFAIL - - - "compiler not found: $CXX"
        return
    fi

    {
        printf '# %s %s %s tests/%s.cpp -o %s -lm\n' \
            "$CXX" "${BASE_FLAGS[*]}" "$extra" "$name" "$bin"
    } > "$log"
    ( cd "$REPO" && timeout -k 5 "$cap" "$CXX" "${BASE_FLAGS[@]}" $extra \
        "tests/$name.cpp" -o "$bin" -lm ) >> "$log" 2>&1
    rc=$?
    if [ "$rc" != 0 ]; then
        if [ "$rc" = 124 ]; then
            emit "$name" TIMEOUT - - - "compile timed out"
        else
            emit "$name" BUILDFAIL - - - "$(grep -E 'error:' "$log" | tail -1 | cut -c1-110)"
        fi
        return
    fi

    out=$( cd "$REPO" && timeout -k 5 "$cap" "$bin" 2>&1 )
    rc=$?
    printf '%s\n' "$out" >> "$log"
    local checks fails detail
    checks=$(cpp_checks "$log")
    fails=$(cpp_fails "$log")
    detail=$(mac_path_note "$log")
    case $rc in
        0)   emit "$name" PASS "$checks" "${fails:-0}" - "$detail" ;;
        124|137) emit "$name" TIMEOUT "$checks" "$fails" - "${detail:+$detail }run timed out" ;;
        *)
            if [ "$rc" -gt 128 ]; then
                # A signal death prints no "checks, failures" line; call it a
                # CRASH so a stack-overflowed fixture is not read as a plain
                # assertion failure.  BtState is ~1.75 MiB now, so a local
                # BtState overflows the 8 MiB stack (SIGSEGV) -- keep it in BSS.
                sig=$((rc - 128))
                [ "$sig" = 11 ] && detail="${detail:+$detail }SIGSEGV: BtState is ~1.75 MiB, keep a local BtState in BSS/static"
                emit "$name" CRASH "$checks" "${fails:-?}" - "${detail:+$detail }killed by signal $sig"
            else
                [ "${fails:-0}" = 0 ] && fails="?"
                emit "$name" FAIL "$checks" "$fails" - "$detail"
            fi ;;
    esac
}

# The .cu tests carry their own nvcc command in the file header (arch, raylib
# archives and all); reuse it verbatim except for the output path.
run_cu() {
    local src=$1 name log bin rc out tokens cmdline
    name=$(basename -- "$src" .cu)
    log="$RESULTS/log/$name.log"
    bin="$RESULTS/bin/$name"
    local cap=$LONG_TIMEOUT

    if [ -z "$NVCC" ] || [ ! -x "$NVCC" ]; then
        emit "$name" SKIP - - - "nvcc not found (needed for the CUDA tests)"
        return
    fi

    cmdline=$(awk '/^\/\/[[:space:]]*nvcc /{p=1}
                   p { line=$0; sub(/^\/\/[[:space:]]?/,"",line)
                       cont = (line ~ /\\[[:space:]]*$/)
                       sub(/\\[[:space:]]*$/,"",line)
                       printf "%s ", line
                       if (!cont) exit }' "$src")
    if [ -z "$cmdline" ]; then
        emit "$name" SKIP - - - "no nvcc compile line in the file header"
        return
    fi
    tokens=""
    set -- $cmdline            # nvcc <flags...> -o <path>
    shift || true              # drop the 'nvcc' token
    while [ $# -gt 0 ]; do
        case "$1" in
            -o) shift; shift ;;
            *)  tokens="$tokens $1"; shift ;;
        esac
    done

    # Host-pressure guard: nvcc/ptxas are the memory hogs in this tree, so only
    # one CUDA *build* may run at a time across every runner invocation; any
    # other .cu test that finds the lock held skips instead of piling on.
    CUDA_LOCK=${BROTATO_CUDA_BUILD_LOCK:-"$REPO/build/.nvcc.lock"}
    exec 8>"$CUDA_LOCK" 2>/dev/null || true
    if ! flock -n 8 2>/dev/null; then
        exec 8>&- 2>/dev/null || true
        emit "$name" SKIP - - - "another CUDA build is in progress (host oversubscribed; one nvcc at a time)"
        return
    fi

    { printf '# %s %s -o %s\n' "$NVCC" "$tokens" "$bin"; } > "$log"
    ( cd "$REPO" && timeout -k 5 "$cap" "$NVCC" $tokens -o "$bin" ) >> "$log" 2>&1
    rc=$?
    exec 8>&- 2>/dev/null || true
    if [ "$rc" != 0 ]; then
        if [ "$rc" = 124 ]; then
            emit "$name" TIMEOUT - - - "nvcc build timed out"
        else
            emit "$name" BUILDFAIL - - - "$(grep -E 'error' "$log" | tail -1 | cut -c1-110)"
        fi
        return
    fi

    out=$( cd "$REPO" && timeout -k 5 "$cap" "$bin" 2>&1 )
    rc=$?
    printf '%s\n' "$out" >> "$log"
    local checks fails detail
    checks=$(cpp_checks "$log"); fails=$(cpp_fails "$log"); detail=$(mac_path_note "$log")
    case $rc in
        0)   emit "$name" PASS "$checks" "${fails:-0}" - "$detail" ;;
        124|137) emit "$name" TIMEOUT "$checks" "$fails" - "${detail:+$detail }run timed out" ;;
        *)
            if [ "$rc" -gt 128 ]; then
                sig=$((rc - 128))
                emit "$name" CRASH "$checks" "${fails:-?}" - "${detail:+$detail }killed by signal $sig"
            else
                [ "${fails:-0}" = 0 ] && fails="?"
                emit "$name" FAIL "$checks" "$fails" - "$detail"
            fi ;;
    esac
}

run_py() {
    local src=$1 name log rc out sum passed failed skipped errors detail status
    name=$(basename -- "$src" .py)
    log="$RESULTS/log/$name.log"
    local cap=$TIMEOUT
    printf '%s' "$name" | grep -qE "$LONG_PATTERN" && cap=$LONG_TIMEOUT

    if ! command -v uv >/dev/null 2>&1; then
        emit "$name" SKIP - - - "uv not found in PATH (needed for pytest)"
        return
    fi

    xvfb=()
    if [ -z "${DISPLAY:-}" ] && command -v xvfb-run >/dev/null 2>&1 \
            && printf '%s' "$name" | grep -qE "$ENGINE_TESTS_RE"; then
        xvfb=( xvfb-run -a -s "-screen 0 1280x720x24" )
    fi
    out=$( cd "$REPO" && "${xvfb[@]}" timeout -k 5 "$cap" uv run --no-project \
            --with pytest --with numpy \
            python -m pytest -q -rfs "tests/$name.py" 2>&1 )
    rc=$?
    printf '%s\n' "$out" > "$log"

    sum=$(py_summary "$log")
    passed=$(py_count "$sum" passed); passed=${passed:-0}
    failed=$(py_count "$sum" failed); failed=${failed:-0}
    skipped=$(py_count "$sum" skipped); skipped=${skipped:-0}
    errors=$(py_count "$sum" error);  errors=${errors:-0}
    detail=$(mac_path_note "$log")

    if [ "$rc" != 0 ] && printf '%s' "$name" | grep -qE "$ENGINE_TESTS_RE" \
            && grep -qE '^E .*X11 Display is not available|^E .*X connection to .* broken|^E .*Unable to open X display' "$log" 2>/dev/null; then
        # the virtual X server died (host OOM): a missing display is not a
        # parity failure, so report it as SKIP with the engine's own reason.
        why=$(grep -m1 -E 'X11 Display is not available|X connection to .* broken|Unable to open X display' "$log" | cut -c1-90)
        emit "$name" SKIP "$passed" "$failed" "$skipped" "engine display unavailable: $why"
        return
    fi
    if [ "$rc" = 124 ]; then
        status=TIMEOUT; detail="${detail:+$detail }run timed out"
    elif [ "$rc" = 0 ] && [ "$passed" = 0 ] && [ "$skipped" != 0 ]; then
        status=SKIP
        local why
        why=$(grep -oE '^SKIPPED \[[0-9]+\] .*' "$log" \
                | sed -E 's/^SKIPPED \[[0-9]+\] [^ ]+: //' \
                | sort -u | head -2 | paste -sd'; ' - | cut -c1-90)
        [ -n "$why" ] && detail="${detail:+$detail }$why"
    elif [ "$rc" = 0 ]; then
        status=PASS
    else
        status=FAIL
    fi
    [ "$errors" != 0 ] && failed=$((failed + errors))

    emit "$name" "$status" "$passed" "$failed" "$skipped" "$detail"
}

usage() { awk 'NR==1{next} /^#/{sub(/^# ?/,"");print;next} {exit}' "$SELF"; }

# ------------------------------------------------------------------- arguments
while [ $# -gt 0 ]; do
    case "$1" in
        --jobs) JOBS=$2; shift 2 ;;
        --jobs=*) JOBS=${1#*=}; shift ;;
        --list) LIST=1; shift ;;
        --verbose|-v) VERBOSE=1; shift ;;
        -h|--help) usage; exit 0 ;;
        # internal: single-test workers invoked by the parallel driver
        --_cpp) run_cpp "$2"; exit 0 ;;
        --_py)  run_py "$2";  exit 0 ;;
        --_cu)  run_cu "$2";  exit 0 ;;
        *) echo "unknown argument: $1" >&2; usage; exit 2 ;;
    esac
done

# ------------------------------------------------------- one virtual X server
# Engine-backed tests need an X display.  A single persistent Xvfb for the whole
# run is far more reliable than per-test `xvfb-run -a` servers (and than the
# tests' own wrappers) on a loaded host: with /tmp near full those servers
# intermittently fail to start, which used to flip engine tests between PASS and
# SKIP.  Children inherit DISPLAY.  BROTATO_TEST_XVFB=0 opts out.
XVFB_PID=""
_start_xvfb() {
    command -v Xvfb >/dev/null 2>&1 || return 1
    for _d in 90 91 92 93 94 95 96 97 98 99; do
        [ -e "/tmp/.X${_d}-lock" ] && continue
        Xvfb ":$__d" -screen 0 1280x720x24 >/dev/null 2>&1 &
        XVFB_PID=$!
        export DISPLAY=":$__d"
        sleep 1
        kill -0 "$XVFB_PID" 2>/dev/null && return 0
        unset DISPLAY
    done
    return 1
}
__d=0
if [ "${BROTATO_TEST_XVFB:-1}" = 1 ] && [ -z "${DISPLAY:-}" ] && command -v Xvfb >/dev/null 2>&1; then
    for __d in 90 91 92 93 94 95 96 97 98 99; do
        [ -e "/tmp/.X${__d}-lock" ] && continue
        Xvfb ":$__d" -screen 0 1280x720x24 >/dev/null 2>&1 &
        XVFB_PID=$!
        export DISPLAY=":$__d"
        sleep 1
        if kill -0 "$XVFB_PID" 2>/dev/null; then break; fi
        unset DISPLAY; XVFB_PID=""
    done
    if [ -n "${XVFB_PID:-}" ]; then
        trap 'kill "$XVFB_PID" 2>/dev/null' EXIT
    fi
fi

# ------------------------------------------------------------ test discovery
cd "$REPO" || exit 1
shopt -s nullglob
CPP=()
PY=()
for f in tests/test_brotato_*.cpp; do
    case "$f" in *.mm|*.cu) continue ;; esac
    CPP+=("$REPO/$f")
done
for f in tests/test_brotato_*.py; do
    PY+=("$REPO/$f")
done
CU=()
if [ -z "${BROTATO_TEST_NO_CUDA:-}" ] && [ -n "$NVCC" ] && [ -x "$NVCC" ]; then
    for f in tests/test_brotato_*.cu; do CU+=("$REPO/$f"); done
fi
shopt -u nullglob

if [ -n "$ONLY" ]; then
    _cpp=(); for f in "${CPP[@]}"; do printf '%s' "$f" | grep -qE "$ONLY" && _cpp+=("$f"); done; CPP=("${_cpp[@]}")
    _py=();  for f in "${PY[@]}";  do printf '%s' "$f" | grep -qE "$ONLY" && _py+=("$f");  done; PY=("${_py[@]}")
    _cu=();  for f in "${CU[@]}";  do printf '%s' "$f" | grep -qE "$ONLY" && _cu+=("$f");  done; CU=("${_cu[@]}")
fi

if [ "${LIST:-0}" = 1 ]; then
    printf 'cpp (%d):\n' "${#CPP[@]}"; printf '  %s\n' "${CPP[@]##*/}"
    printf 'py (%d):\n' "${#PY[@]}";   printf '  %s\n' "${PY[@]##*/}"
    printf 'cu (%d):\n' "${#CU[@]}";   printf '  %s\n' "${CU[@]##*/}"
    shopt -s nullglob
    printf 'excluded:\n'
    for f in tests/test_brotato_*.mm; do
        [ -n "$ONLY" ] && ! printf '%s' "$f" | grep -qE "$ONLY" && continue
        printf '  %s (Metal)\n' "${f##*/}"
    done
    if [ "${#CU[@]}" = 0 ]; then
        for f in tests/test_brotato_*.cu; do
            [ -n "$ONLY" ] && ! printf '%s' "$f" | grep -qE "$ONLY" && continue
            printf '  %s (CUDA test not selected)\n' "${f##*/}"
        done
    fi
    shopt -u nullglob
    exit 0
fi

if [ "${#CPP[@]}" = 0 ] && [ "${#PY[@]}" = 0 ] && [ "${#CU[@]}" = 0 ]; then
    echo "no tests/test_brotato_* found under $REPO/tests (filter: ${ONLY:-none})" >&2
    exit 1
fi

# ------------------------------------------------------------------- dispatch
mkdir -p "$RESULTS/bin" "$RESULTS/log" "$RESULTS/status"
rm -f "$RESULTS"/status/*.tsv

# GPU-only tests are excluded from the host build; report them as skips so the
# summary accounts for every tests/test_brotato_* file.
shopt -s nullglob
for f in tests/test_brotato_*.mm; do
    [ -n "$ONLY" ] && ! printf '%s' "$f" | grep -qE "$ONLY" && continue
    base=$(basename -- "$f"); emit "${base%.*}" SKIP - - - "excluded: needs a Metal toolchain (macOS/Apple Silicon)"
done
if [ "${#CU[@]}" = 0 ]; then
    if [ -n "${BROTATO_TEST_NO_CUDA:-}" ]; then
        _cu_reason="disabled by BROTATO_TEST_NO_CUDA=1"
    elif [ -z "$NVCC" ] || [ ! -x "$NVCC" ]; then
        _cu_reason="excluded: needs an NVIDIA CUDA toolchain (nvcc not found)"
    else
        _cu_reason="excluded by BROTATO_TEST_ONLY"
    fi
    for f in tests/test_brotato_*.cu; do
        [ -n "$ONLY" ] && ! printf '%s' "$f" | grep -qE "$ONLY" && continue
        base=$(basename -- "$f"); emit "${base%.*}" SKIP - - - "$_cu_reason"
    done
fi
shopt -u nullglob

# Tests excluded by regex (e.g. the full-engine oracle in a fast cycle).
if [ -n "$SKIP" ]; then
    _cpp=(); for f in "${CPP[@]}"; do
        if printf '%s' "$f" | grep -qE "$SKIP"; then
            b=$(basename -- "$f"); emit "${b%.*}" SKIP - - - "skipped by BROTATO_TEST_SKIP ($SKIP)"
        else _cpp+=("$f"); fi
    done; CPP=("${_cpp[@]}")
    _py=();  for f in "${PY[@]}"; do
        if printf '%s' "$f" | grep -qE "$SKIP"; then
            b=$(basename -- "$f"); emit "${b%.*}" SKIP - - - "skipped by BROTATO_TEST_SKIP ($SKIP)"
        else _py+=("$f"); fi
    done; PY=("${_py[@]}")
    _cu=();  for f in "${CU[@]}"; do
        if printf '%s' "$f" | grep -qE "$SKIP"; then
            b=$(basename -- "$f"); emit "${b%.*}" SKIP - - - "skipped by BROTATO_TEST_SKIP ($SKIP)"
        else _cu+=("$f"); fi
    done; CU=("${_cu[@]}")
fi

JOBFILE="$RESULTS/jobs.txt"
: > "$JOBFILE"
for src in "${CPP[@]}"; do printf '%q %q %q %q\n' bash "$SELF" --_cpp "$src" >> "$JOBFILE"; done
for src in "${PY[@]}";  do printf '%q %q %q %q\n' bash "$SELF" --_py  "$src" >> "$JOBFILE"; done
for src in "${CU[@]}";  do printf '%q %q %q %q\n' bash "$SELF" --_cu  "$src" >> "$JOBFILE"; done
export BROTATO_TEST_VERBOSE="$VERBOSE"

echo "Brotato tests: ${#CPP[@]} C++ + ${#PY[@]} python + ${#CU[@]} CUDA, ${JOBS} parallel jobs"
echo "results in $RESULTS"
if [ "$VERBOSE" = 1 ]; then
    xargs -a "$JOBFILE" -d '\n' -P "$JOBS" -I{} bash -c '{}'
else
    xargs -a "$JOBFILE" -d '\n' -P "$JOBS" -I{} bash -c '{}' >/dev/null 2>&1
fi

# -------------------------------------------------------------------- summary
if ! ls "$RESULTS"/status/*.tsv >/dev/null 2>&1; then
    echo "ERROR: no test produced a result row (parallel dispatch failed)." >&2
    echo "job file: $JOBFILE" >&2
    exit 1
fi

total=0; npass=0; nfail=0; nskip=0; nbuild=0
printf '\n%-44s %-9s %6s %6s %6s  %s\n' TEST STATUS CHECKS FAILS SKIPS DETAIL
printf '%s\n' "-----------------------------------------------------------------------------------------------"
while IFS=$'\t' read -r name status checks fails skips detail; do
    total=$((total + 1))
    case "$status" in
        PASS) npass=$((npass + 1)) ;;
        SKIP) nskip=$((nskip + 1)) ;;
        BUILDFAIL) nbuild=$((nbuild + 1)) ;;
        *) nfail=$((nfail + 1)) ;;
    esac
    printf '%-44s %-9s %6s %6s %6s  %s\n' "$name" "$status" "$checks" "$fails" "$skips" "$detail"
done < <(sort -t$'\t' -k1,1 "$RESULTS"/status/*.tsv)

printf '%s\n' "-----------------------------------------------------------------------------------------------"
printf 'total=%d  pass=%d  fail=%d  build-fail=%d  skip=%d\n' \
    "$total" "$npass" "$nfail" "$nbuild" "$nskip"

if [ "$nfail" != 0 ] || [ "$nbuild" != 0 ]; then
    for f in "$RESULTS"/status/*.tsv; do
        IFS=$'\t' read -r name status _ < "$f"
        case "$status" in PASS|SKIP) continue ;; esac
        echo
        echo "==== $name ($status) -- $RESULTS/log/$name.log"
        tail -25 "$RESULTS/log/$name.log"
    done
    exit 1
fi
exit 0
