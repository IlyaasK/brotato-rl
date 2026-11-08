#!/usr/bin/env bash
# One-command smoke run for the Brotato RL bridge.
#
#   * loads the retail DLC pack through the bridge before the game autoloads
#     need it (macOS: stages a minimal bundle around the vanilla Godot engine);
#   * builds the patched pack next to the untouched retail pack with
#     GDRE --pck-patch (ocean/brotato/bridge/build_mod_pck.py);
#   * runs one bounded Bull / Danger-5 episode with the bridge environment;
#   * reaps the engine with a wall-clock watchdog, because Godot 3 has no
#     "quit after N seconds" flag and a broken payload must not hang the shell;
#   * validates the full per-tick observation and native-input acceptance gates.
#
# Nothing here modifies the retail pack.  The retail `Brotato.arm64` is a Linux
# aarch64 ELF and cannot run on macOS; on both platforms the engine actually used
# is the vanilla Godot build that matches the pack's bytecode (v3.7.dev1, commit
# 0a9011743), built with tools=yes (the bridge's `editor_feature` path).
#
# Platforms:
#   Darwin - the engine is staged inside a minimal Brotato.app bundle and the
#            staged copy is ad-hoc signed (the source engine bundle is never
#            touched; macOS SIGKILLs a Mach-O whose bundle signature is stale).
#   Linux  - the engine binary is run directly; no bundle, no signing.  With
#            --no-window the x11 platform still creates an (unmapped) window and
#            a GLX context, so when there is no DISPLAY/WAYLAND_DISPLAY the
#            launcher wraps the engine in xvfb-run (set BROTATO_XVFB=0 to omit,
#            or BROTATO_XVFB_DISPLAY to use an existing :N display).
#
# Environment overrides (all optional):
#   BROTATO_REPO          repo root (default relative to this script)
#   BROTATO_RUN_DIR       run/output directory (default $HOME/brotato_run)
#   BROTATO_EXTERNAL_ROOT generated patch sources (default <repo>/migration/game/external,
#                         falling back to $HOME/brotato_mod/external)
#   BROTATO_ENGINE        vanilla engine binary
#                         (Linux default $HOME/tools/bin/godot-3.7-dev1,
#                          macOS default $HOME/tools/Godot.app/Contents/MacOS/Godot)
#   BROTATO_RETAIL_SRC    retail pack source (default <repo>/migration/game/Brotato.pck,
#                         else <run dir>/Brotato.pck); staged into the run dir
#   BROTATO_DLC_SRC       retail DLC pack (default <repo>/migration/game/BrotatoAbyssalTerrors.pck)
#   BROTATO_STEAM_DATA_SRC retail steam_data.json (default <repo>/migration/game/steam_data.json)
#   BROTATO_GDRE          GDRE executable (default $HOME/tools/gdre_linux/gdre_tools.x86_64
#                         on Linux, $HOME/tools/Godot RE Tools.app/Contents/MacOS/Godot RE Tools on macOS)
#   BROTATO_MAX_SECONDS   simulated seconds per run     (default 60)
#   BROTATO_WATCHDOG      wall-clock seconds before termination (default 300)
#   BROTATO_NO_WINDOW     pass --no-window              (default 1)
#   BROTATO_XVFB          Linux: auto (default) wraps with xvfb-run when there is
#                         no display; 0 disables; 1 forces xvfb-run
#   BROTATO_XVFB_SCREEN   Linux: xvfb screen spec (default 1280x720x24)
#   BROTATO_CHAR          profile character id          (default character_bull)
#   BROTATO_WEAPON        explicit starting weapon id string, e.g. weapon_pistol
#                         (default empty: the character's own starting weapons)
#   BROTATO_RL_DANGER     danger 1..5                   (default 5)
#   BROTATO_WAVE_SECONDS  override every wave's duration in seconds (DebugService
#                         custom_wave_duration; -1 = the game's own durations)
#   BROTATO_ENDLESS       set to 1 for an endless run (RunData.is_endless_run,
#                         so the run continues past wave 20)  (default 0).  Pair
#                         with BROTATO_RL_WAVE to start near the endless waves.
#   BROTATO_POLICY        enable the policy harness     (default 0; needs a policy artifact)
#   BROTATO_PARTIAL       write the partial diagnostics trace instead of the
#                         full 161-observation contract trace (default 0)
#   BROTATO_ACTION_SOURCE script | policy | none         (default script)
#   BROTATO_TAG           suffix for the artifacts of this run (e.g. "policy"
#                         writes rl_trace.policy.jsonl / rl_state.policy.json /
#                         smoke.policy.log) so successive runs do not overwrite
#                         each other
#   BROTATO_EXTRA_ARGS    extra engine CLI arguments
#   BROTATO_ISO_HOME      user-data root for this run (default
#                         <run dir>/isolated_home[.<tag>]).  It is recreated empty
#                         for every run unless BROTATO_KEEP_ISO_HOME=1: ModLoader
#                         keeps its own state in the user directory
#                         (mod_user_profiles.json, mod_loader_cache.json), and a
#                         reused directory can come back as "Mods are currently
#                         disabled" (observed: the same pack boots with a fresh
#                         directory and refuses to load the mod with a stale one).
#                         A fresh directory per run also makes parallel runs safe.
#                         It is exported as HOME (plus CFFIXED_USER_HOME on macOS
#                         and the XDG base dirs on Linux) so the engine's own user
#                         directory lands inside it; the retail user directory is
#                         never touched, and the resolved user:// path is asserted
#                         against this root after the run.
#                         Exported as HOME (plus CFFIXED_USER_HOME on macOS and
#                         XDG_DATA_HOME/XDG_CONFIG_HOME/XDG_CACHE_HOME on Linux) so
#                         the engine's user directory lands inside it: the retail
#                         user directory is never touched.  The resolved user://
#                         path is asserted against this root after the run.
#   BROTATO_SKIP_SIGN     macOS only: 1 skips ad-hoc signing the staged copy (debug)
#   BROTATO_SKIP_STAGE    set to 1 to reuse the already-staged retail inputs
#   BROTATO_BOOT_GRACE    seconds to wait for the first RLBRIDGE line (default 45)
#   BROTATO_ALLOW_DISABLED_MODS set to 1 to permit an explicit --disable-mods flag
#                         (the acceptance gates still reject an inert bridge)
#   BROTATO_SKIP_BUILD    set to 1 to reuse an existing Brotato.rl.pck
#   BROTATO_NO_LAUNCH     set to 1 to build and validate only
#   BROTATO_RL_ORDER_HURTS 1 (default) = the bridge resolves a step's simultaneous
#                         hurtbox/hitbox overlaps in a fixed order (ascending
#                         victim instance id).  The engine's own order is
#                         pointer-ordered and is NOT reproducible between runs of
#                         the same binary, which makes a capture a sample rather
#                         than an oracle (migration/HANDOFF.md: "the real game is
#                         not run-to-run deterministic"); 0 reproduces retail.
#   BROTATO_RL_ENEMY_DIAG  path prefix: writes the observer's per-tick enemy view
#                         (path) and the Hurtbox area_entered order (path.events)
#                         for determinism work; empty = off.  Read-only.

