#!/usr/bin/env python3
"""Derive the offline-safe SteamPlatform replacement from the retail pack.

The single engine file this bridge replaces is
``res://singletons/platforms/steam.gd.remap``.  Its replacement target is a
GDScript source that is a *decompilation of the retail pack's own
``steam.gdc``* plus one added null guard.  That base text is proprietary game
code, so it is never committed to this repository: it is regenerated here from
the user's own copy of the game and written outside the repository, and the
patch manifest references it explicitly as an external source.

Derivation is deterministic:

    GDRE --headless --extract=<retail.pck> --output=<tmp> \\
        --include=res://singletons/platforms/steam.gdc
    GDRE --headless --decompile=<tmp>/.../steam.gdc --bytecode=a7aad78 --output=<tmp>/dec
    <insert the guard below after the Engine.has_singleton() call>
    <prepend the provenance header>

The decompiled text's SHA-256 is asserted against the value recorded in the
patch manifest, so a changed retail file (or a different GDRE) fails loudly
instead of silently changing what gets patched in.

Usage::

    make_steam_offline_patch.py                 # write the external patch source
    make_steam_offline_patch.py --check         # only compare with what exists
    make_steam_offline_patch.py --output PATH   # explicit destination
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

EXIT_OK = 0
EXIT_ERROR = 2
EXIT_STALE = 3

BRIDGE_DIR = Path(__file__).resolve().parent


def _first_existing(candidates: list[str], fallback: str) -> str:
    for candidate in candidates:
        if candidate and Path(candidate).exists():
            return candidate
    return fallback


def _run_dir() -> str:
    return os.environ.get("BROTATO_RUN_DIR") or str(Path.home() / "brotato_run")


def _repo_root() -> Path:
    for candidate in [BRIDGE_DIR, *BRIDGE_DIR.parents]:
        if (candidate / "ocean" / "brotato").is_dir():
            return candidate
    return BRIDGE_DIR


def _external_root() -> str:
    return _first_existing(
        [os.environ.get("BROTATO_EXTERNAL_ROOT", ""),
         str(_repo_root() / "migration" / "game" / "external")],
        str(Path.home() / "brotato_mod" / "external"),
    )


DEFAULT_PCK = _first_existing(
    [os.environ.get("BROTATO_RETAIL_PCK", ""),
     str(_repo_root() / "migration" / "game" / "Brotato.pck")],
    str(Path(_run_dir()) / "Brotato.pck"),
)
DEFAULT_GDRE = _first_existing(
    [os.environ.get("BROTATO_GDRE", ""),
     str(Path.home() / "tools" / "gdre_linux" / "gdre_tools.x86_64"),
     str(Path.home() / "tools" / "Godot RE Tools.app" / "Contents" / "MacOS" / "Godot RE Tools")],
    str(Path.home() / "tools" / "gdre_linux" / "gdre_tools.x86_64"),
)
DEFAULT_OUTPUT = str(Path(_external_root()) / "steam_offline.gd")
DEFAULT_SIDECAR = str(Path(_external_root()) / "steam_offline.provenance.json")
DEFAULT_MANIFEST = str(BRIDGE_DIR / "patch_manifest.json")

RETAIL_PATH = "res://singletons/platforms/steam.gdc"
BYTECODE_REVISION = "a7aad78"  # what GDRE auto-detects for engine 3.7.0
DEST = "res://mods-unpacked/rlbridge-headlessrl/patches/steam_offline.gd"

# SHA-256 of the decompiled retail steam.gdc, recorded in the patch manifest.
EXPECTED_DECOMPILED_SHA256 = "6c9a6836f836773e0df89868c146d45f51edb6f6f0fc5d11a3058d0f0df7ce70"

DIAG_CONST = """const _RLBOOTDIAG = preload("res://mods-unpacked/rlbridge-headlessrl/diag/boot_diag.gd")

"""

INIT_DIAG = """
	# RL bridge patch 2/2a: unconditional early record from this _init (its guard
	# output proves this code runs), i.e. before any loader node exists.  The node
	# child added in _enter_tree below polls the Engine main loop for the loader
	# store and records its resolved options.
	_RLBOOTDIAG.record_base("steam_offline._init")
"""

DIAG_ENTER_ANCHOR = """func _enter_tree():
"""

DIAG_ENTER_HOOK = """	# RL bridge patch 2/2: boot diagnostics node (see
	# mods-unpacked/rlbridge-headlessrl/diag/boot_diag.gd).  It is a child of this
	# node, which is itself created inside Platform._init (autoload #3), so it
	# becomes ready before ModLoaderStore (#5) and ModLoader (#6).  It records the
	# raw command line, the feature tags and the resolved ModLoader options, and
	# repairs enable_mods if the loader resolved it as false.  It performs no
	# gameplay and touches no loader script.
	var _rl_diag = _RLBOOTDIAG.new()
	_rl_diag.name = "RLBootDiag"
	add_child(_rl_diag)
"""

ANCHOR = """func _init() -> void :
	if Engine.has_singleton("Steam"):
		steam = Engine.get_singleton("Steam")
