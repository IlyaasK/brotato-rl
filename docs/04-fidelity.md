# Fidelity: bit-exact where possible, verified everywhere

A port is only as good as the equivalence contract you enforce. Three layers,
three distinct exactness targets.

## Contract

| Layer | Content | Target | Mechanism |
|---|---|---|---|
| **L1 Data & ints** | item/weapon/enemy tables, prices, reroll costs, rarity tables, RNG streams | **Bit-exact** | auto-extract from decompiled resources; integer math; replicate PRNG |
| **L2 Stats pipeline** | derived stats, modifiers, tags, scaling, damage calcs | **Golden-vector exact** | float64, same libm, same op order; ported function-by-function |
| **L3 Physics/AI** | movement, collision, area-overlap timing, knockback, enemy behaviors | **Trajectory-equivalent** (event-equal, positions within ε) | differential replay vs instrumented real game |

## L1: bit-exact by construction

- **Data**: parsed directly from decompiled `.tres`/`.tscn`/CSV → C structs.
  A mismatch is a generator bug, not a port bug. Diff tables between game patch
  versions to catch regressions.
- **PRNG**: Godot 3.2+ uses **PCG32** (`RandomNumberGenerator`) — xoshiro/xorshift
  is *not* compatible. Port PCG32 with identical seed → identical stream
  (verify: first 1M outputs vs known-good vector).
- **Integer rules**: all shop price/rarity/reroll math is round-down int math
  (see wiki formulas) → exact.

## L2: golden-vector exact

- Every stat computation in GDScript decompiles to clear verbatim ops.
  Port them with **identical expression tree** (no C++-style reordering that
  changes float rounding). `double` real_t on desktop Godot 3.
- **Goldens from instrumentation**: we add a debug mod to the decompiled game
  that logs `(inputs → computed stat)` for a sweep of builds, items, characters,
  waves, danger. C asserts those vectors exactly (within 0 ULP, or tolerance for
  differing libm — glibc vs MSVC edge cases).
- Regression: goldens regenerated automatically per game patch.

## L3: trajectory equivalence via differential replay

No engine can be bit-exact ported cheaply (move_and_slide, Area2D ordering,
physics solver internals). Instead:

1. **Instrument the real game** (decompiled copy): a mod that replays a scripted
   policy, logs every frame: player pos/HP/stats, entity spawns, projectiles,
   pickups, events (damage taken, kills, drops), plus **per-call RNG** `(seed,
   call_id, output)`.
2. **Replay the same inputs + same seed through C**.
3. **Compare**: discrete event streams must match **exactly**; continuous
   positions within tight ε; wave timings identical.
4. Any divergence → **bisect to the subsystem** and fix (burn L3 bugs one by one).

Determinism requirements:
- Brotato is deterministic given seed + inputs (single-player, no netcode).
- We must replicate **entity iteration order** (Godot scene-tree order) — match
  insertion order documented in decompiled scenes.
- Frame count/timestep: 60 Hz physics; verify from decompiled `project.godot`
  (Brotato may set physics tick rate) and match exactly.

## Test pyramid (order they are built)

```
L1 unit tests        (tables, prices, PCG32 streams)        — minute-scale
L2 golden vectors    (stats sweep vs instrumentation)        — minutes
L3 replay diff       (100+ episodes, scripted policy)        — hour-scale
Statistical match    (seeded win-rate parity scripted bot)   — hours
Transfer test        (train on C → deploy to REAL game)      — ultimate gate
```

## The ultimate gate: transfer

A C env that passes L1–L3 and statistical parity is still unproven until the
policy trained on it wins in the **real game** (via a mod like brotatoai's
interface). This is the sim2real moment: if the agent trained in C wins real
Danger 5 runs at comparable rates, the port is done. If it falls over, the bug
is in a layer we didn't test enough — and the replay harness identifies which.

## What we refuse to claim

- “Binary identical to the game” (impossible for L3, unneeded).
- “The C env is the game” — C is the **oracle-gated approximation**; the game is
  the oracle. We only claim the agent's *behavior* transfers.

## Done criteria per layer

- L1: 100% of tables, streams and int formulas match by construction + tests.
- L2: 100% of stat goldens pass; zero unexplained ULP diffs.
- L3: 100% of event streams match over test corpus; ≤1e-6 normalized position error.
- Transfer: win-rate parity within CI noise on 100+ real-game seeded runs.