set -euo pipefail

REPO_ROOT="${BROTATO_REPO:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)}"
PLATFORM="$(uname -s)"
RUN_DIR="${BROTATO_RUN_DIR:-$HOME/brotato_run}"

# pick_first: first argument that exists (empty arguments are skipped); the last
# argument is the fallback when nothing exists.
pick_first() {
	local candidate
	for candidate in "$@"; do
		if [[ -n "$candidate" && -e "$candidate" ]]; then
			printf '%s\n' "$candidate"
			return 0
		fi
	done
	printf '%s\n' "${!#}"
}

EXTERNAL_ROOT="${BROTATO_EXTERNAL_ROOT:-$(pick_first "$REPO_ROOT/migration/game/external" "$HOME/brotato_mod/external")}"
if [[ "$PLATFORM" == "Darwin" ]]; then
	ENGINE_DEFAULT="$(pick_first "$HOME/tools/Godot.app/Contents/MacOS/Godot")"
	GDRE_DEFAULT="$(pick_first "$HOME/tools/Godot RE Tools.app/Contents/MacOS/Godot RE Tools")"
else
	ENGINE_DEFAULT="$(pick_first "$HOME/tools/bin/godot-3.7-dev1" \
		"$HOME/tools/godot-src/bin/godot.x11.opt.tools.64" \
		"$HOME/tools/godot-src/bin/godot.x11.opt.64")"
	GDRE_DEFAULT="$(pick_first "$HOME/tools/gdre_linux/gdre_tools.x86_64")"
fi
ENGINE_SRC="${BROTATO_ENGINE:-$ENGINE_DEFAULT}"
GDRE="${BROTATO_GDRE:-$GDRE_DEFAULT}"
RETAIL_SRC="${BROTATO_RETAIL_SRC:-$(pick_first "$REPO_ROOT/migration/game/Brotato.pck" "$RUN_DIR/Brotato.pck")}"
DLC_SRC="${BROTATO_DLC_SRC:-$(pick_first "$REPO_ROOT/migration/game/BrotatoAbyssalTerrors.pck" "$RUN_DIR/BrotatoAbyssalTerrors.pck")}"
STEAM_DATA_SRC="${BROTATO_STEAM_DATA_SRC:-$(pick_first "$REPO_ROOT/migration/game/steam_data.json" "$RUN_DIR/steam_data.json")}"
BOOT_GRACE="${BROTATO_BOOT_GRACE:-45}"
MAX_SECONDS="${BROTATO_MAX_SECONDS:-60}"
WATCHDOG_SECONDS="${BROTATO_WATCHDOG:-300}"
NO_WINDOW="${BROTATO_NO_WINDOW:-1}"
POLICY="${BROTATO_POLICY:-0}"
EXTRA_ARGS="${BROTATO_EXTRA_ARGS:-}"

