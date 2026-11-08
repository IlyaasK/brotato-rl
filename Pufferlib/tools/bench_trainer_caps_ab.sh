#!/usr/bin/env bash
# Capacity attribution + depth-resolved ratio, all from ONE frozen include root.
# Main's #ifndef guards make the caps build flags:
#   -DBT_MAX=100  -DBT_MAX_PROJ=64  -DBT_MAX_CONS=16  -DBT_MAX_PET=128
# Parts:
#   A) baseline + one-capacity-at-a-time + full-OLD-caps binaries.
#   B) A/B baseline-vs-each at 512/1024/4096 (per-capacity deltas, additive check).
#   C) new/old-cap RATIO at wave 1 vs wave 40 (4096 agents, private cwds for
#      [env] start_wave), which separates the reset term from the per-tick loop term:
#      per-tick ~= step_loops + reset_cost/episode_length, so the ratio must FALL with
#      depth if the reset path is a major term, and be flat if it is not.
set -uo pipefail
export TMPDIR=/home/ilyaas/tmp
REPO=/home/ilyaas/workspace/github.com/IlyaasK/brotato-rl/Pufferlib
export REPO
cd $REPO
DEST=/home/ilyaas/pt_frozen_caps
rm -rf "$DEST"; mkdir -p "$DEST/ocean" "$DEST/build"
cp -a build.sh src vendor config "$DEST/" 2>/dev/null || true
cp -a ocean/brotato "$DEST/ocean/" 2>/dev/null || true
[ -d raylib-5.5_linux_amd64 ] && cp -a raylib-5.5_linux_amd64 "$DEST/" || true
{
  echo "frozen at $(date -Is)"
  echo "aggregate md5: $(md5sum "$DEST"/ocean/brotato/*.h "$DEST"/ocean/brotato/*.cuh | md5sum | cut -d' ' -f1)"
  md5sum "$DEST"/ocean/brotato/brotato_core.h "$DEST"/ocean/brotato/brotato_state.h
} > "$DEST/FINGERPRINT.txt"
cat "$DEST/FINGERPRINT.txt"
cd "$DEST"
build() { local name=$1; shift; local extra=""; for f in "$@"; do extra="$extra $f"; done
  if [ -x "build/bench_$name" ]; then echo "$name already built, skipping"; return 0; fi
  NVCC_EXTRA="$extra" flock $REPO/build/.nvcc.lock ./build.sh brotato "build/bench_$name" --cu > "/tmp/nv_$name.log" 2>&1
  echo "$name build exit=$? ($(tail -1 /tmp/nv_$name.log | cut -c1-40))"; }
AB=$REPO/tools/bench_trainer_regression_ab.sh
build baseline
build oldcaps   -DBT_MAX=100 -DBT_MAX_PROJ=64 -DBT_MAX_CONS=16 -DBT_MAX_PET=128
echo "############ TOTAL new-caps vs old-caps (512/1024/4096 + no-graph 4096)"
"$AB" "$DEST/build/bench_oldcaps" "$DEST/build/bench_baseline" build/bench_sps_caps_total 2>&1 | tail -20
echo "############ depth-resolved new/old-cap ratio (4096 agents, h64, mb8192)"
DEPTHS="wave1:/home/ilyaas/pt_shal wave40:/home/ilyaas/pt_deep40"
for d in $DEPTHS; do
    tag="${d%%:*}"; cwd="${d#*:}"
    for pair in "newcaps:$DEST/build/bench_baseline" "oldcaps:$DEST/build/bench_oldcaps"; do
        which="${pair%%:*}"; bin="${pair#*:}"
        out="$REPO/build/bench_sps_depth_${tag}_${which}"
        mkdir -p "$out"
        ( cd "$cwd" && timeout 600 "$bin" train --vec.total_agents=4096 --policy.hidden_size=256 \
            --policy.num_layers=2 --train.horizon=64 --train.minibatch_size=8192 \
            --train.total_timesteps=24576000 --env.character=demon \
            --env.starting_weapon=weapon_ghost_scepter --base.cudagraphs=1 \
            --base.checkpoint_dir="$out/ckpt" --base.checkpoint_interval=10000000 ) \
            > "$out/run.log" 2>&1
        echo "$tag $which exit=$? $(grep -oE 'SPS +[0-9.]+[KM]?' "$out/run.log" | tail -1) $(grep -oE 'perf +[0-9.]+' "$out/run.log" | tail -1)"
    done
done
python3 - <<'PY'
import os, re, glob, importlib.util, statistics
spec = importlib.util.spec_from_file_location('bt', REPO + '/tools/bench_trainer_sps.py')
bt = importlib.util.module_from_spec(spec); spec.loader.exec_module(bt)
spec2 = importlib.util.spec_from_file_location('rp', REPO + '/tools/bench_trainer_report.py')
rp = importlib.util.module_from_spec(spec2); spec2.loader.exec_module(rp)
print("\n%-8s %-10s %-10s %-8s" % ("depth", "newcaps", "oldcaps", "ratio"))
for tag in ("wave1", "wave40"):
    row = {}
    for which in ("newcaps", "oldcaps"):
        p = f"build/bench_sps_depth_{tag}_{which}/run.log"
        if os.path.exists(p):
            s = rp.plateau_sps(bt.parse_frames(open(p).read())) or 0
            row[which] = s
    n, o = row.get("newcaps", 0), row.get("oldcaps", 0)
    print("%-8s %-10s %-10s %-8s" % (tag, "%.3fM" % (n/1e6), "%.3fM" % (o/1e6),
          ("%.2fx" % (n/o)) if o else "-"))
PY
echo "############ per-capacity decomposition (queued last, least decisive)"
for spec in "onlyproj:-DBT_MAX_PROJ=64" "onlyenemy:-DBT_MAX=100" "onlycons:-DBT_MAX_CONS=16" "onlypet:-DBT_MAX_PET=128"; do
    name="${spec%%:*}"; flag="${spec#*:}"
    NVCC_EXTRA="$flag" flock $REPO/build/.nvcc.lock ./build.sh brotato "build/bench_$name" --cu > "/tmp/nv_$name.log" 2>&1
    echo "$name build exit=$?"
    "$AB" "$DEST/build/bench_baseline" "$DEST/build/bench_$name" "build/bench_sps_caps_$name" 2>&1 | tail -18
done
