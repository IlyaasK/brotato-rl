#!/usr/bin/env python3
"""Extract the Brotato gameplay-audio event graph (campaign + Abyss DLC) into
the portable header ``brotato_audio_data.h``.

Ground truth is the recovered Godot 3.x project (campaign root
``brotato_recovered`` + ``brotato_abyss`` DLC root).  Sound playback is not
rendered in this simulation, but the *selection* of which clip plays is not
optional: every call site below draws the global RNG through
``Utils.get_rand_element`` (``Array.pick_random``, ``singletons/utils.gd:391``),
so a missing or extra draw desynchronises every later random decision.

This generator only publishes DATA: the ordered clip sequences and each clip's
true length in seconds.  The RNG draw and the manager enqueue are the runtime's
job (brotato_combat_audio.h).  The survivors of the weapon-shot / weapon-hit
paths (``extract_weapons.py`` / brotato_weapon_audio.h) and the birth / healer
positional path (``brotato_birth_audio.h``) are NOT duplicated here.

Covered source events (site -> manager):
  * unit.gd:298/301      ``hurt_sounds`` / ``crit_sounds``   SoundManager2D pitch 0.2
    (per enemy kind + per neutral; scene-inheritance resolved, unit.tscn default)
  * unit.gd:642,672      ``burn_sounds``                    SoundManager2D pitch 0.2
    (burn apply 642; the burn TICK 672 stashes a burn draw in
    ``TakeDamageArgs.custom_sound``, see the CUSTOM-SOUND note below)
  * player.gd:440,507    ``hurt_sounds`` then ``dodge_sounds`` (only while
    dodging -- the hurt draw happens EITHER WAY) SoundManager2D pitch 0.2 force
  * player.gd:736        ``hp_regen_sounds``                SoundManager pitch 0.1
  * player.gd:855        ``alien_sounds``                   SoundManager pitch 0.1
  * player.gd:407        ``WeaponService.breaking_sounds``  SoundManager pitch 0.1 force
  * main.gd:455          ``run_lost_sounds``                SoundManager pitch 0 force
  * main.gd:802,804      ``gold_alt_pickup_sounds`` / ``gold_pickup_sounds``
    SoundManager pitch 0.2 (alt gated by ProgressData.settings.alt_gold_sounds,
    default false -- singletons/progress_data.gd:387,422)
  * main.gd:863          ``level_up_sound``                 SoundManager DIRECT
    (a single Resource, NOT an array: no pick_random, no draw) pitch 0 force
  * main.gd:958          ``end_wave_sounds``                SoundManager pitch 0
  * main.gd:1030         ``run_won_sounds``                 SoundManager pitch 0 force
  * unit.gd:750          ``OneShotOnHitEffect.sound_one_shot`` SoundManager2D DIRECT
    pitch 0.2 (a single AudioStream, no pick_random, no draw)
  * player.gd:560 (``stats.shooting_sounds`` of the exploding effect) and
    weapon_service.gd:350-351 (``weapon_stats.shooting_sounds`` of the
    special-spawn projectile): both index the SAME canonical BT_ITEM_WSTATS
    rows that extract_items.py emits, so they are published as
    BT_AUDIO_WSTATS_SHOOT_FIRST/COUNT and NOT duplicated as a per-weapon table
    (that one belongs to extract_weapons.py / BT_WEAPON_AUDIO_DURATION).
  * player_explosion.gd:31  the PlayerExplosion NODE's own ``explosion_sounds``
    (projectiles/explosion.tscn, projectiles/flesh_explosion/flesh_explosion.tscn)
    SoundManager2D pitch 0.2, one draw per freshly instantiated node.

Per-clip lengths are read from the real ``.wav`` / ``.mp3`` / ``.ogg`` bytes by
``extract_weapons.audio_clip_duration`` (RIFF/WAVE chunk sizes, MPEG frame scan,
Ogg granule) -- never guessed, never a fixed default.  A clip the resolver
cannot find aborts the run.

CUSTOM-SOUND note (explicit provenance, no guessing): the only
``TakeDamageArgs.custom_sound`` assignment in the whole project is unit.gd:672
(snapshot of a fresh ``burn_sounds`` pick on a burn tick).  unit.gd:298 still
draws ``hurt_sounds`` and unit.gd:301 still draws ``crit_sounds`` before that
override is applied, so a burn tick consumes the burn pick PLUS the hurt pick
(PLUS the crit pick on a crit).  ``Hitbox.custom_hit_sounds`` and the weapon
shot/hit paths are NOT custom_sound overrides and stay with the weapon runtime.
``OneShotOnHitEffect.sound_one_shot`` (unit.gd:750) is a direct play, resolved
here from the ONE reachable clip; if a future source ships two distinct
one-shot clips this generator refuses to emit a single span and fails instead.

Usage::

    python3 ocean/brotato/extract_audio.py \\
        --campaign-root /path/brotato_recovered \\
        --dlc-root      /path/brotato_abyss \\
        --out           ocean/brotato/brotato_audio_data.h \\
        --provenance-out /path/elsewhere/brotato_audio.json
"""