RETAIL_PCK="$RUN_DIR/Brotato.pck"
PATCHED_PCK="$RUN_DIR/Brotato.rl.pck"
APP_DIR="$RUN_DIR/Brotato.app"
OUT_DIR="$RUN_DIR/rl_out"
TAG_SUFFIX=""
if [[ -n "${BROTATO_TAG:-}" ]]; then
	TAG_SUFFIX=".${BROTATO_TAG}"
fi
TRACE="$OUT_DIR/rl_trace${TAG_SUFFIX}.jsonl"
PARTIAL_TRACE="$OUT_DIR/rl_trace.partial${TAG_SUFFIX}.jsonl"
LOG="$OUT_DIR/smoke${TAG_SUFFIX}.log"

BUILD_TOOL="$REPO_ROOT/ocean/brotato/bridge/build_mod_pck.py"
PARSE_TOOL="$REPO_ROOT/ocean/brotato/bridge/parse_trace.py"
PATCH_GEN="$REPO_ROOT/ocean/brotato/bridge/make_steam_offline_patch.py"
TABLES_GEN="$REPO_ROOT/ocean/brotato/bridge/gen_obs_tables.py"
MANIFEST="$REPO_ROOT/ocean/brotato/bridge/patch_manifest.json"
ISO_HOME="${BROTATO_ISO_HOME:-$RUN_DIR/isolated_home${TAG_SUFFIX:-}}"

die() { echo "smoke: $*" >&2; exit 2; }
if command -v sha256sum >/dev/null 2>&1; then
	hash_of() { sha256sum "$1" | cut -d' ' -f1; }
else
	hash_of() { shasum -a 256 "$1" | cut -d' ' -f1; }
fi

for required in "$RETAIL_SRC" "$ENGINE_SRC" "$DLC_SRC" "$STEAM_DATA_SRC" "$GDRE" "$MANIFEST" "$BUILD_TOOL" "$PARSE_TOOL"; do
	[[ -e "$required" ]] || die "missing required input: $required"
done

mkdir -p "$OUT_DIR" "$EXTERNAL_ROOT"

# ---------------------------------------------------------------------------
# 0. Stage the untouched retail inputs into the run directory (outside the repo).
# ---------------------------------------------------------------------------
if [[ "${BROTATO_SKIP_STAGE:-0}" != "1" ]]; then
	if [[ ! -f "$RETAIL_PCK" ]] || ! cmp -s "$RETAIL_SRC" "$RETAIL_PCK"; then
		echo "smoke: staging retail pack -> $RETAIL_PCK"
		cp "$RETAIL_SRC" "$RETAIL_PCK"
	fi
	if [[ ! -f "$RUN_DIR/BrotatoAbyssalTerrors.pck" ]] || ! cmp -s "$DLC_SRC" "$RUN_DIR/BrotatoAbyssalTerrors.pck"; then
		echo "smoke: staging DLC pack -> $RUN_DIR/BrotatoAbyssalTerrors.pck"
		cp "$DLC_SRC" "$RUN_DIR/BrotatoAbyssalTerrors.pck"
	fi
	if [[ ! -f "$RUN_DIR/steam_data.json" ]] || ! cmp -s "$STEAM_DATA_SRC" "$RUN_DIR/steam_data.json"; then
		echo "smoke: staging steam_data.json -> $RUN_DIR/steam_data.json"
		cp "$STEAM_DATA_SRC" "$RUN_DIR/steam_data.json"
	fi
fi
if [[ ! -f "$EXTERNAL_ROOT/steam_data.json" ]] || ! cmp -s "$STEAM_DATA_SRC" "$EXTERNAL_ROOT/steam_data.json"; then
	cp "$STEAM_DATA_SRC" "$EXTERNAL_ROOT/steam_data.json"
fi

# ---------------------------------------------------------------------------
# 1. External generated patch source.
#
# steam_offline.gd is derived from the retail pack's own steam.gdc and is never
# committed; regenerate it if it is missing or stale (read-only on the pack).
# ---------------------------------------------------------------------------
if [[ ! -f "$EXTERNAL_ROOT/steam_offline.gd" ]]; then
	echo "smoke: generating external patch source into $EXTERNAL_ROOT"
	python3 "$PATCH_GEN" --pck "$RETAIL_PCK" --gdre "$GDRE" \
		--manifest "$MANIFEST" --output "$EXTERNAL_ROOT/steam_offline.gd" \
		--sidecar "$EXTERNAL_ROOT/steam_offline.provenance.json"
