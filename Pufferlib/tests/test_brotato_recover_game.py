"""Deterministic, isolated tests for ocean/brotato/recover_game.py.

These tests never invoke the real GDRE binary and never assert anything about
decompiled game source text.  They exercise, in isolation:

* the pure provenance-parsing helpers (GDRE export log, project.godot),
* hashing / counting of a recovered tree,
* manifest construction and round-tripping,
* manifest verification failures (pack hash, file hash, missing/extra files,
  counts) and the resulting exit codes,
* output-path rejection (never write recovered content inside the repo),
* real subprocess behaviour (argv, no shell) and honest failure propagation
  using a tiny fake GDRE executable.
"""

from __future__ import annotations

import hashlib
import importlib.util
import json
import os
from pathlib import Path

import pytest

_MODULE_PATH = (
    Path(__file__).resolve().parents[1] / "ocean" / "brotato" / "recover_game.py"
)
_spec = importlib.util.spec_from_file_location("recover_game_under_test", _MODULE_PATH)
rg = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(rg)


# ---------------------------------------------------------------------------
# fixtures / helpers
# ---------------------------------------------------------------------------

#: A trimmed but structurally faithful slice of a real GDRE export log.  This
#: is tool output metadata, not decompiled game source.
GDRE_LOG = """\
GDRE Tools v2.7.4
macOS 26.6.2, headless
Opening file: /tmp/Brotato.pck
ERROR: Failed to detect GDScript revision for bytecode version 13, engine version 3.7, candidates: 3.5.0-stable (a7aad78), 3.2.0-stable (5565f55).
Guessing best bytecode revision from engine version 3.7.0: 3.5.0-stable (rev 0xa7aad78)
Loaded 1928 imported files
Detected Engine Version: 3.7.0
Detected Bytecode Revision: 3.5.0-stable (a7aad78)
Successfully loaded PCK!
Version: 3.7.0
Verified 7093 files, no errors detected!
Extracted 7093 files, no errors detected!
Recreating plugin configs...
Loading resource strings, this may take a while!!
Resource strings loaded!
Saved project config.


********************************EXPORT REPORT********************************

Totals:                                 
Decompiled scripts:                     425
Scripts not decompiled:                 0
Imported resources for export session:  1928
Successfully converted:                 1928
Lossy:                                  0
-------------

The following plugins failed to have their plugin.cfg regenerated:
resave_scenes

---------------------------------IMPORTANT NOTES----------------------------------

Translation Export Incomplete:
Could not recover 71/1149 keys for res://resources/translations/translations.csv

------
Incomplete Plugin Export:
The following addons failed to have their plugin.cfg regenerated
You may encounter editor errors due to this.
Tip: Try finding the plugin in the Godot Asset Library or Github.

 - resave_scenes

------------------------------------------------------------------------------------

Use Godot editor version 3.7.0 to edit the project.

Recovery finished in 00m04s
"""


def make_recovery(
    root: Path,
    *,
    scripts: int = 3,
    scenes: int = 1,
    resources: int = 1,
    gdc: int | None = None,
    log: bool = True,
    project: bool = True,
    log_text: str = GDRE_LOG,
) -> Path:
    """Create a minimal synthetic recovered tree (never real game content)."""
    root.mkdir(parents=True, exist_ok=True)
    if project:
        (root / "project.godot").write_text(
            'config_version=4\n\n[application]\n\nconfig/name="T"\n',
            encoding="utf-8",
        )
    for i in range(scripts):
        (root / f"script_{i}.gd").write_text(f"extends Node\n\nvar i = {i}\n")
    for i in range(scenes):
        (root / f"scene_{i}.tscn").write_text("[gd_scene format=2]\n")
    for i in range(resources):
        (root / f"res_{i}.tres").write_text("[resource]\n")
    if gdc is not None:
        autoconverted = root / ".autoconverted"
        autoconverted.mkdir(exist_ok=True)
        for i in range(gdc):
            (autoconverted / f"script_{i}.gdc").write_text("bytecode")
    if log:
        (root / "gdre_export.log").write_text(log_text, encoding="utf-8")
    return root


def make_pack(path: Path, payload: bytes = b"pck-bytes") -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(payload)
    return path


