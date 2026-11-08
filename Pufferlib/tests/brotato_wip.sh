#!/usr/bin/env bash
# Create (or refresh) a private work mirror of ocean/brotato for safe editing.
#
#   tests/brotato_wip.sh <wip_root>
#
# Copies ocean/brotato/ to <wip_root>/ocean/brotato/ AND a pristine snapshot to
# <wip_root>/.base/ocean/brotato/. tests/brotato_land.sh uses that snapshot as
# the merge base, so landing applies only YOUR changes on top of whatever peers
# landed meanwhile (3-way merge) instead of overwriting their work.
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[[ $# -eq 1 ]] || { echo "usage: $0 <wip_root>" >&2; exit 2; }
WIP="$1"
mkdir -p "$WIP/ocean" "$WIP/.base/ocean"
rm -rf "$WIP/ocean/brotato" "$WIP/.base/ocean/brotato"
cp -a "$REPO/ocean/brotato" "$WIP/ocean/brotato"
cp -a "$REPO/ocean/brotato" "$WIP/.base/ocean/brotato"
echo "wip ready: $WIP/ocean/brotato (base snapshot in $WIP/.base)"