fi
# Always (re)generate: the tables are a pure function of the core headers, and
# the observation schema needs weapons/characters tables that a tables file
# from an older core does not carry.  A stale artifact would fail the bridge's
# boot (rl_obs requires the weapons/characters tables).
echo "smoke: generating observation tables into $EXTERNAL_ROOT"
python3 "$TABLES_GEN" --output "$EXTERNAL_ROOT/brotato_obs_tables.json"

# ---------------------------------------------------------------------------
# 2. Stage the engine.
#
# macOS: the bundle provides the native macOS executable layout.  Under the
# editor feature the game's path helper instead resolves res://, so the bridge
# loads the DLC explicitly from BROTATO_RL_DLC_PCK before ProgressData
# initialises.  The vanilla engine is a Developer ID signed + notarized bundle;
# copying just its Mach-O into our own bundle invalidates the embedded Info.plist
# hash, and macOS then kills the process immediately with SIGKILL (observed:
# 'invalid Info.plist (plist or signature have been modified) In architecture:
# arm64', exit 137 after 9 ms).  So the STAGED COPY - never the source - is
# signed ad-hoc after the Info.plist exists, and the signature is verified
# before anything is launched.
#
# Linux: no bundle and no signing; the engine binary is used directly.
# ---------------------------------------------------------------------------
if [[ "$PLATFORM" == "Darwin" ]]; then
	BUNDLED_ENGINE="$APP_DIR/Contents/MacOS/Brotato"
	mkdir -p "$APP_DIR/Contents/MacOS"
	# No override.cfg is staged here: a non-code file in Contents/MacOS is treated as
	# a nested executable by codesign (and an in-pack override.cfg is not applied by
	# this engine in --main-pack mode), so user-data isolation is done with HOME /
	# CFFIXED_USER_HOME below.  Remove a stale copy from an earlier staging.
	rm -f "$APP_DIR/Contents/MacOS/override.cfg"
	if [[ ! -f "$BUNDLED_ENGINE" ]] || [[ "$(hash_of "$BUNDLED_ENGINE")" != "$(hash_of "$ENGINE_SRC")" ]]; then
		echo "smoke: staging vanilla engine into the bundle"
		cp "$ENGINE_SRC" "$BUNDLED_ENGINE"
		chmod +x "$BUNDLED_ENGINE"
	fi
	# Drop quarantine/provenance attributes the copy may carry; locally produced
	# code is allowed to run, downloaded provenance flags are not what we want here.
	xattr -cr "$APP_DIR" 2>/dev/null || true

	if [[ ! -f "$APP_DIR/Contents/Info.plist" ]]; then
		cat > "$APP_DIR/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleExecutable</key><string>Brotato</string>
	<key>CFBundleIdentifier</key><string>com.blobfishgames.brotato.rl</string>
	<key>CFBundleName</key><string>Brotato</string>
	<key>CFBundlePackageType</key><string>APPL</string>
	<key>CFBundleVersion</key><string>1.0</string>
	<key>CFBundleShortVersionString</key><string>1.0</string>
	<key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
PLIST
	fi

	# Ad-hoc sign the staged copy (binary first, then the bundle seal).  The source
	# engine bundle is never touched.
	if [[ "${BROTATO_SKIP_SIGN:-0}" != "1" ]]; then
		if ! codesign --verify "$APP_DIR" >/dev/null 2>&1; then
			echo "smoke: ad-hoc signing the staged copy (source engine left untouched)"
			codesign --force --sign - "$BUNDLED_ENGINE" || die "codesign failed for $BUNDLED_ENGINE"
			codesign --force --sign - "$APP_DIR" || die "codesign failed for $APP_DIR"
		fi
		if ! codesign --verify --verbose=2 "$APP_DIR" > "$OUT_DIR/codesign_verify.log" 2>&1; then
			echo "smoke: staged bundle failed signature verification; facts:" >&2
			tail -5 "$OUT_DIR/codesign_verify.log" >&2 || true
			codesign -dv --verbose=4 "$BUNDLED_ENGINE" 2>&1 | sed -n '1,12p' >&2 || true
			xattr -l "$BUNDLED_ENGINE" 2>&1 | head -3 >&2 || true
			die "staged bundle is not launchable; do not launch (macOS would SIGKILL it)"
		fi
		echo "smoke: staged bundle signature verified"
	else
		echo "smoke: BROTATO_SKIP_SIGN=1 - skipping ad-hoc signing (debug only)"
	fi