def _write_fake_gdre(path: Path, *, body: str = "echo boom >&2", code: int = 1) -> Path:
    text = "\n".join(
        [
            "#!/bin/sh",
            'if [ -n "$ARGV_LOG" ]; then',
            '  printf \'%s\\n\' BEGIN >> "$ARGV_LOG"',
            '  for a in "$@"; do printf \'%s\\n\' "$a" >> "$ARGV_LOG"; done',
            "fi",
            'if [ "$1" = "--headless" ] && [ "$2" = "--gdre-version" ]; then',
            '  echo "Godot RE Tools v9.9.9"',
            "  exit 0",
            "fi",
            body,
            f"exit {code}",
            "",
        ]
    )
    path.write_text(text, encoding="utf-8")
    os.chmod(path, 0o755)
    return path


def read_argv_groups(log_path: Path):
    groups, current = [], None
    for line in log_path.read_text(encoding="utf-8").splitlines():
        if line == "BEGIN":
            current = []
            groups.append(current)
        elif current is not None:
            current.append(line)
    return groups


def index_tree(tmp_path: Path, pack: Path, root: Path, *args):
    """Run the CLI in --index-existing mode over an existing tree."""
    return rg.main(
        [
            "--pack",
            str(pack),
            "--output",
            str(root),
            "--index-existing",
            *args,
        ]
    )


def unique_pack(tmp_path: Path) -> Path:
    return make_pack(tmp_path / "Brotato.pck")


# ---------------------------------------------------------------------------
# hashing helpers
# ---------------------------------------------------------------------------


def test_sha256_file_matches_hashlib(tmp_path):
    target = tmp_path / "blob.bin"
    target.write_bytes(b"hello world")
    assert rg.sha256_file(target) == hashlib.sha256(b"hello world").hexdigest()


def test_sha256_file_streams_large_content(tmp_path):
    target = tmp_path / "big.bin"
    payload = bytes(range(256)) * 4096
    target.write_bytes(payload)
    assert rg.sha256_file(target, chunk_size=7) == hashlib.sha256(payload).hexdigest()


def test_sha256_bytes_matches_hashlib():
    assert rg.sha256_bytes(b"abc") == hashlib.sha256(b"abc").hexdigest()


# ---------------------------------------------------------------------------
# GDRE export log parsing
# ---------------------------------------------------------------------------


def test_parse_gdre_log_extracts_engine_metadata():
    parsed = rg.parse_gdre_log(GDRE_LOG)["fields"]
    assert parsed["gdre_version"] == "2.7.4"
    assert parsed["engine_version"] == "3.7.0"
    assert parsed["pck_version"] == "3.7.0"
    assert parsed["bytecode_version"] == 13
    assert parsed["bytecode_engine_guess"] == "3.7"
    assert parsed["bytecode_revision"] == "3.5.0-stable (a7aad78)"
    assert parsed["bytecode_revision_commit"] == "a7aad78"
    assert parsed["recommended_editor"] == "3.7.0"
    assert parsed["loaded_imported_files"] == 1928
    assert parsed["verified_files"] == 7093
    assert parsed["extracted_files"] == 7093
    assert parsed["decompiled_scripts"] == 425
    assert parsed["scripts_not_decompiled"] == 0
    assert parsed["converted_resources"] == 1928


def test_parse_gdre_log_records_recovery_notes():
    notes = rg.parse_gdre_log(GDRE_LOG)["notes"]
    assert notes["translations_incomplete"] == [
        {
            "path": "res://resources/translations/translations.csv",
            "recovered": 71,
            "total": 1149,
        }
    ]
    assert notes["plugins_not_regenerated"] == ["resave_scenes"]
    assert "IMPORTANT NOTES" in notes["raw"]


def test_parse_gdre_log_without_export_report_is_all_none():
    parsed = rg.parse_gdre_log("GDRE Tools v2.7.4\nnothing useful here\n")
    assert parsed["fields"]["decompiled_scripts"] is None
    assert parsed["fields"]["scripts_not_decompiled"] is None
    assert parsed["fields"]["bytecode_version"] is None
    assert parsed["notes"]["raw"] is None


