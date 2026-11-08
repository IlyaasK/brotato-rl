#!/usr/bin/env python3
"""Export a trained PufferLib checkpoint into a portable, self-describing
artifact that the Brotato GDScript bridge can load and run.

The native trainer (``src/pufferl.cu``) is the producer.  ``weights_create``
(src/algo.cu:982-990) registers the policy parameters with
``encoder_reg_params`` -> ``decoder_reg_params`` -> ``mingru_reg_params``, and
``puf_save_weights`` (src/pufferl.cu:1722-1736) dumps
``numel(master_weights.shape)`` float32 values, where in float32 builds
``master_weights`` aliases the parameter allocation with
``shape = {params_alloc.total_elems}`` (``master_weights_setup``,
src/pufferl.cu:1766-1781 and the ``pol->param`` assignment at
src/pufferl.cu:2006-2009).  This tool reads exactly that layout, in exactly that
order, for the requested dimensions, and refuses any file that does not match.

Layout for the Brotato policy (linear encoder, MinGRU trunk, linear decoder):

    encoder.weight      [hidden, obs_size]       encoder_reg_params, algo.cu:601
    decoder.weight      [actions + 1, hidden]    decoder_reg_params, algo.cu:654
    decoder.logstd      [1, actions]             (only when --continuous)
    mingru.{0..L-1}.weight   [3 * hidden, hidden] mingru_reg_params, algo.cu:759

Weight matrices are stored row-major ``[out, in]`` and are applied as
``out = x @ W^T`` (``puf_mm``, src/algo.cu:137-146).  The decoder emits
``actions`` logits followed by a single scalar value in the last column
(``sample_logits`` reads ``logits[logits_base + fused_cols - 1]``,
src/pufferl.cu:702; ``assemble_decoder_grad`` fills the same split,
src/algo.cu:564-573).

Checkpoint packing: the file is the registered tensors concatenated in
registration order with NO padding, exactly ``params_alloc.total_elems`` float32
(``puf_save_weights`` / ``puf_load_weights_into`` walk ``alloc->regs``, src/
pufferl.cu), so the file is independent of the device image's alignment and any
shape is readable -- no alignment assumption is made here.  The device parameter
image is still 16-byte padded by ``alloc_create`` (src/pufferl.cu:257-266), and
the separate native CPU reader (``get_weights``, src/puffercpu.c:47-51) still
rounds each tensor to eight floats, so it only agrees with a packed checkpoint
when every tensor size is a whole number of 8-float blocks; ``native_reader_
matches`` reports that for consumers that run that reader.

The observation metadata is derived at export time from the *actual* core
headers (``brotato.h`` / ``brotato_core.h`` / ``brotato_state.h``): the divisors
come from the constants those headers define, and the artifact stamps their
sha256.  Nothing here hard-codes an old arena or enemy cap.

Observation width is 161 (79 core + 48 weapon + 12 shop + 2 profile + 20 shop action)
and the action space is 21 (0..8 movement, 9..14 recycle, 15..20 combine).
This is a clean cutover: a checkpoint whose encoder width is not the live core
schema is REJECTED.  There is no padding and no migration utility - an artifact
labelled with a different observation schema cannot exist here, so a stale
checkpoint can never be reused by resizing or zero-filling it.

Usage::

    # 1. train (native CUDA trainer; writes checkpoints/brotato/<run>/<step>.bin)
    ./build.sh brotato && ./puffer train --base.run_id=myrun

    # 2. export the artifact the game loads
    python3 ocean/brotato/policy/export_policy.py \
        --checkpoint checkpoints/brotato/myrun/<step>.bin \
        --output /tmp/brotato_policy.json \
        --hidden 256 --layers 2 --actions 21         # --obs-size defaults to core

    # 3. (optional) check the Godot forward against policy_reference.py:
    #    policy_reference.py fixture -> validate_forward.gd -> policy_reference.py check

The checkpoint values are float32 whichever precision the trainer ran at
(``master_weights`` is an fp32 buffer in bf16 builds too,
src/pufferl.cu:1766-1781).  ``--hidden`` must be a multiple of the 8-float
boundary for the trainer to dump and read it back consistently.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import re
import struct
import sys
from pathlib import Path

FORMAT = "pufferlib-brotato-policy"
FORMAT_VERSION = 1


class ExportError(Exception):
    """The checkpoint, dimensions, or core headers are unusable."""


# LEGACY (pre-packing) reader model: before the packer landed, checkpoints were
# the device image truncated at total_elems and the native CPU reader rounded
# every tensor start to eight floats (src/puffercpu.c ``get_weights`` and
# ``puf_align8``).  Current checkpoints are packed (no padding) and the reader is
# packed too, so nothing here depends on this; it is retained to describe and
# reason about old files, and it is what the artifact's ``alignment_floats``
# metadata refers to.
ALIGNMENT_FLOATS = 8
# Byte boundary the native trainer's parameter allocator packs at
# (``total_bytes = (total_bytes + 15) & ~15``, src/pufferl.cu:241).
TRAINER_ALIGNMENT_BYTES = 16
FLOAT_BYTES = 4  # precision_t == float in PRECISION_FLOAT builds (src/pufferl.cu:39-42)

# Core headers the observation schema is read from, next to this file
# (ocean/brotato/policy/ -> ocean/brotato/).
DEFAULT_CORE_DIR = Path(__file__).resolve().parents[1]
# Every header the observation *and action* contract is read from, so the sha256
# revision and the parsed constants cover the weapon/shop/profile extension.
CORE_HEADERS = ("brotato.h", "brotato_core.h", "brotato_state.h",
                "brotato_weapon_observe.h", "brotato_weapons.h",
                "brotato_characters.h", "brotato_weapon_inventory.h",
                "brotato_weapon_shop.h", "brotato_progression.h")

# Masked-off action logits.  The trainer substitutes -1e4 (not -inf) before the
# softmax (``load_logit_masked``, src/algo.cu:1364-1371), so in-game inference
# must substitute the same value before the argmax: a masked action can then
# never win and the all-masked degenerate case lands where the trainer's own
# argmax/sample would.
MASKED_LOGIT = -1e4

_MOVE_TABLE = re.compile(r"BT_MOVE_X\s*\[\s*BT_ACTIONS\s*\]\s*=\s*\{([^}]*)\}")
_MOVE_TABLE_Y = re.compile(r"BT_MOVE_Y\s*\[\s*BT_ACTIONS\s*\]\s*=\s*\{([^}]*)\}")
# Action contract.  ids and names live in the BT_ACT_* block
# (brotato_state.h:199-212); the per-phase legality rule is bt_action_mask
# (brotato_core.h:1459-1470); the movement table is BT_MOVE_X (brotato_core.h:148).
_MOVE_TABLE = re.compile(r"BT_MOVE_X\s*\[\s*BT_ACTIONS\s*\]\s*=\s*\{([^}]*)\}")
_MOVE_TABLE_Y = re.compile(r"BT_MOVE_Y\s*\[\s*BT_ACTIONS\s*\]\s*=\s*\{([^}]*)\}")
_GUIDE_ITEMS = re.compile(
    r"BT_OBS_GUIDE_ITEMS\s*\[\s*BT_OBS_T_ITEMS\s*\]\s*=\s*\{([^}]*)\}")
# bt_action_mask's four legality loops, as expressions that must still name the
# BT_ACT_* constants (count = how often each appears).
# bt_action_mask delegates every id to bt_action_legal, which is now the single
# source of truth (brotato_core.h: bt_action_legal) for both the CPU row and the
# lane-strided GPU writer.  The legality function's clauses must still name the
# boundary constants this export reports, and the ban branch must keep the game's
# own gate (item offer + ban_mode + a remaining token, shop_item.gd:106-119).
_MASK_DELEGATION = ("bt_action_legal(s, i)", 1)
_LEGAL_CLAUSES = (("id < BT_ACT_MOVE_COUNT", 1),
                  ("s->phase == 2", 1),
                  # phase 2 with an open item box: take/recycle are the ONLY legal
                  # ids (upgrades_ui.gd:88-115), so the crate branch must exist.
                  ("s->crate_kind != BT_SHOP_NONE", 1),
                  ("id == BT_ACT_CRATE_TAKE || id == BT_ACT_CRATE_RECYCLE", 1),
                  ("id == BT_ACT_REROLL", 2),
                  ("id == BT_ACT_SKIP", 1),
                  ("id >= BT_ACT_RECYCLE0 && id < BT_ACT_RECYCLE0 + 6", 1),
                  ("id >= BT_ACT_COMBINE0 && id < BT_ACT_COMBINE0 + 6", 1),
                  ("id >= BT_ACT_LOCK0 && id < BT_ACT_LOCK0 + 4", 1),
                  ("id >= BT_ACT_BAN0 && id < BT_ACT_BAN0 + 4", 1),
                  # the state-dependent rules this exporter hands to consumers:
                  # lock needs a live offer in the slot, ban needs an ITEM offer
                  # there (shop_item.gd:106-109: weapons are never bannable)
                  ("s->shop_kind[id - BT_ACT_LOCK0] != BT_SHOP_NONE", 1),
                  ("s->shop_kind[slot] != BT_SHOP_ITEM", 1),
                  ("s->ban_mode && s->ban_tokens > 0", 1))

_MOVE_NAMES = ("hold", "north", "north_east", "east", "south_east", "south",
               "south_west", "west", "north_west")


def _func_body(text: str, name: str) -> str:
    # Callers pass the DEFINITION's return type + name + "(BT_DEVICE", because the
    # header also forward-declares the helper early (and bt_action_legal's name
    # recurs at its call site inside bt_action_mask), so the FIRST occurrence of
    # a bare name is not the body: the last one with that signature is.
    start = text.rfind(name)
    if start < 0:
        return ""
    end = text.find("\n}", start)
    return text[start:end if end >= 0 else len(text)]


def _count_initializers(text: str, pattern, what: str) -> int:
    match = pattern.search(text)
    if match is None:
        raise ExportError(
            "cannot read %s from the core sources; the exported action map "
            "would be a guess, so this export is refused" % what)
    return len([v for v in match.group(1).split(",") if v.strip()])


def parse_guide_items(sources: dict) -> list:
    """Names of the curated guide items the v4 block reports, in table order."""
    match = _GUIDE_ITEMS.search(sources.get("brotato_core.h", ""))
    if match is None:
        raise ExportError("brotato_core.h does not define BT_OBS_GUIDE_ITEMS; "
                          "cannot name the guide-item observation fields")
    names = []
    for entry in match.group(1).split(","):
        entry = entry.strip()
        if not entry:
            continue
        if not entry.startswith("BT_ITEM_IDX_"):
            raise ExportError("BT_OBS_GUIDE_ITEMS contains %r, which is not an "
                              "owned-item index" % entry)
        names.append(entry.replace("BT_ITEM_IDX_", "").lower())
    return names


def parse_action_bounds(defined: dict, sources: dict) -> dict:
    """Read the live action contract out of the core sources.

    Ids/names come from the ``BT_ACT_*`` block (brotato_state.h:199-212), the
    phase legality rule from ``bt_action_mask`` (brotato_core.h:1459-1470) whose
    loops must still name those constants, and the movement table from
    ``BT_MOVE_X`` (brotato_core.h:148) which must agree with
    ``BT_ACT_MOVE_COUNT``.  Raises ExportError rather than describing a space the
    core does not implement.
    """
    wanted = ("BT_ACT_MOVE_COUNT", "BT_ACT_BUY0", "BT_ACT_REROLL", "BT_ACT_SKIP",
              "BT_ACT_RECYCLE0", "BT_ACT_COMBINE0", "BT_ACT_LOCK0", "BT_ACT_BAN0",
              "BT_ACT_CRATE_TAKE", "BT_ACT_CRATE_RECYCLE",
              # named by the lock/ban conditionals this exporter emits
              "BT_SHOP_NONE", "BT_SHOP_ITEM")
    absent = [name for name in wanted if name not in defined]
    if absent:
        raise ExportError(
            "the core headers do not define %s; the action contract changed "
            "and this export is refused" % ", ".join(absent))
    move = int(defined["BT_ACT_MOVE_COUNT"])
    core = sources.get("brotato_core.h", "")
    table_x = _count_initializers(core, _MOVE_TABLE, "the movement table")
    table_y = _count_initializers(core, _MOVE_TABLE_Y, "the movement table")
    if table_x != table_y or table_x != move:
        raise ExportError(
            "BT_MOVE_X has %d entries, BT_MOVE_Y %d, BT_ACT_MOVE_COUNT %d; the "
            "movement action table is inconsistent" % (table_x, table_y, move))
    mask_body = _func_body(core, "void bt_action_mask(BT_DEVICE")
    if not mask_body:
        raise ExportError("brotato_core.h does not define bt_action_mask; "
                          "cannot read the per-phase legality rule")
    legal_body = _func_body(core, "int bt_action_legal(BT_DEVICE")
    if not legal_body:
        raise ExportError("brotato_core.h does not define bt_action_legal; the "
                          "per-id legality rule cannot be read")
    for expr, count in (_MASK_DELEGATION,) + _LEGAL_CLAUSES:
        body = mask_body if expr == _MASK_DELEGATION[0] else legal_body
        if body.count(expr) != count:
            raise ExportError(
                "the action-legality rule no longer names %r (%d occurrences, "
                "expected %d); the artifact's per-phase legality must be "
                "re-derived from the new rule"
                % (expr, body.count(expr), count))
    bounds = {name: int(defined[name]) for name in wanted}
    bounds["move_count"] = move
    return bounds


def build_action_mask(actions: int, bounds: dict, phase_index: int,
                      crate_index: int, kind_field: int, kind_stride: int,
                      buyable_field: int, buyable_stride: int,
                      reroll_available_field: int, weapon_id_field: int,
                      weapon_id_stride: int, ban_field: int,
                      level_up_present_field: int, gold_field: int,
                      lvl_reroll_price_field: int) -> dict:
    """Per-phase legality + the state-dependent rules, straight from the core's
    single legality function (``bt_action_legal``, brotato_core.h; the mask
    function just delegates to it).

    ``phase_index``/``crate_index`` are the positions of the ``phase`` and
    ``crate.offer_kind`` observation fields, and ``kind_field``/``kind_stride``
    the first typed-offer-kind field and its stride (``shop%d.offer_type``), so a
    consumer can apply every rule without hard-coding an index.

    ``conditionals`` carry every id whose legality depends on live state the
    observation exposes, so a consumer's mask equals the core's ``bt_action_legal``
    for the state the observation describes.  Each entry is scoped to a
    ``legal_by_phase`` clause (so the crate clause REPLACES the level-up rules
    while a box is open, exactly like bt_action_legal) and holds a list of
    ANDed ``tests``; an id is legal iff EVERY test passes:

      ``{"op": "nonzero"|"equals", "field_base", "field_stride", "value"?}``
          observation[field_base + field_stride*k] (nonzero / == value)
      ``{"op": "block_duplicate", "field_base", "field_stride", "count"}``
          a second slot in the block carries the same value (bt_weapon_can_combine)
      ``{"op": "block_count_gte", "field_base", "field_stride", "count", "value"}``
          at least `value` nonzero entries in the block (the level-up reroll)
      ``{"op": "ge", "field", "other", "scale"}``
          observation[field] >= observation[other] * scale (gold vs the price)

    The per-id source of each conditional:
      buy ``buy0+i``        obs ``shop<i>.buyable`` (eligibility AND affordability
                            AND the purchase delay: bt_action_legal's four gates)
      reroll                obs ``shop.reroll_available``
      recycle ``recycle0+i`` obs ``weapon<i>.id`` != 0 (a weapon is in the slot)
      combine ``combine0+i`` obs ``weapon<i>.id`` equals ANOTHER slot's id in the
                            same block (bt_weapon_can_combine: a second copy)
      lock ``lock0+i``      obs ``shop<i>.offer_type`` != 0 (a live offer)
      ban ``ban0+i``        obs ``shop<i>.offer_type`` == BT_SHOP_ITEM AND the
                            ban-token budget field != 0 (ban_mode && ban_tokens > 0)
      level-up ``0+i``      the level-up presence field != 0 (lvl_offers[i] != 0)
      level-up reroll       at least two presence fields set AND gold >= price
    """
    move = bounds["move_count"]
    reroll, skip = bounds["BT_ACT_REROLL"], bounds["BT_ACT_SKIP"]
    buy0 = bounds["BT_ACT_BUY0"]
    recycle0, combine0, lock0, ban0 = (
        bounds["BT_ACT_RECYCLE0"], bounds["BT_ACT_COMBINE0"],
        bounds["BT_ACT_LOCK0"], bounds["BT_ACT_BAN0"])
    take, crate_recycle = bounds["BT_ACT_CRATE_TAKE"], bounds["BT_ACT_CRATE_RECYCLE"]
    combine_end = lock0 - 1     # combine spans the weapon slots; lock starts next
    return {
        "width": actions,
        "masked_logit": MASKED_LOGIT,
        "source": "bt_action_legal, ocean/brotato/brotato_core.h (bt_action_mask "
                  "delegates to it)",
        # The ranges are the ids that can be legal in that clause at all; the
        # per-slot refinements the ranges cannot express are in `conditionals`.
        "legal_by_phase": {
            "0_combat": [[0, move - 1]],
            "1_shop": [[0, reroll], [skip, combine_end]],
            # phase 2 with no item box open
            "2_level_up": [[0, reroll]],
            # phase 2 with an open item box: the take/recycle pair ONLY
            "2_crate": [[take, crate_recycle]],
        },
        "conditionals": [
            # buy i: the observation's own buyable flag already folds
            # eligibility, affordability and the purchase-delay timer
            # (bt_action_legal's four gates, brotato_core.h).
            {"clause": "1_shop", "id_base": buy0, "count": reroll - buy0,
             "tests": [{"op": "nonzero", "field_base": buyable_field,
                        "field_stride": buyable_stride}]},
            # reroll: affordability/free state as the observer reports it
            {"clause": "1_shop", "id_base": reroll, "count": 1,
             "tests": [{"op": "nonzero", "field_base": reroll_available_field,
                        "field_stride": 0}]},
            # recycle i: the slot holds a weapon
            {"clause": "1_shop", "id_base": recycle0, "count": combine0 - recycle0,
             "tests": [{"op": "nonzero", "field_base": weapon_id_field,
                        "field_stride": weapon_id_stride}]},
            # combine i: a second copy of the same variant exists in the block
            {"clause": "1_shop", "id_base": combine0, "count": lock0 - combine0,
             "tests": [{"op": "block_duplicate", "field_base": weapon_id_field,
                        "field_stride": weapon_id_stride, "count": lock0 - combine0}]},
            # lock i: a live offer in the slot
            {"clause": "1_shop", "id_base": lock0, "count": ban0 - lock0,
             "tests": [{"op": "nonzero", "field_base": kind_field,
                        "field_stride": kind_stride}]},
            # ban i: an ITEM offer (weapons cannot be banned) AND a token left
            {"clause": "1_shop", "id_base": ban0, "count": take - ban0,
             "tests": [{"op": "equals", "value": bounds["BT_SHOP_ITEM"],
                        "field_base": kind_field, "field_stride": kind_stride},
                       {"op": "nonzero", "field_base": ban_field,
                        "field_stride": 0}]},
            # level-up i: the slot holds a real offer (the presence field is the
            # unambiguous sim-side marker; the empty slot no longer aliases it)
            {"clause": "2_level_up", "id_base": buy0, "count": reroll - buy0,
             "tests": [{"op": "nonzero", "field_base": level_up_present_field,
                        "field_stride": 1}]},
            # level-up reroll: more than one offer AND gold >= the reroll price
            # (upgrades_ui_player_container.gd:71 hides the button for one offer;
            # obs[gold_field] is materials/100, obs[lvl_reroll_price_field] the
            # raw price, so compare them in the same 0.01 scale to stay exact)
            {"clause": "2_level_up", "id_base": reroll, "count": 1,
             "tests": [{"op": "block_count_gte", "value": 2,
                        "field_base": level_up_present_field, "field_stride": 1,
                        "count": reroll - buy0},
                       {"op": "ge", "field": gold_field,
                        "other": lvl_reroll_price_field, "scale": 0.01}]},
        ],
        "phase_field_index": phase_index,
        "selectors": [{
            "phase": 2,
            "use": "2_crate",
            "when_nonzero": {"field": "crate.offer_kind", "index": crate_index},
        }],
        "note": ("the mask is NOT part of the observation (Agent.action_mask, "
                 "src/pufferenv.h:29; published by brotato.h puf_reset/puf_step). "
                 "Resolve the clause from observation[phase_field_index] plus the "
                 "selectors, take that clause's legal_by_phase ranges, then apply "
                 "every `conditionals` entry whose clause matches: those ids are "
                 "masked unless every test in the entry passes "
                 "(observation[field_base + field_stride*k] for the id k-th). "
                 "Masked-off logits become masked_logit before the argmax, exactly "
                 "as the trainer does before its softmax (src/algo.cu:1364-1371), "
                 "and masked ids are no-ops in the core too."),
        "unobservable_conditions": (
            "One bt_action_legal condition is still not expressible from the "
            "observation: bt_weapon_can_combine also needs "
            "bt_weapon_upgrades(id) != BT_WEAPON_NONE and bt_weapon_tier(id) < "
            "bt_weapon_max_tier(s) (brotato_weapon_inventory.h:752-754), neither "
            "of which is observed (the first is a static property of the weapon "
            "table, the second a run stat), so a combine whose duplicate exists "
            "but which is maxed or has no upgrade path reads legal here while the "
            "sim no-ops it (bt_wshop_combine re-checks can_combine, "
            "brotato_weapon_shop.h); close it with a per-weapon "
            "`has_upgrade_path` flag plus the run's max tier in the schema."),
    }


def build_action_map(actions: int, slots: int, bounds: dict) -> dict:
    """Name the live action space, refusing anything the core does not cover."""
    move = bounds["move_count"]
    if move > len(_MOVE_NAMES):
        raise ExportError("the movement table has %d entries but only %d "
                          "directions are named" % (move, len(_MOVE_NAMES)))
    buy, reroll = bounds["BT_ACT_BUY0"], bounds["BT_ACT_REROLL"]
    skip = bounds["BT_ACT_SKIP"]
    recycle0, combine0 = bounds["BT_ACT_RECYCLE0"], bounds["BT_ACT_COMBINE0"]
    lock0, ban0 = bounds["BT_ACT_LOCK0"], bounds["BT_ACT_BAN0"]
    take, crate_recycle = bounds["BT_ACT_CRATE_TAKE"], bounds["BT_ACT_CRATE_RECYCLE"]
    if buy != 0 or not buy < reroll < skip < recycle0:
        raise ExportError("shop action order is unexpected (buy0=%d, reroll=%d, "
                          "skip=%d, recycle0=%d)" % (buy, reroll, skip, recycle0))
    if recycle0 != move:
        raise ExportError("weapon recycle actions start at %d but the movement "
                          "table ends at %d" % (recycle0, move))
    if combine0 - recycle0 != slots or lock0 - combine0 != slots:
        raise ExportError(
            "recycle/combine spans are %d/%d but the live weapon slot count is "
            "%d" % (combine0 - recycle0, lock0 - combine0, slots))
    if ban0 - lock0 != 4 or take - ban0 != 4 or crate_recycle != take + 1:
        raise ExportError(
            "lock/ban/crate spans are %d/%d/%d; the shop has four offer slots "
            "and two crate ids" % (ban0 - lock0, take - ban0, crate_recycle - take))
    if crate_recycle != actions - 1:
        raise ExportError(
            "the core's last action id is %d (BT_ACT_CRATE_RECYCLE, "
            "brotato_state.h) but BT_ACTIONS is %d; the action space and the "
            "decode disagree" % (crate_recycle, actions))

    def span(first, last, name):
        return {"%d" % first if first == last else "%d-%d" % (first, last): name}

    combat = {str(i): name for i, name in enumerate(_MOVE_NAMES[:move])}
    combat.update(span(move, actions - 1, "masked_in_combat"))
    shop = span(buy, buy + 3, "buy_offer")
    shop.update(span(reroll, reroll, "reroll"))
    shop.update(span(reroll + 1, skip - 1, "masked_in_shop"))
    shop.update(span(skip, skip, "skip"))
    shop.update(span(recycle0, combine0 - 1, "recycle_weapon_slot"))
    shop.update(span(combine0, lock0 - 1, "combine_weapon_slot"))
    shop.update(span(lock0, ban0 - 1, "lock_offer"))
    shop.update(span(ban0, take - 1, "ban_offer"))
    shop.update(span(take, actions - 1, "masked_in_shop"))
    level_up = span(buy, buy + 3, "choose_offer")
    level_up.update(span(reroll, reroll, "reroll_offers"))
    if reroll + 1 <= take - 1:
        level_up.update(span(reroll + 1, take - 1, "masked_in_level_up"))
    level_up.update(span(take, take, "take_crate_item"))
    level_up.update(span(crate_recycle, crate_recycle, "recycle_crate_item"))
    return {"head0": {"combat": combat, "shop": shop, "level_up": level_up}}


# ---------------------------------------------------------------------------
# Core-derived observation schema
# ---------------------------------------------------------------------------

_NUM_F = re.compile(r"(?<=\d)f\b")
_NUM_OR_EXPR = re.compile(r"^[0-9A-Za-z_\s+\-*/().]+$")
_IDENT = re.compile(r"[A-Za-z_]\w*")


def _strip_comments(line: str) -> str:
    line = re.sub(r"/\*.*?\*/", " ", line)
    line = re.sub(r"//.*$", " ", line)
    return line


def _join_continuations(text: str) -> str:
    return text.replace("\\\n", " ")


def _parse_defines(text: str) -> dict:
    """Evaluate the numeric/expression ``#define BT_*`` values in a header.

    Only pure arithmetic expressions over already-known ``BT_*`` names are
    accepted; anything else (function-like macros, string macros, type
    aliases) is skipped, so this can never invent a value.
    """
    ns: dict = {}
    for raw in _join_continuations(text).splitlines():
        line = _strip_comments(raw).strip()
        m = re.match(r"#define\s+([A-Za-z_]\w*)\s+(.*)$", line)
        if not m:
            continue
        name, expr = m.group(1), m.group(2).strip()
        if not name.startswith("BT_"):
            continue
        expr = _NUM_F.sub("", expr)
        if not expr or not _NUM_OR_EXPR.match(expr):
            continue
        if not set(_IDENT.findall(expr)) <= set(ns):
            continue
        try:
            value = eval(expr, {"__builtins__": {}}, dict(ns))  # noqa: S307
        except Exception:
            continue
        if isinstance(value, (int, float)) and not isinstance(value, bool):
            ns[name] = value
    return ns


def _fmt_num(value) -> str:
    if isinstance(value, float) and value.is_integer():
        return str(int(value))
    return repr(value) if isinstance(value, float) else str(value)


class CoreSchema:
    def __init__(self, obs_size, constants, fields, revision, missing):
        self.obs_size = obs_size
        self.constants = constants
        self.fields = fields
        self.revision = revision
        self.missing = missing


def read_core_schema(core_dir=None) -> CoreSchema:
    """Read the live observation schema from the core headers.

    Raises ExportError (never falls back to stale numbers) when a required
    constant or header is missing or the declared width disagrees with the
    field layout.
    """
    core_dir = Path(core_dir) if core_dir else DEFAULT_CORE_DIR
    defined: dict = {}
    revision: dict = {}
    sources: dict = {}
    missing = []
    for header in CORE_HEADERS:
        path = core_dir / header
        if not path.is_file():
            missing.append(header)
            revision[header] = None
            continue
        raw = path.read_bytes()
        revision[header] = hashlib.sha256(raw).hexdigest()
        text = raw.decode("utf-8", "replace")
        sources[header] = text
        defined.update(_parse_defines(text))
    if missing:
        raise ExportError(
            "core headers not found under %s: %s (pass --core-dir)"
            % (core_dir, ", ".join(missing)))

    required = ("BT_ARENA_W", "BT_ARENA_H", "BT_MAX", "BT_SLOTS", "BT_WAVES",
                "BT_MAX_PROJ", "BT_MAX_CONS", "BT_ACTIONS", "BT_OBS_SIZE",
                "BT_NWEAPONS", "BT_NCHARACTERS", "BT_CHARACTER_SLOT_DEFAULT",
                "BT_OBS_T_STATS", "BT_OBS_T_LOCKS", "BT_OBS_T_CRATE",
                "BT_OBS_T_LVLRR", "BT_OBS_T_ITEMS", "BT_OBS_T_BAN",
                "BT_OBS_T_LVLOFF", "BT_OBS_SIZE_PINNED")
    absent = [name for name in required if name not in defined]
    if absent:
        raise ExportError(
            "core headers under %s do not define %s; refusing to emit a "
            "possibly stale observation schema" % (core_dir, ", ".join(absent)))

    # The header's literal pin of the composed width, so this exporter can never
    # drift from the bridge/engine consumers that read it instead of re-evaluating
    # the BT_OBS_* expression (brotato_state.h:462-468).
    pinned = int(defined["BT_OBS_SIZE_PINNED"])
    if pinned != int(defined["BT_OBS_SIZE"]):
        raise ExportError(
            "brotato_state.h declares BT_OBS_SIZE_PINNED %d but the composed "
            "BT_OBS_SIZE evaluates to %d; bump both together"
            % (pinned, int(defined["BT_OBS_SIZE"])))

    aw = defined["BT_ARENA_W"]
    ah = defined["BT_ARENA_H"]
    max_enemies = int(defined["BT_MAX"])
    slots = int(defined["BT_SLOTS"])
    waves = int(defined["BT_WAVES"])
    weapons = int(defined["BT_NWEAPONS"])
    characters = int(defined["BT_NCHARACTERS"])
    obs_size = int(defined["BT_OBS_SIZE"])

    fields = [
        "hp/max_hp", "max_hp/64", "regen/30", "armor/20",
        "px/%s" % _fmt_num(aw), "py/%s" % _fmt_num(ah),
        "iframe_active", "phase", "materials/100", "level/20", "xp/xp_need",
    ]
    for i in range(4):
        fields += ["shop%d.tier/4" % i, "shop%d.affordable" % i,
                   "shop%d.dominant_stat/10" % i, "shop%d.magnitude*0.1" % i]
    for i in range(4):
        fields += ["lvl%d.stat/12" % i, "lvl%d.value*0.1" % i]
    for i in range(slots):
        fields += ["enemy%d.dx/%s" % (i, _fmt_num(aw)),
                   "enemy%d.dy/%s" % (i, _fmt_num(ah)),
                   "enemy%d.hp_frac" % i, "enemy%d.dmg*0.1" % i,
                   "enemy%d.elite" % i]
    fields += ["wave/%s" % _fmt_num(waves), "wave.time_left_frac",
               "wave.enemy_count/%s" % _fmt_num(max_enemies)]
    for i in range(3):
        fields += ["proj%d.dx/%s" % (i, _fmt_num(aw)),
                   "proj%d.dy/%s" % (i, _fmt_num(ah)), "proj%d.dmg*0.1" % i]
    fields += ["cons.dx/%s" % _fmt_num(aw), "cons.dy/%s" % _fmt_num(ah)]
    # --- weapon / shop / profile extension (bt_weapon_observe_append) ---------
    for i in range(slots):
        fields += ["weapon%d.id/%d" % (i, weapons + 1),
                   "weapon%d.tier/4" % i,
                   "weapon%d.kind" % i,
                   "weapon%d.cooldown_frac" % i,
                   "weapon%d.damage*0.1" % i,
                   "weapon%d.range/%s" % (i, _fmt_num(aw)),
                   "weapon%d.crit_chance" % i,
                   "weapon%d.cursed" % i]
    for i in range(4):
        fields += ["shop%d.offer_type" % i, "shop%d.content_id" % i,
                   "shop%d.cursed" % i]
    # The core writes (character + 1) / (BT_NCHARACTERS + 1), so the index is
    # recovered as round(field * (characters + 1)) - 1 (brotato_weapon_observe.h:170).
    fields += ["profile.character_id_plus1/%d" % (characters + 1),
               "profile.weapon_slot_limit/%s"
               % _fmt_num(int(defined["BT_CHARACTER_SLOT_DEFAULT"]))]
    fields += ["shop.currency_is_hp"]
    for i in range(4):
        fields += ["shop%d.price" % i, "shop%d.eligible" % i,
                   "shop%d.affordable_action" % i, "shop%d.buyable" % i]
    fields += ["shop.reroll_price", "shop.reroll_free", "shop.reroll_available"]

    # --- Demon/endless block (v4, BT_OBS_TARGET floats) ----------------------
    # Written by bt_core_observe (brotato_core.h:1436-1452): stats, the ids the
    # shop module reserves (offer locks, open crate, level-up reroll - the sim
    # writes zeros there until AuditShop lands that state, but the slots are
    # part of the final layout), then the curated guide-item counts in
    # BT_OBS_GUIDE_ITEMS order (brotato_core.h:1301).
    t_stats = int(defined["BT_OBS_T_STATS"])
    t_locks = int(defined["BT_OBS_T_LOCKS"])
    t_crate = int(defined["BT_OBS_T_CRATE"])
    t_lvlrr = int(defined["BT_OBS_T_LVLRR"])
    t_items = int(defined["BT_OBS_T_ITEMS"])
    t_ban = int(defined["BT_OBS_T_BAN"])
    t_lvloff = int(defined["BT_OBS_T_LVLOFF"])
    if t_stats != 4 or t_lvlrr != 1 or t_crate != 4 or t_ban != 1 or t_lvloff != 4:
        raise ExportError(
            "the Demon/endless block changed shape (stats=%d, crate=%d, "
            "level-up reroll=%d, ban=%d, level-up offers=%d); the schema mapping "
            "needs updating" % (t_stats, t_crate, t_lvlrr, t_ban, t_lvloff))
    fields += ["target.harvesting/100", "target.luck/100",
               "target.endless_factor_squashed", "target.wave/50"]
    for i in range(t_locks):
        fields += ["shop%d.locked" % i]
    fields += ["crate.offer_kind", "crate.offer_id", "crate.cursed",
               "crate.ready"]
    fields += ["level_up.reroll_price"]
    guide_items = parse_guide_items(sources)
    if len(guide_items) != t_items:
        raise ExportError("BT_OBS_GUIDE_ITEMS has %d entries but BT_OBS_T_ITEMS "
                          "is %d" % (len(guide_items), t_items))
    for item in guide_items:
        fields += ["item_%s.owned_frac" % item]
    # v5 tail: the ban-token budget (the divisor is BT_BAN_MAX_TOKEN) and the
    # four level-up offer presence flags.
    for _ in range(t_ban):
        fields += ["target.ban_tokens/8"]
    for i in range(t_lvloff):
        fields += ["level_up%d.offer_present" % i]

    if len(fields) != obs_size:
        raise ExportError(
            "core field layout is %d entries but the headers declare "
            "BT_OBS_SIZE=%d; the schema mapping is out of date"
            % (len(fields), obs_size))

    action_bounds = parse_action_bounds(defined, sources)
    build_action_map(int(defined["BT_ACTIONS"]), slots, action_bounds)
    phase_index = fields.index("phase")
    crate_index = fields.index("crate.offer_kind")
    kind_field = fields.index("shop0.offer_type")
    kind_stride = fields.index("shop1.offer_type") - kind_field
    ban_field = fields.index("target.ban_tokens/8")
    level_up_present_field = fields.index("level_up0.offer_present")
    gold_field = fields.index("materials/100")
    lvl_reroll_price_field = fields.index("level_up.reroll_price")
    build_action_mask(int(defined["BT_ACTIONS"]), action_bounds, phase_index,
                      crate_index, kind_field, kind_stride,
                      fields.index("shop0.buyable"),
                      fields.index("shop1.buyable") - fields.index("shop0.buyable"),
                      fields.index("shop.reroll_available"),
                      next(i for i, f in enumerate(fields)
                           if f.startswith("weapon0.id/")),
                      (next(i for i, f in enumerate(fields)
                            if f.startswith("weapon1.id/"))
                       - next(i for i, f in enumerate(fields)
                              if f.startswith("weapon0.id/"))),
                      ban_field, level_up_present_field, gold_field,
                      lvl_reroll_price_field)

    constants = {
        "arena_w": aw, "arena_h": ah, "max_enemies": max_enemies,
        "slots": slots, "waves": waves,
        "max_projectiles": int(defined["BT_MAX_PROJ"]),
        "max_consumables": int(defined["BT_MAX_CONS"]),
        "obs_size_pinned": pinned,
        "actions": int(defined["BT_ACTIONS"]),
        "weapons": weapons, "characters": characters,
        "shop_records": 4, "profile_fields": 2,
        "weapon_slot_default": int(defined["BT_CHARACTER_SLOT_DEFAULT"]),
        "target_block": {"stats": t_stats, "locks": t_locks, "crate": t_crate,
                         "level_up_reroll": t_lvlrr, "guide_items": t_items,
                         "ban_tokens": t_ban, "level_up_offers": t_lvloff},
        "guide_items": guide_items,
        "phase_field_index": phase_index,
        "crate_field_index": crate_index,
        "shop_offer_kind_field": kind_field,
        "shop_offer_kind_stride": kind_stride,
        # Mask sources the action_mask conditionals read (indices into `fields`).
        "shop_buyable_field": fields.index("shop0.buyable"),
        "shop_buyable_stride": fields.index("shop1.buyable") - fields.index("shop0.buyable"),
        "reroll_available_field": fields.index("shop.reroll_available"),
        "weapon_id_field": next(i for i, f in enumerate(fields)
                                if f.startswith("weapon0.id/")),
        "weapon_id_stride": (next(i for i, f in enumerate(fields)
                                  if f.startswith("weapon1.id/"))
                             - next(i for i, f in enumerate(fields)
                                    if f.startswith("weapon0.id/"))),
        "ban_field": ban_field,
        "level_up_present_field": level_up_present_field,
        "level_up_present_count": t_lvloff,
        "gold_field": gold_field,
        "lvl_reroll_price_field": lvl_reroll_price_field,
        "action_bounds": action_bounds,
    }
    return CoreSchema(obs_size, constants, fields, revision, [])


# ---------------------------------------------------------------------------
# Checkpoint layout
# ---------------------------------------------------------------------------

def build_layout(obs_size: int, hidden: int, layers: int, actions: int,
                 continuous: bool):
    """Parameter descriptors in exact registration order (name, shape, numel)."""
    dims = [("encoder.weight", [hidden, obs_size]),
            ("decoder.weight", [actions + 1, hidden])]
    if continuous:
        dims.append(("decoder.logstd", [1, actions]))
    for i in range(layers):
        dims.append(("mingru.%d.weight" % i, [3 * hidden, hidden]))
    out = []
    for name, shape in dims:
        numel = 1
        for d in shape:
            numel *= d
        out.append((name, shape, numel))
    return out


def layout_floats(layout) -> int:
    """float32 count of the LEGACY aligned model (padded), not of a current file."""
    total = 0
    for _, _, numel in layout:
        total = (total + ALIGNMENT_FLOATS - 1) & ~(ALIGNMENT_FLOATS - 1)
        total += numel
    return total


def tensor_offsets(layout) -> list:
    """(name, shape, offset_floats, numel) in the LEGACY aligned model.

    Only used to reason about pre-packing checkpoints; current checkpoints are
    packed (``trainer_tensor_offsets``)."""
    out = []
    off = 0
    for name, shape, numel in layout:
        off = (off + ALIGNMENT_FLOATS - 1) & ~(ALIGNMENT_FLOATS - 1)
        out.append((name, shape, off, numel))
        off += numel
    return out


def trainer_tensor_offsets(layout) -> list:
    """(name, offset_bytes) inside the trainer checkpoint's packed image.

    ``puf_save_weights`` walks the registered tensors in registration order and
    concatenates them with no padding (src/pufferl.cu), so tensor i starts at the
    sum of the float32 sizes before it.
    """
    out = []
    off = 0
    for name, _shape, numel in layout:
        out.append((name, off))
        off += numel * FLOAT_BYTES
    return out


def trainer_checkpoint_floats(layout) -> int:
    """float32 count the trainer checkpoint holds for this layout.

    ``puf_save_weights`` writes ``params_alloc.total_elems`` float32, the sum of
    the registered tensor sizes (src/pufferl.cu; ``total_elems`` accumulates in
    ``_alloc_register``), with no alignment gaps.
    """
    return sum(numel for _name, _shape, numel in layout)


def write_trainer_checkpoint(path: str, layout, fill) -> None:
    """Write a checkpoint exactly as the trainer does (fixtures / tests).

    The registered tensors are concatenated in registration order with no
    padding, reproducing ``puf_save_weights`` byte for byte (src/pufferl.cu).
    ``fill(name, index) -> float`` supplies each tensor's values in registration
    order.
    """
    image = bytearray(trainer_checkpoint_floats(layout) * FLOAT_BYTES)
    for (name, off_bytes), (_n, _shape, numel) in zip(trainer_tensor_offsets(layout),
                                                      layout):
        blob = struct.pack("<%df" % numel,
                           *[fill(name, i) for i in range(numel)])
        image[off_bytes:off_bytes + len(blob)] = blob
    with open(path, "wb") as handle:
        handle.write(bytes(image))


def _validate_dims(obs_size, hidden, layers, actions, continuous):
    if obs_size <= 0:
        raise ExportError("--obs-size must be positive")
    if hidden <= 0:
        raise ExportError("--hidden must be positive")
    if layers <= 0:
        raise ExportError("--layers must be positive")
    if actions <= 0:
        raise ExportError("--actions must be positive")
    if continuous and actions != 1:
        raise ExportError(
            "--continuous requires --actions 1 (the trainer sets continuous "
            "iff every action size is 1)")


def read_checkpoint(path: str, layout):
    """Read and validate a packed checkpoint file against the layout.

    The file must be exactly ``trainer_checkpoint_floats(layout)`` float32: the
    registered tensors concatenated in registration order with no padding, which
    is what ``puf_save_weights`` writes for any shape (the packer walks
    ``alloc->regs`` instead of copying the device image).

    Returns a list of (name, shape, struct-tuple of float32).
    """
    if not os.path.isfile(path):
        raise ExportError("checkpoint not found: %s" % path)
    size = os.path.getsize(path)
    if size % 4 != 0:
        raise ExportError("checkpoint size %d is not a multiple of 4 bytes"
                          % size)
    expected_floats = trainer_checkpoint_floats(layout)
    actual_floats = size // 4
    if actual_floats != expected_floats:
        raise ExportError(
            "checkpoint size mismatch: file has %d float32 (%d bytes), the "
            "layout holds %d float32.\n  check --obs-size/--hidden/--layers/"
            "--actions/--continuous against the training config"
            % (actual_floats, size, expected_floats))

    with open(path, "rb") as handle:
        raw = handle.read()
    offsets = dict(trainer_tensor_offsets(layout))
    tensors = []
    for name, shape, numel in layout:
        values = struct.unpack_from("<%df" % numel, raw, offsets[name])
        tensors.append((name, shape, values))
    return tensors


def check_finite(tensors) -> None:
    for name, _, values in tensors:
        for i, v in enumerate(values):
            if v != v or v in (float("inf"), float("-inf")):
                raise ExportError(
                    "checkpoint tensor %s[%d] is not finite (%r)" % (name, i, v))


def encode_tensor(values) -> str:
    return base64.b64encode(struct.pack("<%df" % len(values), *values)).decode("ascii")


# ---------------------------------------------------------------------------
# Artifact
# ---------------------------------------------------------------------------

def build_artifact(checkpoint_path, tensors, obs_size, hidden, layers, actions,
                   continuous, core: CoreSchema, checkpoint_obs_size=None,
                   train_revision=None):
    if checkpoint_obs_size is None:
        checkpoint_obs_size = obs_size
    if actions != core.constants["actions"]:
        raise ExportError(
            "decoder width %d is not the live core action count %d; an "
            "artifact must carry the action space its trainer used"
            % (actions, core.constants["actions"]))
    action_map = build_action_map(actions, core.constants["slots"],
                                  core.constants["action_bounds"])
    encoded = []
    for name, shape, values in tensors:
        encoded.append({
            "name": name,
            "shape": shape,
            "dtype": "f32le",
            "data": encode_tensor(values),
        })

    with open(checkpoint_path, "rb") as handle:
        digest = hashlib.sha256(handle.read()).hexdigest()

    artifact = {
        "format": FORMAT,
        "format_version": FORMAT_VERSION,
        "alignment_floats": ALIGNMENT_FLOATS,
        "weight_order": [name for name, _, _ in tensors],
        "layout": {
            "obs_size": obs_size,
            "hidden": hidden,
            "layers": layers,
            "actions": actions,
            "continuous": bool(continuous),
            "num_heads": 1,
            "head_sizes": [actions],
            # Per-action legality mask the env supplies (width = action count,
            # 1 = legal).  Masked-off logits become `masked_logit` before the
            # argmax in game, exactly as the trainer does before its softmax
            # (src/algo.cu:1364-1371).
            "mask_size": actions,
            "masked_logit": MASKED_LOGIT,
        },
        "observation_schema": {
            "core_obs_size": core.obs_size,
            "obs_size": obs_size,
            "matches_layout": obs_size == core.obs_size,
            "constants": core.constants,
            "fields": core.fields,
            "revision": core.revision,
        },
        "action_map": action_map,
        "action_mask": build_action_mask(
            actions, core.constants["action_bounds"],
            core.constants["phase_field_index"],
            core.constants["crate_field_index"],
            core.constants["shop_offer_kind_field"],
            core.constants["shop_offer_kind_stride"],
            core.constants["shop_buyable_field"],
            core.constants["shop_buyable_stride"],
            core.constants["reroll_available_field"],
            core.constants["weapon_id_field"],
            core.constants["weapon_id_stride"],
            core.constants["ban_field"],
            core.constants["level_up_present_field"],
            core.constants["gold_field"],
            core.constants["lvl_reroll_price_field"]),
        "tensors": encoded,
        "source_checkpoint": {
            "path": os.path.basename(checkpoint_path),
            "size_bytes": os.path.getsize(checkpoint_path),
            "floats": trainer_checkpoint_floats(
                build_layout(checkpoint_obs_size, hidden, layers, actions,
                             continuous)),
            "obs_size": checkpoint_obs_size,
            "sha256": digest,
            "train_time_revision": train_revision,
            "packed": True,
            # Device parameter-image padding (alloc_create); the file is packed
            # and does NOT contain these gap bytes.
            "trainer_alignment_bytes": TRAINER_ALIGNMENT_BYTES,
        },
        "notes": {
            "encoder": ("linear, out = x @ W_enc^T (no bias, no activation); "
                        "encoder_forward, src/algo.cu:575-584, puf_mm at "
                        "src/algo.cu:137-146"),
            "mingru": ("per layer: combined = x @ W^T -> [hidden, gate, proj]; "
                       "gate_sig = sigmoid_f(gate); "
                       "hidden_tilde = hidden >= 0 ? hidden + 0.5 : sigmoid_f(hidden); "
                       "mingru_out = lerp_f(state, hidden_tilde, gate_sig); "
                       "proj_sig = sigmoid_f(proj); "
                       "out = proj_sig * mingru_out + (1 - proj_sig) * x; "
                       "next_state = mingru_out (mingru_gate, src/algo.cu:218-244; "
                       "sigmoid src/algo.cu:207, lerp src/algo.cu:212)"),
            "decoder": ("linear, out = x @ W_dec^T (decoder_forward, "
                        "src/algo.cu:635-643); the first `actions` columns are "
                        "logits, the last column is the scalar value "
                        "(src/pufferl.cu:702, src/algo.cu:564-573)"),
            "action": "argmax over the logits of head 0",
            "action_mask": (
                "optional per-action legality vector of length `actions` "
                "(1 = legal); masked-off logits become `layout.masked_logit` "
                "before the argmax, which is exactly what the trainer does "
                "before its softmax (load_logit_masked, src/algo.cu:1364-1371). "
                "The mask is NOT part of the observation: the environment "
                "supplies it (Agent.action_mask, src/pufferenv.h:29) and the "
                "in-game forward must be given the same mask. No mask means "
                "every action is legal, which is what the sim env currently "
                "supplies (ocean/brotato/brotato.h:443)."),
            "checkpoint": (
                "float32 in registration order "
                "(encoder.weight, decoder.weight[, decoder.logstd], "
                "mingru.0.weight .. mingru.L-1.weight; weights_create, "
                "src/algo.cu:982-990), 8-float aligned (src/puffercpu.c:47-51). "
                "The file is packed: the registered tensors concatenated "
                "with no padding (puf_save_weights, src/pufferl.cu), so it is "
                "independent of the device parameter image's 16-byte "
                "alignment. The native CPU reader still rounds each tensor to "
                "8 float32 (src/puffercpu.c:47-51); native_reader_matches() "
                "reports whether that reader agrees with this file."),
            "weapon_cooldown": (
                "weapon*.cooldown_frac is 0.0 in the shop phase (phase 1): the "
                "shop scene has no live weapon attack clock, so every weapon is "
                "reported ready and the stored cooldown counter is not reset"),
            "observation_schema": (
                "fields/divisors are read from the live core headers at export "
                "time; observation_schema.revision is the sha256 of those "
                "headers. That revision describes the schema THIS artifact was "
                "labelled with; it does NOT prove which revision trained the "
                "weights (see source_checkpoint.train_time_revision)."),
        },
    }
    return artifact


def export(checkpoint_path: str, output_path: str, obs_size, hidden: int,
           layers: int, actions: int, continuous: bool = False,
           overwrite: bool = True,
           core_dir=None, train_revision=None) -> dict:
    core = read_core_schema(core_dir)
    if obs_size is None:
        obs_size = core.obs_size
    _validate_dims(obs_size, hidden, layers, actions, continuous)

    # Clean cutover: the artifact's observation width must BE the live core
    # schema. There is no opt-out, no padding and no migration, so an artifact
    # can never be labelled with a schema the core does not have.
    if obs_size != core.obs_size:
        raise ExportError(
            "observation width %d is not the live core schema %d; retrain at "
            "the live width (there is no padding or migration)"
            % (obs_size, core.obs_size))

    layout = build_layout(obs_size, hidden, layers, actions, continuous)
    tensors = read_checkpoint(checkpoint_path, layout)
    check_finite(tensors)
    artifact = build_artifact(checkpoint_path, tensors, obs_size, hidden,
                              layers, actions, continuous, core,
                              checkpoint_obs_size=obs_size,
                              train_revision=train_revision)
    if not overwrite and os.path.exists(output_path):
        raise ExportError("output already exists: %s" % output_path)
    tmp = output_path + ".tmp"
    with open(tmp, "w") as handle:
        json.dump(artifact, handle, separators=(",", ":"))
    os.replace(tmp, output_path)
    return artifact


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Export a native PufferLib policy checkpoint to a portable "
                    "JSON artifact (src/pufferl.cu puf_save_weights format).")
    parser.add_argument("--checkpoint", required=True,
                        help="trainer checkpoint .bin (puf_save_weights dump: "
                             "float32, registration order, 8-float aligned)")
    parser.add_argument("--output", required=True, help="artifact JSON to write")
    parser.add_argument("--obs-size", type=int, default=None,
                        help="encoder input width in the checkpoint; defaults to "
                             "the live core schema width. New exports must equal it")
    parser.add_argument("--hidden", type=int, required=True)
    parser.add_argument("--layers", type=int, required=True,
                        help="MinGRU layer count")
    parser.add_argument("--actions", type=int, required=True,
                        help="total discrete action count (decoder logits)")
    parser.add_argument("--continuous", action="store_true",
                        help="decoder registers a logstd row (trainer: actions == 1)")
    parser.add_argument("--core-dir", default=None,
                        help="directory holding brotato.h/brotato_core.h/"
                             "brotato_state.h and the weapon/character schema headers "
                             "(default: %s)" % DEFAULT_CORE_DIR)
    parser.add_argument("--train-revision", default=None,
                        help="optional revision label (e.g. core header hashes) "
                             "recorded as the training-time revision of the weights")
    parser.add_argument("--no-overwrite", action="store_true")
    return parser


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)
    try:
        artifact = export(args.checkpoint, args.output, args.obs_size,
                          args.hidden, args.layers, args.actions,
                          args.continuous, overwrite=not args.no_overwrite,
                          core_dir=args.core_dir,
                          train_revision=args.train_revision)
    except ExportError as exc:
        print("export_policy: %s" % exc, file=sys.stderr)
        return 1
    except OSError as exc:
        print("export_policy: %s" % exc, file=sys.stderr)
        return 1
    layout = artifact["layout"]
    schema = artifact["observation_schema"]
    print("wrote %s" % args.output)
    print("  format        : %s v%d" % (artifact["format"], artifact["format_version"]))
    print("  layout        : obs=%d hidden=%d layers=%d actions=%d continuous=%s"
          % (layout["obs_size"], layout["hidden"], layout["layers"],
             layout["actions"], layout["continuous"]))
    print("  core schema   : obs=%d matches=%s arena=%sx%s max_enemies=%s"
          % (schema["core_obs_size"], schema["matches_layout"],
             _fmt_num(schema["constants"]["arena_w"]),
             _fmt_num(schema["constants"]["arena_h"]),
             schema["constants"]["max_enemies"]))
    print("  weight_order  : %s" % ", ".join(artifact["weight_order"]))
    print("  source sha256 : %s" % artifact["source_checkpoint"]["sha256"])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