else
	BUNDLED_ENGINE="$ENGINE_SRC"
	[[ -x "$BUNDLED_ENGINE" ]] || die "engine is not executable: $BUNDLED_ENGINE"
	rm -f "$OUT_DIR/codesign_verify.log"
fi

# ---------------------------------------------------------------------------
# 3. Build the patched pack (retail pack stays untouched).
# ---------------------------------------------------------------------------
if [[ "${BROTATO_SKIP_BUILD:-0}" != "1" ]]; then
	# --refresh-generated: the tables/steam patch were just regenerated from the
	# current core headers and the retail pack, so a moved pin for a regenerable
	# artifact is expected here; committed payload files stay strictly pinned.
	python3 "$BUILD_TOOL" build \
		--refresh-generated \
		--refresh-payload \
		--manifest "$MANIFEST" \
		--pck "$RETAIL_PCK" \
		--out "$PATCHED_PCK" \
		--gdre "$GDRE" \
		--external-root "$EXTERNAL_ROOT" \
		--inventory "$RUN_DIR/patch_inventory.json"
else
	[[ -f "$PATCHED_PCK" ]] || die "BROTATO_SKIP_BUILD=1 but $PATCHED_PCK does not exist"
fi

if [[ "${BROTATO_NO_LAUNCH:-0}" == "1" ]]; then
	echo "smoke: build complete, launch skipped (BROTATO_NO_LAUNCH=1)"
	exit 0
fi

# ---------------------------------------------------------------------------
# 4. One bounded episode, reaped by a wall-clock watchdog.
# ---------------------------------------------------------------------------
# The episode is written to a per-run temporary name and moved onto the canonical
# path only after the parse gate has accepted every row: a killed or failed run
# must never replace a previous valid capture with a partial file.
CAPTURE_ID="capture.$$"
TRACE_WRITE="$TRACE.$CAPTURE_ID"

export BROTATO_RL=1
export BROTATO_RL_TRACE="$TRACE_WRITE"
export BROTATO_RL_META="$TRACE_WRITE.meta.json"
export BROTATO_RL_SUMMARY="$TRACE_WRITE.summary.json"
export BROTATO_RL_CHAR="${BROTATO_RL_CHAR:-${BROTATO_CHAR:-character_bull}}"
export BROTATO_RL_WEAPON="${BROTATO_RL_WEAPON:-${BROTATO_WEAPON:-}}"
export BROTATO_RL_DANGER="${BROTATO_RL_DANGER:-${BROTATO_DANGER:-5}}"
export BROTATO_RL_MAX_TICKS="${BROTATO_RL_MAX_TICKS:-$((MAX_SECONDS * 60))}"
export BROTATO_RL_POLICY="$POLICY"
export BROTATO_RL_ACTION_SOURCE="${BROTATO_ACTION_SOURCE:-script}"
export BROTATO_RL_PARTIAL="${BROTATO_PARTIAL:-0}"
export BROTATO_RL_ENDLESS="${BROTATO_RL_ENDLESS:-${BROTATO_ENDLESS:-0}}"
# Shorten every wave (DebugService.custom_wave_duration) so a bounded run reaches
# shops with enemies still spawning; -1 keeps the game's own wave durations.
export BROTATO_RL_WAVE_SECONDS="${BROTATO_RL_WAVE_SECONDS:-${BROTATO_WAVE_SECONDS:--1}}"
# Deterministic damage resolution (mod default is on): the physics server reports
# a step's simultaneous hurtbox/hitbox overlaps in a pointer-ordered sequence that
# is not reproducible between runs, and that order decides which enemy a one-hit
# projectile damages.  Pinned here so a capture from this launcher is an oracle.
export BROTATO_RL_ORDER_HURTS="${BROTATO_RL_ORDER_HURTS:-1}"
# Runtime loader diagnostics (which options/args the loader actually resolved).
export BROTATO_RL_DIAG="$OUT_DIR/rl_bridge_diag${TAG_SUFFIX}.json"
# Retail DLC pack: under the editor feature the game's own lookup resolves to
# res:// and cannot load a nested pack, so the bridge loads it from this path.
export BROTATO_RL_DLC_PCK="$RUN_DIR/BrotatoAbyssalTerrors.pck"
if [[ "${BROTATO_PARTIAL:-0}" == "1" ]]; then
	TRACE="$PARTIAL_TRACE"
	TRACE_WRITE="$PARTIAL_TRACE.$CAPTURE_ID"
	export BROTATO_RL_TRACE="$TRACE_WRITE"
	export BROTATO_RL_META="$TRACE_WRITE.meta.json"
	export BROTATO_RL_SUMMARY="$TRACE_WRITE.summary.json"
fi