def test_parse_gdre_log_does_not_confuse_engine_and_pck_version_lines():
    text = "Detected Engine Version: 3.7.0\nVersion: 4.0.0\n"
    fields = rg.parse_gdre_log(text)["fields"]
    assert fields["engine_version"] == "3.7.0"
    assert fields["pck_version"] == "4.0.0"


# ---------------------------------------------------------------------------
# project.godot + tree inspection
# ---------------------------------------------------------------------------


def test_parse_project_config_version(tmp_path):
    project = tmp_path / "project.godot"
    project.write_text('; comment\nconfig_version=4\n\n[application]\n')
    assert rg.parse_project_config_version(project) == 4


def test_parse_project_config_version_absent(tmp_path):
    project = tmp_path / "project.godot"
    project.write_text("[application]\nconfig/name=\"x\"\n")
    assert rg.parse_project_config_version(project) is None
    assert rg.parse_project_config_version(tmp_path / "missing.godot") is None


def test_count_recovered_counts_sources_and_bytecode(tmp_path):
    root = make_recovery(tmp_path / "r", scripts=4, scenes=2, resources=3, gdc=4)
    counts = rg.count_recovered(root)
    assert counts == {
        "recovered_scripts": 4,
        "recovered_scenes": 2,
        "recovered_resources": 3,
        "source_bytecode_remaining": 4,
    }


def test_count_recovered_ignores_editor_import_cache(tmp_path):
    root = make_recovery(tmp_path / "r", scripts=1, scenes=0, log=False)
    cache = root / ".import"
    cache.mkdir()
    (cache / "ghost.gd").write_text("should not count")
    (cache / "ghost.tscn").write_text("should not count")
    counts = rg.count_recovered(root)
    assert counts["recovered_scripts"] == 1
    assert counts["recovered_scenes"] == 0


def test_iter_recovered_is_deterministic_and_relative(tmp_path):
    root = make_recovery(tmp_path / "r", scripts=2, scenes=1, resources=1)
    (root / "sub").mkdir()
    (root / "sub" / "nested.gd").write_text("extends Node\n")
    first = list(rg.iter_recovered(root))
    second = list(rg.iter_recovered(root))
    assert first == second
    rels = [rel for _, rel in first]
    assert "script_0.gd" in rels
    assert "sub/nested.gd" in rels
    assert all(not rel.startswith("/") and "\\" not in rel for rel in rels)
    assert len(rels) == len(set(rels))


# ---------------------------------------------------------------------------
# manifest round trip
# ---------------------------------------------------------------------------


def test_manifest_write_read_round_trip(tmp_path):
    manifest = rg.build_manifest([{"role": "campaign"}], kind=rg.KIND_VERIFIED)
    path = tmp_path / "m.json"
    rg.write_manifest(path, manifest)
    assert rg.read_manifest(path) == manifest


def test_read_manifest_rejects_wrong_schema(tmp_path):
    path = tmp_path / "m.json"
    path.write_text(json.dumps({"schema": "other", "recoveries": []}))
    with pytest.raises(rg.UsageError, match="unexpected manifest schema"):
        rg.read_manifest(path)


def test_read_manifest_rejects_missing_file(tmp_path):
    with pytest.raises(rg.UsageError, match="manifest not found"):
        rg.read_manifest(tmp_path / "nope.json")


def test_read_manifest_rejects_invalid_json(tmp_path):
    path = tmp_path / "m.json"
    path.write_text("{not json")
    with pytest.raises(rg.UsageError, match="not valid JSON"):
        rg.read_manifest(path)


def test_manifest_created_utc_honours_source_date_epoch(tmp_path, monkeypatch):
    monkeypatch.setenv("SOURCE_DATE_EPOCH", "1700000000")
    manifest = rg.build_manifest([], kind=rg.KIND_VERIFIED)
    assert manifest["created_utc"] == "2023-11-14T22:13:20Z"


# ---------------------------------------------------------------------------
# indexing an existing recovery
# ---------------------------------------------------------------------------


