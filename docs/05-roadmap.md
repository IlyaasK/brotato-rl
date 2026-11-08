# Roadmap

Phased; each phase ends with an observable acceptance test. Rough timeline for
one engineer with CUDA familiarity. Oracle work is non-negotiable and first.

| Phase | Deliverable | Acceptance | Est. |
|---|---|---|---|
| **M0** | Instrumented real game (decompile → mod): RNG logs, stat goldens, replay traces | Logs generated for sweep of seeds/builds | 3–5 d |
| **M1** | C core sim + stats engine + data-table generator | L1/L2 tests green; random policy completes episodes | 1–2 w |
| **M2** | PufferLib emulation wrapper + PPO skeleton | 500+ envs; 20k+ steps/s; no hangs | 2–3 d |
| **M3** | Scripted shop + movement training | D0 win rate ≥50% (100 seeded runs) | 1–2 w |
| **M4** | Danger ladder → D5 | D5 win rate ≥50% | 2 w |
| **M5** | Learned shop (combat frozen) | Full-run autonomous win rate parity vs scripted shop | 2 w |
| **M6** | CUDA port of Tier-A env | Same-seed diff vs oracle within contract; 500k+ steps/s | 2–4 w |
| **M7** | Character curriculum; domain randomization | ≥10 characters D5; transfer test in real game | 2–3 w |
| **M8** | Real-game transfer deployment (mod bridge) | Trained policy wins real-game seeded runs at parity | 1 w |

## Sequencing rules

1. **No Tier B (CUDA) before Tier A (C) passes L1–L3.** C is the oracle; kernels
   are written against C, never against GDScript.
2. **No learned shop before movement wins D0.** Credit assignment needs a
   competent enemy-facing policy to measure against.
3. **No full 62-character training before 3–5 characters pass.**
4. Game patches: regenerate goldens + traces; re-run L1–L3; a failing test blocks
   any training run.

## Open questions (tracked here until decided)

- Exact weapon/class pools per character — from decompiled data, not wiki.
- Frame-skip value that preserves wave difficulty curve (4 vs 6 vs 8).
- Whether shop autos (combine, auto-equip) run on host side or in-env.
- Naming for the C/CUDA project (placeholder: `cristal`, `potato-sim`).
- Repo/ToS posture: sim owns no game assets; data-driven tables regenerated locally.