"""Consumer-visible tests for the Brotato policy export artifact.

These tests only touch the exporter's public contract: the CLI/``export`` entry
point, the checkpoint file format the native trainer writes
(``puf_save_weights``, src/pufferl.cu:1722-1736: float32 in registration order,
8-float aligned, src/puffercpu.c:47-51), and the JSON artifact a consumer reads.
They never assert on trainer internals: the expectations below are the
documented artifact schema (names, shapes, metadata, alignment) plus the format
the native reader consumes, not a re-statement of the CUDA kernels.

Observation schema is the LIVE core's (BT_OBS_SIZE floats) and the action space
is BT_ACTIONS.  The cutover is strict: there is no padding, no
migration helper and no opt-out, so every artifact this exporter produces
carries exactly the live core schema and a checkpoint at any other width is
rejected.  Synthetic arithmetic (tensor offsets/alignment) is exercised on
layouts only, never through ``export``.

``HID``/``HID_BIG``: exported fixtures keep every tensor a multiple of the
8-float checkpoint boundary, because that is the only shape the native trainer
can both dump and read back (see ``native_reader_matches``); the exporter itself
reads the packed file for any shape, which the unaligned-layout test covers.
"""

from __future__ import annotations

import base64
import importlib.util
import json
import os
import random
import shutil
import struct
import subprocess
import sys
from pathlib import Path

import pytest

_POLICY_DIR = Path(__file__).resolve().parents[1] / "ocean" / "brotato" / "policy"
if str(_POLICY_DIR) not in sys.path:
    sys.path.insert(0, str(_POLICY_DIR))