from __future__ import annotations

import argparse
import json
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import extract_physics as physics  # noqa: E402
import extract_gameplay as gameplay  # noqa: E402
import extract_items as items  # noqa: E402
import extract_weapons as weapons  # noqa: E402

ExtractError = physics.ExtractError

_ANY_REF_RE = items._ANY_REF_RE
_PRELOAD_RE = re.compile(r'preload\(\s*"([^"]+)"\s*\)')

MAIN_SCENE = "res://main.tscn"
PLAYER_SCENE = "res://entities/units/player/player.tscn"
WEAPON_SERVICE_GD = "res://singletons/weapon_service.gd"
BULL_EXPLOSION_STATS = "res://items/characters/bull/bull_explosion_stats.tres"

# PlayerExplosion NODES (projectiles/player_explosion.gd:31) play their OWN
# `explosion_sounds` on the node's first physics frame -- a different sound from
# the exploding effect's stats shooting_sounds.  Every scene below is reached by
# an effect's `explosion_scene` (explosion.tscn: bull/landmine/spicy_sauce/
# turret_rocket/plank/fireball/rocket_launcher/...; flesh_explosion.tscn:
# DLC ogre explode_on_overkill).  A scene carrying explosion_sounds that is not
# declared here aborts the run rather than being silently dropped.
EXPLOSION_SCENES = (
    ("BT_AUDIO_EV_EXPLOSION_NODE", "res://projectiles/explosion.tscn"),
    ("BT_AUDIO_EV_FLESH_EXPLOSION_NODE",
     "res://projectiles/flesh_explosion/flesh_explosion.tscn"),
)

# Event ids.  ORDER IS THE ABI: BT_AUDIO_EV_FIRST/COUNT are indexed by these.
# A direct (non-array) source play still occupies one span row with count 1.
EVENTS = [
    ("BT_AUDIO_EV_PLAYER_HURT", "player.tscn hurt_sounds (scene chain)"),
    ("BT_AUDIO_EV_PLAYER_DODGE", "unit.tscn dodge_sounds (inherited by player.tscn)"),
    ("BT_AUDIO_EV_HP_REGEN", "player.tscn hp_regen_sounds"),
    ("BT_AUDIO_EV_ALIEN_EYES", "player.tscn alien_sounds"),
    ("BT_AUDIO_EV_GOLD_PICKUP", "main.tscn gold_pickup_sounds"),
    ("BT_AUDIO_EV_LEVEL_UP", "main.tscn level_up_sound (DIRECT)"),
    ("BT_AUDIO_EV_END_WAVE", "main.tscn end_wave_sounds"),
    ("BT_AUDIO_EV_RUN_WON", "main.tscn run_won_sounds"),
    ("BT_AUDIO_EV_RUN_LOST", "main.tscn run_lost_sounds"),
    ("BT_AUDIO_EV_WEAPON_BREAK", "weapon_service.gd breaking_sounds"),
    ("BT_AUDIO_EV_ONE_SHOT", "OneShotOnHitEffect.sound_one_shot (DIRECT)"),
    ("BT_AUDIO_EV_BULL_EXPLODE", "bull_explosion_stats.tres shooting_sounds"),
    ("BT_AUDIO_EV_GOLD_ALT_PICKUP", "main.tscn gold_alt_pickup_sounds (settings-gated)"),
] + [(name, "%s explosion_sounds (PlayerExplosion node)" % scene)
     for name, scene in EXPLOSION_SCENES]
EVENT_INDEX = {name: i for i, (name, _) in enumerate(EVENTS)}


# --------------------------------------------------------------------------
# float32 literal (clip lengths are GDScript AudioStream.get_length(), binary32)
# --------------------------------------------------------------------------
def _f32_seconds(value):
    """C float literal that round-trips the value's binary32 encoding.

    The shared 6-significant-digit ``cfloat`` would perturb it; round to
    binary32, then emit exactly enough digits to round-trip any float32."""
    value = struct.unpack("<f", struct.pack("<f", float(value)))[0]
    text = "%.9g" % value
    if "e" in text or "E" in text:
        return text + "f"
    if "." not in text:
        text += ".0"
    return text + "f"


# --------------------------------------------------------------------------
# Scene sound arrays (root export, resolved through packed-scene inheritance)
# --------------------------------------------------------------------------
def _root_chain(resolver, scene_res):
    """Yield ``(parsed_file, root_node_props)`` from the scene up its bases.

    Property override semantics: the nearest scene on the chain that DEFINES a
    root property wins.  ExtResource ids are relative to the file that wrote the
    value, so the owning :class:`ParsedFile` must accompany it (a base scene's
    array must not be resolved against the derived file's ext table)."""
    seen = set()
    cur = scene_res
    while cur and cur not in seen:
        seen.add(cur)
        pf = resolver.parse(cur)
        root = None
        for node in pf.nodes:
            if node["parent"] is None:
                root = node
                break
        if root is None:
            break
        yield pf, root["props"]
        cur = pf.ext_path(root["instance"]) if root["instance"] is not None else None


