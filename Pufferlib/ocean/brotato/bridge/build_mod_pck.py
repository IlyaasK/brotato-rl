#!/usr/bin/env python3
"""Build the RL-bridge PCK with GDRE's documented pck patch mode.

This is the reproducible packer for the Brotato RL bridge.  It never edits the
retail pack: the payload declared in ``patch_manifest.json`` is applied to a
*copy* by::

    Godot RE Tools --headless --pck-patch=<retail.pck> \\
        --output=<patched.pck> --patch-file=<src>=res://<dest> ...

and the retail pack's SHA-256 is re-verified afterwards, so "original pack
unchanged" is an enforced invariant rather than a claim.  Every applied file is
recorded (source path, destination path, SHA-256, kind, reason, what it
replaces) in the inventory next to the output pack.

Manifest sources are resolved from three explicit roots:

  * ``payload`` (default) - the canonical staged tree in this directory, at
    ``<manifest dir>/<payload_root>``; these are the bridge's own files;
  * ``repo``              - a path relative to the repository root, used for
    shared files such as ``ocean/brotato/policy/brotato_policy.gd``;
  * ``external: true``    - a generated artifact under ``--external-root``
    (default ``<repo>/migration/game/external``, else ``$HOME/brotato_mod/external``,
    or ``$BROTATO_EXTERNAL_ROOT``).  Generated or
    proprietary-derived files live there and are deliberately absent from the
    repository; their producer is named in the entry (``generate_with``) and
    checked by ``verify``.

Usage::

    build_mod_pck.py verify [--manifest M] [--pck P] [--external-root DIR]
    build_mod_pck.py build  [--manifest M] [--pck P] [--out O] [--external-root DIR]
                            [--dry-run] [--refresh-hashes]

Exit codes: 0 success, 2 usage/manifest error, 3 verification failure,
4 GDRE failure.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import selectors
import shutil
import subprocess
import sys
import time
from pathlib import Path

EXIT_OK = 0
EXIT_USAGE = 2
EXIT_VERIFY = 3
EXIT_GDRE = 4

BRIDGE_DIR = Path(__file__).resolve().parent
DEFAULT_MANIFEST = str(BRIDGE_DIR / "patch_manifest.json")


def _first_existing(candidates: list[str], fallback: str) -> str:
    """Return the first existing path, else `fallback` (env/default resolution)."""
    for candidate in candidates:
        if candidate and Path(candidate).exists():
            return candidate
    return fallback


def _run_dir() -> str:
    return os.environ.get("BROTATO_RUN_DIR") or str(Path.home() / "brotato_run")


def _retail_pck() -> str:
    """Retail pack: prefer the repository copy, else the run directory's."""
    repo = find_repo_root(BRIDGE_DIR)
    return _first_existing(
        [os.environ.get("BROTATO_RETAIL_PCK", ""),
         str(repo / "migration" / "game" / "Brotato.pck")],
        str(Path(_run_dir()) / "Brotato.pck"),
    )


def _gdre() -> str:
    """GDRE executable: Linux build first, then the macOS app bundle."""
    return _first_existing(
        [os.environ.get("BROTATO_GDRE", ""),
         str(Path.home() / "tools" / "gdre_linux" / "gdre_tools.x86_64"),
         str(Path.home() / "tools" / "Godot RE Tools.app" / "Contents" / "MacOS" / "Godot RE Tools")],
        str(Path.home() / "tools" / "gdre_linux" / "gdre_tools.x86_64"),
    )


def _external_root() -> str:
    """Generated external artifacts: repository migration dir, else ~/brotato_mod/external."""
    repo = find_repo_root(BRIDGE_DIR)
    return _first_existing(
        [os.environ.get("BROTATO_EXTERNAL_ROOT", ""),
         str(repo / "migration" / "game" / "external")],
        str(Path.home() / "brotato_mod" / "external"),
    )


