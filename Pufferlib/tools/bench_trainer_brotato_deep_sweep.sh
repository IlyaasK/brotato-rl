#!/usr/bin/env bash
# Deep-endless vs shallow trainer sweep for the Brotato GPU env.
#
# Runs from private config cwds (see bench_trainer_make_deep_cwds.sh) because the
# trainer only accepts [env] start_wave / warp_step / audio_rng when the key exists in
# the loaded config file; a CLI-only override exits with "missing key [env] <key>".
#
# Each row advances AGENTS * 6000 game ticks (total_timesteps = agents * ticks_per_env)
# so agent counts are compared over the same game-time window.
#
# Usage: ./tools/bench_trainer_brotato_deep_sweep.sh [BIN] [DEST_ROOT]
#   BIN        trainer binary                (default ./puffer)
#   DEST_ROOT  private cwd root              (default /home/$USER)
set -uo pipefail
cd "$(dirname "$0")/.."
export TMPDIR="${TMPDIR:-/home/$USER/tmp}"
REPO="$PWD"
BIN="${1:-$REPO/puffer}"
ROOT="${2:-/home/$USER}"
OUT="$REPO/build/bench_sps_brdeep"
mkdir -p "$OUT/ckpt" "$TMPDIR"
COMMON="--policy.hidden_size=256 --policy.num_layers=2 --env.character=demon \
--env.starting_weapon=weapon_ghost_scepter --base.checkpoint_dir=$OUT/ckpt \
--base.checkpoint_interval=10000000"

run() { # name cwd agents horizon mb ticks_per_env [extra...]
  local name=$1 cwd=$2 agents=$3 h=$4 mb=$5 tpe=$6; shift 6
  local ts=$((agents * tpe))
  ( cd "$cwd" && timeout 900 "$BIN" train --vec.total_agents=$agents \
      --train.horizon=$h --train.minibatch_size=$mb --train.total_timesteps=$ts \
      $COMMON "$@" ) > "$OUT/$name.log" 2>&1
  echo "$name exit=$?"
}

for c in pt_deep25 pt_deep40 pt_deep60 pt_shal pt_warp60 pt_warp1; do
  [ -d "$ROOT/$c" ] || { echo "missing cwd $ROOT/$c - run tools/bench_trainer_make_deep_cwds.sh"; exit 1; }
done

run deep25_4096_h64_mb8k     "$ROOT/pt_deep25" 4096 64 8192  6000
run deep40_4096_h64_mb8k     "$ROOT/pt_deep40" 4096 64 8192  6000
run deep60_4096_h64_mb8k     "$ROOT/pt_deep60" 4096 64 8192  6000
run deep60_4096_h64_mb8k_nog "$ROOT/pt_deep60" 4096 64 8192  6000 --base.cudagraphs=-1
run deep60_6144_h64_mb8k     "$ROOT/pt_deep60" 6144 64 8192  6000
run deep60_4096_h128_mb32k   "$ROOT/pt_deep60" 4096 128 32768 6000
run shal_4096_h64_mb8k       "$ROOT/pt_shal"   4096 64 8192  6000
run shal_6144_h64_mb8k       "$ROOT/pt_shal"   6144 64 8192  6000
run warp60_4096_h64_mb8k     "$ROOT/pt_warp60" 4096 64 8192  6000
run warp60_4096_h64_mb8k_nog "$ROOT/pt_warp60" 4096 64 8192  6000 --base.cudagraphs=-1
run warp1_4096_h64_mb8k      "$ROOT/pt_warp1"  4096 64 8192  6000
run inv40_4096_h64_mb8k      "$ROOT/pt_inv40" 4096 64 8192  6000   # sustained deep crowd (invulnerable=1); needs a build containing the flag
run inv40_4096_h64_mb8k_nog  "$ROOT/pt_inv40" 4096 64 8192  6000 --base.cudagraphs=-1
run inv60_4096_h64_mb8k      "$ROOT/pt_inv60" 4096 64 8192  6000
run inv40_6144_h64_mb8k      "$ROOT/pt_inv40" 6144 64 8192  6000   # 8192 would OOM at ~1.42 MiB/agent

echo "=== deep sweep done; rendering ==="
python3 "$REPO/tools/bench_trainer_report.py" "$OUT" "$REPO/tools/bench_trainer_runs_brotato_deep.txt"