def _clips_from_prop(pf, raw, where):
    """Ordered AudioStream res paths of one scene/resource property value."""
    if raw is None:
        return []
    if isinstance(raw, tuple):
        # flatten_scene normalizes a bare ExtResource(n) into ("ext", path).
        if raw[0] == "ext":
            return [raw[1]]
        raise ExtractError("%s: unsupported sound reference %r" % (where, raw))
    text = raw.strip()
    if not text:
        return []
    refs = _ANY_REF_RE.findall(text)
    if refs:
        return [pf.ext_path(eid) for eid in refs]
    if re.fullmatch(r"\[\s*\]", text):
        return []
    raise ExtractError("%s: sound property has no ExtResource refs: %r" % (where, text))


def scene_sound_array(resolver, scene_res, prop):
    """Resolve ``prop`` on a scene root through the inheritance chain.

    Returns ``[]`` when no scene on the chain defines it: the GDScript
    ``export (Array, Resource)`` default is an empty array, and the source's
    ``size() > 0`` guard then skips the pick entirely (no draw)."""
    for pf, props in _root_chain(resolver, scene_res):
        if prop in props:
            return _clips_from_prop(pf, props[prop], "%s.%s" % (scene_res, prop))
    return []


def resource_sound_array(resolver, res_path, prop):
    pf = resolver.parse(res_path)
    return _clips_from_prop(pf, pf.root_props.get(prop),
                            "%s.%s" % (res_path, prop))


def script_preload_array(resolver, gd_path, var_name):
    """Ordered preload() res paths of a GDScript `var <name>: Array = [...]`."""
    abs_path = resolver.find(gd_path)
    with open(abs_path, "r", encoding="utf-8", errors="replace") as handle:
        text = handle.read()
    match = re.search(r"^var\s+%s\b[^\n]*=\s*\[(.*?)\]" % re.escape(var_name),
                      text, re.MULTILINE | re.DOTALL)
    if not match:
        raise ExtractError("%s: no `var %s = [...]` declaration" % (gd_path, var_name))
    refs = _PRELOAD_RE.findall(match.group(1))
    if not refs:
        raise ExtractError("%s: `%s` has no preload() entries" % (gd_path, var_name))
    for ref in refs:
        resolver.find(ref)          # fail loudly on a missing clip
    return refs


def scenes_with_prop(roots, prop):
    """Sorted ``res://`` paths of every .tscn whose ROOT node defines ``prop``.

    Read straight off disk (``physics.parse_file``) rather than through the
    resolver so a file shadowed by the other root cannot be mis-reported."""
    found = set()
    for root in roots:
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames.sort()
            for name in filenames:
                if not name.endswith(".tscn"):
                    continue
                abs_path = os.path.join(dirpath, name)
                pf = physics.parse_file(abs_path)
                for node in pf.nodes:
                    if node["parent"] is None:
                        if prop in node["props"]:
                            rel = os.path.relpath(abs_path, root).replace(os.sep, "/")
                            found.add("res://" + rel)
                        break
    return sorted(found)


# --------------------------------------------------------------------------
# Global clip table: one contiguous row per distinct ordered sequence
# --------------------------------------------------------------------------
class ClipTable(object):
    """Ordered clip-duration rows + shared spans for repeated sequences.

    A span is ``(first, count)`` into ``BT_AUDIO_CLIP_SECONDS`` where the run is
    always contiguous: an identical ordered sequence is interned once, so the
    shared unit.tscn arrays used by all 66 enemy kinds cost 8 rows, not 528."""

    def __init__(self, resolver):
        self.resolver = resolver
        self.paths = []
        self.spans = {}
        self.seconds = {}       # res path -> float (source bytes read once)

    def _duration(self, res_path):
        cached = self.seconds.get(res_path)
        if cached is None:
            cached = weapons.audio_clip_duration(self.resolver, res_path)
            self.seconds[res_path] = cached
        return cached

    def span(self, clip_paths):
        key = tuple(clip_paths)
        if not key:
            return (0, 0)
        span = self.spans.get(key)
        if span is not None:
            return span
        first = len(self.paths)
        for clip in key:
            self._duration(clip)            # validates the resource exists
            self.paths.append(clip)
        span = (first, len(key))
        self.spans[key] = span
        return span

    def duration(self, res_path):
        return self._duration(res_path)