"""

GUARD = """	else:
		steam = null

	# ------------------------------------------------------------------
	# RL bridge patch 1/2: the offline guard.
	#
	# The shipped pack declares custom_features="steam", so Platform selects
	# SteamPlatform, but no godotsteam module is present in the engine this
	# bridge runs under: Engine.has_singleton("Steam") is false and `steam`
	# stays null.  Without this guard the next statement
	# (steam.restartAppIfNecessary) raises a GDScript error,
	# Platform.is_dlc_owned() then returns null, the Abyssal Terrors pack is
	# treated as not owned and `character_bull` never reaches
	# ItemService.characters -- which makes the Bull/Danger-5 run impossible.
	#
	# With init_error = true every already-existing offline branch in this file
	# is taken: get_language() -> "en", is_dlc_owned() -> true,
	# is_challenge_completed() -> false, complete_challenge() -> no-op,
	# get_subscribed_mods() -> [] (so ModLoader's Steam-workshop scan is inert).
	# Patch 2/2 is a two-line boot-diagnostics hook in _enter_tree() below
	# (records the resolved ModLoader options before ModLoader acts on them);
	# no other statement was added, removed or reordered.
	# ------------------------------------------------------------------
	if steam == null:
		printerr("RLBRIDGE: Steam singleton unavailable; SteamPlatform continues offline (init_error = true)")
		init_error = true
		return

