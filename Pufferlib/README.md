[![Discord](https://dcbadge.limes.pink/api/server/puffer?style=plastic)](https://discord.gg/puffer)
[![Twitter](https://img.shields.io/twitter/url/https/twitter.com/cloudposse.svg?style=social&label=Follow%20%40jsuarez)](https://twitter.com/jsuarez)

PufferLib is a fast and sane reinforcement learning library that can train tiny, super-human models in seconds. The included learning algorithm, hyperparameter tuning, and simulation methods are the product of our own research. All our tools are free and open source. Need a high performance environment for your application? We build them professionally and offer training + extended support. Contact jsuarez🐡puffer🐡ai.

All of our documentation is hosted at [puffer.ai](https://puffer.ai "PufferLib Documentation"). @jsuarez5341 on [Discord](https://discord.gg/puffer) for support. Post there before opening issues. We're always looking for new contributors!

## Metal training on Apple Silicon

On macOS 26 or newer, install the Xcode command-line tools and Homebrew `libomp`,
then build a CPU environment with the Metal trainer:

```sh
./build.sh breakout --metal
./puffer train --base.async=0
```

The environment runs in C on the CPU; policy inference and training use the
Apple GPU. New environments implementing the existing CPU environment API can
use the shared trainer without their own Metal kernels. Breakout, Admiral and
Brotato also have GPU environment implementations built with `--cu --metal`; they
synchronize through separate command queues and do not yet support GPU-env
self-play or rendering. Robot Arm has no Metal GPU environment implementation.

The Metal trainer supports synchronous and concurrent actor/learner training,
self-play on CPU environments, evaluation, matches, and single-GPU sweeps.
Concurrent training uses an actor snapshot that can lag the learner by one update.
Float observations are copied in bulk for actor inference and into immutable
rollout slots; other observation types are converted to float. The learner
stages minibatches on the GPU. PPO diagnostics use fixed-order GPU reductions
and are read at epoch boundaries.
Multi-GPU training is not supported. Metal trains in float32 by default: on M1
Pro the tensor_ops GEMM used by the learner runs at roughly 1150 GFLOPS in
float32, 1462 in bfloat16, and 2701 in float16 (measured at a
32768x384x128 tile). `--base.mingru_fp16=1` runs the steering GEMMs with half
operands while keeping float32 master weights, float32 accumulation, and
float32 scans. On M1 Pro that is 1.28-1.32x end-to-end with unchanged
evaluation scores at equal step counts; results shift at bf16-class precision,
so it stays opt-in. Bfloat16 is not a speed win on M1 Pro and CUDA's default
bfloat16 results cannot be reproduced bit-exactly on this hardware; float16 is
the only faster path. Metal also uses a different
weight initializer. Action sampling uses persistent per-agent Philox states,
but identical seeds do not imply identical CUDA and Metal policies.
Environments used by the Metal trainer draw randomness from a per-environment
stream, so equal runs produce equal policies regardless of worker-thread
scheduling or count. The osrs_visual demo and CUDA-only environments are
outside that guarantee.
Checkpoints store complete float32 parameters with each tensor starting at an
eight-float boundary. Truncated checkpoints are rejected; missing parameters
cannot be recovered. Set `--base.profile=1` to print per-epoch GPU sync,
execution, and scheduling times. Custom architectures and environment ports
should be validated for the intended workload; not all precision and
configuration variants are equivalent to CUDA.

## Brotato shared simulation and validation

Brotato's CPU, CUDA and Metal environments share `ocean/brotato/brotato_core.h`,
POD state, PCG32 streams and a 161-float observation schema with 21 discrete actions.
The contact solver transcribes the circle/wall subset of GodotPhysics2D; it retains contact caches
and source attribution. Pair ordering differs from Godot's hash broadphase, so
isolated fixture agreement is not a claim of full-game equivalence.

The game uses separate global and `Utils` RNG streams. Spawn sampling retains
the source's group/unit distance rejection and visual RNG draws, including
the per-wave background-gradient choice and birth rotation. Global
floating-point ranges preserve Godot's binary64 arithmetic on Metal through
integer-based emulation; native CPU/CUDA builds use hardware doubles.
Older 79-input and 141-input checkpoints are incompatible and must be retrained.

The default remains a fully-unlocked, weaponless Bull at Danger 5 through wave
20. Source-backed melee and ranged weapons include tiered inventory, set bonuses,
curse variants, projectiles and hit effects. The shop exposes buying, rerolling,
leaving, recycling slots 0–5 and combining slots 0–5; locking and stealing remain
outside the action space. Items retain their source-derived effect graphs.
An isolated original-game save does not imply the same unlock profile.

`config/brotato.ini` selects `env.character` (`bull`, `knight`, `demon`) and
`env.starting_weapon` (`none` or an exact per-tier weapon ID). For example,
Knight accepts `weapon_hatchet_2`; Demon accepts `weapon_spear_1`. These profiles
are configurable and smoke-checked; Knight/Demon policy training has not started.
The live bridge selects the same resources with `BROTATO_CHAR=character_knight`
and `BROTATO_WEAPON=weapon_hatchet_2` (or the corresponding Demon resources).
Shop observations read current owned resources after inventory mutations;
their weapon cooldown feature is zero because no arena Weapon node exists.
The shop-action observation block reports offer prices in the active purchase
currency, eligibility and affordability, plus reroll cost and availability.

A scripted, non-invulnerable 161-observation Bull replay matches the original
through tick 187 at `atol=rtol=1e-5`; it diverges at tick 188. Both first lose
health at tick 278, but later health differs. Full-game parity is unproven; see
`ocean/brotato/bridge/bridge_status.json`.

```sh
./build.sh brotato build/brotato_cpu --metal
./build.sh brotato build/brotato_gpu --cu --metal
python3 tests/brotato_physics_compare.py --engine /path/to/Godot
```

The default batch is 128 environments. State memory includes persistent contacts,
owned effects and source-object hit histories. Metal GPU environments are
headless; use the CPU environment for the raylib viewer. That viewer is not the
original game's UI.
CUDA execution additionally requires an NVIDIA host and CUDA toolkit.

Recovery, table extraction and the original-bytecode bridge live under
`ocean/brotato/`. Retail packs, recovered proprietary scripts and generated
offline patches stay outside the repository. Bridge pack builds record patch
hashes and verify that the retail pack remains unchanged. Original-game
transfer must pass live observation/action and transition checks separately;
native fixture tests alone cannot establish transfer.

## Star to puff up the project!

<a href="https://star-history.com/#pufferai/pufferlib&Date">
 <picture>
   <source media="(prefers-color-scheme: dark)" srcset="https://api.star-history.com/svg?repos=pufferai/pufferlib&type=Date&theme=dark" />
   <source media="(prefers-color-scheme: light)" srcset="https://api.star-history.com/svg?repos=pufferai/pufferlib&type=Date" />
   <img alt="Star History Chart" src="https://api.star-history.com/svg?repos=pufferai/pufferlib&type=Date" />
 </picture>
</a>