# ---------------------------------------------------------------------------
# GDScript-3 parse check (the recurring break class)
#
# GDScript 3 rejects `:=` when the initialiser has no static type (Dictionary
# indexing, autoload-singleton calls, .get()/.size() results) and it rejects real
# syntax slips; both only surface when the engine actually loads the script.  A
# --check-only run outside the game context stops at the first unresolved
# identifier (autoloads/global classes come from the pack), so it cannot see the
# inference error behind it - the only reliable check is the engine loading the
# built pack, which is what this does: boot the patched pack for a couple of
# physics ticks under xvfb and fail loudly on a load-time parse/preload error or
# on the bridge never being instantiated.
PARSE_FAIL_PATTERNS = (
    "Parse Error:",
    "Failed to load script",
    "Cannot open file",
    "Couldn't fully preload the script",
    "Invalid call. Nonexistent function 'new' in base 'GDScript'",
    "Mods are currently disabled",
)
MOD_READY_MARKER = "rlbridge-headlessrl instantiated"


def _engine() -> str:
    return os.environ.get("BROTATO_ENGINE") or _first_existing(
        [str(Path.home() / "tools" / "bin" / "godot-3.7-dev1"),
         str(Path.home() / "tools" / "godot-src" / "bin" / "godot.x11.opt.tools.64")],
        str(Path.home() / "tools" / "bin" / "godot-3.7-dev1"),
    )


def _launch_prefix(engine: str) -> list[str]:
    """xvfb-run wrapper when there is no display (x11 --no-window still needs one)."""
    if os.environ.get("DISPLAY") or os.environ.get("BROTATO_XVFB") == "0":
        return [engine]
    shutil_which = os.popen("command -v xvfb-run").read().strip()
    if not shutil_which:
        return [engine]
    return [shutil_which, "-a", "-s", "-screen 0 640x360x24", engine]


def check_scripts(pack: Path, timeout: float = 180.0) -> tuple[bool, str]:
    """Returns (ok, report).  Boots `pack` briefly and inspects the engine log."""
    engine = _engine()
    if not Path(engine).is_file():
        return False, f"engine not found: {engine} (set BROTATO_ENGINE)"
    if not pack.is_file():
        return False, f"no pack to check: {pack}"
    argv = _launch_prefix(engine) + [
        "--main-pack", str(pack), "--fixed-fps", "60", "--audio-driver", "Dummy",
        "--enable-mods", "--no-window",
    ]
    env = dict(os.environ)
    # A couple of ticks is enough to load the mod and write a row; the bridge then
    # exits by itself.  No trace file is written outside BORATO_RL_TRACE.
    env["BROTATO_RL"] = "1"
    env["BROTATO_RL_MAX_TICKS"] = "2"
    env["BROTATO_RL_TRACE"] = "/dev/null"
    env["BROTATO_RL_META"] = "/dev/null.meta.json"
    env["BROTATO_RL_SUMMARY"] = "/dev/null.summary.json"
    # Bounded wait: the engine is reaped as soon as the mod is up (a couple of
    # ticks) or a load error appears, so the common cases cost a few seconds.  A
    # pack whose mod never loads (e.g. the retail pack) is cut off at the timeout
    # instead of running the game forever.
    selector = selectors.DefaultSelector()
    process = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, bufsize=1, env=env, start_new_session=True)
    selector.register(process.stdout, selectors.EVENT_READ)
    lines: list[str] = []
    deadline = time.monotonic() + timeout
    while True:
        if process.poll() is not None:
            lines.extend((process.stdout.read() or "").splitlines())
            break
        if time.monotonic() > deadline:
            lines.append(f"<parse check timeout after {timeout:.0f}s; stopping the engine>")
            break
        events = selector.select(timeout=0.5)
        if not events:
            continue
        line = process.stdout.readline()
        if not line:
            continue
        lines.append(line.rstrip("\n"))
        if MOD_READY_MARKER in line:
            break
        if any(pattern in line for pattern in PARSE_FAIL_PATTERNS):
            # give the engine a moment to print the file:line that follows
            for _ in range(4):
                more = selector.select(timeout=0.25)
                if not more:
                    break
                extra = process.stdout.readline()
                if not extra:
                    break
                lines.append(extra.rstrip("\n"))
            break
    selector.close()
    try:
        import os as _os
        import signal as _signal
        _os.killpg(process.pid, _signal.SIGTERM)
    except (ProcessLookupError, PermissionError, AttributeError):
        pass
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        process.kill()
    log = "\n".join(lines) + "\n"
    failures = [line.strip() for line in log.splitlines() if any(p in line for p in PARSE_FAIL_PATTERNS)]
    ready = MOD_READY_MARKER in log
    if failures or not ready:
        reason = ("load-time script error" if failures
              else "the bridge was never instantiated (mod not loaded, or it aborted before its log line)")
        return False, (f"GDScript-3 parse/load check FAILED on {pack}\n"
                       f"  reason: {reason}\n"
                       + "".join(f"  {line}\n" for line in failures[:20])
                       + ("" if failures else "") + tail(log))
    return True, f"GDScript-3 parse/load check OK ({pack}): mod instantiated, no load-time script error"


