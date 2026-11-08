#!/usr/bin/env bash
# Atomically land edited brotato files from a private work dir into the repo.
#
#   tests/brotato_wip.sh  <wip_root>                      # once, before editing
#   tests/brotato_land.sh <wip_root> <relative/path> [...]  # when green
#
# Under an exclusive lock, for each path:
#   * if <wip_root>/.base/<path> exists (made by brotato_wip.sh), the change is
#     3-way merged (git merge-file live base wip): only YOUR edits are applied on
#     top of the current repo file, so peers' newer edits are never reverted.
#     Any merge conflict rejects the whole landing.
#   * new files (no live copy) are copied in.
#   * without a base snapshot the wip file must be byte-identical to the live
#     file plus your edits — refused if the live file changed since you copied
#     it is impossible to know, so a missing base is REFUSED for existing files.
# Then tests/test_brotato_core.cpp is compiled and run; on any failure every
# file is restored. On success the base snapshot is refreshed to the landed
# content so you can keep working in the same wip.
set -euo pipefail
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[[ $# -ge 2 ]] || { echo "usage: $0 <wip_root> <path>..." >&2; exit 2; }
WIP="$1"; shift
command -v git >/dev/null || { echo "git (for merge-file) required" >&2; exit 2; }
mkdir -p "$REPO/build"
exec 9>"$REPO/build/.brotato_land.lock"
flock 9
BACKUP="$(mktemp -d)"
STAGE="$(mktemp -d)"
trap 'rm -rf "$BACKUP" "$STAGE"' EXIT
for p in "$@"; do
    [[ -f "$WIP/$p" ]] || { echo "missing $WIP/$p" >&2; exit 2; }
    mkdir -p "$STAGE/$(dirname "$p")" "$BACKUP/$(dirname "$p")"
    if [[ -f "$REPO/$p" ]]; then
        cp -p "$REPO/$p" "$BACKUP/$p"
        if [[ -f "$WIP/.base/$p" ]]; then
            cp "$REPO/$p" "$STAGE/$p"
            if ! git merge-file -q "$STAGE/$p" "$WIP/.base/$p" "$WIP/$p"; then
                echo "LAND REJECTED: merge conflict in $p (peer edited the same lines); refresh with tests/brotato_wip.sh and re-apply" >&2
                exit 1
            fi
        elif cmp -s "$REPO/$p" "$WIP/$p"; then
            cp "$WIP/$p" "$STAGE/$p"
        else
            echo "LAND REJECTED: $p has no base snapshot ($WIP/.base/$p); create the wip with tests/brotato_wip.sh so peers' edits are merged, not overwritten" >&2
            exit 1
        fi
    else
        cp "$WIP/$p" "$STAGE/$p"
    fi
done
restore() {
    for p in "$@"; do
        if [[ -f "$BACKUP/$p" ]]; then cp -p "$BACKUP/$p" "$REPO/$p"; else rm -f "$REPO/$p"; fi
    done
}
for p in "$@"; do
    mkdir -p "$REPO/$(dirname "$p")"
    cp "$STAGE/$p" "$REPO/$p"
done
BIN="$(mktemp -u /tmp/brotato_land_core.XXXXXX)"
if ! (cd "$REPO" && g++ -std=c++17 -O1 -Wno-narrowing -ffp-contract=off -I. -Iocean/brotato \
        tests/test_brotato_core.cpp -o "$BIN" -lm); then
    echo "LAND REJECTED: test_brotato_core does not compile; repo restored" >&2
    restore "$@"; exit 1
fi
if ! "$BIN" >/dev/null; then
    echo "LAND REJECTED: test_brotato_core fails; repo restored" >&2
    rm -f "$BIN"; restore "$@"; exit 1
fi
rm -f "$BIN"
for p in "$@"; do
    mkdir -p "$WIP/.base/$(dirname "$p")"
    cp "$REPO/$p" "$WIP/.base/$p"
    cp "$REPO/$p" "$WIP/$p"
done
echo "LANDED: $*"