def test_index_existing_labels_provenance_honestly(tmp_path, capsys):
    pack = unique_pack(tmp_path)
    root = make_recovery(tmp_path / "recovered", scripts=2, gdc=2)
    assert index_tree(tmp_path, pack, root) == rg.EXIT_OK
    out = capsys.readouterr().out
    assert "indexed existing recovery" in out

    manifest = rg.read_manifest(root / rg.MANIFEST_DEFAULT_NAME)
    assert manifest["kind"] == rg.KIND_INDEXED
    assert manifest["schema"] == rg.MANIFEST_SCHEMA
    recovery = manifest["recoveries"][0]
    assert recovery["role"] == rg.ROLE_CAMPAIGN
    assert recovery["kind"] == rg.KIND_INDEXED
    assert recovery["pack"]["sha256"] == rg.sha256_file(pack)
    assert recovery["pack"]["size_bytes"] == pack.stat().st_size
    assert recovery["engine"]["engine_version"] == "3.7.0"
    assert recovery["engine"]["project_config_version"] == 4
    assert recovery["gdre"]["version"] == "2.7.4"
    assert recovery["counts"]["recovered_scripts"] == 2
    assert recovery["counts"]["source_bytecode_remaining"] == 2


def test_indexed_manifest_lists_every_recovered_file_with_hash(tmp_path):
    pack = unique_pack(tmp_path)
    root = make_recovery(tmp_path / "recovered", scripts=2, scenes=1, resources=1)
    index_tree(tmp_path, pack, root)
    recovery = rg.read_manifest(root / rg.MANIFEST_DEFAULT_NAME)["recoveries"][0]
    files = recovery["files"]
    assert sorted(files) == ["resources", "scenes", "scripts"]
    seen = set()
    for group in files.values():
        for entry in group:
            seen.add(entry["path"])
            assert entry["res_path"] == f"res://{entry['path']}"
            assert entry["sha256"] == rg.sha256_file(root / entry["path"])
            assert entry["size_bytes"] == (root / entry["path"]).stat().st_size
    assert seen == {
        "script_0.gd",
        "script_1.gd",
        "scene_0.tscn",
        "res_0.tres",
    }
    # the manifest itself must not be part of its own file list
    assert rg.MANIFEST_DEFAULT_NAME not in seen


def test_indexed_manifest_bytecode_report_is_honest(tmp_path):
    pack = unique_pack(tmp_path)
    root = make_recovery(tmp_path / "recovered", scripts=2, gdc=2)
    index_tree(tmp_path, pack, root)
    report = rg.read_manifest(root / rg.MANIFEST_DEFAULT_NAME)["recoveries"][0][
        "bytecode_report"
    ]
    assert report["original_bytecode_remaining"] == 2
    assert report["decompiled_scripts"] == 425
    assert report["recovered_scripts"] == 2
    # 2 on disk != 425 + 0 reported: the flag must say so, not pretend.
    assert report["bytecode_matches_decompiler_totals"] is False
    # and it must not mislabel whitespace as lost control blocks
    assert "not evidence of lost control flow" in report["note"]


def test_index_existing_refuses_to_clobber_manifest_without_force(tmp_path, capsys):
    pack = unique_pack(tmp_path)
    root = make_recovery(tmp_path / "recovered", scripts=1)
    assert index_tree(tmp_path, pack, root) == rg.EXIT_OK
    assert index_tree(tmp_path, pack, root) == rg.EXIT_UNSAFE_OUTPUT
    assert "already exists" in capsys.readouterr().err


def test_index_existing_force_overwrites_manifest(tmp_path):
    pack = unique_pack(tmp_path)
    root = make_recovery(tmp_path / "recovered", scripts=1)
    assert index_tree(tmp_path, pack, root) == rg.EXIT_OK
    assert index_tree(tmp_path, pack, root, "--force") == rg.EXIT_OK


def test_index_existing_requires_project_godot(tmp_path, capsys):
    pack = unique_pack(tmp_path)
    root = make_recovery(tmp_path / "recovered", scripts=2, project=False)
    assert index_tree(tmp_path, pack, root) == rg.EXIT_RECOVERY_FAILED
    assert "not a recovered Godot project" in capsys.readouterr().err
    assert not (root / rg.MANIFEST_DEFAULT_NAME).exists()