def tail(text: str, lines: int = 8) -> str:
    kept = [line for line in (text or "").splitlines() if line.strip()][-lines:]
    return "".join(f"  | {line}\n" for line in kept)


VALID_KINDS = ("add", "replace")
VALID_BASES = ("payload", "repo")


class ManifestError(Exception):
    """The patch manifest is missing, malformed, or inconsistent."""


class VerificationError(Exception):
    """A file on disk does not match the manifest."""


def sha256_file(path: Path, chunk: int = 1 << 20) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while True:
            block = handle.read(chunk)
            if not block:
                break
            digest.update(block)
    return digest.hexdigest()


def find_repo_root(start: Path) -> Path:
    """Walk up from `start` until a directory containing ocean/brotato is found."""
    for candidate in [start, *start.parents]:
        if (candidate / "ocean" / "brotato").is_dir():
            return candidate
    return start


def load_manifest(path: Path) -> dict:
    if not path.is_file():
        raise ManifestError(f"no patch manifest at {path}")
    try:
        manifest = json.loads(path.read_text())
    except json.JSONDecodeError as exc:
        raise ManifestError(f"{path}: invalid JSON ({exc})") from exc
    files = manifest.get("files")
    if not isinstance(files, list) or not files:
        raise ManifestError(f"{path}: 'files' must be a non-empty list")
    seen_dest: dict[str, str] = {}
    for index, entry in enumerate(files):
        where = f"{path}: files[{index}]"
        for key in ("dest", "src", "kind", "reason"):
            if key not in entry:
                raise ManifestError(f"{where} is missing '{key}'")
        if not entry["dest"].startswith("res://"):
            raise ManifestError(f"{where}.dest must start with res://")
        if entry["dest"] in seen_dest:
            raise ManifestError(
                f"{where}.dest {entry['dest']!r} duplicated (also files[{seen_dest[entry['dest']]}])"
            )
        seen_dest[entry["dest"]] = str(index)
        if entry["kind"] not in VALID_KINDS:
            raise ManifestError(f"{where}.kind must be one of {VALID_KINDS}")
        if entry.get("base", "payload") not in VALID_BASES:
            raise ManifestError(f"{where}.base must be one of {VALID_BASES}")
        if entry.get("external") and entry.get("base", "payload") != "payload":
            raise ManifestError(f"{where}: 'external' entries cannot also set 'base'")
    return manifest