# --------------------------------------------------------------------------
# Extraction
# --------------------------------------------------------------------------
def extract(args):
    resolver = physics.Resolver([args.campaign_root, args.dlc_root])

    # The canonical item/weapon graph, built exactly like extract_items.emit_all
    # but WITHOUT writing any header: it freezes the BT_ITEM_WSTATS row order and
    # the set of reachable effect rows (for OneShotOnHitEffect.sound_one_shot).
    data = items.build(args)
    wdata = weapons.build_weapons(data, args)
    builder = data["builder"]

    table = ClipTable(resolver)
    report = {"events": [], "kinds": [], "neutrals": [], "wstats": [],
              "one_shot_rows": [], "custom_sound_overrides": []}

    def event_span(name, clips, source, direct=False):
        first, count = table.span(clips)
        report["events"].append({
            "id": EVENT_INDEX[name], "name": name, "source": source,
            "direct": direct, "first": first, "count": count, "clips": list(clips),
        })
        return (first, count)

    events = {}
    events["BT_AUDIO_EV_PLAYER_HURT"] = event_span(
        "BT_AUDIO_EV_PLAYER_HURT",
        scene_sound_array(resolver, PLAYER_SCENE, "hurt_sounds"),
        "player.gd:440 Utils.get_rand_element(hurt_sounds); player.gd:507 play pitch 0.2 force")
    events["BT_AUDIO_EV_PLAYER_DODGE"] = event_span(
        "BT_AUDIO_EV_PLAYER_DODGE",
        scene_sound_array(resolver, PLAYER_SCENE, "dodge_sounds"),
        "player.gd:442 Utils.get_rand_element(dodge_sounds) [second draw, only when dodging]")
    events["BT_AUDIO_EV_HP_REGEN"] = event_span(
        "BT_AUDIO_EV_HP_REGEN",
        scene_sound_array(resolver, PLAYER_SCENE, "hp_regen_sounds"),
        "player.gd:736 SoundManager.play(pick(hp_regen_sounds), get_heal_db(), 0.1)")
    events["BT_AUDIO_EV_ALIEN_EYES"] = event_span(
        "BT_AUDIO_EV_ALIEN_EYES",
        scene_sound_array(resolver, PLAYER_SCENE, "alien_sounds"),
        "player.gd:855 SoundManager.play(pick(alien_sounds), 0, 0.1)")
    events["BT_AUDIO_EV_GOLD_PICKUP"] = event_span(
        "BT_AUDIO_EV_GOLD_PICKUP",
        scene_sound_array(resolver, MAIN_SCENE, "gold_pickup_sounds"),
        "main.gd:804 SoundManager.play(pick(gold_pickup_sounds), 0, 0.2)")
    events["BT_AUDIO_EV_LEVEL_UP"] = event_span(
        "BT_AUDIO_EV_LEVEL_UP",
        scene_sound_array(resolver, MAIN_SCENE, "level_up_sound"),
        "main.gd:863 SoundManager.play(level_up_sound, 0, 0, true) [DIRECT resource, no draw]",
        direct=True)
    events["BT_AUDIO_EV_END_WAVE"] = event_span(
        "BT_AUDIO_EV_END_WAVE",
        scene_sound_array(resolver, MAIN_SCENE, "end_wave_sounds"),
        "main.gd:958 SoundManager.play(pick(end_wave_sounds))")
    events["BT_AUDIO_EV_RUN_WON"] = event_span(
        "BT_AUDIO_EV_RUN_WON",
        scene_sound_array(resolver, MAIN_SCENE, "run_won_sounds"),
        "main.gd:1030 SoundManager.play(pick(run_won_sounds), -5, 0, true)")
    events["BT_AUDIO_EV_RUN_LOST"] = event_span(
        "BT_AUDIO_EV_RUN_LOST",
        scene_sound_array(resolver, MAIN_SCENE, "run_lost_sounds"),
        "main.gd:455 SoundManager.play(pick(run_lost_sounds), -5, 0, true)")
    events["BT_AUDIO_EV_WEAPON_BREAK"] = event_span(
        "BT_AUDIO_EV_WEAPON_BREAK",
        script_preload_array(resolver, WEAPON_SERVICE_GD, "breaking_sounds"),
        "player.gd:407 SoundManager.play(pick(WeaponService.breaking_sounds), -15, 0.1, true)")
    events["BT_AUDIO_EV_BULL_EXPLODE"] = event_span(
        "BT_AUDIO_EV_BULL_EXPLODE",
        resource_sound_array(resolver, BULL_EXPLOSION_STATS, "shooting_sounds"),
        "player.gd:560 if stats.shooting_sounds.size() > 0: play(pick, db, 0.2, true) "
        "[bull_item character explode_on_hit stats]")
    events["BT_AUDIO_EV_GOLD_ALT_PICKUP"] = event_span(
        "BT_AUDIO_EV_GOLD_ALT_PICKUP",
        scene_sound_array(resolver, MAIN_SCENE, "gold_alt_pickup_sounds"),
        "main.gd:802 SoundManager.play(pick(gold_alt_pickup_sounds), -5, 0.2) "
        "[only when ProgressData.settings.alt_gold_sounds]")

    # ---- PlayerExplosion node sounds (own scene root export) ---------------
    # Guard: any scene carrying `explosion_sounds` must be declared in
    # EXPLOSION_SCENES, so a new explosion scene cannot be silently unwired.
    declared_explosion = {scene for _, scene in EXPLOSION_SCENES}
    discovered_explosion = set(scenes_with_prop(resolver.roots, "explosion_sounds"))
    if discovered_explosion != declared_explosion:
        raise ExtractError(
            "explosion_sounds scene set changed: declared %s, discovered %s"
            % (sorted(declared_explosion), sorted(discovered_explosion)))
    for name, scene in EXPLOSION_SCENES:
        events[name] = event_span(
            name,
            scene_sound_array(resolver, scene, "explosion_sounds"),
            "%s root PlayerExplosion.explosion_sounds; player_explosion.gd:31 "
            "SoundManager2D.play(pick(explosion_sounds), pos, sound_db_mod, 0.2) -- one draw "
            "per freshly instantiated node (gated on the node's sound_db_mod > -50)" % scene)

    # ---- the same sound keyed by the canonical BT_ITEM_SCENES row ----------
    # The runtime can only match on the ROW (BT_ITEM_SCENES[row].scene is an
    # interned istr; there is no host-only scene path array at runtime), so the
    # scene view is published too.  Both views are asserted equal here.
    scene_rows = builder.scene_rows
    scene_row_paths = [builder.strings.values[row["scene"]] for row in scene_rows]
    row_by_path = {}
    for row_index, scene_path in enumerate(scene_row_paths):
        row_by_path.setdefault(scene_path, row_index)
    explnode_spans = []
    for scene_path in scene_row_paths:
        explnode_spans.append(
            table.span(scene_sound_array(resolver, scene_path, "explosion_sounds")))
    nonzero_rows = {(i, scene_row_paths[i]) for i, span in enumerate(explnode_spans) if span[1]}
    declared_rows = {}
    for name, scene in EXPLOSION_SCENES:
        if scene not in row_by_path:
            raise ExtractError("%s (explosion-node sound) is not a BT_ITEM_SCENES row" % scene)
        declared_rows[scene] = (row_by_path[scene], name)
    if nonzero_rows != {(row, scene) for scene, (row, _) in declared_rows.items()}:
        raise ExtractError(
            "BT_ITEM_SCENES rows with a root explosion_sounds array do not match "
            "EXPLOSION_SCENES: rows %s vs declared %s"
            % (sorted(nonzero_rows), sorted((row, scene)
                                            for scene, (row, _) in declared_rows.items())))
    for scene, (row, name) in declared_rows.items():
        if explnode_spans[row] != events[name]:
            raise ExtractError(
                "explosion-node span drift at BT_ITEM_SCENES row %d (%s): row %s vs event %s"
                % (row, scene, explnode_spans[row], events[name]))

    # ---- per enemy kind (canonical BT_NKINDS order) ------------------------
    gdata = gameplay.build(args)
    enemy_kinds = gdata["enemy_kinds"]
    neutral_kinds = gdata["neutral_kinds"]
    if gdata["resolver"].roots != resolver.roots:
        raise ExtractError("kind enumeration used a different resolver root set")

    def kind_arrays(scene):
        return {
            "hurt": scene_sound_array(resolver, scene, "hurt_sounds"),
            "crit": scene_sound_array(resolver, scene, "crit_sounds"),
            "burn": scene_sound_array(resolver, scene, "burn_sounds"),
        }

    kind_spans = []
    for scene in enemy_kinds:
        arrays = kind_arrays(scene)
        spans = {key: table.span(clips) for key, clips in arrays.items()}
        kind_spans.append(spans)
        report["kinds"].append({
            "index": len(kind_spans) - 1, "scene": scene,
            "hurt": arrays["hurt"], "crit": arrays["crit"], "burn": arrays["burn"],
        })

    neutral_spans = []
    for scene in neutral_kinds:
        arrays = kind_arrays(scene)
        spans = {key: table.span(clips) for key, clips in arrays.items()}
        neutral_spans.append(spans)
        report["neutrals"].append({
            "index": len(neutral_spans) - 1, "scene": scene,
            "hurt": arrays["hurt"], "crit": arrays["crit"], "burn": arrays["burn"],
        })

    # ---- canonical BT_ITEM_WSTATS rows ------------------------------------
    # builder.wstats maps res path -> row index; invert to keep the row order.
    stats_path_by_index = {}
    for res_path, index in builder.wstats.items():
        if index in stats_path_by_index and stats_path_by_index[index] != res_path:
            raise ExtractError("BT_ITEM_WSTATS row %d claimed by two resources" % index)
        stats_path_by_index[index] = res_path
    rows = builder.wstats_rows
    if sorted(stats_path_by_index) != list(range(len(rows))):
        raise ExtractError("BT_ITEM_WSTATS row order is not contiguous")

    wstats_spans = []
    for index in range(len(rows)):
        res_path = stats_path_by_index[index]
        clips = resource_sound_array(resolver, res_path, "shooting_sounds")
        wstats_spans.append(table.span(clips))
        report["wstats"].append({
            "index": index, "resource": res_path, "clips": clips,
        })

    # ---- OneShotOnHitEffect.sound_one_shot (reachable effect rows) ---------
    one_shot_clips = []
    for row_index, row in enumerate(builder.rows):
        if row is None:
            continue
        pf = resolver.parse(row["source"])
        if not pf.root_props.get("sound_one_shot"):
            continue
        clips = _clips_from_prop(pf, pf.root_props["sound_one_shot"],
                                 "%s.sound_one_shot" % row["source"])
        if not clips:
            continue
        if clips not in one_shot_clips:
            one_shot_clips.append(clips)
        report["one_shot_rows"].append({
            "row": row_index, "source": row["source"], "clips": clips,
        })
    if len(one_shot_clips) > 1:
        raise ExtractError(
            "OneShotOnHitEffect ships %d distinct sound_one_shot clips; a single "
            "event span cannot key them -- wire per effect row instead: %s"
            % (len(one_shot_clips), one_shot_clips))
    events["BT_AUDIO_EV_ONE_SHOT"] = event_span(
        "BT_AUDIO_EV_ONE_SHOT",
        one_shot_clips[0] if one_shot_clips else [],
        "unit.gd:750 SoundManager2D.play(effect.sound_one_shot, pos, 0, 0.2) [DIRECT stream]",
        direct=True)

    # ---- custom_sound override provenance ---------------------------------
    report["custom_sound_overrides"] = [
        {"site": "unit.gd:672 (_on_BurningTimer_timeout)",
         "value": "Utils.get_rand_element(burn_sounds)",
         "note": "TakeDamageArgs.custom_sound; unit.gd:298 still draws hurt_sounds and "
                 "unit.gd:301 still draws crit_sounds on a crit before the override "
                 "applies, so a burn tick costs burn+hurt (+crit) draws."},
        {"site": "unit.gd:750 (_on_one_shotted)",
         "value": "effect.sound_one_shot (direct, no RNG draw)",
         "note": "resolved from the reachable OneShotOnHitEffect rows; see one_shot_rows."},
    ]

    return {
        "resolver": resolver,
        "builder": builder,
        "wdata": wdata,
        "table": table,
        "events": events,
        "kind_spans": kind_spans,
        "neutral_spans": neutral_spans,
        "enemy_kinds": enemy_kinds,
        "neutral_kinds": neutral_kinds,
        "wstats_spans": wstats_spans,
        "explnode_spans": explnode_spans,
        "scene_row_paths": scene_row_paths,
        "report": report,
    }