if [[ "$EXTRA_ARGS" == *"--disable-mods"* && "${BROTATO_ALLOW_DISABLED_MODS:-0}" != "1" ]]; then
	die "BROTATO_EXTRA_ARGS contains --disable-mods: this is the only switch in ModLoader 6.3 that sets enable_mods=false (mod_loader_store.gd:207), and the RL bridge is inert without mods. Remove it, or set BROTATO_ALLOW_DISABLED_MODS=1 to proceed anyway."
fi

# --enable-mods is the loader's own CLI toggle (mod_loader.gd:46 honours it when
# REQUIRE_CMD_LINE is set); passing it is harmless here and pins the intent.
CLI_ARGS=(--main-pack "$PATCHED_PCK" --fixed-fps 60 --audio-driver Dummy --enable-mods)
# Godot 3.7.dev1 has no --disable-audio flag (checked against the engine binary),
# so the dummy audio driver is the equivalent switch.
if [[ "$NO_WINDOW" == "1" ]]; then
	CLI_ARGS+=(--no-window)
else
	CLI_ARGS+=(--resolution 640x360 --position 0,0)
fi

# X11 --no-window still creates an unmapped window and a GLX context, so a
# display is required; wrap with xvfb-run when there is none (Linux only).
LAUNCH_CMD=("$BUNDLED_ENGINE")
if [[ "$PLATFORM" != "Darwin" ]]; then
	XVFB_MODE="${BROTATO_XVFB:-auto}"
	if [[ -n "${BROTATO_XVFB_DISPLAY:-}" ]]; then
		export DISPLAY="$BROTATO_XVFB_DISPLAY"
	fi
	if [[ -z "${DISPLAY:-}" ]]; then
		if [[ "$XVFB_MODE" == "0" ]]; then
			die "no DISPLAY and BROTATO_XVFB=0: the x11 platform needs an X server (--no-window still opens one)"
		fi
		command -v xvfb-run >/dev/null 2>&1 || die "no DISPLAY and xvfb-run is not installed"
		LAUNCH_CMD=(xvfb-run -a -s "-screen 0 ${BROTATO_XVFB_SCREEN:-1280x720x24}" "$BUNDLED_ENGINE")
	fi
fi

if [[ "${BROTATO_KEEP_ISO_HOME:-0}" != "1" ]]; then
	rm -rf "$ISO_HOME"
fi
mkdir -p "$ISO_HOME"
export HOME="$ISO_HOME"
# macOS: CFFIXED_USER_HOME; Linux: the XDG base dirs (Godot 3 resolves
# user:// under $XDG_DATA_HOME/godot/app_userdata/<name>).
export CFFIXED_USER_HOME="$ISO_HOME"
export XDG_DATA_HOME="$ISO_HOME/.local/share"
export XDG_CONFIG_HOME="$ISO_HOME/.config"
export XDG_CACHE_HOME="$ISO_HOME/.cache"
mkdir -p "$XDG_DATA_HOME" "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME"
ENGINE_VERSION="$("$BUNDLED_ENGINE" --version 2>&1 | head -1 || true)"
echo "smoke: platform = $PLATFORM | engine = $BUNDLED_ENGINE ($ENGINE_VERSION)"
echo "smoke: isolated home = $ISO_HOME"
echo "smoke: tag = ${BROTATO_TAG:-<none>} | action_source = ${BROTATO_RL_ACTION_SOURCE} | char = ${BROTATO_RL_CHAR} | weapon = ${BROTATO_RL_WEAPON:-<none>} | danger = ${BROTATO_RL_DANGER}"
echo "smoke: launching ${LAUNCH_CMD[*]} ${CLI_ARGS[*]} ${EXTRA_ARGS}"
echo "smoke: log -> $LOG"

# Job control gives the launched job its own process group, so the watchdog can
# reap the whole group (xvfb-run and the engine under it), not just the wrapper.
kill_engine() {
	local signal="$1"
	kill -s "$signal" -- "-$ENGINE_PID" 2>/dev/null || kill -s "$signal" "$ENGINE_PID" 2>/dev/null || true
}

set +e
set -m
"${LAUNCH_CMD[@]}" "${CLI_ARGS[@]}" $EXTRA_ARGS > "$LOG" 2>&1 &
ENGINE_PID=$!
set +m
set -e