def resolve_entries(manifest: dict, manifest_path: Path, external_root: Path,
                    *, refresh: bool, refresh_generated: bool = False,
                    refresh_payload: bool = False) -> tuple[list[dict], list[dict]]:
    """Returns (entries_to_patch, skipped_optional)."""
    payload_root = (manifest_path.parent / manifest.get("payload_root", "payload")).resolve()
    repo_root = find_repo_root(manifest_path.parent)
    resolved: list[dict] = []
    skipped: list[dict] = []
    for entry in manifest["files"]:
        optional = bool(entry.get("optional", False))
        if entry.get("external"):
            src = (external_root / entry["src"]).resolve()
            origin = f"external:{external_root}"
        elif entry.get("base", "payload") == "repo":
            src = (repo_root / entry["src"]).resolve()
            origin = f"repo:{repo_root}"
        else:
            src = (payload_root / entry["src"]).resolve()
            origin = f"payload:{payload_root}"

        if not src.is_file():
            message = f"missing source for {entry['dest']}: {src}"
            if optional:
                skipped.append({"dest": entry["dest"], "src": str(src), "origin": origin,
                                "reason": entry.get("reason", ""),
                                "generate_with": entry.get("generate_with", ""),
                                "why": message})
                continue
            raise VerificationError(message)

        actual = sha256_file(src)
        expected = entry.get("sha256", "")
        if refresh:
            entry["sha256"] = actual
        elif not expected:
            raise VerificationError(f"{src}: manifest has no sha256 (use --refresh-hashes)")
        elif expected != actual and refresh_payload and not entry.get("external"):
            # Mod source files (base payload/repo) are edited in the working tree by
            # whoever owns them; this launcher packs the working tree, so a moved pin
            # is reported and the on-disk file is used for this build.  The committed
            # manifest is not rewritten (a deliberate refresh is still required to
            # re-pin), and external/generated artifacts keep their own rule above.
            print(f"note: {src} differs from the manifest pin\n"
                  f"  manifest {expected}\n  on disk  {actual}\n"
                  f"  (mod payload source; using the on-disk file for this build)")
            entry["sha256"] = actual
        elif expected != actual and refresh_generated and entry.get("generate_with"):
            # A regenerable artifact (produced from the current core headers or the
            # retail pack by its own generator) legitimately moves with its inputs,
            # and this launcher regenerates it immediately before packing.  Note the
            # drift loudly and pin the on-disk file for this invocation only; the
            # committed manifest is not rewritten, so `verify` still reports drift.
            print(f"note: {src} differs from the manifest pin\n"
                  f"  manifest {expected}\n  on disk  {actual}\n"
                  f"  regenerate with: {entry['generate_with']}\n"
                  f"  (regenerable artifact; using the on-disk file for this build)")
            entry["sha256"] = actual
        elif expected != actual:
            raise VerificationError(
                f"{src}: sha256 mismatch\n  manifest {expected}\n  on disk  {actual}\n"
                f"  (generated file? regenerate with: {entry.get('generate_with', 'n/a')})"
            )
        resolved.append({"src": src, "dest": entry["dest"], "entry": entry,
                         "sha256": actual, "origin": origin})
    if refresh:
        manifest_path.write_text(json.dumps(manifest, indent="\t") + "\n")
        print(f"refreshed sha256 fields in {manifest_path}")
    return resolved, skipped


def check_original_pck(manifest: dict, pck: Path) -> str:
    if not pck.is_file():
        raise VerificationError(f"retail pack not found: {pck}")
    actual = sha256_file(pck)
    expected = (manifest.get("original_pck") or {}).get("sha256", "")
    if expected and expected != actual:
        raise VerificationError(
            "retail pack does not match the manifest\n"
            f"  {pck}\n  manifest {expected}\n  on disk  {actual}"
        )
    return actual


def gdre_version(gdre: Path) -> str:
    try:
        completed = subprocess.run([str(gdre), "--headless", "--gdre-version"],
                                   capture_output=True, text=True, timeout=120)
    except (OSError, subprocess.SubprocessError) as exc:
        return f"<unavailable: {exc}>"
    for line in ((completed.stdout or "") + (completed.stderr or "")).splitlines():
        if line.strip():
            return line.strip()
    return "<empty>"


def build_argv(gdre: Path, pck: Path, out: Path, files: list[dict]) -> list[str]:
    argv = [str(gdre), "--headless", f"--pck-patch={pck}", f"--output={out}"]
    for item in files:
        argv.append(f"--patch-file={item['src']}={item['dest']}")
    return argv