# --------------------------------------------------------------------------
# Emission
# --------------------------------------------------------------------------
def _clip_comment(res_path):
    return res_path[len("res://"):] if res_path.startswith("res://") else res_path


def emit_audio_data(data, out_path):
    table = data["table"]
    lines = []
    lines.append("/* Generated by ocean/brotato/extract_audio.py -- DO NOT EDIT.")
    lines.append(" *")
    lines.append(" * Gameplay-audio event metadata for the non-rendering simulation.  Each")
    lines.append(" * clip length is the true AudioStream.get_length() value read from the")
    lines.append(" * real .wav/.mp3/.ogg source bytes (RIFF/WAVE chunk sizes, MPEG frame scan,")
    lines.append(" * Ogg granule) -- never guessed, never a fixed default.")
    lines.append(" *")
    lines.append(" * A span (first, count) indexes BT_AUDIO_CLIP_SECONDS[first .. first+count).")
    lines.append(" * count == 0 means the source's `size() > 0` guard is false: the event is")
    lines.append(" * skipped with NO RNG draw at all.  Events marked DIRECT are a single")
    lines.append(" * Resource/AudioStream played by value (no Array.pick_random): count == 1")
    lines.append(" * and the caller must NOT draw.  Every other event draws exactly ONE")
    lines.append(" * global bounded pick -- `bt_rng_bounded(&s->rng, count)` -- even when")
    lines.append(" * count == 1 (Array.pick_random always draws).")
    lines.append(" *")
    lines.append(" * Per enemy kind (BT_KINDS order) and per neutral (BT_NEUTRALS order) the")
    lines.append(" * hurt / crit / burn spans are resolved through packed-scene inheritance")
    lines.append(" * from unit.tscn; identical inherited sequences share one span.")
    lines.append(" *")
    lines.append(" * BT_AUDIO_WSTATS_SHOOT_* is indexed by the canonical BT_ITEM_WSTATS row")
    lines.append(" * (extract_items.py): both the exploding-effect sound (player.gd:560) and")
    lines.append(" * the special-spawn projectile sound (weapon_service.gd:350) use")
    lines.append(" * `weapon_stats.shooting_sounds` of that stats row.")
    lines.append(" */")
    lines.append("#pragma once")
    lines.append('#include "brotato_rng.h"          /* BT_TABLE / BT_HOST_ONLY */')
    lines.append('#include "brotato_waves.h"        /* BT_NKINDS / BT_NNEUTRALS */')
    lines.append('#include "brotato_item_effects.h" /* BT_NITEM_WSTATS */')
    lines.append("")
    nkinds = len(data["enemy_kinds"])
    nneutrals = len(data["neutral_kinds"])
    nwstats = len(data["wstats_spans"])
    lines.append("#if defined(BT_NKINDS) && BT_NKINDS != %d" % nkinds)
    lines.append('#error "brotato_audio_data.h: BT_NKINDS changed; regenerate with extract_audio.py"')
    lines.append("#endif")
    lines.append("#if defined(BT_NNEUTRALS) && BT_NNEUTRALS != %d" % nneutrals)
    lines.append('#error "brotato_audio_data.h: BT_NNEUTRALS changed; regenerate with extract_audio.py"')
    lines.append("#endif")
    lines.append("#if defined(BT_NITEM_WSTATS) && BT_NITEM_WSTATS != %d" % nwstats)
    lines.append('#error "brotato_audio_data.h: BT_NITEM_WSTATS changed; regenerate with extract_audio.py"')
    lines.append("#endif")
    lines.append("#if defined(BT_NITEM_SCENES) && BT_NITEM_SCENES != %d" % len(data["explnode_spans"]))
    lines.append('#error "brotato_audio_data.h: BT_NITEM_SCENES changed; regenerate with extract_audio.py"')
    lines.append("#endif")
    lines.append("")

    # clip table
    lines.append("#define BT_NAUDIO_CLIPS %d" % max(1, len(table.paths)))
    lines.append("BT_TABLE float BT_AUDIO_CLIP_SECONDS[BT_NAUDIO_CLIPS] = {")
    for res_path in table.paths:
        lines.append("    %s,   /* %s */"
                     % (_f32_seconds(table.duration(res_path)), _clip_comment(res_path)))
    if not table.paths:
        lines.append("    0.0f,")
    lines.append("};")
    lines.append("")
    lines.append("BT_HOST_ONLY(static const char* const BT_AUDIO_CLIP_PATH[BT_NAUDIO_CLIPS] = {")
    if table.paths:
        for res_path in table.paths:
            lines.append('    "%s",' % res_path)
    else:
        lines.append('    "",')
    lines.append("};)")
    lines.append("")

    # event ids + spans
    lines.append("/* Event ids.  Order is the ABI consumed by BT_AUDIO_EV_FIRST/COUNT. */")
    for index, (name, _) in enumerate(EVENTS):
        lines.append("#define %s %d" % (name, index))
    lines.append("#define BT_NAUDIO_EVENTS %d" % max(1, len(EVENTS)))
    lines.append("BT_TABLE unsigned short BT_AUDIO_EV_FIRST[BT_NAUDIO_EVENTS] = {")
    for name, _ in EVENTS:
        lines.append("    %d," % data["events"][name][0])
    lines.append("};")
    lines.append("BT_TABLE unsigned short BT_AUDIO_EV_COUNT[BT_NAUDIO_EVENTS] = {")
    for name, _ in EVENTS:
        lines.append("    %d," % data["events"][name][1])
    lines.append("};")
    lines.append("")
    lines.append("BT_HOST_ONLY(static const char* const BT_AUDIO_EV_NAME[BT_NAUDIO_EVENTS] = {")
    for name, _ in EVENTS:
        lines.append('    "%s",' % name)
    lines.append("};)")
    lines.append("")
    lines.append("/* EVENT -> source site + call (pitch/queue live in the source; the runtime")
    lines.append(" * owns the manager choice, published here for provenance).")
    for event in data["report"]["events"]:
        lines.append(" *   %-28s %s" % (event["name"], event["source"]))
    lines.append(" */")
    lines.append("")

    # per-kind
    for stem, spans in (("KIND", data["kind_spans"]), ("NEUTRAL", data["neutral_spans"])):
        n = len(spans)
        lines.append("#define BT_NAUDIO_%s %d" % (stem, max(1, n)))
        for key in ("HURT", "CRIT", "BURN"):
            lines.append("BT_TABLE unsigned short BT_AUDIO_%s_%s_FIRST[BT_NAUDIO_%s] = {"
                         % (stem, key, stem))
            for span in spans:
                lines.append("    %d," % span[key.lower()][0])
            lines.append("};")
            lines.append("BT_TABLE unsigned short BT_AUDIO_%s_%s_COUNT[BT_NAUDIO_%s] = {"
                         % (stem, key, stem))
            for span in spans:
                lines.append("    %d," % span[key.lower()][1])
            lines.append("};")
        lines.append("")

    # wstats
    lines.append("/* Canonical BT_ITEM_WSTATS row -> its WeaponStats.shooting_sounds clips")
    lines.append(" * (player.gd:560 exploding effect / weapon_service.gd:350 special spawn).")
    lines.append(" * count == 0 => the source guard skips both the pick and the play. */")
    lines.append("#define BT_NAUDIO_WSTATS_SHOOT %d" % max(1, nwstats))
    lines.append("BT_TABLE unsigned short BT_AUDIO_WSTATS_SHOOT_FIRST[BT_NAUDIO_WSTATS_SHOOT] = {")
    for span in data["wstats_spans"]:
        lines.append("    %d," % span[0])
    lines.append("};")
    lines.append("BT_TABLE unsigned short BT_AUDIO_WSTATS_SHOOT_COUNT[BT_NAUDIO_WSTATS_SHOOT] = {")
    for span in data["wstats_spans"]:
        lines.append("    %d," % span[1])
    lines.append("};")
    lines.append("")
    lines.append("/* Explosion-node sound per BT_ITEM_SCENES row (device-side scene matching:")
    lines.append(" * BT_ITEM_SCENES[row].scene is an interned istr and there is no host-only")
    lines.append(" * path array, so the runtime can only key on the ROW).  0-count = the")
    lines.append(" * scene's root node defines no explosion_sounds.  Non-zero rows are exactly")
    lines.append(" * the BT_AUDIO_EV_EXPLOSION_NODE / BT_AUDIO_EV_FLESH_EXPLOSION_NODE scenes")
    lines.append(" * (asserted at generation time). */")
    lines.append("#define BT_NAUDIO_EXPLNODE_SCENES BT_NITEM_SCENES")
    lines.append("BT_TABLE unsigned short BT_AUDIO_EXPLNODE_FIRST[BT_NAUDIO_EXPLNODE_SCENES] = {")
    for span in data["explnode_spans"]:
        lines.append("    %d," % span[0])
    lines.append("};")
    lines.append("BT_TABLE unsigned short BT_AUDIO_EXPLNODE_COUNT[BT_NAUDIO_EXPLNODE_SCENES] = {")
    for span in data["explnode_spans"]:
        lines.append("    %d," % span[1])
    lines.append("};")
    lines.append("")
    return items._write("\n".join(lines), out_path)


