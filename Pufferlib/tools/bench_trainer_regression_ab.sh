#!/usr/bin/env bash
# A/B two trainer binaries on an identical command line (same argv, same cwd/config),
# at several agent counts, and print the comparison the regression work needs:
# interior-median SPS, wall-clock per-tick, the SPS ratio, and the absolute per-env-tick
# delta. Written for the 00:23-vs-current Brotato regression, and reusable for any
# before/after pair (e.g. folded guards vs guarded).
#
# Usage: ./tools/bench_trainer_regression_ab.sh OLD_BIN NEW_BIN [OUT_ROOT]
# Example: ./tools/bench_trainer_regression_ab.sh build/bench_brotato_bf16 ./puffer
#
# Notes: both binaries must be run from the repo root so they load the same
# config/brotato.ini; per-tick is wall-clock (epoch_batch/SPS) because the graph-on
# rollout accumulator double-counts under async.
set -uo pipefail
cd "$(dirname "$0")/.."
export TMPDIR="${TMPDIR:-/home/$USER/tmp}"
OLD="${1:?old binary}"; NEW="${2:?new binary}"
ROOT="${3:-build/bench_sps_ab}"
HARNESS=tools/bench_trainer_sps.py
mkdir -p "$ROOT/ckpt" "$TMPDIR"

RUNS="$ROOT/runs.txt"
cat > "$RUNS" <<'EOF'
ab0512_h64_mb8k | --vec.total_agents=512  --policy.hidden_size=256 --policy.num_layers=2 --train.horizon=64 --train.minibatch_size=8192 --train.total_timesteps=4000000  --env.character=demon --env.starting_weapon=weapon_ghost_scepter --base.cudagraphs=1
ab1024_h64_mb8k | --vec.total_agents=1024 --policy.hidden_size=256 --policy.num_layers=2 --train.horizon=64 --train.minibatch_size=8192 --train.total_timesteps=8000000  --env.character=demon --env.starting_weapon=weapon_ghost_scepter --base.cudagraphs=1
ab4096_h64_mb8k | --vec.total_agents=4096 --policy.hidden_size=256 --policy.num_layers=2 --train.horizon=64 --train.minibatch_size=8192 --train.total_timesteps=24576000 --env.character=demon --env.starting_weapon=weapon_ghost_scepter --base.cudagraphs=1
ab1024_h64_mb8k_nog | --vec.total_agents=1024 --policy.hidden_size=256 --policy.num_layers=2 --train.horizon=64 --train.minibatch_size=8192 --train.total_timesteps=8000000  --env.character=demon --env.starting_weapon=weapon_ghost_scepter --base.cudagraphs=-1
ab4096_h64_mb8k_nog | --vec.total_agents=4096 --policy.hidden_size=256 --policy.num_layers=2 --train.horizon=64 --train.minibatch_size=8192 --train.total_timesteps=24576000 --env.character=demon --env.starting_weapon=weapon_ghost_scepter --base.cudagraphs=-1
EOF

GLOBALS="--base.checkpoint_dir=$ROOT/ckpt --base.checkpoint_interval=10000000"
for pair in "old:$OLD" "new:$NEW"; do
    tag="${pair%%:*}"; bin="${pair#*:}"
    echo "=== $tag ($bin)"
    python3 "$HARNESS" --bin "$bin" --runs "$RUNS" --out "$ROOT/$tag" --timeout 90 --window 3 \
        --globals "$GLOBALS" | grep -E "SPS=" || true
done
python3 tools/bench_trainer_report.py "$ROOT/old" "$RUNS" "$ROOT/new" "$RUNS" > "$ROOT/ab_report.md" 2>&1 || true
echo
echo "=== wall-clock comparison (per-tick = agents*horizon/SPS) ==="
python3 - "$ROOT" <<'PY'
import glob, os, re, statistics, sys
root = sys.argv[1]
def plateau(path):
    # reuse the harness parser without importing its CLI
    import importlib.util
    spec = importlib.util.spec_from_file_location('bt', 'tools/bench_trainer_sps.py')
    bt = importlib.util.module_from_spec(spec); spec.loader.exec_module(bt)
    spec2 = importlib.util.spec_from_file_location('rp', 'tools/bench_trainer_report.py')
    rp = importlib.util.module_from_spec(spec2); spec2.loader.exec_module(rp)
    fr = bt.parse_frames(open(path).read())
    return (rp.plateau_sps(fr) or 0), (bt.summarize(fr, 3).get('vram_used_gb'))
rows = {}
for tag in ('old', 'new'):
    for p in sorted(glob.glob(f'{root}/{tag}/*.log')):
        n = os.path.basename(p)[:-4]
        rows.setdefault(n, {})[tag] = plateau(p)
print("%-22s %-12s %-12s %-8s %-14s %s" % ('run','old SPS','new SPS','ratio','old us/tick','new us/tick'))
for n in sorted(rows):
    o = rows[n].get('old', (0, None)); w = rows[n].get('new', (0, None))
    m = re.search(r'_(\d+)_h', n); agents = int(m.group(1)) if m else 0
    per = agents*64/1e6  # epoch batch in steps (millions) -> tick us = batch/SPS
    ot = per/o[0]*1e6 if o[0] else 0
    wt = per/w[0]*1e6 if w[0] else 0
    print("%-22s %-12.3f %-12.3f %-8s %-14.1f %.1f" % (
        n, o[0]/1e6, w[0]/1e6, ("%.2fx" % (o[0]/w[0])) if w[0] else "-", ot, wt))
PY

# Structural side of the same question (Main's point: GPU cost is register/stack bound,
# which SPS alone does not show): resource usage of the plain step kernel in each binary.
echo
echo "=== cuobjdump resource usage for bt_gpu_step_kernel / btfw_step_kernel ==="
for pair in "old:$OLD" "new:$NEW"; do
    tag="${pair%%:*}"; bin="${pair#*:}"
    [ -x "$bin" ] || continue
    echo "-- $tag ($bin)"
    cuobjdump -res-usage "$bin" 2>/dev/null \
        | awk -v t="$tag" '/Function .*(bt_gpu_step_kernel|btfw_step_kernel)/{fn=$0; getline r; getline s; print t": "fn" | "r" | "s}' \
        | sed 's/  */ /g' | head -4
done
rm -f "$RUNS"*