def shell_quote(value: str) -> str:
    if value and all(ch.isalnum() or ch in "-_./=:+@" for ch in value):
        return value
    return "'" + value.replace("'", "'\\''") + "'"


def report_entries(files: list[dict], skipped: list[dict]) -> None:
    for item in files:
        print(f"  {item['entry']['kind']:7s} {item['dest']}")
        print(f"          <- {item['src']} [{item['origin']}] sha256={item['sha256']}")
    for item in skipped:
        print(f"  SKIPPED optional {item['dest']}")
        print(f"          <- {item['src']} [{item['origin']}]")
        if item.get("generate_with"):
            print(f"          generate with: {item['generate_with']}")


def cmd_verify(args: argparse.Namespace) -> int:
    manifest_path = Path(args.manifest)
    manifest = load_manifest(manifest_path)
    files, skipped = resolve_entries(manifest, manifest_path,
                                     Path(args.external_root), refresh=False)
    pck_hash = check_original_pck(manifest, Path(args.pck))
    print(f"manifest      : {manifest_path}")
    print(f"repo root     : {find_repo_root(manifest_path.parent)}")
    print(f"external root : {args.external_root}")
    print(f"retail pack   : {args.pck} sha256={pck_hash}")
    print(f"payload files : {len(files)} to patch, {len(skipped)} optional skipped")
    report_entries(files, skipped)
    if args.parse_check:
        ok, report = check_scripts(Path(args.pck))
        print(report)
        if not ok:
            print("verify failed the GDScript-3 parse/load check", file=sys.stderr)
            return EXIT_VERIFY
    print("verify: OK")
    return EXIT_OK


