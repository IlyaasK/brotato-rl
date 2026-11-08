#!/usr/bin/env bash
# Create the private config cwds the deep-endless sweep runs from.
#
# Why: the trainer only accepts `[env] start_wave` / `warp_step` / `audio_rng` when the
# key exists in the LOADED config file (`config/<env_name>.ini` resolved against the
# trainer's cwd); passing them only on the command line makes it print
# "missing key [env] <key>" and exit 1. These cwds carry the keys, so the shared
# config/brotato.ini is never touched.
#
# Usage: ./tools/bench_trainer_make_deep_cwds.sh [DEST_ROOT]   (default /home/$USER)
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="${1:-/home/$USER}"

python3 - "$ROOT" <<'EOF'
import os, shutil, sys
root = sys.argv[1]
src = open('config/brotato.ini').read()
# dest -> (start_wave, endless, warp_step, invulnerable)
specs = {
    'pt_deep25':  (25, 1, 0, 0),
    'pt_deep40':  (40, 1, 0, 0),
    'pt_deep60':  (60, 1, 0, 0),
    'pt_deep100': (100, 1, 0, 0),
    'pt_shal':    (1, 0, 0, 0),
    'pt_warp60':  (60, 1, 1, 0),
    'pt_warp1':   (1, 0, 1, 0),
    # Sustained deep-crowd instrument: [env] invulnerable = 1 (mirrors player.gd
    # disable_hurtbox; enemy AI/RNG/spawns unchanged), so episodes are not reset-dominated.
    # Needs a trainer built from a tree that contains the flag.
    'pt_inv40':   (40, 1, 0, 1),
    'pt_inv60':   (60, 1, 0, 1),
}
for name, (sw, endless, warp, invuln) in specs.items():
    d = os.path.join(root, name)
    os.makedirs(d + '/config', exist_ok=True)
    shutil.copy('config/default.ini', d + '/config/default.ini')
    out = []
    for line in src.splitlines(True):
        s = line.strip()
        if s == '[env]':
            out.append(line)
            out.append('start_wave = %d\n' % sw)
            out.append('audio_rng = 0\n')      # match the Demon/eval setting
            if warp:
                out.append('warp_step = 1\n')
            if invuln:
                out.append('invulnerable = 1\n')
            continue
        if s.startswith('endless'):
            out.append('endless = %d\n' % endless)
            continue
        if invuln and s.startswith('eval_episodes'):
            # An invulnerable endless episode never ends (no death and no wave-20
            # clear), and the post-training eval waits for `n >= eval_episodes`
            # COMPLETED episodes (src/pufferl.cu eval_loop: while(true) ... if
            # (n < eval_episodes) continue;), so an eval against this profile can
            # never return: the run would hang after the last training epoch.
            # These configs are throughput instruments, not eval targets, so skip
            # it.  (Every other generated cwd keeps its eval: a vulnerable run's
            # episodes end on death, so their eval terminates.)
            out.append('eval_episodes = 0\n')
            continue
        if s.startswith(('audio_rng', 'warp_step', 'invulnerable')):
            continue
        out.append(line)
    open(d + '/config/brotato.ini', 'w').write(''.join(out))
    for t in ('resources', 'ocean', 'src', 'vendor', 'raylib-5.5_linux_amd64'):
        if os.path.exists(t) and not os.path.exists(f'{d}/{t}'):
            os.symlink(os.path.abspath(t), f'{d}/{t}')
    print('made', d, [l.strip() for l in open(d + '/config/brotato.ini')
                      if l.startswith(('start_wave', 'endless', 'audio_rng', 'warp_step',
                                       'invulnerable', 'eval_episodes'))])
EOF
