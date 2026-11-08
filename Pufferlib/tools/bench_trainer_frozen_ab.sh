#!/usr/bin/env bash
# Freeze the trainer's include root and build BOTH halves of a guards A/B from it, so the
# two builds read byte-identical headers no matter what lands in the live tree between
# them (nvcc resolves -I. -Isrc -I$SRC_DIR relative to the cwd, so building from a frozen
# cwd is the way to do this without editing build.sh).
#
# Usage: ./tools/bench_trainer_frozen_ab.sh [DEST]      (default /home/$USER/pt_frozen)
# Output: $DEST/bench_brotato_folded, $DEST/bench_brotato_guarded, and a fingerprint log
#         $DEST/FINGERPRINT.txt (aggregate md5 + sizeof(BtState) at freeze time).
# Then A/B with:
#   ./tools/bench_trainer_regression_ab.sh $DEST/bench_brotato_guarded \
#                                          $DEST/bench_brotato_folded build/bench_sps_ab_guarded
set -euo pipefail
cd "$(dirname "$0")/.."
REPO="$PWD"
DEST="${1:-/home/$USER/pt_frozen}"
export TMPDIR="${TMPDIR:-/home/$USER/tmp}"
mkdir -p "$DEST" "$TMPDIR"

# --- freeze ---------------------------------------------------------------
rm -rf "$DEST/src" "$DEST/ocean" "$DEST/vendor" "$DEST/build" "$DEST/config"
mkdir -p "$DEST/ocean"
cp -a build.sh "$DEST/"
cp -a src "$DEST/"
cp -a ocean/brotato "$DEST/ocean/"
cp -a vendor "$DEST/"
cp -a config "$DEST/"
for d in raylib-5.5_linux_amd64 raylib-5.5_macos; do
    [ -d "$d" ] && cp -a "$d" "$DEST/" || true
done
mkdir -p "$DEST/build"

# --- record what was frozen ----------------------------------------------
{
    echo "frozen at $(date -Is)"
    echo "aggregate md5 of ocean/brotato/*.h + *.cuh: $(md5sum "$DEST"/ocean/brotato/*.h "$DEST"/ocean/brotato/*.cuh | md5sum | cut -d' ' -f1)"
    md5sum "$DEST"/ocean/brotato/brotato_core.h "$DEST"/ocean/brotato/brotato_state.h
} > "$DEST/FINGERPRINT.txt"
cat "$DEST/FINGERPRINT.txt"

# --- build both halves from the frozen root ------------------------------
cd "$DEST"
# Idempotent: skip a half that already exists (survives job kills / restarts).
if [ ! -x build/bench_brotato_folded ]; then
  echo "--- folded (production: brotato.cu includes the plain core)"
  flock "$REPO/build/.nvcc.lock" ./build.sh brotato build/bench_brotato_folded --cu 2>&1 | tail -2
fi
if [ ! -x build/bench_brotato_guarded ]; then
  echo "--- guarded (-DBTFW_EXPERIMENTAL_LANE_SCHED)"
  NVCC_EXTRA=-DBTFW_EXPERIMENTAL_LANE_SCHED flock "$REPO/build/.nvcc.lock" \
      ./build.sh brotato build/bench_brotato_guarded --cu 2>&1 | tail -2
fi
ls -la build/bench_brotato_folded build/bench_brotato_guarded
echo "both halves built from $DEST ($(grep -m1 aggregate "$DEST/FINGERPRINT.txt"))"