def test_index_existing_requires_scripts(tmp_path, capsys):
    pack = unique_pack(tmp_path)
    root = make_recovery(tmp_path / "recovered", scripts=0)
    assert index_tree(tmp_path, pack, root) == rg.EXIT_RECOVERY_FAILED
    assert "nothing to hand to the pipeline" in capsys.readouterr().err


def test_index_existing_without_log_is_allowed_but_engine_metadata_absent(tmp_path):
    pack = unique_pack(tmp_path)
    root = make_recovery(tmp_path / "recovered", scripts=1, log=False)
    assert index_tree(tmp_path, pack, root) == rg.EXIT_OK
    recovery = rg.read_manifest(root / rg.MANIFEST_DEFAULT_NAME)["recoveries"][0]
    assert recovery["gdre_log"] is None
    assert recovery["engine"]["engine_version"] is None
    assert recovery["engine"]["project_config_version"] == 4
    assert recovery["bytecode_report"]["decompiled_scripts"] is None


def test_index_existing_records_dlc_role_when_asked(tmp_path):
    pack = unique_pack(tmp_path)
    dlc_pack = make_pack(tmp_path / "dlc" / "BrotatoAbyssalTerrors.pck", b"dlc")
    root = make_recovery(tmp_path / "campaign", scripts=1)
    dlc_root = make_recovery(tmp_path / "abyss", scripts=1)
    code = rg.main(
        [
            "--pack",
            str(pack),
            "--output",
            str(root),
            "--dlc-pack",
            str(dlc_pack),
            "--dlc-output",
            str(dlc_root),
            "--index-existing",
        ]
    )
    assert code == rg.EXIT_OK
    roles = [
        r["role"]
        for r in rg.read_manifest(root / rg.MANIFEST_DEFAULT_NAME)["recoveries"]
    ]
    assert roles == [rg.ROLE_CAMPAIGN, rg.ROLE_DLC]


# ---------------------------------------------------------------------------
# reuse verification
# ---------------------------------------------------------------------------


def _reuse(tmp_path, pack, root, *args):
    return rg.main(
        ["--pack", str(pack), "--output", str(root), "--reuse", *args]
    )


def test_reuse_passes_on_untouched_indexed_tree(tmp_path, capsys):
    pack = unique_pack(tmp_path)
    root = make_recovery(tmp_path / "recovered", scripts=2, gdc=2)
    index_tree(tmp_path, pack, root)
    capsys.readouterr()
    assert _reuse(tmp_path, pack, root) == rg.EXIT_OK
    out = capsys.readouterr().out
    assert "reuse verification passed" in out
    assert rg.KIND_INDEXED in out


def test_reuse_detects_tampered_recovered_file(tmp_path, capsys):
    pack = unique_pack(tmp_path)
    root = make_recovery(tmp_path / "recovered", scripts=2, gdc=2)
    index_tree(tmp_path, pack, root)
    (root / "script_0.gd").write_text("extends Node\n# edited after recovery\n")
    assert _reuse(tmp_path, pack, root) == rg.EXIT_VERIFY_FAILED
    err = capsys.readouterr().err
    assert "sha256 mismatch for script_0.gd" in err


def test_reuse_detects_missing_recovered_file(tmp_path, capsys):
    pack = unique_pack(tmp_path)
    root = make_recovery(tmp_path / "recovered", scripts=2, gdc=2)
    index_tree(tmp_path, pack, root)
    (root / "script_1.gd").unlink()
    assert _reuse(tmp_path, pack, root) == rg.EXIT_VERIFY_FAILED
    err = capsys.readouterr().err
    assert "missing recovered file: script_1.gd" in err
    assert "count mismatch recovered_scripts" in err


def test_reuse_detects_extra_recovered_file(tmp_path, capsys):
    pack = unique_pack(tmp_path)
    root = make_recovery(tmp_path / "recovered", scripts=1, gdc=1)
    index_tree(tmp_path, pack, root)
    (root / "smuggled.gd").write_text("extends Node\n")
    assert _reuse(tmp_path, pack, root) == rg.EXIT_VERIFY_FAILED
    assert "unexpected file not in manifest: smuggled.gd" in capsys.readouterr().err