def cmd_build(args: argparse.Namespace) -> int:
    manifest_path = Path(args.manifest)
    manifest = load_manifest(manifest_path)
    files, skipped = resolve_entries(manifest, manifest_path, Path(args.external_root),
                                     refresh=args.refresh_hashes,
                                     refresh_generated=args.refresh_generated,
                                     refresh_payload=args.refresh_payload)
    pck = Path(args.pck)
    pck_hash = check_original_pck(manifest, pck)
    out = Path(args.out)
    gdre = Path(args.gdre)

    argv = build_argv(gdre, pck, out, files)
    command_line = " ".join(shell_quote(part) for part in argv)
    print("retail pack  :", pck, "sha256=", pck_hash)
    print("output pack  :", out)
    report_entries(files, skipped)
    print("command      :")
    print(" ", command_line)

    if args.dry_run:
        print("dry-run: nothing written")
        return EXIT_OK

    if not gdre.is_file():
        raise VerificationError(f"GDRE executable not found: {gdre}")
    if out.resolve() == pck.resolve():
        raise VerificationError("refusing to overwrite the retail pack (--out must differ)")

    out.parent.mkdir(parents=True, exist_ok=True)
    if out.exists():
        out.unlink()
    completed = subprocess.run(argv, capture_output=True, text=True)
    sys.stdout.write(completed.stdout or "")
    sys.stderr.write(completed.stderr or "")
    if completed.returncode != 0 or not out.is_file():
        print(f"GDRE failed with exit code {completed.returncode}", file=sys.stderr)
        return EXIT_GDRE

    # The invariant: patching must not have touched the retail pack.
    pck_hash_after = sha256_file(pck)
    out_hash = sha256_file(out)
    inventory = {
        "bridge": manifest.get("bridge", ""),
        "bridge_version": manifest.get("bridge_version", ""),
        "generated_at_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "gdre": {"path": str(gdre), "version": gdre_version(gdre)},
        "retail_pck": {
            "path": str(pck),
            "sha256_before": pck_hash,
            "sha256_after": pck_hash_after,
            "unchanged": pck_hash == pck_hash_after,
            "size": pck.stat().st_size,
        },
        "output_pck": {"path": str(out), "sha256": out_hash, "size": out.stat().st_size},
        "manifest": str(manifest_path),
        "external_root": str(args.external_root),
        "command": argv,
        "command_line": command_line,
        "files": [
            {
                "dest": item["dest"],
                "src": str(item["src"]),
                "origin": item["origin"],
                "sha256": item["sha256"],
                "kind": item["entry"]["kind"],
                "optional": bool(item["entry"].get("optional", False)),
                "reason": item["entry"]["reason"],
                "replaces": item["entry"].get("replaces"),
                "derived_from": item["entry"].get("derived_from"),
            }
            for item in files
        ],
        "skipped_optional": skipped,
    }
    inventory_path = Path(args.inventory)
    inventory_path.write_text(json.dumps(inventory, indent="\t") + "\n")
    print(f"output pack  : sha256={out_hash} size={out.stat().st_size}")
    if not args.no_parse_check:
        ok, report = check_scripts(out)
        print(report)
        if not ok:
            print("build failed the GDScript-3 parse/load check (use --no-parse-check to skip)", file=sys.stderr)
            return EXIT_VERIFY
    print(f"retail pack unchanged: {inventory['retail_pck']['unchanged']}")
    print(f"inventory    : {inventory_path}")
    if not inventory["retail_pck"]["unchanged"]:
        return EXIT_VERIFY
    return EXIT_OK


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)

    def add_common(target: argparse.ArgumentParser) -> None:
        target.add_argument("--manifest", default=DEFAULT_MANIFEST)
        target.add_argument("--pck", default=_retail_pck())
        target.add_argument("--external-root", default=_external_root())

    scripts = sub.add_parser("check-scripts",
                            help="boot a built pack under the pinned engine and fail on GDScript load errors")
    scripts.add_argument("--pck", default=_retail_pck(),
                         help="pack to boot (must contain the bridge payload: a built Brotato.rl.pck)")
    scripts.add_argument("--timeout", type=float, default=180.0)
    scripts.set_defaults(func=cmd_check_scripts)

    verify = sub.add_parser("verify", help="check payload hashes and the retail pack hash")
    verify.add_argument("--parse-check", action="store_true",
                        help="also boot the pack at --pck and require the bridge mod to load cleanly")
    add_common(verify)
    verify.set_defaults(func=cmd_verify)

    build = sub.add_parser("build", help="apply the payload to a copy of the retail pack")
    add_common(build)
    build.add_argument("--out", default=str(Path(_run_dir()) / "Brotato.rl.pck"))
    build.add_argument("--gdre", default=_gdre())
    build.add_argument("--inventory", default=str(Path(_run_dir()) / "patch_inventory.json"))
    build.add_argument("--no-parse-check", action="store_true",
                       help="skip the post-build GDScript-3 parse/load check (the built pack is booted "
                            "briefly under the pinned engine; a parse error fails the build with file:line)")
    build.add_argument("--dry-run", action="store_true",
                       help="print the command without running GDRE")
    build.add_argument("--refresh-hashes", action="store_true",
                       help="rewrite sha256 fields in the manifest from disk (intentional edits only)")
    build.add_argument("--refresh-payload", action="store_true",
                       help="accept on-disk mod payload sources (non-external entries) whose committed pin "
                            "moved because the owner edited them in the working tree; prints each moved pin, "
                            "does not rewrite the manifest, and leaves external/generated artifacts strict")
    build.add_argument("--refresh-generated", action="store_true",
                       help="accept on-disk regenerable artifacts (entries with 'generate_with') whose "
                            "committed pin has moved with the core headers/retail pack; the manifest is "
                            "not rewritten and strict pinning is kept for every committed payload file")
    build.set_defaults(func=cmd_build)
    return parser


def cmd_check_scripts(args: argparse.Namespace) -> int:
    ok, report = check_scripts(Path(args.pck), args.timeout)
    print(report)
    return EXIT_OK if ok else EXIT_VERIFY


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return args.func(args)
    except ManifestError as exc:
        print(f"manifest error: {exc}", file=sys.stderr)
        return EXIT_USAGE
    except VerificationError as exc:
        print(f"verification failed: {exc}", file=sys.stderr)
        return EXIT_VERIFY


if __name__ == "__main__":
    raise SystemExit(main())
