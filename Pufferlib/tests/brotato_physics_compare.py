#!/usr/bin/env python3
"""Compare native body transitions with measured Godot fixture frames.

The first recorded frame seeds each native scenario; all later frames are
predicted, not copied. Area signals are outside this body-solver gate.
"""
import argparse
import ctypes as c
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument('--engine')
    source.add_argument('--engine-trace', type=Path)
    parser.add_argument('--atol', type=float, default=0.002)
    args = parser.parse_args()
    if args.atol < 0:
        parser.error('--atol must be non-negative')
    with tempfile.TemporaryDirectory(prefix='brotato-physics-') as directory:
        directory = Path(directory)
        trace = args.engine_trace
        if args.engine:
            trace = directory / 'engine.jsonl'
            env = dict(os.environ, BROTATO_PHYSICS_TRACE=str(trace))
            subprocess.run([args.engine, '--path', str(ROOT/'tests/brotato_physics_project'),
                '--no-window', '--fixed-fps', '60', '--disable-audio', '--script',
                str(ROOT/'tests/brotato_physics_fixture.gd')], env=env, check=True,
                timeout=90, stdout=subprocess.DEVNULL)
        library = directory / ('physics.dylib' if os.uname().sysname == 'Darwin' else 'physics.so')
        subprocess.run([os.environ.get('CXX', 'clang++'), '-std=c++11', '-O2', '-shared',
            '-fPIC', str(ROOT/'tests/brotato_physics_native.cpp'), '-o', str(library)], check=True)
        native = c.CDLL(str(library))
        signatures = {'reset': [], 'body': [c.c_int, c.c_float, c.c_float, c.c_float, c.c_float, c.c_int],
            'wall': [c.c_int], 'velocity': [c.c_int,c.c_float,c.c_float], 'step': [],
            'player_step': [c.c_float,c.c_float], 'get': [c.c_int,c.c_int,c.POINTER(c.c_float)]}
        for name, signature in signatures.items():
            function = getattr(native, 'bt_fixture_'+name)
            function.argtypes = signature
            function.restype = None
        cases = {}
        for line in trace.read_text().splitlines():
            row = json.loads(line)
            if 'case' in row:
                cases.setdefault(row['case'], []).append(row)
        required = {'free','overlap','unequal_mass','head_on','coincident','wall',
            'kinematic_wall','filtered_player_enemy','fast_discrete'}
        if required - cases.keys():
            raise ValueError('Missing fixture cases: '+str(sorted(required-cases.keys())))
        failures = 0
        for name in sorted(required):
            rows = cases[name]
            if len(rows) != 36 or [r['frame'] for r in rows] != list(range(36)):
                raise ValueError('Incomplete frame sequence: '+name)
            native.bt_fixture_reset()
            kinematic = name in ('kinematic_wall','filtered_player_enemy')
            count = len(rows[0]['bodies'])
            for index, body in enumerate(rows[0]['bodies']):
                radius = 24 if kinematic and index == 0 or name == 'wall' else 22 if name == 'filtered_player_enemy' else 20
                native.bt_fixture_body(index,body['x'],body['y'],radius,
                    100 if name == 'unequal_mass' and index == 1 else 1,
                    int(kinematic and index == 0))
            if name in ('wall','kinematic_wall'):
                native.bt_fixture_wall(count if not kinematic else 0)
            maximum = 0.0
            first = None
            for row in rows[1:]:
                for index in range(count):
                    if kinematic and index == 0:
                        native.bt_fixture_player_step(-120 if name == 'kinematic_wall' else 120,
                            60 if name == 'kinematic_wall' else 0)
                    else:
                        vx = 120 if name in ('free','head_on') and index == 0 else -120 if name in ('head_on','filtered_player_enemy','wall') else 36000 if name == 'fast_discrete' and index == 0 else 0
                        vy = 60 if name in ('free','wall') else 0
                        native.bt_fixture_velocity(index,vx,vy)
                native.bt_fixture_step()
                for index, body in enumerate(row['bodies']):
                    values = (c.c_float*4)()
                    native.bt_fixture_get(index,int(kinematic and index == 0),values)
                    for field, value in zip(('x','y','vx','vy'),values):
                        error = abs(body[field]-value)
                        maximum = max(maximum,error)
                        if error > args.atol and first is None:
                            first = (row['frame'],index,field,body[field],value)
            failures += first is not None
            print(f'{name}: max_abs={maximum:.8g} '+('MATCH' if first is None else f'DIVERGED first={first}'))
        print(f'body cases={len(required)} divergent={failures}; area signals not compared')
        return int(failures != 0)


if __name__ == '__main__':
    raise SystemExit(main())