def test_reuse_detects_pack_hash_change(tmp_path, capsys):
    pack = unique_pack(tmp_path)
    root = make_recovery(tmp_path / "recovered", scripts=1, gdc=1)
    index_tree(tmp_path, pack, root)
    pack.write_bytes(b"different pack bytes")
    assert _reuse(tmp_path, pack, root) == rg.EXIT_VERIFY_FAILED
    assert "pack sha256 mismatch" in capsys.readouterr().err


def test_reuse_detects_size_change_without_hash_change_is_impossible(tmp_path):
    # sanity: our size check and hash check agree for a stable file
    pack = unique_pack(tmp_path)
    root = make_recovery(tmp_path / "recovered", scripts=1, gdc=1)
    index_tree(tmp_path, pack, root)
    manifest = rg.read_manifest(root / rg.MANIFEST_DEFAULT_NAME)
    assert rg.verify_manifest(manifest, pack_overrides={"campaign": pack}) == []


def test_reuse_requires_existing_manifest(tmp_path, capsys):
    pack = unique_pack(tmp_path)
    root = make_recovery(tmp_path / "recovered", scripts=1, gdc=1)
    assert _reuse(tmp_path, pack, root) == rg.EXIT_USAGE
    assert "manifest not found" in capsys.readouterr().err


def test_reuse_reports_missing_dlc_role(tmp_path, capsys):
    pack = unique_pack(tmp_path)
    root = make_recovery(tmp_path / "recovered", scripts=1, gdc=1)
    index_tree(tmp_path, pack, root)
    dlc_pack = make_pack(tmp_path / "dlc.pck", b"dlc")
    code = rg.main(
        [
            "--pack",
            str(pack),
            "--output",
            str(root),
            "--dlc-pack",
            str(dlc_pack),
            "--dlc-output",
            str(tmp_path / "abyss"),
            "--reuse",
        ]
    )
    assert code == rg.EXIT_VERIFY_FAILED
    assert "no recovery for role 'dlc'" in capsys.readouterr().err


def test_verify_manifest_empty_recoveries_is_a_problem():
    assert rg.verify_manifest({"recoveries": []}) == [
        "manifest records no recoveries"
    ]


def test_verify_manifest_reports_missing_root(tmp_path):
    pack = unique_pack(tmp_path)
    root = make_recovery(tmp_path / "recovered", scripts=1)
    index_tree(tmp_path, pack, root)
    manifest = rg.read_manifest(root / rg.MANIFEST_DEFAULT_NAME)
    manifest["recoveries"][0]["output_root"] = str(tmp_path / "gone")
    problems = rg.verify_manifest(manifest)
    assert any("recovery root missing" in p for p in problems)


# ---------------------------------------------------------------------------
# output-path rejection
# ---------------------------------------------------------------------------


def test_rejects_output_inside_repository(tmp_path, capsys):
    pack = unique_pack(tmp_path)
    inside = rg.REPO_ROOT / "ocean" / "brotato" / "_forbidden_recovery"
    assert rg.is_within(inside.resolve(), rg.REPO_ROOT.resolve())
    code = rg.main(
        ["--pack", str(pack), "--output", str(inside), "--index-existing"]
    )
    assert code == rg.EXIT_UNSAFE_OUTPUT
    assert "inside the repository" in capsys.readouterr().err
    assert not inside.exists()


def test_rejects_output_equal_to_repository_root(tmp_path, capsys):
    pack = unique_pack(tmp_path)
    code = rg.main(
        ["--pack", str(pack), "--output", str(rg.REPO_ROOT), "--index-existing"]
    )
    assert code == rg.EXIT_UNSAFE_OUTPUT
    assert "inside the repository" in capsys.readouterr().err


def test_rejects_output_that_contains_the_repository(tmp_path, capsys):
    pack = unique_pack(tmp_path)
    code = rg.main(
        ["--pack", str(pack), "--output", str(rg.REPO_ROOT.parent), "--index-existing"]
    )
    assert code == rg.EXIT_UNSAFE_OUTPUT
    assert "contains the repository" in capsys.readouterr().err


def test_rejects_output_that_is_a_file(tmp_path, capsys):
    pack = unique_pack(tmp_path)
    file_out = tmp_path / "not_a_dir"
    file_out.write_text("x")
    code = rg.main(
        ["--pack", str(pack), "--output", str(file_out), "--index-existing"]
    )
    assert code == rg.EXIT_UNSAFE_OUTPUT
    assert "not a directory" in capsys.readouterr().err


