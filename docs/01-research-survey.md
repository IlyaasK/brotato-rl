# Prior-art survey

Survey of public Brotato AI/RL work (GitHub, web, Reddit, academic indexes). As of 2026-08.

## Projects

### guenichone/brotatoai — the only RL-in-decompiled-game attempt
- Decompiles Brotato with **GDRETools/gdsdecomp** (recover `.pck`), runs the recovered
  project in **GodotSteam editor v3.25** (Godot 3.x, Steam bindings required),
  hooks **GodotModLoader**, wraps via **godot-rl** (Gymnasium bridge), trains
  **SB3 PPO** (`MultiInputPolicy`), supports `n_parallel`, physics `speedup`,
  headless exports, ONNX export.
- Evidence: 17 commits, 1 star, 0 forks. **No published results**: no win rate,
  no reward curves, no difficulty level, no checkpoints.
- Value: proves the hard integration (decompile → mod sensors → gym loop) is solvable
  and forkable. Dormant; glue quality iffy.
- Pipeline: SteamCMD pull → gdre_tools --headless --recover → GodotSteam editor →
  mod → godot-rl → SB3.

### boardengineer/botato — scripted auto-battler mod (NOT RL)
- Heuristic bot (distance maintenance / kiting), humans pick weapons/items.
- Author-reported on r/brotato (Apr 2023): wins with every character **except
  Pacifist**; Danger 5 with **most** characters.
- Key evidence: **simple kiting + human drafting beats the whole game.**

### HelpFreedom/brotato-full-autobot — scripted full-run bot
- One-click bot playing max difficulty across all 64 characters; potential-field
  movement + DPS/eHP evaluation. Rule-based, not learned.

### saslifat-gif/brotato_ai_bot — vision-based PPO (external, Windows)
- Black-box env: window capture, Roboflow/YOLO detection, HP-pixel labeling, OCR,
  synthetic clicks; PPO with TPU?? no — CPU PPO; reward engine; shop policy;
  victory detection + `kpi/success` win-rate TensorBoard metrics.
- 0 stars, no published results, Windows-only, ~12.5 decisions/sec.

### Adesii/Neuro-sdk-Brotato-Integration — LLM plays (not RL)
- Neuro/LLM driving the game; relevance: another mod-side control path.

## Gaps (what nobody has done)

| Gap | Evidence |
|---|---|
| **Full-run RL winner** | All RL projects lack results; only scripted bots win |
| **PufferLib integration** | GitHub code search `pufferlib brotato`: 0 results; env registry has no Brotato |
| **GPU-resident env** | No JAX/CUDA survivors-like env; closest: `mutkach/idle-survivors` (CPU Gym), torus.graphics blog (plan only), Korea Univ. Unity thesis (slow, non-GPU) |
| **Self-play** | N/A: single-player PvE. Population/domain-randomization is the analogue; untouched |
| **Learned shop/draft** | All existing bots script the shop or pair a human with combat bot |

## Implications

1. The **movement policy is simple** (BOTato proves kiting wins with decent builds) →
   dense reward + small MLP converges in single-digit millions of steps.
2. The **shop is the unsolved part** → separate sparse decision problem, script it first,
   learn it later.
3. **Fidelity** of any reimplementation is the make-or-break: no published port exists,
   so this repo's test plan (see 04-fidelity.md) is the differentiator.

## Sources

- github.com/guenichone/brotatoai · github.com/boardengineer/botato ·
  reddit.com/r/brotato/comments/12x9bgp · github.com/HelpFreedom/brotato-full-autobot ·
  github.com/saslifat-gif/brotato_ai_bot · github.com/GDRETools/gdsdecomp ·
  brotato.wiki.spellsandguns.com (Modding, Shop, Characters) ·
  github.com/mutkach/idle-survivors · torus.graphics/blog (JAX survivors plan) ·
  github.com/pufferai/pufferlib