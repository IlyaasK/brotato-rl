# Architecture: C reimplementation + PufferLib → CUDA

## Why not Godot itself

Godot per-instance (brotatoai route): one process per env, socket bridge, editor
runtime overhead → hundreds of steps/s aggregate at best, fragile across game patches.
A single-user C env with the same logic: **~1 ms/step or better**, tens of thousands
of envs per machine, fully deterministic, no IPC.

## Two tiers

| | Tier A: C env + PufferLib | Tier B: CUDA env (ocean-style) |
|---|---|---|
| Env on | CPU (parallel processes/threads) | GPU (kernels) |
| Agent on | GPU (PufferLib PPO) | GPU |
| Throughput | 20k–100k steps/s | 500k–2M steps/s |
| Fidelity risk | Low (C easy to validate) | High (kernels simplify mechanics) |
| Effort | ~1–2 weeks | +~2–4 weeks after Tier A exists |

**Tier A is the ground truth. Tier B must match Tier A on the same seed** — C is the
oracle, CUDA is the rocket. Never write kernels against GDScript directly.

## Module layout (C)

```
cristal/  (internal name placeholder)
  core.c    player, waves, enemy pool, pickups, grid
  stats.c   derived stats, tags, scaling, modifications (float64)
  combat.c  weapons, projectiles, damage, armor, dodge, knockback
  items.c   auto-generated data tables from decompiled resources
  shop.c    prices, rerolls, locks, combines, rarity
  rng.c     PCG32 (Godot 3.2+ compatible)
  agents.c  phase state machine: combat / level-up / shop
```

**Never hand-enter data tables.** Generate them (C structs) from the private decompile
at build time — mechanical, exhaustive, and diffable.

## PufferLib integration

- C env → `pufferlib.emulation` wrapper (gym API) → PufferLib PPO, agent on CUDA.
- Frame-skip 4–6 in the env; 500–4096 envs across cores.
- Determinism: per-env seeds, xoshiro/PCG32, no shared mutable state.
- Eval harness: 100 seeded runs → win rate / mean wave / dmg-taken stats.

## Speed math (why Tier A is enough, then when B pays)

Full run ≈ 20 min game time ≈ 72k frames ≈ 18k decisions @ frameskip 4.

- Tier A @ 50k steps/s → ~2.8 runs/s → 10M samples ≈ ~3.9 h wall
- Tier B @ 1M steps/s → ~55 runs/s → 10M samples ≈ ~12 min wall

Tier A covers movement + scripted shop comfortably. Tier B pays off when the **learned
shop** needs many runs (>100k runs) or population-scale domain randomization.

## GPU-resident notes (Tier B)

- Dense-vector game: 100s–1000s entities, simple geometry — GPU-shaped.
- Batch 512–2048 envs per GPU; state entirely in device memory (zero host transfer).
- Per-env PRNG streams (xoshiro in device memory); no atomics on state sums;
  deterministic reductions for derived stats.
- Shop stays host-side state machine (20 decisions/run vs 70k steps → 0.03% of work).
- **Camera/obs cut-off**: obs assembly on device, directly into PufferLib buffer.

## Why CUDA is a *backend*, and env parallelism is the real lever

PufferLib already runs PPO on CUDA. The speedup order is:

1. Replace Godot with C: 20–100× (env per-instance cost).
2. Batch more envs per core: another 5–50× (CPU is the limit).
3. Move env to GPU: another 10–100× (only if 1+2 exhaust the machine).

The famous “GPU-resident” result is step 3; steps 1+2 are what most projects
need and skip.