def test_ensure_safe_output_root_allows_external_tmpdir(tmp_path):
    external = tmp_path / "external"
    assert rg.ensure_safe_output_root(external) == external.resolve()


# ---------------------------------------------------------------------------
# argument validation
# ---------------------------------------------------------------------------


def test_missing_pack_file_is_a_usage_error(tmp_path, capsys):
    code = rg.main(
        [
            "--pack",
            str(tmp_path / "absent.pck"),
            "--output",
            str(tmp_path / "out"),
            "--index-existing",
        ]
    )
    assert code == rg.EXIT_USAGE
    assert "pack not found" in capsys.readouterr().err


def test_dlc_pack_requires_dlc_output(tmp_path, capsys):
    pack = unique_pack(tmp_path)
    dlc_pack = make_pack(tmp_path / "dlc.pck", b"dlc")
    code = rg.main(
        [
            "--pack",
            str(pack),
            "--output",
            str(tmp_path / "out"),
            "--dlc-pack",
            str(dlc_pack),
            "--index-existing",
        ]
    )
    assert code == rg.EXIT_USAGE
    assert "--dlc-output is required" in capsys.readouterr().err


def test_same_pack_for_campaign_and_dlc_is_rejected(tmp_path, capsys):
    pack = unique_pack(tmp_path)
    code = rg.main(
        [
            "--pack",
            str(pack),
            "--output",
            str(tmp_path / "a"),
            "--dlc-pack",
            str(pack),
            "--dlc-output",
            str(tmp_path / "b"),
            "--index-existing",
        ]
    )
    assert code == rg.EXIT_USAGE
    assert "must be different files" in capsys.readouterr().err


def test_overlapping_output_roots_rejected(tmp_path, capsys):
    pack = unique_pack(tmp_path)
    dlc_pack = make_pack(tmp_path / "dlc.pck", b"dlc")
    code = rg.main(
        [
            "--pack",
            str(pack),
            "--output",
            str(tmp_path / "out"),
            "--dlc-pack",
            str(dlc_pack),
            "--dlc-output",
            str(tmp_path / "out" / "nested"),
            "--index-existing",
        ]
    )
    assert code == rg.EXIT_USAGE
    assert "must be disjoint" in capsys.readouterr().err


def test_reuse_and_index_are_mutually_exclusive(tmp_path):
    pack = unique_pack(tmp_path)
    with pytest.raises(SystemExit) as excinfo:
        rg.main(
            [
                "--pack",
                str(pack),
                "--output",
                str(tmp_path / "out"),
                "--reuse",
                "--index-existing",
            ]
        )
    assert excinfo.value.code == 2


def test_parser_requires_pack_and_output():
    with pytest.raises(SystemExit):
        rg.main([])


# ---------------------------------------------------------------------------
# fresh recovery: preconditions, failure propagation, argv
# ---------------------------------------------------------------------------


def test_fresh_recovery_refuses_nonempty_dir(tmp_path, monkeypatch, capsys):
    pack = unique_pack(tmp_path)
    out = tmp_path / "recovered"
    out.mkdir()
    (out / "leftover.txt").write_text("already here")
    argv_log = tmp_path / "argv.log"
    monkeypatch.setenv("ARGV_LOG", str(argv_log))
    fake = _write_fake_gdre(tmp_path / "gdre", code=1)

    code = rg.main(["--pack", str(pack), "--output", str(out), "--gdre", str(fake)])
    assert code == rg.EXIT_UNSAFE_OUTPUT
    assert "refusing to overwrite non-empty recovery directory" in capsys.readouterr().err
    assert not (out / rg.MANIFEST_DEFAULT_NAME).exists()
    # GDRE was only asked for its version, never to recover
    assert all(
        not any(arg.startswith("--recover=") for arg in group)
        for group in read_argv_groups(argv_log)
    )


