#!/usr/bin/env python3
"""Recover the Brotato project from retail packs with GDRE -- and prove it.

The Brotato campaign and its Abyssal Terrors DLC ship as compiled Godot 3.7
pack files (``Brotato.pck`` / ``BrotatoAbyssalTerrors.pck``).  Recovering the
GDScript / scene / resource sources from them is the first step of the
source-backed Bull/Danger5 physics and RL pipeline.  Because the recovered tree
is *proprietary game content* it must never be committed to this repository;
this tool therefore refuses to write outside of an explicit external output
root, and it never copies recovered text into the repo -- it only records a
hash manifest.

What the tool does:

* invokes the documented GDRE ``--recover`` command with an explicit
  ``subprocess`` argv (never a shell), for the campaign pack and optionally for
  the DLC pack;
* validates the recovered project (``project.godot`` + GDRE export log present,
  script/scene/resource counts consistent with the log's decompiler totals, the
  retained source bytecode cross-checked against the decompiled script count);
* writes a JSON provenance manifest into the primary recovery root recording
  the pack SHA256, engine metadata parsed from the GDRE log and the recovered
  Godot project, the GDRE tool version, and the ``res://`` path + SHA256 + size
  of every recovered ``.gd`` / ``.tscn`` / ``.tres`` file.

Modes:

* default -- run GDRE and build a fresh manifest (``kind=verified-recovery``).
  Refuses to overwrite a non-empty recovery directory.
* ``--reuse`` -- do not run GDRE; verify the existing manifest (pack hash,
  every recorded file hash/size, directory counts) and report honestly which
  provenance the manifest carries.
* ``--index-existing`` -- do not run GDRE; index an already-recovered tree and
  label the manifest ``kind=indexed-existing-recovery``.  This is *not* a fresh
  verified recovery: the pack hash is computed now and the files were produced
  by an earlier, unobserved run.

Exit codes::

    0  success
    1  GDRE run / recovery validation failed (nothing fabricated)
    2  argument / usage error
    3  manifest verification failed (--reuse)
    4  unsafe or refused output location

Usage::

    python3 ocean/brotato/recover_game.py \\
        --pack   /Users/me/brotato_run/Brotato.pck \\
        --output /Users/me/recovery/brotato_campaign \\
        --gdre   "/Users/me/tools/Godot RE Tools.app/Contents/MacOS/Godot RE Tools" \\
        --dlc-pack   /path/BrotatoAbyssalTerrors.pck \\
        --dlc-output /Users/me/recovery/brotato_abyss

    # verify a previous recovery without touching GDRE
    python3 ocean/brotato/recover_game.py \\
        --pack /Users/me/brotato_run/Brotato.pck \\
        --output /Users/me/recovery/brotato_campaign --reuse

    # index a tree recovered by hand (honest provenance only)
    python3 ocean/brotato/recover_game.py \\
        --pack /Users/me/brotato_run/Brotato.pck \\
        --output /Users/me/brotato_recovered --index-existing

Local artifacts on this workstation::

    # index the campaign tree that GDRE already produced (outside the repo)
    python3 ocean/brotato/recover_game.py \\
        --pack /Users/leelachebiyyam/brotato_run/Brotato.pck \\
        --output /Users/leelachebiyyam/brotato_recovered --index-existing

    # then re-verify the manifest without touching GDRE
    python3 ocean/brotato/recover_game.py \\
        --pack /Users/leelachebiyyam/brotato_run/Brotato.pck \\
        --output /Users/leelachebiyyam/brotato_recovered --reuse

A fresh, self-contained recovery of the campaign pack (GDRE writes the project
into the external ``--output`` root, this tool records the manifest)::

    python3 ocean/brotato/recover_game.py \\
        --pack /Users/leelachebiyyam/brotato_run/Brotato.pck \\
        --output /Users/leelachebiyyam/brotato_recovered_fresh
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import re
import subprocess
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

EXIT_OK = 0
EXIT_RECOVERY_FAILED = 1
EXIT_USAGE = 2
EXIT_VERIFY_FAILED = 3
EXIT_UNSAFE_OUTPUT = 4

MANIFEST_SCHEMA = "brotato-recover-game/1"
MANIFEST_DEFAULT_NAME = "recovery_manifest.json"
GDRE_LOG_NAME = "gdre_export.log"
GDRE_CLI_LOG_NAME = "gdre_cli_output.log"
GODOT_PROJECT_NAME = "project.godot"

KIND_VERIFIED = "verified-recovery"
KIND_INDEXED = "indexed-existing-recovery"

ROLE_CAMPAIGN = "campaign"
ROLE_DLC = "dlc"

#: suffix -> manifest file group
RECOVERED_SUFFIXES = {".gd": "scripts", ".tscn": "scenes", ".tres": "resources"}
SOURCE_BYTECODE_SUFFIX = ".gdc"

#: trees we never descend into when hashing recovered sources
SKIP_DIR_NAMES = frozenset({".import", ".godot"})

GDRE_ENV = "GDRE_TOOLS"
DEFAULT_GDRE = (
    "/Users/leelachebiyyam/tools/Godot RE Tools.app/Contents/MacOS/Godot RE Tools"
)

#: repository root, derived from this file: <repo>/ocean/brotato/recover_game.py
REPO_ROOT = Path(__file__).resolve().parents[2]

_HASH_CHUNK = 1 << 20


class RecoverError(Exception):
    """Base class for expected failures."""


class UsageError(RecoverError):
    """Bad arguments / missing prerequisites."""


class UnsafeOutputError(RecoverError):
    """Refused to write recovered proprietary content to a location."""


class RecoveryFailure(RecoverError):
    """GDRE failed or produced a tree that does not validate."""


class VerificationError(RecoverError):
    """An existing manifest did not verify against the tree it records."""


# ---------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------


def sha256_file(path: Path, *, chunk_size: int = _HASH_CHUNK) -> str:
    """Return the hex SHA256 of ``path``."""
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(chunk_size), b""):
            digest.update(block)
    return digest.hexdigest()


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _now_utc() -> str:
    """UTC timestamp; honours ``SOURCE_DATE_EPOCH`` for reproducible manifests."""
    epoch = os.environ.get("SOURCE_DATE_EPOCH")
    if epoch is not None:
        try:
            stamp = int(epoch)
        except ValueError:
            stamp = None
        if stamp is not None:
            return datetime.datetime.fromtimestamp(
                stamp, datetime.timezone.utc
            ).strftime("%Y-%m-%dT%H:%M:%SZ")
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def is_within(child: Path, parent: Path) -> bool:
    """True when ``child`` is ``parent`` or lives underneath it (both resolved)."""
    try:
        child.relative_to(parent)
    except ValueError:
        return False
    return True


def iter_recovered(root: Path):
    """Yield ``(abs_path, posix_relative_path)`` for recovered sources.

    Ordered deterministically and skips editor caches (``.import``/``.godot``).
    """
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = sorted(d for d in dirnames if d not in SKIP_DIR_NAMES)
        for name in sorted(filenames):
            path = Path(dirpath) / name
            if path.suffix in RECOVERED_SUFFIXES:
                yield path, path.relative_to(root).as_posix()


def count_recovered(root: Path) -> dict:
    """Count recovered sources and the original bytecode retained by GDRE."""
    counts = {
        "recovered_scripts": 0,
        "recovered_scenes": 0,
        "recovered_resources": 0,
        "source_bytecode_remaining": 0,
    }
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = sorted(d for d in dirnames if d not in SKIP_DIR_NAMES)
        for name in filenames:
            suffix = Path(name).suffix
            if suffix == ".gd":
                counts["recovered_scripts"] += 1
            elif suffix == ".tscn":
                counts["recovered_scenes"] += 1
            elif suffix == ".tres":
                counts["recovered_resources"] += 1
            elif suffix == SOURCE_BYTECODE_SUFFIX:
                counts["source_bytecode_remaining"] += 1
    return counts


# ---------------------------------------------------------------------------
# GDRE export-log parsing
# ---------------------------------------------------------------------------

_RE_GDRE_HEADER = re.compile(r"^GDRE Tools v(\S+)\s*$", re.MULTILINE)
_RE_ENGINE = re.compile(r"^Detected Engine Version:\s*(\S+)\s*$", re.MULTILINE)
_RE_PCK_VERSION = re.compile(r"^Version:\s*(\S+)\s*$", re.MULTILINE)
_RE_BYTECODE_REV = re.compile(r"^Detected Bytecode Revision:\s*(.+?)\s*$", re.MULTILINE)
_RE_BYTECODE_LINE = re.compile(
    r"bytecode version (\d+), engine version ([0-9][0-9A-Za-z_.\-]*)"
)
_RE_RECOMMENDED_EDITOR = re.compile(
    r"Use Godot editor version ([0-9][0-9A-Za-z_.\-]*)"
)
_RE_LOADED_IMPORTED = re.compile(r"^Loaded (\d+) imported files\s*$", re.MULTILINE)
_RE_VERIFIED = re.compile(r"^Verified (\d+) files\b", re.MULTILINE)
_RE_EXTRACTED = re.compile(r"^Extracted (\d+) files\b", re.MULTILINE)
_RE_DECOMPILED = re.compile(r"^Decompiled scripts:\s*(\d+)\s*$", re.MULTILINE)
_RE_NOT_DECOMPILED = re.compile(r"^Scripts not decompiled:\s*(\d+)\s*$", re.MULTILINE)
_RE_CONVERTED = re.compile(r"^Successfully converted:\s*(\d+)\s*$", re.MULTILINE)
_RE_TRANSLATIONS = re.compile(
    r"Could not recover (\d+)/(\d+) keys for (\S+)"
)
_RE_PLUGIN_BULLET = re.compile(r"^\s*-\s+([A-Za-z0-9_.\-]+)\s*$", re.MULTILINE)


def _maybeint(match: re.Match | None):
    return int(match.group(1)) if match else None


def parse_gdre_log(text: str) -> dict:
    """Parse the metadata GDRE writes into ``gdre_export.log``.

    Pure string function so it is deterministic to test.  Fields that are not
    present are ``None`` -- nothing is fabricated.
    """
    header = _RE_GDRE_HEADER.search(text)
    engine = _RE_ENGINE.search(text)
    pck_version = _RE_PCK_VERSION.search(text)
    revision = _RE_BYTECODE_REV.search(text)
    bytecode_line = _RE_BYTECODE_LINE.search(text)
    recommended = _RE_RECOMMENDED_EDITOR.search(text)

    revision_text = revision.group(1) if revision else None
    revision_commit = None
    if revision_text:
        commit = re.search(r"\(([0-9a-f]{6,})\)", revision_text)
        if commit:
            revision_commit = commit.group(1)

    parsed = {
        "gdre_version": header.group(1) if header else None,
        "engine_version": engine.group(1) if engine else None,
        "pck_version": pck_version.group(1) if pck_version else None,
        "bytecode_version": int(bytecode_line.group(1)) if bytecode_line else None,
        "bytecode_engine_guess": bytecode_line.group(2) if bytecode_line else None,
        "bytecode_revision": revision_text,
        "bytecode_revision_commit": revision_commit,
        "recommended_editor": recommended.group(1) if recommended else None,
        "loaded_imported_files": _maybeint(_RE_LOADED_IMPORTED.search(text)),
        "verified_files": _maybeint(_RE_VERIFIED.search(text)),
        "extracted_files": _maybeint(_RE_EXTRACTED.search(text)),
        "decompiled_scripts": _maybeint(_RE_DECOMPILED.search(text)),
        "scripts_not_decompiled": _maybeint(_RE_NOT_DECOMPILED.search(text)),
        "converted_resources": _maybeint(_RE_CONVERTED.search(text)),
    }

    notes_index = text.find("IMPORTANT NOTES")
    notes_text = text[notes_index:] if notes_index != -1 else ""
    translations = [
        {"path": path, "recovered": int(rec), "total": int(total)}
        for rec, total, path in _RE_TRANSLATIONS.findall(notes_text)
    ]
    plugins = []
    if "Incomplete Plugin Export" in notes_text:
        plugins = sorted(set(_RE_PLUGIN_BULLET.findall(notes_text)))
    notes = {
        "translations_incomplete": translations,
        "plugins_not_regenerated": plugins,
        "raw": notes_text.strip() or None,
    }
    return {"fields": parsed, "notes": notes}


# ---------------------------------------------------------------------------
# Recovered-project inspection
# ---------------------------------------------------------------------------

_RE_CONFIG_VERSION = re.compile(r"^config_version\s*=\s*(\d+)\s*$", re.MULTILINE)


def parse_project_config_version(project_godot: Path) -> int | None:
    """Return ``config_version`` from a Godot ``project.godot`` (or None)."""
    if not project_godot.is_file():
        return None
    match = _RE_CONFIG_VERSION.search(
        project_godot.read_text(encoding="utf-8", errors="replace")
    )
    return int(match.group(1)) if match else None


def read_manifest(path: Path) -> dict:
    """Load and minimally validate a provenance manifest."""
    if not path.is_file():
        raise UsageError(f"manifest not found: {path}")
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise UsageError(f"manifest is not valid JSON ({path}): {exc}") from exc
    if not isinstance(manifest, dict):
        raise UsageError(f"manifest is not a JSON object: {path}")
    if manifest.get("schema") != MANIFEST_SCHEMA:
        raise UsageError(
            f"unexpected manifest schema {manifest.get('schema')!r} "
            f"(expected {MANIFEST_SCHEMA!r}): {path}"
        )
    if not isinstance(manifest.get("recoveries"), list):
        raise UsageError(f"manifest has no recoveries list: {path}")
    return manifest


def write_manifest(path: Path, manifest: dict) -> None:
    path.write_text(
        json.dumps(manifest, indent=2, sort_keys=False) + "\n", encoding="utf-8"
    )


# ---------------------------------------------------------------------------
# GDRE process handling
# ---------------------------------------------------------------------------


def resolve_gdre(explicit: str | None) -> str:
    candidate = explicit or os.environ.get(GDRE_ENV) or DEFAULT_GDRE
    path = Path(candidate).expanduser()
    if not path.is_file() or not os.access(path, os.X_OK):
        raise UsageError(f"GDRE executable not found or not executable: {path}")
    return str(path)


def probe_gdre_version(gdre: str) -> str | None:
    """Ask GDRE for its version.  Returns ``None`` if it cannot be determined."""
    try:
        proc = subprocess.run(
            [gdre, "--headless", "--gdre-version"],
            capture_output=True,
            text=True,
        )
    except OSError:
        return None
    if proc.returncode != 0:
        return None
    match = re.search(r"Godot RE Tools v(\S+)", proc.stdout + proc.stderr)
    return match.group(1) if match else None


def run_gdre_recover(
    gdre: str,
    pack: Path,
    output: Path,
    *,
    extra_args: tuple[str, ...] = (),
) -> tuple[subprocess.CompletedProcess, list[str]]:
    """Run ``GDRE --headless --recover=<pack> --output=<dir>`` (argv, no shell)."""
    argv = [
        gdre,
        "--headless",
        f"--recover={pack}",
        f"--output={output}",
        *extra_args,
    ]
    proc = subprocess.run(argv, capture_output=True, text=True)
    (output / GDRE_CLI_LOG_NAME).write_text(
        "$ "
        + " ".join(argv)
        + "\n\n[stdout]\n"
        + proc.stdout
        + "\n[stderr]\n"
        + proc.stderr,
        encoding="utf-8",
    )
    return proc, argv


# ---------------------------------------------------------------------------
# Validation of a recovered tree
# ---------------------------------------------------------------------------


def validate_recovery(
    root: Path,
    *,
    strict: bool,
    log_present_required: bool = True,
) -> dict:
    """Inspect and validate a recovery root.

    Returns the parsed log (``None`` when no log is present) plus the tree
    counts.  Raises :class:`RecoveryFailure` when the tree is not a credible
    Godot recovery, or (``strict=True``) when the script counts disagree.
    """
    if not root.is_dir():
        raise RecoveryFailure(f"recovery root is not a directory: {root}")
    project = root / GODOT_PROJECT_NAME
    log_path = root / GDRE_LOG_NAME

    if not project.is_file():
        raise RecoveryFailure(
            f"no {GODOT_PROJECT_NAME} under {root}: not a recovered Godot project"
        )

    parsed = None
    if log_path.is_file():
        parsed = parse_gdre_log(log_path.read_text(encoding="utf-8", errors="replace"))
    elif log_present_required and strict:
        raise RecoveryFailure(
            f"no {GDRE_LOG_NAME} under {root}: cannot verify the recovery or read "
            "engine metadata"
        )

    counts = count_recovered(root)
    if counts["recovered_scripts"] == 0:
        raise RecoveryFailure(
            f"no .gd scripts recovered under {root}: nothing to hand to the pipeline"
        )

    if strict:
        fields = (parsed or {}).get("fields", {})
        decompiled = fields.get("decompiled_scripts")
        not_decompiled = fields.get("scripts_not_decompiled")
        if decompiled is None:
            raise RecoveryFailure(
                f"{log_path.name} has no 'Decompiled scripts' total: recovery "
                "unverified"
            )
        if counts["recovered_scripts"] < decompiled:
            raise RecoveryFailure(
                f"found {counts['recovered_scripts']} .gd scripts but GDRE reported "
                f"decompiling {decompiled}"
            )
        if not_decompiled is not None:
            expected = decompiled + not_decompiled
            if counts["source_bytecode_remaining"] != expected:
                raise RecoveryFailure(
                    "retained source bytecode count does not match GDRE's decompiler "
                    f"totals: {counts['source_bytecode_remaining']} .gdc on disk vs "
                    f"{expected} reported ({decompiled} decompiled + "
                    f"{not_decompiled} not decompiled)"
                )
    return {"log": parsed, "counts": counts}


# ---------------------------------------------------------------------------
# Record / manifest construction
# ---------------------------------------------------------------------------


def _engine_block(parsed_log: dict | None, root: Path) -> dict:
    fields = (parsed_log or {}).get("fields", {})
    return {
        "engine_version": fields.get("engine_version"),
        "pck_version": fields.get("pck_version"),
        "bytecode_version": fields.get("bytecode_version"),
        "bytecode_engine_guess": fields.get("bytecode_engine_guess"),
        "bytecode_revision": fields.get("bytecode_revision"),
        "bytecode_revision_commit": fields.get("bytecode_revision_commit"),
        "recommended_editor": fields.get("recommended_editor"),
        "project_config_version": parse_project_config_version(
            root / GODOT_PROJECT_NAME
        ),
    }


def _bytecode_report(parsed_log: dict | None, counts: dict) -> dict:
    fields = (parsed_log or {}).get("fields", {})
    decompiled = fields.get("decompiled_scripts")
    not_decompiled = fields.get("scripts_not_decompiled")
    remaining = counts["source_bytecode_remaining"]
    consistent = None
    if decompiled is not None and not_decompiled is not None:
        consistent = remaining == decompiled + not_decompiled
    return {
        "original_bytecode_remaining": remaining,
        "decompiled_scripts": decompiled,
        "scripts_not_decompiled": not_decompiled,
        "recovered_scripts": counts["recovered_scripts"],
        "bytecode_matches_decompiler_totals": consistent,
        "note": (
            "Recovered GDScript text replaces the shipped bytecode at its res:// "
            "path; GDRE retains the original bytecode under .autoconverted/. "
            "Bytecode retention is expected and is not a decompilation failure. "
            "Whitespace differences in the decompiled text (for example collapsed "
            "blank lines) are not evidence of lost control flow and are not "
            "reported as such."
        ),
    }


def build_recovery_record(
    root: Path,
    role: str,
    pack: Path,
    *,
    kind: str,
    parsed_log: dict | None,
    counts: dict,
    gdre_path: str | None = None,
    gdre_version: str | None = None,
    invocation: dict | None = None,
) -> dict:
    """Assemble one recovery entry for the manifest (hashes every source file)."""
    files = {"scripts": [], "scenes": [], "resources": []}
    for path, rel in iter_recovered(root):
        group = RECOVERED_SUFFIXES[path.suffix]
        files[group].append(
            {
                "path": rel,
                "res_path": f"res://{rel}",
                "size_bytes": path.stat().st_size,
                "sha256": sha256_file(path),
            }
        )

    log_path = root / GDRE_LOG_NAME
    cli_log_path = root / GDRE_CLI_LOG_NAME
    record = {
        "role": role,
        "kind": kind,
        "pack": {
            "path": str(pack),
            "sha256": sha256_file(pack),
            "size_bytes": pack.stat().st_size,
        },
        "output_root": str(root),
        "engine": _engine_block(parsed_log, root),
        "counts": dict(counts),
        "bytecode_report": _bytecode_report(parsed_log, counts),
        "gdre": {
            "executable": gdre_path,
            "version": gdre_version
            or ((parsed_log or {}).get("fields", {}).get("gdre_version")),
        },
        "recovery_notes": (parsed_log or {}).get("notes"),
        "gdre_log": (
            {
                "path": GDRE_LOG_NAME,
                "sha256": sha256_file(log_path),
                "size_bytes": log_path.stat().st_size,
            }
            if log_path.is_file()
            else None
        ),
        "gdre_cli_log": (
            {
                "path": GDRE_CLI_LOG_NAME,
                "sha256": sha256_file(cli_log_path),
                "size_bytes": cli_log_path.stat().st_size,
            }
            if cli_log_path.is_file()
            else None
        ),
        "invocation": invocation,
        "files": files,
    }
    return record


def build_manifest(recoveries: list[dict], *, kind: str) -> dict:
    return {
        "schema": MANIFEST_SCHEMA,
        "kind": kind,
        "generator": "ocean/brotato/recover_game.py",
        "created_utc": _now_utc(),
        "recoveries": recoveries,
    }


# ---------------------------------------------------------------------------
# Manifest verification
# ---------------------------------------------------------------------------


def verify_manifest(
    manifest: dict,
    *,
    pack_overrides: dict | None = None,
) -> list:
    """Verify a manifest against the tree and packs it records.

    Returns a list of human-readable problems; empty means verified.  The
    manifest is the contract, so files present in the tree but absent from the
    manifest are reported too.
    """
    problems: list = []
    recoveries = manifest.get("recoveries") or []
    if not recoveries:
        problems.append("manifest records no recoveries")
        return problems

    overrides = pack_overrides or {}
    seen_roles = set()
    for record in recoveries:
        role = record.get("role", "?")
        seen_roles.add(role)
        root = Path(record.get("output_root", ""))
        if not root.is_dir():
            problems.append(f"[{role}] recovery root missing: {root}")
            continue

        pack_info = record.get("pack", {})
        recorded_sha = pack_info.get("sha256")
        override = overrides.get(role)
        if override is not None:
            pack_path = Path(override)
        else:
            pack_path = Path(pack_info.get("path", ""))
        if not pack_path.is_file():
            problems.append(f"[{role}] pack missing: {pack_path}")
        else:
            actual_sha = sha256_file(pack_path)
            if actual_sha != recorded_sha:
                problems.append(
                    f"[{role}] pack sha256 mismatch for {pack_path}: "
                    f"manifest {recorded_sha}, on disk {actual_sha}"
                )

        recorded_paths = set()
        for group in RECOVERED_SUFFIXES.values():
            for entry in record.get("files", {}).get(group, []):
                rel = entry.get("path")
                recorded_paths.add(rel)
                target = root / rel
                if not target.is_file():
                    problems.append(f"[{role}] missing recovered file: {rel}")
                    continue
                size = target.stat().st_size
                if entry.get("size_bytes") is not None and size != entry["size_bytes"]:
                    problems.append(
                        f"[{role}] size mismatch for {rel}: manifest "
                        f"{entry['size_bytes']}, on disk {size}"
                    )
                actual_sha = sha256_file(target)
                if actual_sha != entry.get("sha256"):
                    problems.append(f"[{role}] sha256 mismatch for {rel}")

        actual_paths = {rel for _, rel in iter_recovered(root)}
        for extra in sorted(actual_paths - recorded_paths):
            problems.append(f"[{role}] unexpected file not in manifest: {extra}")

        actual_counts = count_recovered(root)
        for key, value in (record.get("counts") or {}).items():
            if key in actual_counts and actual_counts[key] != value:
                problems.append(
                    f"[{role}] count mismatch {key}: manifest {value}, "
                    f"on disk {actual_counts[key]}"
                )

    for role in overrides:
        if role not in seen_roles:
            problems.append(f"manifest has no recovery for role '{role}'")
    return problems


# ---------------------------------------------------------------------------
# Output-path safety
# ---------------------------------------------------------------------------


def ensure_safe_output_root(output: Path, *, repo_root: Path = REPO_ROOT) -> Path:
    """Reject output roots that would place proprietary content in the repo."""
    resolved = output.expanduser().resolve()
    repo = repo_root.resolve()
    if resolved == repo or is_within(resolved, repo):
        raise UnsafeOutputError(
            f"refusing to write recovered proprietary game content inside the "
            f"repository ({repo}); choose an external --output root"
        )
    if is_within(repo, resolved):
        raise UnsafeOutputError(
            f"refusing --output that contains the repository: {resolved}"
        )
    if resolved.exists() and not resolved.is_dir():
        raise UnsafeOutputError(f"--output exists and is not a directory: {resolved}")
    return resolved


def check_targets(targets: list) -> None:
    """Validate the (role, pack, output) plan before doing any work."""
    if not targets:
        raise UsageError("no recovery targets given")
    packs = [Path(t["pack"]).resolve() for t in targets]
    outs = [Path(t["out"]).resolve() for t in targets]
    if len(set(packs)) != len(packs):
        raise UsageError("--pack and --dlc-pack must be different files")
    for i, out in enumerate(outs):
        for j, other in enumerate(outs):
            if i != j and (out == other or is_within(out, other)):
                raise UsageError(
                    f"--output roots must be disjoint: {out} overlaps {other}"
                )
    for pack, out in zip(packs, outs):
        if is_within(pack, out):
            raise UsageError(f"--pack {pack} lives inside its own --output root {out}")


# ---------------------------------------------------------------------------
# Mode implementations
# ---------------------------------------------------------------------------


def _require_empty_or_absent(output: Path) -> None:
    if output.exists():
        if not output.is_dir():
            raise UnsafeOutputError(f"--output is not a directory: {output}")
        if any(output.iterdir()):
            raise UnsafeOutputError(
                f"refusing to overwrite non-empty recovery directory {output}; "
                "pass --reuse to verify it or --index-existing to index it"
            )
    else:
        output.mkdir(parents=True)


def _print_summary(record: dict) -> None:
    counts = record["counts"]
    bytecode = record["bytecode_report"]
    engine = record["engine"]
    print(f"  role:                {record['role']}")
    print(
        f"  pack:                {record['pack']['path']} "
        f"(sha256 {record['pack']['sha256'][:16]}...)"
    )
    print(f"  output root:         {record['output_root']}")
    print(f"  engine:              {engine['engine_version']} "
          f"bytecode={engine['bytecode_version']} rev={engine['bytecode_revision']}")
    print(
        f"  recovered:           {counts['recovered_scripts']} .gd, "
        f"{counts['recovered_scenes']} .tscn, "
        f"{counts['recovered_resources']} .tres"
    )
    print(
        f"  decompiled scripts:  {bytecode['decompiled_scripts']} "
        f"(not decompiled: {bytecode['scripts_not_decompiled']})"
    )
    print(
        f"  bytecode retained:   {bytecode['original_bytecode_remaining']} .gdc "
        f"(matches decompiler totals: "
        f"{bytecode['bytecode_matches_decompiler_totals']})"
    )


def _record_for(
    target: dict,
    *,
    kind: str,
    strict: bool,
    gdre_path: str | None,
    gdre_version: str | None,
    invocation: dict | None,
    log_present_required: bool,
) -> dict:
    out = Path(target["out"])
    result = validate_recovery(
        out, strict=strict, log_present_required=log_present_required
    )
    return build_recovery_record(
        out,
        target["role"],
        Path(target["pack"]),
        kind=kind,
        parsed_log=result["log"],
        counts=result["counts"],
        gdre_path=gdre_path,
        gdre_version=gdre_version,
        invocation=invocation,
    )


def _run_recover(args, targets: list) -> int:
    gdre = resolve_gdre(args.gdre)
    gdre_version = probe_gdre_version(gdre)
    extra = tuple(args.gdre_arg)

    for target in targets:
        _require_empty_or_absent(Path(target["out"]))

    recoveries = []
    for target in targets:
        out = Path(target["out"])
        pack = Path(target["pack"])
        print(f"recovering {target['role']}: {pack} -> {out}")
        proc, argv = run_gdre_recover(gdre, pack, out, extra_args=extra)
        invocation = {"argv": argv, "returncode": proc.returncode}
        if proc.returncode != 0:
            tail = "\n".join((proc.stdout + proc.stderr).splitlines()[-15:])
            raise RecoveryFailure(
                f"GDRE failed with exit code {proc.returncode} for {pack}\n{tail}"
            )
        record = _record_for(
            target,
            kind=KIND_VERIFIED,
            strict=True,
            gdre_path=gdre,
            gdre_version=gdre_version,
            invocation=invocation,
            log_present_required=True,
        )
        recoveries.append(record)
        _print_summary(record)

    manifest = build_manifest(recoveries, kind=KIND_VERIFIED)
    manifest_path = Path(targets[0]["out"]) / args.manifest_name
    write_manifest(manifest_path, manifest)
    print(f"wrote provenance manifest: {manifest_path}")
    return EXIT_OK


def _run_index(args, targets: list) -> int:
    manifest_path = Path(targets[0]["out"]) / args.manifest_name
    if manifest_path.exists() and not args.force:
        raise UnsafeOutputError(
            f"{manifest_path} already exists; pass --force to re-index and "
            "overwrite it"
        )
    recoveries = []
    for target in targets:
        record = _record_for(
            target,
            kind=KIND_INDEXED,
            strict=False,
            gdre_path=None,
            gdre_version=None,
            invocation=None,
            log_present_required=False,
        )
        recoveries.append(record)
        _print_summary(record)
    manifest = build_manifest(recoveries, kind=KIND_INDEXED)
    write_manifest(manifest_path, manifest)
    print(
        "wrote provenance manifest (provenance: indexed existing recovery; the "
        "pack hash was computed now and the recovered files were produced by an "
        f"earlier run): {manifest_path}"
    )
    return EXIT_OK


def _run_reuse(args, targets: list) -> int:
    manifest_path = Path(targets[0]["out"]) / args.manifest_name
    manifest = read_manifest(manifest_path)
    overrides = {t["role"]: Path(t["pack"]) for t in targets}
    problems = verify_manifest(manifest, pack_overrides=overrides)
    print(f"verifying {manifest_path}")
    print(f"  provenance: {manifest.get('kind')}")
    if problems:
        for problem in problems:
            print(f"  FAIL {problem}", file=sys.stderr)
        raise VerificationError(f"reuse verification found {len(problems)} problem(s)")
    print("reuse verification passed: pack hash, file hashes, and counts match")
    return EXIT_OK


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="recover_game.py",
        description=(
            "Recover the Brotato project from retail packs with GDRE and record a "
            "provenance manifest of the recovered sources."
        ),
    )
    parser.add_argument("--pack", required=True, help="retail .pck to recover from")
    parser.add_argument(
        "--output",
        required=True,
        help="external recovery root for --pack (must be outside this repository)",
    )
    parser.add_argument(
        "--dlc-pack", default=None, help="optional DLC .pck recovered in the same run"
    )
    parser.add_argument(
        "--dlc-output",
        default=None,
        help="external recovery root for --dlc-pack (required with --dlc-pack)",
    )
    parser.add_argument(
        "--gdre",
        default=None,
        help=(
            "GDRE executable (default: $" + GDRE_ENV + " or the bundled known path)"
        ),
    )
    parser.add_argument(
        "--manifest-name",
        default=MANIFEST_DEFAULT_NAME,
        help=f"manifest filename written into the primary output root "
        f"(default: {MANIFEST_DEFAULT_NAME})",
    )
    parser.add_argument(
        "--gdre-arg",
        action="append",
        default=[],
        metavar="ARG",
        help="extra argument forwarded verbatim to GDRE (repeatable)",
    )
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument(
        "--reuse",
        action="store_true",
        help="verify the existing manifest instead of running GDRE",
    )
    mode.add_argument(
        "--index-existing",
        action="store_true",
        help="index an existing recovery root without running GDRE",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="allow --index-existing to overwrite an existing manifest",
    )
    return parser


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return _main(args)
    except UsageError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return EXIT_USAGE
    except UnsafeOutputError as exc:
        print(f"refused: {exc}", file=sys.stderr)
        return EXIT_UNSAFE_OUTPUT
    except VerificationError as exc:
        print(f"verification failed: {exc}", file=sys.stderr)
        return EXIT_VERIFY_FAILED
    except RecoveryFailure as exc:
        print(f"recovery failed: {exc}", file=sys.stderr)
        return EXIT_RECOVERY_FAILED


def _main(args) -> int:
    pack = Path(args.pack).expanduser()
    if not pack.is_file():
        raise UsageError(f"pack not found: {pack}")

    targets = [
        {"role": ROLE_CAMPAIGN, "pack": pack, "out": Path(args.output).expanduser()}
    ]
    if args.dlc_pack:
        if not args.dlc_output:
            raise UsageError("--dlc-output is required when --dlc-pack is given")
        dlc_pack = Path(args.dlc_pack).expanduser()
        if not dlc_pack.is_file():
            raise UsageError(f"DLC pack not found: {dlc_pack}")
        targets.append(
            {
                "role": ROLE_DLC,
                "pack": dlc_pack,
                "out": Path(args.dlc_output).expanduser(),
            }
        )

    check_targets(targets)

    for target in targets:
        target["out"] = ensure_safe_output_root(Path(target["out"]))

    if args.reuse:
        return _run_reuse(args, targets)
    if args.index_existing:
        return _run_index(args, targets)
    return _run_recover(args, targets)


if __name__ == "__main__":
    raise SystemExit(main())
