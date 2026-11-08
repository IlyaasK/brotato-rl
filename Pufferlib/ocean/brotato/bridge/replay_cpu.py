#!/usr/bin/env python3
"""Replay every live-game action through the shared CPU core and gate divergence.

A MATCH requires aligned initialization and RNG consumption as well as matching
rules. Different engine/core RNG streams are reported as divergence, not hidden.

Initialization is taken from the source trace's own recorded profile
(meta.config / meta.facts) and resolved through the canonical generated tables
(characters/weapons), so a Knight/Demon/weapon run is never silently replayed as
Bull.  --character/--starting-weapon are optional explicit overrides and are
rejected if they disagree with the recording or name something unknown.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[3]


def comparator():
    path = ROOT / 'tests/brotato_trace_compare.py'
    spec = importlib.util.spec_from_file_location('brotato_replay_compare', path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def contract_module():
    """The bridge's own contract reader, so the width/action bound match
    state_contract.json (the payload's schema) instead of a literal here."""
    path = ROOT / 'ocean/brotato/bridge/parse_trace.py'
    spec = importlib.util.spec_from_file_location('brotato_replay_contract', path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


# Canonical generated tables (brotato_weapons.h / brotato_characters.h order),
# emitted by ocean/brotato/bridge/gen_obs_tables.py.  The same artifact the
# bridge loads; replay resolves the source trace's recorded profile through it so
# the replayed initialization is the run's, not a hardcoded Bull.
def default_tables_path() -> Path:
    override = os.environ.get('BROTATO_RL_OBS_TABLES') or os.environ.get('BROTATO_OBS_TABLES')
    if override and not override.startswith('res://'):
        return Path(override)
    external = os.environ.get('BROTATO_EXTERNAL_ROOT', '')
    if external:
        return Path(external) / 'brotato_obs_tables.json'
    # Repository migration copy, then the historical macOS location.
    repo = ROOT / 'migration' / 'game' / 'external' / 'brotato_obs_tables.json'
    if repo.is_file():
        return repo
    return Path.home() / 'brotato_mod' / 'external' / 'brotato_obs_tables.json'


def load_tables(path: Path) -> dict:
    if not path.is_file():
        raise TraceError(f'no observation tables at {path} (generate with gen_obs_tables.py)')
    tables = json.loads(path.read_text())
    if not isinstance(tables, dict):
        raise TraceError(f'observation tables at {path} are not a JSON object')
    return tables


class TraceError(Exception):
    """Structural / profile problem in the trace or tables."""


def resolve_character(tables: dict, name: str):
    """-> (canonical id, canonical id string) or (None, None)."""
    for index, row in enumerate(tables.get('characters') or []):
        if row.get('id') == name or row.get('name') == name:
            return index, str(row.get('id', name))
    return None, None


NOTHING = 0xFFFF


def resolve_weapon(tables: dict, name: str):
    """-> (canonical id, canonical id string).  Empty/none -> (BT_WEAPON_NONE, '')."""
    if name in ('', 'none', None):
        return NOTHING, ''
    rows = tables.get('weapons') or []
    for index, row in enumerate(rows):
        if row.get('my_id') == name:
            return index, str(row['my_id'])
    stem_hits = [index for index, row in enumerate(rows) if row.get('id') == name]
    if len(stem_hits) == 1:
        return stem_hits[0], str(rows[stem_hits[0]].get('my_id', name))
    if len(stem_hits) > 1:
        return None, None   # family stem matches several tier rows: ambiguous
    return None, None


def recorded_profile(meta) -> tuple[str, str, bool]:
    """(character name, starting-weapon name, metadata present)."""
    if not isinstance(meta, dict):
        return '', '', False
    config = meta.get('config') if isinstance(meta.get('config'), dict) else {}
    facts = meta.get('facts') if isinstance(meta.get('facts'), dict) else {}
    present = bool(config) or bool(facts)
    character = str(config.get('character_id') or facts.get('character_found') or '')
    weapon = str(config.get('starting_weapon') or facts.get('starting_weapon') or '')
    return character, weapon, present


def resolve_profile(tables: dict, meta, cli_character: str, cli_weapon: str):
    """Canonical (character_id, weapon_id, note) for the replayed initialization.

    The recorded profile wins; an explicit CLI id is checked against it and a
    disagreement, an unknown/ambiguous name, or a missing table is rejected
    rather than silently falling back to Bull.  Defaults (Bull, no weapon) apply
    only when the metadata is genuinely unreachable.
    """
    character_name, weapon_name, present = recorded_profile(meta)
    if cli_character:
        cidx, ccanon = resolve_character(tables, cli_character)
        if cidx is None:
            raise TraceError(f'--character {cli_character!r} is not in the canonical character table')
        if present and character_name:
            ridx, _ = resolve_character(tables, character_name)
            if ridx is None:
                raise TraceError(
                    f'recorded character {character_name!r} is not in the canonical character table')
            if ridx != cidx:
                raise TraceError(
                    f'--character {cli_character!r} (id {cidx}) disagrees with the recorded '
                    f'profile {character_name!r} (id {ridx})')
        character_id, char_src = cidx, f'CLI --character {cli_character}'
    elif present and character_name:
        cidx, ccanon = resolve_character(tables, character_name)
        if cidx is None:
            raise TraceError(
                f'recorded character {character_name!r} is not in the canonical character table '
                '(pass --character to override)')
        character_id, char_src = cidx, f'recorded {ccanon}'
    else:
        character_id, char_src = 0, 'default Bull (metadata unreachable)'

    if cli_weapon:
        widx, wcanon = resolve_weapon(tables, cli_weapon)
        if widx is None:
            raise TraceError(f'--starting-weapon {cli_weapon!r} is unknown or ambiguous in the weapon table')
        recorded_id, _ = resolve_weapon(tables, weapon_name) if present else (None, None)
        if recorded_id is None and weapon_name:
            raise TraceError(
                f'recorded starting weapon {weapon_name!r} is unknown or ambiguous in the weapon table')
        if present and recorded_id is not None and recorded_id != widx:
            raise TraceError(
                f'--starting-weapon {cli_weapon!r} (id {widx}) disagrees with the recorded '
                f'profile {weapon_name!r} (id {recorded_id})')
        weapon_id, weapon_src = widx, f'CLI --starting-weapon {cli_weapon}'
    elif present:
        widx, wcanon = resolve_weapon(tables, weapon_name)
        if widx is None:
            raise TraceError(
                f'recorded starting weapon {weapon_name!r} is unknown or ambiguous in the weapon table '
                '(pass --starting-weapon to override)')
        weapon_id = widx
        weapon_src = f'recorded {wcanon}' if wcanon else 'recorded none'
    else:
        weapon_id, weapon_src = NOTHING, 'default none (metadata unreachable)'

    note = f'character id {character_id} ({char_src}); weapon id {weapon_id} ({weapon_src})'
    return character_id, weapon_id, note



def _reject_non_canonical_capture(meta, meta_config, reference, weapon_id):
    """Refuse a trace the replay cannot reproduce (BridgeGaps tags these).

    The core models neither debug grants (extra weapons/items, forced crates)
    nor a non-baseline capture config, so replaying such a trace can only
    produce a tick-0 divergence that looks like a sim defect.  A canonical
    capture states capture_kind=baseline (when the bridge provides it) and
    starts with exactly the resolved starting weapon (1) or none (0)."""
    if not isinstance(meta, dict):
        return
    kind = str(meta.get('capture_kind') or meta_config.get('capture_kind') or '').strip()
    if kind == 'stress':
        raise TraceError('the trace is a stress capture (capture_kind=stress): it ran with debug '
                         'grants/forced crates, which the core does not model, so it is not '
                         'replay-comparable. Replay a baseline capture instead.')
    grants = []
    for key in ('grant_weapons', 'grant_items'):
        value = meta_config.get(key, meta.get(key))
        if value:
            grants.append(f'{key}={value!r}')
    if meta_config.get('always_crates', meta.get('always_crates')):
        grants.append('always_crates=true')
    if grants:
        raise TraceError('the recording used debug grants (' + ', '.join(grants) + '); the core '
                         'models no grants, so the replay cannot reproduce it (capture the '
                         'baseline configuration instead).')
    row = reference.records[0] if getattr(reference, 'records', None) else {}
    count = row.get('weapon_count') if isinstance(row, dict) else None
    expected = 0 if weapon_id == NOTHING else 1
    if isinstance(count, (int, float)) and not isinstance(count, bool) and int(count) != expected:
        raise TraceError(f'the recording starts with {int(count)} weapon(s) but the replay '
                         f'initializes {expected} (the resolved starting weapon only): the '
                         f'recording carries extra weapons/grants, which the core models none of.')

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('reference', type=Path)
    parser.add_argument('--output-dir', type=Path, required=True)
    parser.add_argument('--seed', type=int, default=None,
                        help='0 = the trace metadata seed (meta.config.seed)')
    parser.add_argument('--zone', type=int, choices=(0, 1), default=None,
                        help='default: the trace metadata zone (meta.config.zone)')
    parser.add_argument('--atol', type=float, default=1e-5)
    parser.add_argument('--rtol', type=float, default=1e-5)
    parser.add_argument('--contract', type=Path, default=None,
                        help='state contract whose observation width and action count gate the replay')
    parser.add_argument('--obs-size', type=int, default=0,
                        help='override the contract observation width (0 = the contract size)')
    parser.add_argument('--tables', type=Path, default=None,
                        help='canonical weapon/character tables (default: the external obs tables)')
    parser.add_argument('--character', default='',
                        help='explicit character id/name; must agree with the recorded profile')
    parser.add_argument('--start-wave', type=int, default=None,
                        help='wave the recording starts on; default: the reference trace\'s first row '
                             '(forwarded to tests/brotato_core_trace.cpp as a trailing argument when > 1)')
    parser.add_argument('--particles-rng', type=int, choices=(0, 1), default=None,
                        help='particle-RNG model of the recording (1 = the sim consumes the source '
                             'CPUParticles2D draws, 0 = the bridge suppressed them); default: the '
                             'trace metadata particles_rng field, else 1')
    parser.add_argument('--audio-rng', type=int, choices=(0, 1), default=None,
                        help='audio-RNG model of the recording (1 = the sim consumes the source sound '
                             'draws, 0 = the bridge neutralized them); default: the trace metadata '
                             'audio_rng field, else 1')
    parser.add_argument('--invulnerable', type=int, choices=(0, 1), default=None,
                        help='DebugService.invulnerable of the recording (1 = the player Hurtbox was '
                             'disabled, so no enemy contact/projectile hit is registered; 0 = the '
                             'normal player hurtbox); default: the trace metadata invulnerable field, '
                             'else 0')
    parser.add_argument('--endless', type=int, choices=(0, 1), default=None,
                        help='RunData.is_endless_run of the recording (1 = the run continued past the '
                             'wave-20 clear into waves 21+; 0 = the campaign, where a wave-20 clear '
                             'ends the episode); default: the trace metadata endless field, else 0')
    parser.add_argument('--starting-weapon', default='',
                        help='explicit starting weapon id; must agree with the recorded profile')
    args = parser.parse_args(argv)
    if (args.seed is not None and not 0 <= args.seed < 2**64) or args.atol < 0 or args.rtol < 0:
        parser.error('seed must be uint64; tolerances must be non-negative')
    gate = comparator()
    contract_reader = contract_module()
    contract_path = args.contract or contract_reader.DEFAULT_CONTRACT
    tables_path = args.tables or default_tables_path()
    try:
        contract = contract_reader.load_contract(Path(contract_path))
    except contract_reader.TraceError as error:
        print(f'replay invalid: {error}', file=sys.stderr)
        return gate.EXIT_INVALID
    obs_size = args.obs_size or contract_reader.contract_obs_length(contract)
    action_count = contract_reader.contract_action_count(contract)
    if obs_size <= 0:
        print(f'replay invalid: {contract_path} declares no observation width', file=sys.stderr)
        return gate.EXIT_INVALID
    try:
        meta = json.loads(Path(str(args.reference) + '.meta.json').read_text()) \
            if Path(str(args.reference) + '.meta.json').is_file() else {}
        # The trace's own recorded run configuration wins over the CLI defaults:
        # a Demon end (or any other profile/zone/difficulty) run must never be
        # replayed as seed-1 zone-0 Bull.  The core models Danger 5 only, so a
        # different danger is refused instead of silently mismatched.
        meta_config = meta.get('config') if isinstance(meta.get('config'), dict) else {}
        if args.seed is None:
            try:
                args.seed = int(meta_config.get('seed') or 1)
            except (TypeError, ValueError):
                args.seed = 1
        if args.zone is None:
            try:
                args.zone = int(meta_config.get('zone') or 0)
            except (TypeError, ValueError):
                args.zone = 0
            if args.zone not in (0, 1):
                raise TraceError(f'recorded zone {args.zone} is not modelled (0 or 1)')
        # The trace's OWN observation width (recorded in its metadata) wins over
        # the live contract: a v3 trace (161 floats) is still replayed after the
        # core grew a v4 block, by emitting that trace's prefix.
        meta_obs = (meta.get('obs_definition') or {}).get('obs_size') \
            if isinstance(meta.get('obs_definition'), dict) else None
        if isinstance(meta_obs, int) and meta_obs > 0:
            obs_size = meta_obs
        recorded_danger = meta_config.get('danger', meta.get('facts', {}).get('difficulty')
                                         if isinstance(meta.get('facts'), dict) else None)
        if recorded_danger is not None and int(recorded_danger) != 5:
            raise TraceError(f'recorded danger {recorded_danger} is not modelled (core is Danger 5)')
        tables = load_tables(Path(tables_path))
        character_id, weapon_id, profile_note = resolve_profile(
            tables, meta, args.character, args.starting_weapon)
        print(f'replay profile: {profile_note}')
        reference = gate.load_trace(str(args.reference), obs_size=obs_size)
        _reject_non_canonical_capture(meta, meta_config, reference, weapon_id)
        previous = None
        inputs = []
        for row in reference.records:
            tick, action = row['tick'], row['action']
            if row.get('obs_complete', 1) != 1:
                raise gate.TraceError(f'tick {tick}: incomplete live observation')
            if previous is not None and tick != previous+1:
                raise gate.TraceError(f'tick {tick}: replay needs every physics tick/action')
            previous = tick
            if isinstance(action, list) and len(action) == 1:
                action = action[0]
            if (not isinstance(action, (int, float)) or isinstance(action, bool)
                    or int(action) != action
                    or (action_count and not 0 <= action < action_count)):
                raise gate.TraceError(
                    f'tick {tick}: replay expects one discrete action 0..{action_count - 1} '
                    f'(contract {contract_path})')
            if tick == reference.records[0]["tick"] and (tick != 0 or action != 0):
                raise gate.TraceError("replay must start at tick 0 with the initial hold snapshot")
            inputs.append(f'{tick} {int(action)}\n')
        args.output_dir.mkdir(parents=True, exist_ok=True)
        candidate_path = args.output_dir / 'cpu_trace.jsonl'
        with tempfile.TemporaryDirectory(prefix='brotato-replay-') as temporary:
            executable = Path(temporary) / 'core_trace'
            subprocess.run([os.environ.get('CXX', 'clang++'), '-std=c++17', '-O2',
                '-ffp-contract=off', str(ROOT/'tests/brotato_core_trace.cpp'),
                '-o', str(executable)], check=True)
            # The wave duration the trace recorded (the bridge's instant_waves
            # debug knob records 1-6 s waves) and the observation width the
            # trace's contract declares (the live core may have appended blocks).
            wave_duration = 0.0
            for row in reference.records:
                wd = row.get('wave_duration')
                if isinstance(wd, (int, float)) and not isinstance(wd, bool) and wd > 0:
                    wave_duration = float(wd)
                    break
            # The wave the recording starts on (DebugService.starting_wave): the
            # trace's first row carries it and the harness takes it as argv[7].
            # --start-wave overrides the recorded value.
            first_wave = reference.records[0].get('wave') if reference.records else None
            trace_start_wave = int(first_wave) if isinstance(first_wave, (int, float)) \
                and not isinstance(first_wave, bool) and first_wave >= 1 else 1
            start_wave = args.start_wave if args.start_wave is not None else trace_start_wave
            # The recording's audio-RNG model: the bridge neutralizes the sound
            # queues/arrays in the RL configuration it records, so a trace
            # recorded with that neutralization must be replayed with the same
            # consumption.  Legacy traces carry no field -> 1 (source draws).
            audio_rng = args.audio_rng
            if audio_rng is None:
                for src in (meta, meta_config):
                    if isinstance(src, dict) and src.get('audio_rng') is not None:
                        try:
                            audio_rng = 1 if int(src['audio_rng']) != 0 else 0
                        except (TypeError, ValueError):
                            audio_rng = None
                        if audio_rng is not None:
                            break
            if audio_rng is None:
                audio_rng = 1
            # argv[7] must be present for argv[8] to be read positionally.
            harness_argv = [str(executable), str(args.seed), str(args.zone), str(character_id),
                            str(weapon_id), str(obs_size), repr(wave_duration), '0']
            if audio_rng == 0:
                print('replay audio_rng: 0 (bridge-neutralized sound draws)')
            harness_argv.append(str(int(audio_rng)))
            particles_rng = args.particles_rng
            if particles_rng is None:
                for src in (meta, meta_config):
                    if isinstance(src, dict) and src.get('particles_rng') is not None:
                        try:
                            particles_rng = 1 if int(src['particles_rng']) != 0 else 0
                        except (TypeError, ValueError):
                            particles_rng = None
                        if particles_rng is not None:
                            break
            if particles_rng is None:
                particles_rng = 1
            if particles_rng == 0:
                print('replay particles_rng: 0 (bridge-suppressed CPUParticles2D draws)')
            harness_argv.append(str(int(particles_rng)))
            # The recording's DebugService.invulnerable (meta.config.invulnerable,
            # written by the bridge from BROTATO_RL_INVULNERABLE): the core models
            # it exactly (BtState.invulnerable), so a baseline capture recorded
            # invulnerable replays with the flag.  Legacy traces carry no field
            # -> 0 (the normal player hurtbox).
            invulnerable = args.invulnerable
            if invulnerable is None:
                for src in (meta, meta_config):
                    if isinstance(src, dict) and src.get('invulnerable') is not None:
                        try:
                            invulnerable = 1 if int(src['invulnerable']) != 0 else 0
                        except (TypeError, ValueError):
                            invulnerable = None
                        if invulnerable is not None:
                            break
            if invulnerable is None:
                invulnerable = 0
            if invulnerable:
                print('replay invulnerable: 1 (player Hurtbox disabled; no enemy contact/'
                      'projectile hit registered)')
            harness_argv.append(str(int(invulnerable)))
            # The recording's RunData.is_endless_run (meta.config.endless): 1
            # continues past the wave-20 clear.  Without it an endless capture
            # replays as a campaign run that ENDS at wave 20, and bt_core_step's
            # own reset makes the candidate loop wave 20 forever (the recorded
            # run advances to wave 21+).  Legacy traces carry no field -> 0.
            endless = args.endless
            if endless is None:
                for src in (meta, meta_config):
                    if isinstance(src, dict) and src.get('endless') is not None:
                        try:
                            endless = 1 if int(src['endless']) != 0 else 0
                        except (TypeError, ValueError):
                            endless = None
                        if endless is not None:
                            break
            if endless is None:
                endless = 0
            if endless:
                print('replay endless: 1 (the run continues past the wave-20 clear)')
            harness_argv.append(str(int(endless)))
            if start_wave > 1:
                harness_argv[7] = str(int(start_wave))
                print(f'replay start wave: {start_wave} (forwarded to the harness)')
            result = subprocess.run(
                harness_argv, input=''.join(inputs), text=True, capture_output=True, check=True)
        candidate_path.write_text(result.stdout)
        candidate = gate.load_trace(str(candidate_path), obs_size=obs_size)
        # Canonicalize single-head representation without changing action values.
        for row in reference.records:
            if isinstance(row['action'], list):
                row['action'] = row['action'][0]
        result = gate.compare(reference, candidate, args.atol, args.rtol)
        report = gate.format_report(reference, candidate, args.atol, args.rtol, result)
        (args.output_dir/'comparison.txt').write_text(report+'\n')
        print(report)
        return result.exit_code
    except (TraceError, gate.TraceError, OSError, subprocess.CalledProcessError) as error:
        print(f'replay invalid: {error}', file=sys.stderr)
        return gate.EXIT_INVALID


if __name__ == '__main__':
    raise SystemExit(main())