def test_gdre_nonzero_exit_propagates_and_writes_no_manifest(tmp_path, capsys):
    pack = unique_pack(tmp_path)
    out = tmp_path / "recovered"
    fake = _write_fake_gdre(tmp_path / "gdre", body="echo 'boom: bad pack' >&2", code=7)
    code = rg.main(["--pack", str(pack), "--output", str(out), "--gdre", str(fake)])
    assert code == rg.EXIT_RECOVERY_FAILED
    err = capsys.readouterr().err
    assert "GDRE failed with exit code 7" in err
    assert not (out / rg.MANIFEST_DEFAULT_NAME).exists()


def test_gdre_success_without_output_is_a_validation_failure(tmp_path, capsys):
    # A tool that exits 0 but produces no project must not be reported as a
    # successful recovery: there is no fabricated fallback.
    pack = unique_pack(tmp_path)
    out = tmp_path / "recovered"
    fake = _write_fake_gdre(tmp_path / "gdre", body="echo 'pretending'", code=0)
    code = rg.main(["--pack", str(pack), "--output", str(out), "--gdre", str(fake)])
    assert code == rg.EXIT_RECOVERY_FAILED
    assert "not a recovered Godot project" in capsys.readouterr().err
    assert not (out / rg.MANIFEST_DEFAULT_NAME).exists()


def test_missing_gdre_executable_is_a_usage_error(tmp_path, capsys):
    pack = unique_pack(tmp_path)
    code = rg.main(
        [
            "--pack",
            str(pack),
            "--output",
            str(tmp_path / "out"),
            "--gdre",
            str(tmp_path / "no_such_gdre"),
        ]
    )
    assert code == rg.EXIT_USAGE
    assert "not found or not executable" in capsys.readouterr().err


def test_probe_gdre_version_returns_none_on_failure(tmp_path):
    fake = _write_fake_gdre(tmp_path / "gdre", code=3)
    # only the version branch is reachable; make it fail instead
    fake.write_text("#!/bin/sh\nexit 3\n")
    os.chmod(fake, 0o755)
    assert rg.probe_gdre_version(str(fake)) is None


def test_probe_gdre_version_parses_real_output_shape(tmp_path):
    fake = tmp_path / "gdre"
    fake.write_text('#!/bin/sh\necho "Godot RE Tools v2.7.4"\n')
    os.chmod(fake, 0o755)
    assert rg.probe_gdre_version(str(fake)) == "2.7.4"


# ---------------------------------------------------------------------------
# strict validation of a fresh recovery
# ---------------------------------------------------------------------------


def test_strict_validation_rejects_missing_log(tmp_path):
    root = make_recovery(tmp_path / "r", scripts=1, log=False)
    with pytest.raises(rg.RecoveryFailure, match="cannot verify the recovery"):
        rg.validate_recovery(root, strict=True)


def test_strict_validation_rejects_log_without_decompiler_totals(tmp_path):
    root = make_recovery(tmp_path / "r", scripts=1, log=True, log_text="GDRE Tools v2.7.4\n")
    with pytest.raises(rg.RecoveryFailure, match="Decompiled scripts"):
        rg.validate_recovery(root, strict=True)


def test_strict_validation_rejects_fewer_scripts_than_decompiled(tmp_path):
    root = make_recovery(tmp_path / "r", scripts=1, gdc=1)
    with pytest.raises(rg.RecoveryFailure, match="but GDRE reported decompiling 425"):
        rg.validate_recovery(root, strict=True)


def test_strict_validation_rejects_bytecode_total_mismatch(tmp_path):
    root = make_recovery(tmp_path / "r", scripts=425, gdc=3)
    with pytest.raises(rg.RecoveryFailure, match="retained source bytecode count"):
        rg.validate_recovery(root, strict=True)


def test_strict_validation_accepts_consistent_tree(tmp_path):
    root = make_recovery(tmp_path / "r", scripts=425, gdc=425)
    result = rg.validate_recovery(root, strict=True)
    assert result["counts"]["recovered_scripts"] == 425
    assert result["counts"]["source_bytecode_remaining"] == 425
    assert result["log"]["fields"]["decompiled_scripts"] == 425


def test_non_strict_validation_tolerates_inconsistent_counts(tmp_path):
    root = make_recovery(tmp_path / "r", scripts=2, gdc=2)
    result = rg.validate_recovery(root, strict=False, log_present_required=False)
    assert result["counts"]["recovered_scripts"] == 2
