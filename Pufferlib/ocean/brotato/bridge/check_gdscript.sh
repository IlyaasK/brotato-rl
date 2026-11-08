#!/usr/bin/env bash
# GDScript-3 parse/load check for the bridge payload.
#
#   bridge/check_gdscript.sh [--pck PACK] [--build] [--timeout SECONDS]
#
# The recurring tree-break class here is a GDScript 3 parse error that only shows
# up when the engine actually loads the mod: `:=` whose initialiser has no static
# type (Dictionary indexing, autoload-singleton calls, .get()/.size()), or a plain
# syntax slip.  ModLoader then aborts the whole mod, ml_options_present comes back
# None and every game run fails with no trace.  Examples from today:
#   rl_run.gd:307  var missing_total := missing["items"].size() + ...
#   rl_obs.gd:1202 var owned := RunData.get_nb_item(...)
#
# A `--check-only` run outside the game context cannot catch these: the parser
# stops at the first unresolved identifier (autoloads/global classes live in the
# pack), so the inference error behind it is never reported.  The only reliable
# check is the engine booting a pack that contains the scripts, which is what this
# does: it boots the pack under xvfb for a couple of physics ticks, requires the
# bridge to instantiate, and prints the engine's own `file:line` for any load-time
# parse/preload error.
#
# Environment:
#   BROTATO_ENGINE   pinned engine (default ~/tools/bin/godot-3.7-dev1)
#   BROTATO_RUN_DIR  run dir holding a built Brotato.rl.pck (default ~/brotato_run)
#   BROTATO_XVFB=0   do not wrap in xvfb-run (when DISPLAY is already set)
#
# Exit status: 0 = the pack's scripts load; 3 = parse/load error or the mod never
# instantiated.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
RUN_DIR="${BROTATO_RUN_DIR:-$HOME/brotato_run}"
PACK=""
BUILD=0
TIMEOUT="180"

while [[ $# -gt 0 ]]; do
	case "$1" in
		--pck)     PACK="$2"; shift 2 ;;
		--build)   BUILD=1; shift ;;
		--timeout) TIMEOUT="$2"; shift 2 ;;
		-h|--help) sed -n '2,30p' "${BASH_SOURCE[0]}"; exit 0 ;;
		*) echo "check_gdscript: unknown argument $1" >&2; exit 2 ;;
	esac
done

PACK="${PACK:-$RUN_DIR/Brotato.rl.pck}"

if [[ "$BUILD" == "1" ]]; then
	echo "check_gdscript: building $PACK first (bridge working tree)"
	EXTERNAL_ROOT="${BROTATO_EXTERNAL_ROOT:-$REPO_ROOT/migration/game/external}"
	if [[ ! -d "$EXTERNAL_ROOT" ]]; then EXTERNAL_ROOT="$HOME/brotato_mod/external"; fi
	python3 "$REPO_ROOT/ocean/brotato/bridge/build_mod_pck.py" build --no-parse-check --refresh-payload --refresh-generated \
		--external-root "$EXTERNAL_ROOT" \
		--gdre "${BROTATO_GDRE:-$HOME/tools/gdre_linux/gdre_tools.x86_64}" \
		--out "$PACK" --inventory "$RUN_DIR/patch_inventory.json" >/dev/null
fi

# The boot lives behind build_mod_pck.py's own xvfb-run.  Under load `xvfb-run -a`
# can pick a display that fails to bind, and the engine then dies with
# "X11 Display is not available" BEFORE the mod logs anything -- which used to be
# reported as "the bridge was never instantiated", i.e. a false parse failure.
# So: use an existing $DISPLAY when there is one, otherwise bind a private Xvfb
# ourselves and retry with a fresh display number on that marker (3 attempts).
CHECK=(python3 "$REPO_ROOT/ocean/brotato/bridge/build_mod_pck.py" check-scripts \
	--pck "$PACK" --timeout "$TIMEOUT")
if [[ -n "${DISPLAY:-}" ]]; then
	BROTATO_XVFB=0 "${CHECK[@]}"
	exit $?
fi
for attempt in 1 2 3; do
	NUM=$((90 + attempt))
	Xvfb ":$NUM" -screen 0 1280x720x24 >/dev/null 2>&1 &
	XVFB_PID=$!
	sleep 2
	OUT="$(DISPLAY=":$NUM" BROTATO_XVFB=0 "${CHECK[@]}" 2>&1)"
	STATUS=$?
	kill "$XVFB_PID" 2>/dev/null || true
	wait "$XVFB_PID" 2>/dev/null || true
	printf '%s\n' "$OUT"
	if ! printf '%s' "$OUT" | grep -q "X11 Display is not available"; then
		exit "$STATUS"
	fi
	echo "check_gdscript: display :$NUM failed to bind; retrying (attempt $attempt/3)" >&2
done
exit 3