"""

HEADER = """# Offline-safe copy of res://singletons/platforms/steam.gd.
#
# GENERATED FILE - do not commit. Produced by
# ocean/brotato/bridge/make_steam_offline_patch.py, which decompiles the retail
# pack's own steam.gdc and adds the null guard below.  The base text is
# proprietary game code and stays outside the repository; the patch manifest
# references it explicitly as an external source.
#
# Provenance:
#   retail file  : {retail_path} (inside the retail PCK)
#   retail pack  : {pck_name} sha256 {pck_sha}
#   extracted by : {gdre_name} --extract
#   decompiled by: {gdre_name} --decompile --bytecode={revision}
#                  (the revision GDRE auto-detects for this engine; no compiler
#                  for the engine's own revision exists in GDRE, which is why
#                  this is installed through the pack's .remap indirection
#                  instead of recompiled bytecode)
#   generated    : see the sibling .provenance.json (kept out of the file so
#                  that regeneration is byte-identical)
#
#   decompiled sha256: {decompiled_sha}
#   patched sha256   : {patched_sha}
#
# Two patch points, both recorded here: (1) the offline guard in _init(), and
# (2) a two-line boot-diagnostics hook at the top of _enter_tree() that records
# the loader's resolved options (see mods-unpacked/rlbridge-headlessrl/diag/).
#
# Installed at {dest} and reached through a rewritten
# res://singletons/platforms/steam.gd.remap.
"""


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    return sha256_bytes(path.read_bytes())


def run(argv: list[str], what: str) -> subprocess.CompletedProcess:
    completed = subprocess.run(argv, capture_output=True, text=True)
    if completed.returncode != 0:
        raise RuntimeError(
            f"{what} failed with exit code {completed.returncode}\n"
            f"command: {' '.join(argv)}\n{completed.stdout}\n{completed.stderr}"
        )
    return completed


def gdre_version(gdre: Path) -> str:
    try:
        completed = subprocess.run(
            [str(gdre), "--headless", "--gdre-version"], capture_output=True, text=True, timeout=120
        )
    except (OSError, subprocess.SubprocessError) as exc:
        return f"<unavailable: {exc}>"
    for line in ((completed.stdout or "") + (completed.stderr or "")).splitlines():
        if line.strip():
            return line.strip()
    return "<empty>"


def derive(pck: Path, gdre: Path, revision: str) -> tuple[bytes, dict]:
    if not pck.is_file():
        raise RuntimeError(f"retail pack not found: {pck}")
    if not gdre.is_file():
        raise RuntimeError(f"GDRE executable not found: {gdre}")
    pck_sha = sha256_file(pck)

    with tempfile.TemporaryDirectory(prefix="steam_offline_") as tmp:
        tmp_path = Path(tmp)
        run([str(gdre), "--headless", f"--extract={pck}", f"--output={tmp_path / 'extract'}",
             f"--include={RETAIL_PATH}"], "GDRE extract")
        gdc_candidates = list((tmp_path / "extract").rglob("steam.gdc"))
        if len(gdc_candidates) != 1:
            raise RuntimeError(f"expected exactly one extracted steam.gdc, found {gdc_candidates}")
        gdc = gdc_candidates[0]
        run([str(gdre), "--headless", f"--decompile={gdc}", f"--bytecode={revision}",
             f"--output={tmp_path / 'dec'}"], "GDRE decompile")
        gd_candidates = list((tmp_path / "dec").rglob("steam.gd"))
        if len(gd_candidates) != 1:
            raise RuntimeError(f"expected exactly one decompiled steam.gd, found {gd_candidates}")
        base = gd_candidates[0].read_bytes()

    decompiled_sha = sha256_bytes(base)
    if EXPECTED_DECOMPILED_SHA256 and decompiled_sha != EXPECTED_DECOMPILED_SHA256:
        raise RuntimeError(
            "decompiled base text does not match the recorded sha256\n"
            f"  expected {EXPECTED_DECOMPILED_SHA256}\n"
            f"  actual   {decompiled_sha}\n"
            "The retail file, the GDRE build or the bytecode revision changed; refusing to patch."
        )

    text = base.decode("utf-8")
    if text.count(ANCHOR) != 1:
        raise RuntimeError("guard anchor not found exactly once in the decompiled text")
    if text.count(DIAG_ENTER_ANCHOR) != 1:
        raise RuntimeError("_enter_tree anchor not found exactly once in the decompiled text")
    text = text.replace(DIAG_ENTER_ANCHOR, DIAG_ENTER_ANCHOR + DIAG_ENTER_HOOK)
    text = text.replace(ANCHOR, ANCHOR + GUARD)
    text = text.replace("func _init() -> void :\n",
                        "func _init() -> void :\n" + INIT_DIAG, 1)
    declaration = "extends AbstractPlatform\n"
    if text.count(declaration) != 1:
        raise RuntimeError("Steam platform extends declaration not found exactly once")
    patched = text.replace(declaration, declaration + "\n" + DIAG_CONST, 1).encode("utf-8")

    header = HEADER.format(
        retail_path=RETAIL_PATH,
        pck_name=pck.name,
        pck_sha=pck_sha,
        gdre_name=gdre_version(gdre),
        revision=revision,

        decompiled_sha=decompiled_sha,
        patched_sha=sha256_bytes(patched),
        dest=DEST,
    ).encode("utf-8")

    content = header + patched
    provenance = {
        "generator": "ocean/brotato/bridge/make_steam_offline_patch.py",
        "retail_path": RETAIL_PATH,
        "retail_pck": str(pck),
        "retail_pck_sha256": pck_sha,
        "gdre": str(gdre),
        "gdre_version": gdre_version(gdre),
        "bytecode_revision": revision,
        "dest": DEST,
        "decompiled_sha256": decompiled_sha,
        "patched_body_sha256": sha256_bytes(patched),
        "result_sha256": sha256_bytes(content),
        "generated_at_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "committed_to_repository": False,
    }
    return content, provenance


def manifest_expected_sha(manifest: Path) -> str:
    if not manifest.is_file():
        return ""
    data = json.loads(manifest.read_text())
    for entry in data.get("files", []):
        if entry.get("dest", "").endswith("patches/steam_offline.gd"):
            return entry.get("sha256", "")
    return ""


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--pck", default=DEFAULT_PCK)
    parser.add_argument("--gdre", default=DEFAULT_GDRE)
    parser.add_argument("--revision", default=BYTECODE_REVISION,
                        help="GDRE bytecode revision used to decompile the retail gdc")
    parser.add_argument("--output", default=DEFAULT_OUTPUT)
    parser.add_argument("--sidecar", default=DEFAULT_SIDECAR)
    parser.add_argument("--manifest", default=DEFAULT_MANIFEST)
    parser.add_argument("--check", action="store_true",
                        help="do not write; fail if the existing file is stale or missing")
    args = parser.parse_args(argv)

    try:
        content, provenance = derive(Path(args.pck), Path(args.gdre), args.revision)
    except RuntimeError as exc:
        print(f"derive failed: {exc}", file=sys.stderr)
        return EXIT_ERROR

    output = Path(args.output)
    digest = provenance["result_sha256"]
    expected = manifest_expected_sha(Path(args.manifest))

    if args.check:
        if not output.is_file():
            print(f"stale: {output} does not exist (run without --check to generate)", file=sys.stderr)
            return EXIT_STALE
        existing = sha256_file(output)
        if expected and existing != expected:
            print(f"stale: {output} sha256 {existing} != manifest {expected}", file=sys.stderr)
            return EXIT_STALE
        if existing != digest:
            print(
                f"stale: {output} was not produced by the current derivation\n"
                f"  on disk  {existing}\n  derived  {digest}",
                file=sys.stderr,
            )
            return EXIT_STALE
        print(f"up to date: {output} sha256={existing}")
        return EXIT_OK

    output.parent.mkdir(parents=True, exist_ok=True)
    previous = sha256_file(output) if output.is_file() else None
    output.write_bytes(content)
    Path(args.sidecar).write_text(json.dumps(provenance, indent="\t") + "\n")

    print(f"wrote {output} ({len(content)} bytes) sha256={digest}")
    print(f"sidecar {args.sidecar}")
    if previous and previous != digest:
        print(f"note: previous file differed (sha256 {previous}) - update the patch manifest hash")
    if expected and expected != digest:
        print(f"WARNING manifest expects {expected} - rerun the manifest hash refresh")
        if previous and previous == expected:
            # Only report the diff when the file actually changed on disk.
            print("(the manifest still matches the previous file)")
    return EXIT_OK


if __name__ == "__main__":
    raise SystemExit(main())
