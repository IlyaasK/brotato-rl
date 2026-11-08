# brotato-rl

A reinforcement-learning environment for **Brotato** (Danger 5, Demon, endless mode) built on PufferLib 5.0. The simulator is a C reimplementation of the game, checked against the real game frame by frame and running on CUDA.

## What's here

- `Pufferlib/ocean/brotato/`: the environment.
  - **Shared core:** `brotato_core.h` plus the `brotato_*.h` headers, compiled for CPU and CUDA (`brotato.cu`).
  - **Contract:** 203-float observation, 31 discrete actions, per-phase action mask.
  - **Shop actions:** buy weapons, items, pets and structures; reroll; lock and ban offers; recycle; combine; level-ups; crates.
- `Pufferlib/ocean/brotato/bridge/`: a Godot mod (`rlbridge-headlessrl`) that runs the real game headless, records traces, plays an exported policy (`eval_game.py`), and replays traces through the simulator (`replay_cpu.py`).
- `Pufferlib/ocean/brotato/policy/`: checkpoint → JSON exporter and the in-game GDScript policy.
- `Pufferlib/tests/*brotato*`: about 74 test programs, plus the strict CPU-vs-CUDA parity test `test_brotato_cuda.cu`.
- `Pufferlib/config/brotato_demon_endless.ini`: the training config.
- `Pufferlib/migration/HANDOFF.md`: detailed engineering log, open gaps, and how each fix was found.
- `docs/`: the original research and design notes.

Game files (the `.pck` files, the decompiled scripts and the Steam data) are **not** included. You need your own copy of Brotato.

## Status (hackathon snapshot)

- **Parity:** three recorded Demon runs (3,600 / 3,600 / 2,799 frames) match the real game frame for frame up to ticks 1977 / 1551 / 1854. Every recorded action matches. The remaining differences are named in `HANDOFF.md`.
- **CUDA:** bit-exact with the CPU on every state field.
- **Training speed:** about **1.8M env steps/s** on an RTX 5070 (4,096 environments).
- **Training result:** a 1B-step policy reaches about wave 3 on average in the simulator, buys weapons, items and pets, and fills 6 weapon slots. In the real game it still dies in wave 1, so transfer isn't solved yet.

## The real-game eval mod (`rlbridge-headlessrl`)

A Godot 3 mod that turns the real Brotato into an RL environment for evaluating policies and recording parity traces. Source: [`Pufferlib/ocean/brotato/bridge/mod/res/mods-unpacked/rlbridge-headlessrl/`](Pufferlib/ocean/brotato/bridge/mod/res/mods-unpacked/rlbridge-headlessrl/).

| File | Role |
|---|---|
| `mod_main.gd` | Boots a seeded run (character, weapon, danger, endless, start wave) and drives every physics frame: combat, shop, level-up and crate phases |
| `rl_obs.gd` | Builds the same 203-float observation as the simulator from live game state |
| `rl_inject.gd` / `rl_progress.gd` | Apply the 31 actions through the game's own input and shop/level-up UI handlers, and check that each one took effect |
| `rl_policy.gd` + `policy/brotato_policy.gd` | Run an exported policy in GDScript (MinGRU forward, phase action mask, argmax or seeded sampling) |
| `rl_trace.gd` | Writes a per-frame JSONL trace (observation, action, applied flag, state fields) plus meta and summary |
| `rl_cfg.gd` / `rl_run.gd` / `rl_env.gd` | Configuration from environment variables, run setup, and turning off cosmetic RNG (particles, sound pitch) so runs repeat exactly |

Tooling in `Pufferlib/ocean/brotato/bridge/`:
- `build_mod_pck.py`: patches the mod into a copy of your own `Brotato.pck`; the retail pack is never modified.
- `run_game.sh`: headless launcher with a watchdog, an isolated profile and validation of the trace.
- `eval_game.py`: runs N seeds in the real game and in the simulator and prints the comparison ([`EVAL.md`](Pufferlib/ocean/brotato/bridge/EVAL.md)).
- `replay_cpu.py`: replays a real-game trace's actions through the C core and reports the first differing field.

```sh
python3 Pufferlib/ocean/brotato/policy/export_policy.py --checkpoint <ckpt.bin> --output policy.json --hidden 256 --layers 2 --actions 31
python3 Pufferlib/ocean/brotato/bridge/eval_game.py --policy policy.json --action-source policy --endless --seeds 1,2,3
```
Needs Godot 3.7-dev1 (commit `0a9011743`), GDRE Tools, and your own copy of Brotato and its DLC.

## Build and train

```sh
cd Pufferlib
./build.sh brotato --cu                         # CUDA trainer -> ./puffer
cp config/brotato_demon_endless.ini config/brotato.ini
./puffer train --headless --vec.total_agents=4096 --train.horizon=128 --train.minibatch_size=32768
BROTATO_TEST_NO_CUDA=1 tests/run_brotato_tests.sh   # CPU test suite
```