WATCHDOG_FIRED=0
EARLY_FAIL=""
DEADLINE=$((SECONDS + WATCHDOG_SECONDS))
# Boot-fatal markers only.  A *runtime* `SCRIPT ERROR` is deliberately NOT here:
# SIGTERMing the engine on the first runtime error truncates the trace mid-row and
# hides the run's real outcome.  Runtime script errors are counted and reported
# after the run instead (and the capture is not published), so a bug stays loud
# without destroying the evidence.
FAIL_MARKERS='Mods are currently disabled|Parse Error|Failed to load script|Cannot open file|invalid Info.plist'
while kill -0 "$ENGINE_PID" 2>/dev/null; do
	if grep -qE "$FAIL_MARKERS" "$LOG" 2>/dev/null; then
		EARLY_FAIL="fatal marker in the engine log"
		echo "smoke: $EARLY_FAIL; stopping early instead of waiting the watchdog" | tee -a "$LOG"
		grep -m3 -E "$FAIL_MARKERS" "$LOG" | sed 's/^/smoke:   /' >&2 || true
		kill_engine TERM
		sleep 1
		kill_engine KILL
		break
	fi
	if (( SECONDS - (DEADLINE - WATCHDOG_SECONDS) >= BOOT_GRACE )) && ! grep -q RLBRIDGE "$LOG" 2>/dev/null; then
		EARLY_FAIL="no RLBRIDGE output after ${BOOT_GRACE}s (the mod never loaded)"
		echo "smoke: $EARLY_FAIL; stopping early" | tee -a "$LOG"
		tail -5 "$LOG" >&2 || true
		kill_engine TERM
		sleep 1
		kill_engine KILL
		break
	fi
	if (( SECONDS >= DEADLINE )); then
		echo "smoke: watchdog expired after ${WATCHDOG_SECONDS}s; terminating pid $ENGINE_PID" | tee -a "$LOG"
		kill_engine TERM
		for _ in 1 2 3 4 5; do
			kill -0 "$ENGINE_PID" 2>/dev/null || break
			sleep 1
		done
		kill_engine KILL
		WATCHDOG_FIRED=1
		break
	fi
	sleep 1
done

set +e
wait "$ENGINE_PID"
ENGINE_STATUS=$?
set -e
echo "smoke: engine exit status $ENGINE_STATUS (watchdog fired: $WATCHDOG_FIRED, early fail: ${EARLY_FAIL:-none})"

# ---------------------------------------------------------------------------
# 4b. Isolation gate: the engine must have written its user data inside our root.
# ---------------------------------------------------------------------------
if [[ ! -f "$BROTATO_RL_DIAG" ]]; then
	echo "smoke: no loader diagnostics at $BROTATO_RL_DIAG - isolation and loader state are unverifiable" >&2
	exit 3
fi
python3 - "$BROTATO_RL_DIAG" "$ISO_HOME" <<'PYCHECK'
import json, sys
diag_path, iso_home = sys.argv[1], sys.argv[2]
try:
    diag = json.load(open(diag_path))
except Exception as exc:  # noqa: BLE001 - the launcher reports the raw error
    print(f"smoke: cannot read {diag_path}: {exc}", file=sys.stderr)
    raise SystemExit(3)
user_dir = str(diag.get("user_dir", ""))
print(f"smoke: engine user_dir = {user_dir or '<absent>'}")
print(f"smoke: custom_user_dir_name = {diag.get('custom_user_dir_name')!r} | "
      f"ml_options_present = {diag.get('ml_options_present')} | enable_mods = {diag.get('enable_mods')}")
if not user_dir.startswith(iso_home.rstrip("/") + "/"):
    print(f"smoke: ISOLATION FAILED - user dir {user_dir!r} is not under {iso_home!r}; "
          "refusing to treat this run's data as isolated", file=sys.stderr)
    raise SystemExit(3)
if diag.get("ml_options_present") is False:
    print("smoke: ModLoaderStore.ml_options is absent - the loader's store script did not initialise; "
          "the disabled-mods path is the null path, see store_script_path/autoload_bindings above", file=sys.stderr)
print("smoke: isolation verified (user data confined to the isolated root)")
PYCHECK
ISO_STATUS=$?
if [[ $ISO_STATUS -ne 0 ]]; then
	exit $ISO_STATUS
fi

# ---------------------------------------------------------------------------
# 5. Validate the full trace by default; partial with BROTATO_PARTIAL=1.
# ---------------------------------------------------------------------------
# A killed writer can leave a partial final line.  Reject any invalid row that is
# not the final one; a partial final row is dropped and reported.  Only complete
# captures are published (see the mv below).
if [[ -f "$TRACE_WRITE" ]]; then
	python3 - "$TRACE_WRITE" <<'PYTRIM'
import json, sys
path = sys.argv[1]
with open(path, 'rb') as handle:
    raw = handle.read()
lines = raw.split(b'\n')
def ok(line):
    try:
        json.loads(line.decode('utf-8'))
        return True
    except Exception:
        return False
bad = [i + 1 for i, line in enumerate(lines) if line.strip() and not ok(line)]
kept = [line for line in lines if line.strip() and ok(line)]
if not bad:
    sys.exit(0)
if bad != [len([1 for line in lines if line.strip()])]:
    print("smoke: trace has an invalid row that is not the final one: line(s) %s" % bad, file=sys.stderr)
    sys.exit(1)