def emit_provenance(data, out_path, args):
    resolver = data["resolver"]
    table = data["table"]
    report = data["report"]
    payload = {
        "generator": "ocean/brotato/extract_audio.py",
        "campaign_root": args.campaign_root,
        "dlc_root": args.dlc_root,
        "counts": {
            "clips": len(table.paths),
            "events": len(EVENTS),
            "enemy_kinds": len(data["enemy_kinds"]),
            "neutral_kinds": len(data["neutral_kinds"]),
            "wstats_rows": len(data["wstats_spans"]),
            "explnode_rows": len(data["explnode_spans"]),
            "one_shot_rows": len(report["one_shot_rows"]),
        },
        "clips": [
            {"row": i, "path": path, "file": resolver.find(path),
             "seconds": table.duration(path)}
            for i, path in enumerate(table.paths)
        ],
        "events": report["events"],
        "kinds": report["kinds"],
        "neutrals": report["neutrals"],
        "wstats": report["wstats"],
        "explnode": [
            {"row": i, "scene": path, "first": span[0], "count": span[1]}
            for i, (path, span) in enumerate(
                zip(data["scene_row_paths"], data["explnode_spans"]))
        ],
        "one_shot_rows": report["one_shot_rows"],
        "custom_sound_overrides": report["custom_sound_overrides"],
    }
    return items._write(json.dumps(payload, indent=1, sort_keys=False) + "\n", out_path)


# --------------------------------------------------------------------------
def main(argv):
    parser = argparse.ArgumentParser(description="Extract Brotato gameplay-audio metadata.")
    parser.add_argument("--campaign-root", required=True)
    parser.add_argument("--dlc-root", required=True)
    parser.add_argument("--out", default="ocean/brotato/brotato_audio_data.h")
    parser.add_argument("--provenance-out", default="")
    args = parser.parse_args(argv)
    try:
        data = extract(args)
        emit_audio_data(data, args.out)
    except ExtractError as exc:
        sys.stderr.write("extract_audio: error: %s\n" % exc)
        return 1
    if args.provenance_out:
        emit_provenance(data, args.provenance_out, args)
    report = data["report"]
    sys.stderr.write(
        "extract_audio: %d clips, %d events, %d enemy kinds, %d neutrals, "
        "%d wstats rows -> %s\n"
        % (len(data["table"].paths), len(EVENTS), len(data["enemy_kinds"]),
           len(data["neutral_kinds"]), len(data["wstats_spans"]), args.out))
    for event in report["events"]:
        sys.stderr.write(
            "extract_audio:   %-28s first=%d count=%d%s\n"
            % (event["name"], event["first"], event["count"],
               " (DIRECT, no draw)" if event["direct"] else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