def _load(name: str):
    spec = importlib.util.spec_from_file_location(name + "_under_test",
                                                  _POLICY_DIR / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


export = _load("export_policy")
reference = _load("policy_reference")

ALIGN = export.ALIGNMENT_FLOATS
CORE = export.read_core_schema()
OBS = CORE.obs_size
ACTIONS = CORE.constants["actions"]
# OBS * hidden need not be a multiple of the 8-float boundary.
HID = 8
HID_BIG = 16


def _layout(obs, hidden, layers, actions, continuous=False):
    return export.build_layout(obs, hidden, layers, actions, continuous)


def _write_checkpoint(path: Path, layout, fill):
    """Write the checkpoint exactly as the trainer does; ``fill(name, i) -> f``."""
    export.write_trainer_checkpoint(str(path), layout, fill)


def _decode(tensor: dict):
    raw = base64.b64decode(tensor["data"])
    assert len(raw) % 4 == 0
    return struct.unpack("<%df" % (len(raw) // 4), raw)


def _good_fill(name, index):
    # A deterministic, non-constant pattern so every tensor is distinguishable.
    return ((index * 37 + len(name) * 7) % 101 - 50) / 128.0


def _export_core(checkpoint: Path, out: Path, hidden, layers, actions,
                 continuous=False, **kwargs):
    """Export a checkpoint written at the live core observation width."""
    layout = _layout(OBS, hidden, layers, actions, continuous)
    _write_checkpoint(checkpoint, layout, _good_fill)
    return export.export(str(checkpoint), str(out), OBS, hidden, layers, actions,
                         continuous, **kwargs)


# ---------------------------------------------------------------------------
# round-trip: values, names, shapes, metadata
# ---------------------------------------------------------------------------

def test_export_roundtrip_preserves_values_and_metadata(tmp_path):
    layout = _layout(OBS, HID, 2, ACTIONS)
    checkpoint = tmp_path / "ckpt.bin"
    _write_checkpoint(checkpoint, layout, _good_fill)
    out = tmp_path / "policy.json"

    artifact = export.export(str(checkpoint), str(out), OBS, HID, 2, ACTIONS)

    assert json.loads(out.read_text()) == artifact
    assert artifact["format"] == "pufferlib-brotato-policy"
    assert artifact["format_version"] == 1
    assert artifact["alignment_floats"] == ALIGN
    assert artifact["source_checkpoint"]["trainer_alignment_bytes"] == 16
    assert artifact["layout"] == {
        "obs_size": OBS, "hidden": HID, "layers": 2, "actions": ACTIONS,
        "continuous": False, "num_heads": 1, "head_sizes": [ACTIONS],
        "mask_size": ACTIONS, "masked_logit": export.MASKED_LOGIT,
    }
    assert artifact["weight_order"] == [
        "encoder.weight", "decoder.weight", "mingru.0.weight", "mingru.1.weight",
    ]

    by_name = {t["name"]: t for t in artifact["tensors"]}
    assert by_name["encoder.weight"]["shape"] == [HID, OBS]
    assert by_name["decoder.weight"]["shape"] == [ACTIONS + 1, HID]
    assert by_name["mingru.0.weight"]["shape"] == [3 * HID, HID]
    assert by_name["mingru.1.weight"]["shape"] == [3 * HID, HID]

    # Every tensor decodes back to the exact bytes written in registration order.
    for name, shape, numel in layout:
        tensor = by_name[name]
        assert tensor["dtype"] == "f32le"
        values = _decode(tensor)
        assert len(values) == numel
        assert values == tuple(_good_fill(name, i) for i in range(numel))


def test_alignment_boundary_keeps_tensors_separated(tmp_path):
    # Pure arithmetic: obs 5 * hidden 3 = 15 encoder floats, not a multiple of
    # the 8-float boundary, so the decoder must start after padding. No export
    # is involved here, so the layout is free to be synthetic.
    layout = _layout(5, 3, 1, 2)
    offsets = export.tensor_offsets(layout)
    assert offsets[0][2] == 0
    assert offsets[1][2] == 16  # 15 rounded up to the next multiple of 8
    assert export.layout_floats(layout) == 59

    # A real artifact at the live core width: OBS * HID is already a multiple of
    # the boundary, so encoder and decoder are contiguous and the values written
    # by the trainer land in the tensors the consumer reads.
    enc_numel = OBS * HID
    dec_off = (enc_numel + ALIGN - 1) // ALIGN * ALIGN
    artifact = _export_core(tmp_path / "ckpt.bin", tmp_path / "p.json", HID, 1,
                            ACTIONS)
    by_name = {t["name"]: t for t in artifact["tensors"]}
    assert artifact["layout"]["obs_size"] == OBS
    enc = _decode(by_name["encoder.weight"])
    dec = _decode(by_name["decoder.weight"])
    assert len(enc) == enc_numel
    assert enc[-1] == _good_fill("encoder.weight", enc_numel - 1)
    assert dec[0] == _good_fill("decoder.weight", 0)
    assert export.tensor_offsets(_layout(OBS, HID, 1, ACTIONS))[1][2] == dec_off


# ---------------------------------------------------------------------------
# rejection: size, finiteness, dimensions
# ---------------------------------------------------------------------------

def test_rejects_wrong_file_size(tmp_path):
    layout = _layout(OBS, HID, 2, ACTIONS)
    checkpoint = tmp_path / "ckpt.bin"
    _write_checkpoint(checkpoint, layout, _good_fill)
    short = tmp_path / "short.bin"
    short.write_bytes(checkpoint.read_bytes()[:-4])
    long = tmp_path / "long.bin"
    long.write_bytes(checkpoint.read_bytes() + b"\x00\x00\x00\x00")

    for bad in (short, long):
        with pytest.raises(export.ExportError):
            export.export(str(bad), str(tmp_path / "out.json"), OBS, HID, 2,
                          ACTIONS)
        assert not (tmp_path / "out.json").exists()


def test_rejects_wrong_network_shape_for_same_file(tmp_path):
    layout = _layout(OBS, HID, 2, ACTIONS)
    checkpoint = tmp_path / "ckpt.bin"
    _write_checkpoint(checkpoint, layout, _good_fill)
    with pytest.raises(export.ExportError):        # hidden differs
        export.export(str(checkpoint), str(tmp_path / "out.json"), OBS, HID_BIG, 2,
                      ACTIONS)
    with pytest.raises(export.ExportError):        # layers differ
        export.export(str(checkpoint), str(tmp_path / "out.json"), OBS, HID, 3,
                      ACTIONS)


def test_rejects_nonfinite_weights(tmp_path):
    layout = _layout(OBS, HID, 1, ACTIONS)
    checkpoint = tmp_path / "ckpt.bin"

    def fill(name, index):
        if name == "decoder.weight" and index == 2:
            return float("inf")
        if name == "mingru.0.weight" and index == 0:
            return float("nan")
        return _good_fill(name, index)

    _write_checkpoint(checkpoint, layout, fill)
    with pytest.raises(export.ExportError):
        export.export(str(checkpoint), str(tmp_path / "out.json"), OBS, HID, 1,
                      ACTIONS)
    assert not (tmp_path / "out.json").exists()


@pytest.mark.parametrize("dims", [
    (0, 4, 1, 3), (-1, 4, 1, 3),
    (OBS, 0, 1, 3), (OBS, 4, 0, 3), (OBS, 4, 1, 0), (OBS, 4, 1, -2),
])
def test_rejects_nonpositive_dimensions(tmp_path, dims):
    checkpoint = tmp_path / "none.bin"
    checkpoint.write_bytes(b"")
    with pytest.raises(export.ExportError):
        export.export(str(checkpoint), str(tmp_path / "out.json"), *dims)


def test_continuous_requires_single_action(tmp_path):
    with pytest.raises(export.ExportError):
        export.export(str(tmp_path / "x.bin"), str(tmp_path / "o.json"),
                      OBS, HID, 1, ACTIONS, continuous=True)
    # actions == 1 is the only continuous layout; it adds a logstd row.
    layout = _layout(OBS, HID, 1, 1, continuous=True)
    assert [n for n, _, _ in layout][:3] == [
        "encoder.weight", "decoder.weight", "decoder.logstd",
    ]
    # The packed file round-trips any shape, but this layout can never be a live
    # artifact: the core's action count is not 1.  The 1-wide logstd row is still
    # worth pinning -- it is the shape that the pre-packing aligned reader would
    # have misread.
    assert ("decoder.logstd", 1) == next((n, num) for n, _s, num in layout
                                         if num % ALIGN == 1)
    checkpoint = tmp_path / "ckpt.bin"
    _write_checkpoint(checkpoint, layout, _good_fill)
    with pytest.raises(export.ExportError, match="not the live core action count"):
        export.export(str(checkpoint), str(tmp_path / "o.json"), OBS, HID, 1, 1,
                      continuous=True)
    assert not (tmp_path / "o.json").exists()


def test_missing_checkpoint(tmp_path):
    with pytest.raises(export.ExportError):
        export.export(str(tmp_path / "absent.bin"), str(tmp_path / "o.json"),
                      OBS, HID, 1, ACTIONS)


def test_no_overwrite_guard(tmp_path):
    out = tmp_path / "policy.json"
    artifact = _export_core(tmp_path / "ckpt.bin", out, HID, 2, ACTIONS)
    existing = out.read_text()
    assert json.loads(existing) == artifact
    with pytest.raises(export.ExportError):
        export.export(str(tmp_path / "ckpt.bin"), str(out), OBS, HID, 2, ACTIONS,
                      overwrite=False)
    assert out.read_text() == existing


def test_cli_exit_code_and_stderr(tmp_path, capsys):
    checkpoint = tmp_path / "ckpt.bin"
    layout = _layout(OBS, HID, 3, ACTIONS)
    _write_checkpoint(checkpoint, layout, _good_fill)
    actions_arg = str(ACTIONS)

    # Valid invocation at the live core width.
    assert export.main(["--checkpoint", str(checkpoint),
                        "--output", str(tmp_path / "ok.json"),
                        "--obs-size", str(OBS), "--hidden", str(HID),
                        "--layers", "3", "--actions", actions_arg]) == 0
    capsys.readouterr()

    # A network shape the checkpoint does not have is a clean failure.
    assert export.main(["--checkpoint", str(checkpoint),
                        "--output", str(tmp_path / "bad.json"),
                        "--obs-size", str(OBS), "--hidden", str(HID_BIG),
                        "--layers", "3", "--actions", actions_arg]) == 1
    err = capsys.readouterr().err
    assert err.strip()
    assert not (tmp_path / "bad.json").exists()

    # A non-core observation width can no longer be requested at all.
    assert export.main(["--checkpoint", str(checkpoint),
                        "--output", str(tmp_path / "wrong.json"),
                        "--obs-size", str(OBS - 1), "--hidden", str(HID),
                        "--layers", "3", "--actions", actions_arg]) == 1
    assert capsys.readouterr().err.strip()
    assert not (tmp_path / "wrong.json").exists()
    assert not hasattr(export, "allow_core_mismatch")


# ---------------------------------------------------------------------------
# artifact schema consumers rely on
# ---------------------------------------------------------------------------

def test_observation_schema_is_derived_from_core_headers(tmp_path):
    # The schema must reflect the live headers, not a hard-coded old arena.
    state_h = (export.DEFAULT_CORE_DIR / "brotato_state.h").read_text()
    assert ("BT_ARENA_W %s" % _fmt(CORE.constants["arena_w"])) in state_h
    assert ("BT_ARENA_H %s" % _fmt(CORE.constants["arena_h"])) in state_h
    assert ("BT_MAX %s" % CORE.constants["max_enemies"]) in state_h

    obs = CORE.obs_size
    artifact = _export_core(tmp_path / "ckpt.bin", tmp_path / "p.json", HID, 1,
                            ACTIONS)
    schema = artifact["observation_schema"]
    assert schema["core_obs_size"] == obs
    assert schema["obs_size"] == obs
    assert schema["matches_layout"] is True
    assert len(schema["fields"]) == obs
    # No implicit padding entries anywhere in the schema.  (A guide ITEM is
    # named "padding"; the guard is about unnamed padding slots, so match the
    # placeholder form, not a substring.)
    assert not any(f == "padding" or f.startswith("padding") for f in schema["fields"])
    assert "padding" not in schema
    # Divisors come from the parsed constants.
    assert schema["constants"] == CORE.constants
    assert ("/%d" % CORE.constants["arena_w"]) in schema["fields"][4]
    assert schema["fields"][5].endswith("/%d" % CORE.constants["arena_h"])
    assert schema["fields"][67].endswith("/%d" % CORE.constants["max_enemies"])
    # Header revision stamp: every header the schema reads.
    for header in export.CORE_HEADERS:
        assert len(schema["revision"][header]) == 64
    # Appended weapon / shop / profile block: names come from the live headers,
    # never a hard-coded old width.
    slots = CORE.constants["slots"]
    base = 11 + 4 * 4 + 4 * 2 + slots * 5 + 3 + 3 * 3 + 2
    assert base == 79
    assert CORE.constants["weapons"] == schema["constants"]["weapons"]
    assert CORE.constants["characters"] == schema["constants"]["characters"]
    assert "padding" not in schema["constants"]
    assert schema["fields"][base + 0] == "weapon0.id/%d" % (CORE.constants["weapons"] + 1)
    assert schema["fields"][base + 1] == "weapon0.tier/4"
    assert schema["fields"][base + 2] == "weapon0.kind"
    assert schema["fields"][base + 3] == "weapon0.cooldown_frac"
    assert schema["fields"][base + 4] == "weapon0.damage*0.1"
    assert schema["fields"][base + 5].endswith("/%d" % CORE.constants["arena_w"])
    assert schema["fields"][base + 6] == "weapon0.crit_chance"
    assert schema["fields"][base + 7] == "weapon0.cursed"
    shop = base + slots * 8
    assert schema["fields"][shop + 0] == "shop0.offer_type"
    assert schema["fields"][shop + 1] == "shop0.content_id"
    assert schema["fields"][shop + 2] == "shop0.cursed"
    profile = shop + 12
    assert schema["fields"][profile] == ("profile.character_id_plus1/%d"
                                         % (CORE.constants["characters"] + 1))
    assert schema["fields"][profile + 1] == "profile.weapon_slot_limit/%d" % CORE.constants["weapon_slot_default"]
    assert schema["fields"][profile + 2] == "shop.currency_is_hp"
    # shop action block: 4 x (price, eligible, affordable_action, buyable) then
    # the reroll triple, ending the v3 161 floats.
    assert schema["fields"][profile + 19:profile + 22] == [
        "shop.reroll_price", "shop.reroll_free", "shop.reroll_available"]

    # Demon/endless block (v4): the reserved ids are part of the layout and are
    # observed as zeros today (AuditShop has not landed that state), so they must
    # stay in the field list.
    target = profile + 22
    assert target == 161
    block = schema["constants"]["target_block"]
    assert block["stats"] == 4 and block["crate"] == 4
    assert block["level_up_reroll"] == 1
    assert block["ban_tokens"] == 1 and block["level_up_offers"] == 4
    assert schema["fields"][target + 0] == "target.harvesting/100"
    assert schema["fields"][target + 1] == "target.luck/100"
    assert schema["fields"][target + 2] == "target.endless_factor_squashed"
    assert schema["fields"][target + 3] == "target.wave/50"
    assert schema["fields"][target + 4:target + 8] == ["shop%d.locked" % i
                                                       for i in range(4)]
    assert schema["fields"][target + 8:target + 12] == [
        "crate.offer_kind", "crate.offer_id", "crate.cursed", "crate.ready"]
    assert schema["fields"][target + 12] == "level_up.reroll_price"
    guide = schema["constants"]["guide_items"]
    assert len(guide) == block["guide_items"] == 24
    assert schema["fields"][target + 13:target + 37] == ["item_%s.owned_frac" % n
                                                        for n in guide]
    assert guide[0] == "bag" and guide[-1] == "alien_magic"
    # v5 tail: the ban-token budget and the four level-up presence flags.
    assert schema["fields"][target + 37] == "target.ban_tokens/8"
    assert schema["fields"][target + 38:target + 42] == [
        "level_up%d.offer_present" % i for i in range(4)]
    assert len(schema["fields"]) == obs == 203


def _fmt(value):
    return str(int(value)) if float(value).is_integer() else repr(value)


def test_obs_size_defaults_to_core(tmp_path):
    # obs_size omitted -> the live core width, labelled as matching.
    artifact = _export_core(tmp_path / "ckpt.bin", tmp_path / "d.json", HID, 1,
                            ACTIONS)
    assert artifact["layout"]["obs_size"] == OBS
    assert artifact["observation_schema"]["matches_layout"] is True
    assert artifact["observation_schema"]["core_obs_size"] == OBS


def test_cutover_rejects_any_width_other_than_the_live_core(tmp_path):
    # Clean cutover: no padding, no migration helper, no opt-out. A checkpoint
    # trained at an older width is rejected, never resized.
    assert not hasattr(export, "trim_encoder_inputs")
    assert not hasattr(export, "allow_core_mismatch")
    for stale in (79, 81, OBS - 1, OBS + 1):
        out = tmp_path / ("o_%d.json" % stale)
        checkpoint = tmp_path / ("ckpt_%d.bin" % stale)
        _write_checkpoint(checkpoint, _layout(stale, HID, 1, ACTIONS), _good_fill)
        with pytest.raises(export.ExportError):
            export.export(str(checkpoint), str(out), stale, HID, 1, ACTIONS)
        assert not out.exists()


def test_train_time_revision_is_stamped_and_not_overclaimed(tmp_path):
    artifact = _export_core(tmp_path / "ckpt.bin", tmp_path / "p.json", HID, 1,
                            ACTIONS, train_revision="rev-abc123")
    assert artifact["source_checkpoint"]["train_time_revision"] == "rev-abc123"
    # Leave the retained-current-revision caveat explicit.
    note = artifact["notes"]["observation_schema"].lower()
    assert "does not prove" in note


def test_action_map_and_mask_are_derived_from_the_core_contract(tmp_path):
    artifact = _export_core(tmp_path / "ckpt.bin", tmp_path / "p.json", HID, 1,
                            ACTIONS)
    head = artifact["action_map"]["head0"]
    combat, shop, level_up = head["combat"], head["shop"], head["level_up"]
    assert combat["0"] == "hold" and combat["8"] == "north_west"
    # Everything past the movement block is masked off in combat, never a
    # diagonal clamp and never a shop action.
    assert combat["9-30"] == "masked_in_combat"
    assert shop == {
        "0-3": "buy_offer", "4": "reroll", "5-7": "masked_in_shop",
        "8": "skip", "9-14": "recycle_weapon_slot",
        "15-20": "combine_weapon_slot", "21-24": "lock_offer",
        "25-28": "ban_offer", "29-30": "masked_in_shop",
    }
    assert level_up == {
        "0-3": "choose_offer", "4": "reroll_offers",
        "5-28": "masked_in_level_up", "29": "take_crate_item",
        "30": "recycle_crate_item",
    }
    assert "mingru" in artifact["notes"]
    layout = _layout(OBS, HID, 1, ACTIONS)
    assert artifact["source_checkpoint"]["floats"] == \
        export.trainer_checkpoint_floats(layout)
    assert len(artifact["source_checkpoint"]["sha256"]) == 64


def test_action_count_and_mask_match_the_live_core(tmp_path):
    # The exported action space must be the live core's BT_ACTIONS (the v5
    # contract: 203 obs / 31 actions).
    assert OBS == 203
    assert ACTIONS == 31
    bounds = CORE.constants["action_bounds"]
    assert bounds == {
        "BT_ACT_MOVE_COUNT": 9, "BT_ACT_BUY0": 0, "BT_ACT_REROLL": 4,
        "BT_ACT_SKIP": 8, "BT_ACT_RECYCLE0": 9, "BT_ACT_COMBINE0": 15,
        "BT_ACT_LOCK0": 21, "BT_ACT_BAN0": 25, "BT_ACT_CRATE_TAKE": 29,
        "BT_ACT_CRATE_RECYCLE": 30, "move_count": 9,
        # named by the lock/ban conditionals
        "BT_SHOP_NONE": 0, "BT_SHOP_ITEM": 1,
    }
    artifact = _export_core(tmp_path / "ckpt.bin", tmp_path / "p.json", HID, 1,
                            ACTIONS)
    mask = artifact["action_mask"]
    assert mask["width"] == ACTIONS
    assert mask["masked_logit"] == export.MASKED_LOGIT == -1e4
    assert "bt_action_legal" in mask["source"]
    # bt_action_legal (brotato_core.h; bt_action_mask delegates to it): phase 0
    # movement only, phase 1 buy/reroll/go/recycle/combine, phase 2 offers/reroll,
    # and phase 2 with an open item box the take/recycle pair ONLY.  Lock/ban are
    # state-dependent and live in `conditionals`.
    assert mask["legal_by_phase"] == {
        "0_combat": [[0, 8]], "1_shop": [[0, 4], [8, 20]],
        "2_level_up": [[0, 4]], "2_crate": [[29, 30]],
    }
    # Each conditional is clause-scoped and AND-combines its `tests`.  Lock
    # id 21+i needs a live offer in slot i; ban id 25+i needs an ITEM offer
    # (shop_item.gd:106-109: weapons are never bannable) AND a ban token left;
    # level-up id i needs the presence flag; the level-up reroll needs two
    # offers and enough gold.  All indices are the schema's own.
    assert mask["conditionals"] == [
        {"clause": "1_shop", "id_base": 0, "count": 4,
         "tests": [{"op": "nonzero", "field_base": 145, "field_stride": 4}]},
        {"clause": "1_shop", "id_base": 4, "count": 1,
         "tests": [{"op": "nonzero", "field_base": 160, "field_stride": 0}]},
        {"clause": "1_shop", "id_base": 9, "count": 6,
         "tests": [{"op": "nonzero", "field_base": 79, "field_stride": 8}]},
        {"clause": "1_shop", "id_base": 15, "count": 6,
         "tests": [{"op": "block_duplicate", "field_base": 79,
                    "field_stride": 8, "count": 6}]},
        {"clause": "1_shop", "id_base": 21, "count": 4,
         "tests": [{"op": "nonzero", "field_base": 127, "field_stride": 3}]},
        {"clause": "1_shop", "id_base": 25, "count": 4,
         "tests": [{"op": "equals", "value": 1, "field_base": 127,
                    "field_stride": 3},
                   {"op": "nonzero", "field_base": 198, "field_stride": 0}]},
        {"clause": "2_level_up", "id_base": 0, "count": 4,
         "tests": [{"op": "nonzero", "field_base": 199, "field_stride": 1}]},
        {"clause": "2_level_up", "id_base": 4, "count": 1,
         "tests": [{"op": "block_count_gte", "value": 2, "field_base": 199,
                    "field_stride": 1, "count": 4},
                   {"op": "ge", "field": 8, "other": 173, "scale": 0.01}]},
    ]
    assert artifact["observation_schema"]["constants"]["shop_offer_kind_field"] == 127
    assert artifact["observation_schema"]["constants"]["shop_offer_kind_stride"] == 3
    constants = artifact["observation_schema"]["constants"]
    assert constants["ban_field"] == 198
    assert constants["level_up_present_field"] == 199
    assert constants["level_up_present_count"] == 4
    assert constants["gold_field"] == 8
    assert constants["lvl_reroll_price_field"] == 173
    # The ban tokens and the level-up presence are now OBSERVED: the only
    # remaining unobservable condition is bt_weapon_can_combine's upgrade path.
    assert "ban_tokens" not in mask["unobservable_conditions"]
    assert "level-up id as legal when its slot is not the empty encoding" not in \
        mask["unobservable_conditions"]
    assert "bt_weapon_can_combine" in mask["unobservable_conditions"]
    # The clause a consumer needs is selected from the observation, and the
    # indices are the schema's own, never hard-coded in a consumer.
    assert mask["phase_field_index"] == artifact["observation_schema"]["fields"].index("phase") == 7
    assert mask["selectors"] == [{
        "phase": 2, "use": "2_crate",
        "when_nonzero": {"field": "crate.offer_kind", "index": 169},
    }]
    assert artifact["observation_schema"]["constants"]["crate_field_index"] == 169
    # Every action id is covered by the emitted rules: ranges plus conditionals.
    covered = set()
    for spans in mask["legal_by_phase"].values():
        for first, last in spans:
            covered.update(range(first, last + 1))
    for cond in mask["conditionals"]:
        covered.update(range(cond["id_base"], cond["id_base"] + cond["count"]))
    assert covered == set(range(ACTIONS))

    # Every action id is named in every phase: no gap the policy could pick.
    for phase in ("combat", "shop", "level_up"):
        named = set()
        for key in artifact["action_map"]["head0"][phase]:
            if "-" in key:
                first, last = (int(v) for v in key.split("-"))
            else:
                first = last = int(key)
            named.update(range(first, last + 1))
        assert named == set(range(ACTIONS)), phase


# ---------------------------------------------------------------------------
# the native trainer's own checkpoint format
# ---------------------------------------------------------------------------

def test_trainer_checkpoint_layout_is_derived_from_the_native_sources():
    """The dump is the registered tensors concatenated with no padding
    (puf_save_weights walks alloc->regs, src/pufferl.cu), so the file is
    independent of the device image's 16-byte alignment."""
    layout = _layout(OBS, 256, 2, ACTIONS)
    offsets = export.trainer_tensor_offsets(layout)
    # Pins for the trained arch (203 obs / 31 actions / hidden 256 / 2 layers):
    # 453376 float32 = 1813504 bytes, packed.
    assert offsets == [("encoder.weight", 0), ("decoder.weight", 207872),
                       ("mingru.0.weight", 240640), ("mingru.1.weight", 1027072)]
    assert export.trainer_checkpoint_floats(layout) == 453376
    # Every tensor of this arch is a multiple of 8 float32, so the packed file
    # happens to equal the legacy aligned image as well.
    assert export.layout_floats(layout) == 453376

    # Any shape packs: no padding anywhere in the file.
    odd = _layout(OBS, 2, 2, ACTIONS)          # encoder = 406 float32
    assert export.trainer_tensor_offsets(odd) == [
        ("encoder.weight", 0), ("decoder.weight", 1624),
        ("mingru.0.weight", 1880), ("mingru.1.weight", 1928)]
    assert export.trainer_checkpoint_floats(odd) == 494


def test_export_accepts_the_trainer_format_for_the_trained_arch(tmp_path):
    """203 obs / 31 actions / hidden 256 / 2 layers (config/brotato.ini), written
    by the trainer's own writer, exports with every tensor byte-exact."""
    hidden, layers = 256, 2
    layout = _layout(OBS, hidden, layers, ACTIONS)
    checkpoint = tmp_path / "ckpt.bin"
    _write_checkpoint(checkpoint, layout, _good_fill)
    assert checkpoint.stat().st_size == export.trainer_checkpoint_floats(layout) * 4

    artifact = export.export(str(checkpoint), str(tmp_path / "policy.json"),
                             None, hidden, layers, ACTIONS)
    assert artifact["layout"]["obs_size"] == OBS == 203
    assert artifact["layout"]["actions"] == ACTIONS == 31
    assert artifact["observation_schema"]["matches_layout"] is True
    by_name = {t["name"]: t for t in artifact["tensors"]}
    for name, _shape, numel in layout:
        assert _decode(by_name[name]) == tuple(_good_fill(name, i)
                                               for i in range(numel))
    assert artifact["source_checkpoint"]["floats"] == 453376
    assert artifact["source_checkpoint"]["size_bytes"] == 1813504


def test_packed_checkpoint_with_unaligned_tensors_exports(tmp_path):
    """A packed checkpoint is unambiguous for any shape: a layout whose tensors
    are not 8-float blocks (which the old padded writer could not represent)
    must export byte-exact, not be refused."""
    layout = _layout(OBS, 2, 2, ACTIONS)       # encoder = 406 float32
    checkpoint = tmp_path / "ckpt.bin"
    _write_checkpoint(checkpoint, layout, _good_fill)
    assert checkpoint.stat().st_size == export.trainer_checkpoint_floats(layout) * 4

    artifact = export.export(str(checkpoint), str(tmp_path / "policy.json"),
                             OBS, 2, 2, ACTIONS)
    by_name = {t["name"]: t for t in artifact["tensors"]}
    for name, _shape, numel in layout:
        assert _decode(by_name[name]) == tuple(_good_fill(name, i)
                                               for i in range(numel))
    assert artifact["source_checkpoint"]["floats"] == 494
    assert artifact["source_checkpoint"]["packed"] is True

    # The native CPU reader (src/puffercpu.c, embedded by the probe) is packed
    # too, so it must read this unaligned file exactly: same tensors, same
    # forward as the artifact's reference.
    rng = random.Random(20261110)
    obs = [[_good_fill("obs", i) for i in range(OBS)]]
    obs_path = tmp_path / "obs.f32"
    obs_path.write_bytes(struct.pack("<%df" % OBS, *obs[0]))
    out_path = tmp_path / "native_out.f32"
    probe = _compile_native_probe(tmp_path)
    proc = subprocess.run([str(probe), str(checkpoint), str(obs_path),
                           str(out_path), "1", str(OBS), "2", "2", str(ACTIONS)],
                          capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    got = struct.unpack("<%df" % (ACTIONS + 1), out_path.read_bytes())
    step = reference.Reference(artifact).step(obs[0])
    for a in range(ACTIONS):
        assert got[a] == pytest.approx(step["logits"][a], rel=1e-4, abs=1e-3)
    assert got[ACTIONS] == pytest.approx(step["value"], rel=1e-4, abs=1e-3)
    assert max(range(ACTIONS), key=lambda a: got[a]) == step["action"]


def _compile_native_probe(tmp_path: Path) -> Path:
    cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if cc is None:
        pytest.skip("no C compiler available for the native forward probe")
    binary = tmp_path / "native_forward_probe"
    proc = subprocess.run([cc, "-O2", "-o", str(binary),
                           str(_POLICY_DIR / "native_forward_probe.c"), "-lm"],
                          capture_output=True, text=True)
    if proc.returncode != 0:
        pytest.skip("native forward probe did not build: %s" % proc.stderr[-400:])
    return binary


def test_native_cpu_forward_matches_the_exported_artifact(tmp_path):
    """The trainer's own inference code (src/puffercpu.c ``PufferNet``, the CPU
    implementation of the same Arch as src/algo.cu ``arch_forward``, used by
    ``./build.sh brotato --cpu``) run on the checkpoint must reproduce the
    artifact's forward pass: every decoded tensor, the encoder/decoder linear
    maps, the MinGRU recurrence and the fused value column."""
    hidden, layers, seq = 256, 2, 4
    layout = _layout(OBS, hidden, layers, ACTIONS)
    checkpoint = tmp_path / "ckpt.bin"
    rng = random.Random(20261107)
    _write_checkpoint(checkpoint, layout,
                      lambda name, i: reference.f32(rng.uniform(-0.5, 0.5)))
    artifact = export.export(str(checkpoint), str(tmp_path / "policy.json"),
                             OBS, hidden, layers, ACTIONS)

    obs = [[reference.f32(rng.uniform(-1.0, 1.0)) for _ in range(OBS)]
           for _ in range(seq)]
    obs_path = tmp_path / "obs.f32"
    obs_path.write_bytes(struct.pack("<%df" % (seq * OBS),
                                     *[v for row in obs for v in row]))
    out_path = tmp_path / "native_out.f32"
    probe = _compile_native_probe(tmp_path)
    proc = subprocess.run([str(probe), str(checkpoint), str(obs_path),
                           str(out_path), str(seq), str(OBS), str(hidden),
                           str(layers), str(ACTIONS)],
                          capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    floats = export.trainer_checkpoint_floats(layout)
    assert "%d consumed, %d in file" % (floats, floats) in proc.stderr
    got = struct.unpack("<%df" % (seq * (ACTIONS + 1)), out_path.read_bytes())

    ref = reference.Reference(artifact)
    # The native CPU mirror accumulates every dot product in float32 while the
    # artifact's reference accumulates in double and rounds on store, so the
    # residual is float32 summation order over 203/256-term dots (measured
    # 1.9e-4 absolute on this arch); it is not a tolerance on the mapping.
    rtol, atol = 1e-4, 1e-3
    for t in range(seq):
        expected = ref.step(obs[t])
        row = got[t * (ACTIONS + 1):(t + 1) * (ACTIONS + 1)]
        for a in range(ACTIONS):
            assert row[a] == pytest.approx(expected["logits"][a], rel=rtol, abs=atol)
        assert row[ACTIONS] == pytest.approx(expected["value"], rel=rtol, abs=atol)
        assert max(range(ACTIONS), key=lambda a: row[a]) == expected["action"]


def _nvcc_build(probe_src: Path, binary: Path) -> bool:
    """Build a harness with the trainer's own toolchain flags (mirrors build.sh
    native mode for brotato). Returns False when the toolchain is unusable."""
    nvcc = shutil.which("nvcc") or "/opt/cuda/bin/nvcc"
    if not Path(nvcc).exists():
        return False
    cuda_home = os.environ.get("CUDA_HOME") or str(Path(nvcc).parent.parent)
    raylib = Path(__file__).resolve().parents[1] / "raylib-5.5_linux_amd64" / "lib" / "libraylib.a"
    cmd = []
    if shutil.which("ccache"):
        cmd.append("ccache")
    cmd += [nvcc, "-O2", "--threads", "0", "-arch=native", "-std=c++17",
            "-DPRECISION_FLOAT", "-DPUFFER_BROTATO",
            "-I.", "-Isrc", "-Iocean/brotato", "-Ivendor",
            "-I" + str(raylib.parent.parent / "include"),
            "-I" + cuda_home + "/include", "-I" + cuda_home + "/include/cccl",
            "-Xcompiler=-DPLATFORM_DESKTOP", "-Xcompiler=-fopenmp",
            "-Xcompiler=-Wno-narrowing", "-Xcompiler=-Wno-unreachable-code",
            "--diag-suppress=2361", "--diag-suppress=111", "--diag-suppress=128",
            "--fmad=false", "-Xcompiler=-ffp-contract=off",
            str(probe_src), str(raylib), "-L" + cuda_home + "/lib64",
            "-lcudart", "-lnccl", "-lnvidia-ml", "-lcublas", "-lcusolver",
            "-lcurand", "-lm", "-Xlinker=-lpthread", "-lomp5", "-lGL",
            "-o", str(binary)]
    # Host rule: one CUDA compile at a time (the box runs many agents).
    lock = Path(__file__).resolve().parents[1] / "build" / ".nvcc.lock"
    if shutil.which("flock") and lock.parent.is_dir():
        lock.touch(exist_ok=True)
        cmd = ["flock", str(lock)] + cmd
    proc = subprocess.run(cmd, cwd=str(Path(__file__).resolve().parents[1]),
                          capture_output=True, text=True)
    if proc.returncode != 0:
        print(proc.stderr[-2000:])
    return proc.returncode == 0


def _native_cuda_probe():
    """The compiled native CUDA forward probe, or None when it cannot be used.

    Returns None (never raises/skips) so a caller that has other assertions can
    still run them: the probe is a parity check, not a capacity test, and this
    box is shared with other agents.
    """
    probe_src = _POLICY_DIR / "native_forward_probe.cu"
    binary = Path("/tmp/omp_brotato_policy_native_forward_probe")
    if (not binary.exists()
            or binary.stat().st_mtime < probe_src.stat().st_mtime):
        # Build aside and publish atomically: two test runs must never see a
        # half-written binary.
        staging = binary.with_name(binary.name + ".%d" % os.getpid())
        if not _nvcc_build(probe_src, staging):
            return None
        os.replace(staging, binary)
    if shutil.which("nvidia-smi"):
        if subprocess.run(["nvidia-smi", "-L"], capture_output=True).returncode != 0:
            return None
        free = subprocess.run(
            ["nvidia-smi", "--query-gpu=memory.free", "--format=csv,noheader,nounits"],
            capture_output=True, text=True)
        if free.returncode == 0:
            try:
                free_mib = min(int(line) for line in free.stdout.split())
            except ValueError:
                free_mib = None
            if free_mib is not None and free_mib < 2048:
                return None
    return binary


def test_native_cuda_forward_matches_the_exported_artifact(tmp_path):
    """The trainer's own CUDA forward (src/pufferl.cu ``arch_forward`` ->
    src/algo.cu encoder_forward / puf_mm / mingru_forward / decoder_forward)
    loaded with the trainer's own ``puf_load_weights_into`` must reproduce the
    artifact's forward pass on the checkpoint the exporter read."""
    hidden, layers, seq = 256, 2, 4
    layout = _layout(OBS, hidden, layers, ACTIONS)
    checkpoint = tmp_path / "ckpt.bin"
    rng = random.Random(20261108)
    _write_checkpoint(checkpoint, layout,
                      lambda name, i: reference.f32(rng.uniform(-0.5, 0.5)))
    artifact = export.export(str(checkpoint), str(tmp_path / "policy.json"),
                             OBS, hidden, layers, ACTIONS)

    obs = [[reference.f32(rng.uniform(-1.0, 1.0)) for _ in range(OBS)]
           for _ in range(seq)]
    obs_path = tmp_path / "obs.f32"
    obs_path.write_bytes(struct.pack("<%df" % (seq * OBS),
                                     *[v for row in obs for v in row]))
    out_path = tmp_path / "native_cuda_out.f32"
    probe = _native_cuda_probe()
    if probe is None:
        pytest.skip("native CUDA forward probe unavailable (no nvcc/GPU/VRAM)")
    proc = subprocess.run([str(probe), str(checkpoint), str(obs_path),
                           str(out_path), str(seq), str(OBS), str(hidden),
                           str(layers), str(ACTIONS)],
                          capture_output=True, text=True)
    if "cudaMalloc failed" in proc.stderr or "cudaErrorMemoryAllocation" in proc.stderr:
        pytest.skip("CUDA allocation failed while running the native parity "
                    "probe (device busy): %s" % proc.stderr.strip()[-200:])
    assert proc.returncode == 0, proc.stderr
    got = struct.unpack("<%df" % (seq * (ACTIONS + 1)), out_path.read_bytes())

    ref = reference.Reference(artifact)
    # cuBLAS accumulates fp32 GEMMs in a different order than the artifact's
    # reference (double accumulation rounded to float32 on store); measured
    # 1.3e-4 absolute on this arch, the same class of residual as the CPU mirror.
    rtol, atol = 1e-4, 1e-3
    for t in range(seq):
        expected = ref.step(obs[t])
        row = got[t * (ACTIONS + 1):(t + 1) * (ACTIONS + 1)]
        for a in range(ACTIONS):
            assert row[a] == pytest.approx(expected["logits"][a], rel=rtol, abs=atol)
        assert row[ACTIONS] == pytest.approx(expected["value"], rel=rtol, abs=atol)
        assert max(range(ACTIONS), key=lambda a: row[a]) == expected["action"]


def test_action_mask_is_applied_exactly_like_the_trainer(tmp_path):
    """A masked action must never be picked, and the pick must be the same one
    the native masked argmax makes (`multidiscrete(hard=1)`, src/puffercpu.c:199-230;
    the trainer's own rule, masked logits -> -1e4, src/algo.cu:1364-1371)."""
    hidden, layers, seq = 256, 2, 4
    layout = _layout(OBS, hidden, layers, ACTIONS)
    checkpoint = tmp_path / "ckpt.bin"
    rng = random.Random(20261109)
    _write_checkpoint(checkpoint, layout,
                      lambda name, i: reference.f32(rng.uniform(-0.5, 0.5)))
    artifact = export.export(str(checkpoint), str(tmp_path / "policy.json"),
                             OBS, hidden, layers, ACTIONS)

    obs = [[reference.f32(rng.uniform(-1.0, 1.0)) for _ in range(OBS)]
           for _ in range(seq)]
    # Build the masks so they provably bite: step 0 masks off the action the
    # unmasked forward would take (plus a few others), so a forward that ignored
    # the mask cannot pass this test.
    plain_probe = reference.Reference(artifact)
    masks = []
    for t in range(seq):
        mask = [1] * ACTIONS
        for a in rng.sample(range(ACTIONS), 7):
            mask[a] = 0
        masks.append(mask)
    first = plain_probe.step(obs[0])
    masks[0][first["action"]] = 0
    masks[0][(first["action"] + 1) % ACTIONS] = 1
    obs_path = tmp_path / "obs.f32"
    obs_path.write_bytes(struct.pack("<%df" % (seq * OBS),
                                     *[v for row in obs for v in row]))
    mask_path = tmp_path / "mask.u8"
    mask_path.write_bytes(bytes(v for row in masks for v in row))
    out_path = tmp_path / "native_out.f32"
    argmax_path = tmp_path / "native_argmax.f32"
    probe = _compile_native_probe(tmp_path)
    proc = subprocess.run([str(probe), str(checkpoint), str(obs_path),
                           str(out_path), str(seq), str(OBS), str(hidden),
                           str(layers), str(ACTIONS), str(mask_path),
                           str(argmax_path)],
                          capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    native_actions = struct.unpack("<%df" % seq, argmax_path.read_bytes())
    got = struct.unpack("<%df" % (seq * (ACTIONS + 1)), out_path.read_bytes())

    plain = reference.Reference(artifact)
    masked = reference.Reference(artifact)
    changed = 0
    for t in range(seq):
        unmasked = plain.step(obs[t])
        step = masked.step(obs[t], masks[t])
        # The mask never touches the recurrence or the raw logits.
        assert step["logits"] == unmasked["logits"]
        assert step["value"] == unmasked["value"]
        for a in range(ACTIONS):
            expected = (unmasked["logits"][a] if masks[t][a]
                        else export.MASKED_LOGIT)
            assert step["masked_logits"][a] == expected
        # The pick is the native one and is always legal.
        assert int(native_actions[t]) == step["action"]
        assert masks[t][step["action"]] == 1
        row = got[t * (ACTIONS + 1):(t + 1) * (ACTIONS + 1)]
        for a in range(ACTIONS):
            assert row[a] == pytest.approx(step["logits"][a], rel=1e-4, abs=1e-3)
        if step["action"] != unmasked["action"]:
            changed += 1
    # The fixture is only meaningful if the mask actually changed a decision.
    assert changed >= 1


def test_reference_rejects_a_wrong_length_mask(tmp_path):
    artifact = _export_core(tmp_path / "ckpt.bin", tmp_path / "p.json", HID, 1,
                            ACTIONS)
    obs = [0.0] * OBS
    with pytest.raises(ValueError):
        reference.Reference(artifact).step(obs, [1] * (ACTIONS - 1))
    step = reference.Reference(artifact).step(obs, [1] * ACTIONS)
    assert step["action"] < ACTIONS
    # An all-legal mask is the same as no mask at all (fresh state each time).
    assert (reference.Reference(artifact).step(obs)
            == reference.Reference(artifact).step(obs, [1] * ACTIONS))


def test_pinned_obs_size_disagreement_is_refused(tmp_path):
    """brotato_state.h carries a literal BT_OBS_SIZE_PINNED for consumers that
    cannot evaluate the composed expression; a stale pin must refuse the export
    rather than label an artifact with a width the engine does not use."""
    assert CORE.constants["obs_size_pinned"] == CORE.obs_size == 203

    core_dir = tmp_path / "core"
    core_dir.mkdir()
    for header in export.CORE_HEADERS:
        (core_dir / header).write_bytes(
            (export.DEFAULT_CORE_DIR / header).read_bytes())
    state = core_dir / "brotato_state.h"
    state.write_text(state.read_text().replace("BT_OBS_SIZE_PINNED 203",
                                               "BT_OBS_SIZE_PINNED 204"))
    with pytest.raises(export.ExportError, match="BT_OBS_SIZE_PINNED"):
        export.read_core_schema(str(core_dir))


# ---------------------------------------------------------------------------
# GDScript forward (the code the game runs) against the same reference
# ---------------------------------------------------------------------------

_GODOT3_CANDIDATES = (
    "BROTATO_GODOT3",
    "~/tools/bin/godot-3.7-dev1",
    "~/tools/godot-src/bin/godot.x11.opt.tools.64",
    "~/tools/godot-src/bin/godot.x11.opt.64",
)


def _godot3() -> Path:
    for candidate in _GODOT3_CANDIDATES:
        value = os.environ.get(candidate) if candidate.isupper() else candidate
        if not value:
            continue
        path = Path(os.path.expanduser(value))
        if path.exists():
            return path
    pytest.skip("no Godot 3.x engine (set BROTATO_GODOT3 to the pinned 3.7-dev1 "
                "binary); the GDScript forward cannot be exercised here")


def test_godot_forward_matches_the_reference(tmp_path):
    """The GDScript the game loads (brotato_policy.gd) must reproduce the
    reference forward through validate_forward.gd: raw logits, the fused value,
    the masked logits and the picked action (including the mask steps the
    fixture carries)."""
    engine = _godot3()
    project = tmp_path / "proj"
    project.mkdir()
    for name in ("brotato_policy.gd", "validate_forward.gd"):
        shutil.copy(_POLICY_DIR / name, project / name)
    (project / "project.godot").write_text(
        'config_version=4\n\n[application]\n\nconfig/name="policy_validate"\n')

    run_dir = tmp_path / "run"
    paths = reference.make_fixture(str(run_dir), None, 256, 2, None, seed=4321)
    godot_out = run_dir / "godot_out.json"
    script = ["%s" % engine, "--path", str(project), "--no-window",
              "--disable-audio", "--fixed-fps", "60",
              "--script", "res://validate_forward.gd", "--",
              "--artifact=%s" % paths["artifact"],
              "--fixture=%s" % paths["fixture"], "--out=%s" % godot_out]
    env = dict(os.environ)
    proc = subprocess.run(script, capture_output=True, text=True, env=env,
                          timeout=300)
    if "X11 Display is not available" in (proc.stdout + proc.stderr):
        # X11-only engine build: run it on a virtual display (the shape
        # LinuxGame uses for the pinned engine on this host).
        if shutil.which("xvfb-run"):
            proc = subprocess.run(["xvfb-run", "-a",
                                   "-s", "-screen 0 1280x720x24"] + script,
                                  capture_output=True, text=True, env=env,
                                  timeout=300)
        else:
            pytest.skip("Godot 3.x engine is X11-only and no DISPLAY/xvfb is "
                        "available")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert godot_out.exists(), proc.stdout + proc.stderr

    with open(paths["fixture"]) as handle:
        fixture = json.load(handle)
    with open(godot_out) as handle:
        produced = json.load(handle)
    problems = reference.compare(fixture, produced, 1e-5, 1e-4)
    assert not problems, "\n".join(problems)
    # The fixture must have exercised the mask and the guard rails.
    assert any("mask" in seq for seq in fixture["sequences"])
    steps = sum(len(seq["expected"]) for seq in fixture["sequences"])
    assert steps == 9


def test_export_reads_a_checkpoint_written_by_the_trainer(tmp_path):
    """A checkpoint the running trainer actually wrote must load: its size is the
    packed tensor sum for its layout, and every tensor must decode.  Uses
    BROTATO_TRAINER_CHECKPOINT when set, else the newest .bin under
    checkpoints/brotato/, else skips; files written by an older format simply do
    not match a live layout and are skipped rather than treated as failures."""
    wanted = []
    for hidden, layers in ((256, 2), (16, 2), (8, 2), (8, 1), (63, 1)):
        layout = _layout(OBS, hidden, layers, ACTIONS)
        wanted.append((hidden, layers,
                       export.trainer_checkpoint_floats(layout) * 4))
    by_size = dict((size, (hidden, layers)) for hidden, layers, size in wanted)

    candidates = []
    if os.environ.get("BROTATO_TRAINER_CHECKPOINT"):
        candidates.append(Path(os.environ["BROTATO_TRAINER_CHECKPOINT"]))
    ckpt_root = Path(__file__).resolve().parents[1] / "checkpoints" / "brotato"
    if ckpt_root.is_dir():
        candidates += sorted(ckpt_root.glob("*/*.bin"),
                             key=lambda path: path.stat().st_mtime, reverse=True)
    match = next((path for path in candidates if path.is_file()
                  and path.stat().st_size in by_size), None)
    if match is None:
        pytest.skip("no checkpoint written by the current trainer found; set "
                    "BROTATO_TRAINER_CHECKPOINT to a puf_save_weights dump "
                    "(expected sizes: %s)" % sorted(by_size))

    hidden, layers = by_size[match.stat().st_size]
    artifact = export.export(str(match), str(tmp_path / "policy.json"), None,
                             hidden, layers, ACTIONS)
    assert artifact["source_checkpoint"]["size_bytes"] == match.stat().st_size
    assert artifact["source_checkpoint"]["packed"] is True
    assert artifact["source_checkpoint"]["floats"] == match.stat().st_size // 4
    assert len(artifact["tensors"]) == 2 + layers
    by_name = {t["name"]: t for t in artifact["tensors"]}
    assert by_name["encoder.weight"]["shape"] == [hidden, OBS]
    assert by_name["decoder.weight"]["shape"] == [ACTIONS + 1, hidden]

    # Strongest available check on a real file: the trainer's own CUDA forward
    # (arch_forward, loading the file with puf_load_weights_into) must reproduce
    # the artifact's reference, which is what proves the tensor mapping the
    # exporter read is the mapping the trainer trained.
    probe = _native_cuda_probe()
    if probe is None:
        return          # GPU busy / no toolchain: the structural checks above stand
    rng = random.Random(20261111)
    obs = [[reference.f32(rng.uniform(-1.0, 1.0)) for _ in range(OBS)]]
    obs_path = tmp_path / "obs.f32"
    obs_path.write_bytes(struct.pack("<%df" % OBS, *obs[0]))
    out_path = tmp_path / "native_out.f32"
    proc = subprocess.run([str(probe), str(match), str(obs_path), str(out_path),
                           "1", str(OBS), str(hidden), str(layers), str(ACTIONS)],
                          capture_output=True, text=True)
    assert proc.returncode == 0, proc.stderr
    got = struct.unpack("<%df" % (ACTIONS + 1), out_path.read_bytes())
    step = reference.Reference(artifact).step(obs[0])
    for a in range(ACTIONS):
        assert got[a] == pytest.approx(step["logits"][a], rel=1e-4, abs=1e-3)
    assert got[ACTIONS] == pytest.approx(step["value"], rel=1e-4, abs=1e-3)
    assert max(range(ACTIONS), key=lambda a: got[a]) == step["action"]


# ---------------------------------------------------------------------------
# Demon-first: the live game's own observations
# ---------------------------------------------------------------------------

_DEMON_CHARACTER_INDEX = 2        # brotato_characters.h:110-128 (bull, knight, demon)


def _demon_trace_rows(limit=None):
    """Rows of a real v5 Demon trace, or None when none is available.

    Only a full_v5 trace qualifies: a v4 (198-float) recording is an older
    contract, and the policy export rejects it, so it is skipped here rather
    than validated against the 203-wide schema.  A v5 run reached the level-up
    phase only after a component (and only the game emits phase 2 there); the
    caller still asserts the Demon profile field itself.
    """
    candidates = []
    if os.environ.get("BROTATO_DEMON_TRACE"):
        candidates.append(Path(os.path.expanduser(os.environ["BROTATO_DEMON_TRACE"])))
    trace_dir = Path.home() / "brotato_run" / "rl_out"
    if trace_dir.is_dir():
        candidates += sorted(trace_dir.glob("rl_trace.*demon*.jsonl"),
                             key=lambda path: path.stat().st_mtime, reverse=True)
    for path in candidates:
        if not path.is_file():
            continue
        rows = []
        with open(path) as handle:
            for line in handle:
                if line.strip():
                    rows.append(json.loads(line))
                if limit is not None and len(rows) >= limit:
                    break
        if rows and rows[0].get("contract") == "full_v5" and rows[0].get("obs_version") == 5:
            return path, rows
    return None


def test_demon_trace_observations_drive_the_exported_policy(tmp_path):
    """The game's own Demon observations must be consumable by the exported
    policy, and every action it picks must be legal under the artifact's mask.

    Uses the live trace (BROTATO_DEMON_TRACE or ~/brotato_run/rl_out/
    rl_trace.*demon*.jsonl, full_v5 only); skips when no v5 trace is present.
    """
    found = _demon_trace_rows(limit=64)
    if found is None:
        pytest.skip("no v4 Demon trace found (set BROTATO_DEMON_TRACE)")
    trace_path, rows = found
    obs_seq = [row["obs"] for row in rows if row.get("obs")]

    # Every observation is exactly the width this artifact declares, and the run
    # really is Demon: the core writes (character + 1) / (BT_NCHARACTERS + 1)
    # (brotato_weapon_observe.h:170) with demon at index 2
    # (brotato_characters.h:123).
    assert CORE.constants["characters"] == 3
    expected_id = (float(_DEMON_CHARACTER_INDEX) + 1.0) / 4.0
    for obs in obs_seq:
        assert len(obs) == OBS == 203
        assert obs[139] == pytest.approx(expected_id, abs=1e-6)
        assert int(obs[7]) in (0, 1, 2)
    assert any(int(obs[7]) == 0 for obs in obs_seq)     # combat rows exist

    # A Demon run's shop phase reports currency_is_hp (demon_effect_2): the field
    # the artifact labels `shop.currency_is_hp` is 1 there.
    shop_rows = [obs for obs in obs_seq if int(obs[7]) == 1]
    if shop_rows:
        assert all(obs[141] == 1.0 for obs in shop_rows)

    hidden, layers = 256, 2
    layout = _layout(OBS, hidden, layers, ACTIONS)
    checkpoint = tmp_path / "ckpt.bin"
    rng = random.Random(2718)
    _write_checkpoint(checkpoint, layout,
                      lambda name, i: reference.f32(rng.uniform(-0.5, 0.5)))
    artifact = export.export(str(checkpoint), str(tmp_path / "policy.json"),
                             OBS, hidden, layers, ACTIONS)

    # Reproduce what the in-game forward does with the artifact's own rules:
    # pick the clause for the observed phase (with the crate selector), build the
    # legality vector and hand it to the forward.
    mask_meta = artifact["action_mask"]
    phase_index = mask_meta["phase_field_index"]
    assert phase_index == 7
    ranges = {}
    for key, spans in mask_meta["legal_by_phase"].items():
        ranges[key] = [tuple(span) for span in spans]
    selectors = mask_meta["selectors"]

    def clause_for(obs):
        phase = int(obs[phase_index])
        key = "%d_%s" % (phase, {0: "combat", 1: "shop", 2: "level_up"}[phase])
        for selector in selectors:
            if selector["phase"] != phase:
                continue
            if obs[selector["when_nonzero"]["index"]] != 0:
                key = selector["use"] if selector["use"] in ranges else key
        return key

    def _test(t, obs, i):
        op = t["op"]
        if op == "ge":
            return obs[t["field"]] >= obs[t["other"]] * t["scale"]
        base, stride = t["field_base"], t["field_stride"]
        if op == "block_count_gte":
            return sum(1 for j in range(t["count"])
                       if obs[base + j * stride] != 0) >= t["value"]
        field = base + i * stride
        if op == "equals":
            return obs[field] == t["value"]
        if op == "block_duplicate":
            mine = obs[field]
            return mine != 0 and any(
                obs[base + j * stride] == mine
                for j in range(t["count"]) if j != i)
        return obs[field] != 0

    def legal_mask(obs):
        legal = [0] * ACTIONS
        for first, last in ranges[clause_for(obs)]:
            for a in range(first, last + 1):
                legal[a] = 1
        # State-dependent ids: each conditional is clause-scoped and its ids
        # are legal iff EVERY test passes, exactly what rl_policy.gd applies.
        key = clause_for(obs)
        for cond in mask_meta["conditionals"]:
            if cond["clause"] and cond["clause"] != key:
                continue
            for i in range(cond["count"]):
                action = cond["id_base"] + i
                if action < 0 or action >= ACTIONS:
                    continue
                legal[action] = 1 if all(_test(t, obs, i)
                                         for t in cond["tests"]) else 0
        return legal

    ref = reference.Reference(artifact)
    for obs in obs_seq:
        legal = legal_mask(obs)
        step = ref.step([reference.f32(v) for v in obs], legal)
        assert legal[step["action"]] == 1, "picked a masked-off action"
        if int(obs[phase_index]) == 0:
            assert step["action"] <= 8, "combat pick outside the movement block"
        # A no-mask forward may pick anything; the mask is what makes it legal,
        # which is exactly the contract the bridge implements.
        unmasked = reference.Reference(artifact).step([reference.f32(v) for v in obs])
        assert 0 <= unmasked["action"] < ACTIONS

    # The trainer's own forward on the very same Demon observations must agree
    # with the artifact's reference over the whole recurrent sequence.
    probe = _native_cuda_probe()
    if probe is not None:
        obs_path = tmp_path / "demon_obs.f32"
        obs_path.write_bytes(struct.pack("<%df" % (len(obs_seq) * OBS),
                                         *[reference.f32(v) for obs in obs_seq
                                           for v in obs]))
        out_path = tmp_path / "demon_out.f32"
        proc = subprocess.run([str(probe), str(checkpoint), str(obs_path),
                               str(out_path), str(len(obs_seq)), str(OBS),
                               str(hidden), str(layers), str(ACTIONS)],
                              capture_output=True, text=True)
        assert proc.returncode == 0, proc.stderr
        got = struct.unpack("<%df" % (len(obs_seq) * (ACTIONS + 1)),
                            out_path.read_bytes())
        ref = reference.Reference(artifact)
        agree = 0
        for t, obs in enumerate(obs_seq):
            step = ref.step([reference.f32(v) for v in obs])
            row = got[t * (ACTIONS + 1):(t + 1) * (ACTIONS + 1)]
            assert row[ACTIONS] == pytest.approx(step["value"], rel=1e-4, abs=1e-3)
            for a in range(ACTIONS):
                assert row[a] == pytest.approx(step["logits"][a], rel=1e-4,
                                               abs=1e-3)
            if max(range(ACTIONS), key=lambda a: row[a]) == step["action"]:
                agree += 1
        assert agree == len(obs_seq)