with open(path, 'wb') as handle:
    handle.write(b'\n'.join(kept) + b'\n')
meta = {"trace_truncated_at_line": bad[0], "rows_kept": len(kept),
        "note": "the killed writer's partial final row was dropped; every kept row parses"}
json.dump(meta, open(path + ".repair.json", "w"), indent=1)
print("smoke: dropped a partial final trace row (line %d); %d rows kept (see %s)"
      % (bad[0], len(kept), path + ".repair.json"))
PYTRIM
	TRIM_STATUS=$?
	if [[ "$TRIM_STATUS" -ne 0 ]]; then
		echo "smoke: the capture is structurally invalid; not publishing it" >&2
		exit 2
	fi
fi

echo "smoke: RLBRIDGE lines in the log: $(grep -c RLBRIDGE "$LOG" || true)"
grep -m 15 "^RLBRIDGE:" "$LOG" || true

set +e
if [[ "${BROTATO_PARTIAL:-0}" == "1" ]]; then
	python3 "$PARSE_TOOL" "$TRACE_WRITE" --partial --check --json "$TRACE_WRITE.arrays.json"
else
	python3 "$PARSE_TOOL" "$TRACE_WRITE" --check --json "$TRACE_WRITE.arrays.json"
fi
CHECK_STATUS=$?
set -e
if [[ -n "$EARLY_FAIL" ]]; then
	echo "smoke: FAILED EARLY - $EARLY_FAIL" >&2
	echo "smoke: diag     $OUT_DIR/rl_bridge_diag${TAG_SUFFIX}.json" >&2
	exit 3
fi
if [[ "$ENGINE_STATUS" -ne 0 ]]; then
	# Godot 3.7-dev1's shutdown path can trip an internal assertion after the run's
	# rows, summary and meta are already written:
	#   ERROR: Condition "_first != nullptr" is true.  at: ~List (./core/self_list.h:108)
	# It is nondeterministic and carries no information about the recorded episode, so
	# it is only tolerated when it is the *only* ERROR in the log and the trace gate
	# passed; anything else keeps the hard failure.
	KNOWN_TEARDOWN='Condition "_first != nullptr" is true'
	OTHER_ERRORS="$(grep -E '^ERROR: ' "$LOG" | grep -vF "$KNOWN_TEARDOWN" || true)"
	if [[ "$CHECK_STATUS" -eq 0 && -z "$OTHER_ERRORS" ]] && grep -qF "$KNOWN_TEARDOWN" "$LOG"; then
		echo "smoke: engine exit status $ENGINE_STATUS is Godot's teardown assertion after a completed run" >&2
		echo "smoke: (only ERROR line: $KNOWN_TEARDOWN); the trace gate passed, treating as a warning" >&2
		ENGINE_STATUS=0
	else
		echo "smoke: engine failed with status $ENGINE_STATUS; a valid partial trace is not success" >&2
		[[ -n "$OTHER_ERRORS" ]] && printf 'smoke:   %s\n' "$OTHER_ERRORS" >&2
		exit 3
	fi
fi
# Runtime script errors must not publish a plausible but invalid rollout.
# Let the writer finish rather than terminating it in the middle of a JSON row.
RUNTIME_ERRORS="$(grep -c 'SCRIPT ERROR' "$LOG" || true)"
if [[ "${RUNTIME_ERRORS:-0}" -gt 0 ]]; then
	echo "smoke: $RUNTIME_ERRORS runtime SCRIPT ERROR line(s) in the engine log:" >&2
	grep -m 5 'SCRIPT ERROR' "$LOG" >&2 || true
	CHECK_STATUS=3
fi

# Publish only a fully validated capture: the canonical paths are left untouched
# on any failure, so a previous valid trace is never replaced by a partial one.
if [[ "$CHECK_STATUS" -eq 0 ]]; then
	mv -f "$TRACE_WRITE" "$TRACE"
	mv -f "$TRACE_WRITE.meta.json" "$TRACE.meta.json"
	mv -f "$TRACE_WRITE.summary.json" "$TRACE.summary.json"
	mv -f "$TRACE_WRITE.arrays.json" "$OUT_DIR/rl_state${TAG_SUFFIX}.json"
else
	echo "smoke: the trace gate failed; kept the partial capture at $TRACE_WRITE (canonical trace untouched)" >&2
fi
echo "smoke: diag     $OUT_DIR/rl_bridge_diag${TAG_SUFFIX}.json"
echo "smoke: trace    $TRACE"
echo "smoke: meta     $TRACE.meta.json"
echo "smoke: summary  $TRACE.summary.json"
echo "smoke: arrays   $OUT_DIR/rl_state${TAG_SUFFIX}.json"
exit $CHECK_STATUS
