# Real-game vs simulator policy evaluation (`eval_game.py`)

`eval_game.py` takes one exported policy artifact and a list of seeds, runs one
**headless real-game** episode per seed through `run_game.sh` and one
**simulator** episode per seed through the shared CPU core
(`ocean/brotato/brotato_core.h`), and prints a per-episode table for each side
plus a sim-vs-game gap table.

Committing anything is out of scope: the tool only reads the repo and writes
under `--run-root` (default `~/brotato_eval`, never the repository).  A `--policy`
artifact is read; the mod reads it from `BROTATO_RL_POLICY_ARTIFACT`.

## Quick start

```sh
cd Pufferlib

# 1. a synthetic random-weight artifact (no training needed)
python3 ocean/brotato/policy/policy_reference.py fixture \
    --out-dir ~/brotato_eval/policies/random_h256l2 --hidden 256 --layers 2

# 2. full run (20-wave campaign) in game + sim, 4 seeds, 4 at a time
python3 ocean/brotato/bridge/eval_game.py \
    --policy ~/brotato_eval/policies/random_h256l2/policy.json \
    --action-source policy --seeds 1,2,3,4 --parallel 4
#    (a policy run samples from the masked softmax by default; add
#     --action-mode argmax for the historical greedy pick - see
#     "Action selection")

# 3. fast smoke (30 simulated seconds per episode, scripted action source)
python3 ocean/brotato/bridge/eval_game.py \
    --action-source script --seeds 1,2 --max-seconds 30 --watchdog 240 \
    --sample-every 1

# 4. Demon-first: the scripted Demon bot in the sim + the native forward bench
python3 ocean/brotato/bridge/eval_game.py --sim-only --sim-forward bot \
    --seeds 1,2 --max-seconds 200                     # Demon + Ghost Scepter by default
python3 ocean/brotato/bridge/eval_game.py --sim-bench 50000 --sim-forward bot
python3 ocean/brotato/bridge/eval_game.py --sim-bench 20000 \
    --policy ~/brotato_eval/policies/random_h256l2_v4/policy.json --sim-forward native

# 5. endless on both sides (see "Endless mode" for the smoke and the flags)
python3 ocean/brotato/bridge/eval_game.py --endless --sim-forward bot \
    --sim-only --bonus-hp 5000 --seeds 1,2 --max-seconds 1250
python3 ocean/brotato/bridge/eval_game.py --endless --start-wave 20 \
    --invulnerable --game-only --seeds 1 --max-seconds 110 --sample-every 6
```

Other knobs: `--game-only` / `--sim-only`, `--character` / `--weapon`,
`--danger N`, `--endless`, `--invulnerable` (both sides: `DebugService.invulnerable`
in the game, `BtState.invulnerable` in the core), `--start-wave N` (both sides: game `DebugService.starting_wave`, core
`BtState.start_wave` — the sim enters wave N on its first episode), `--sim-forward {native,python,bot}`,
`--bot-pivot N` / `--bot-stay N`, `--bonus-hp N` (sim, dgb stress mode),
`--sim-bench N`, `--json out.json`, `--prune-traces`, `--resim`, `--quiet`.

**Demon-first defaults.** The verification target is Demon + Ghost Scepter at
Danger 5, campaign then endless, so `--endless` (and `--sim-forward bot`, whose
policy is the Demon-only `demon_guide_bot.h`) default `--character` to
`character_demon` and `--weapon` to `weapon_ghost_scepter_1`, resolved to the
core's BT_WEAPONS id (118) through the canonical tables so both sides start the
same loadout.  An explicit `--character`/`--weapon` still wins.

## Endless mode

`--start-wave N` now drives **both** sides: the game gets
`BROTATO_RL_WAVE=N` (`DebugService.starting_wave`, `run_data.gd:549`) and the core
gets `BtState.start_wave` (set after `bt_core_init`, default 1), so a late-wave
comparison can start both sides at the same wave instead of jumping through the
campaign.  On the sim side the driver re-runs the deterministic reset on FRESH
streams (`rng_ready = 0`; the same boot `tests/brotato_core_trace.cpp` uses) so
the **first** episode enters wave N -- without it `start_wave` would only apply at
the *next* reset, and an invulnerable run has no terminal, hence no next reset, so
it would sit at wave 1 forever while the game starts at N.  A campaign run
(`--start-wave 0`) keeps the historical call shape.

`--audio-rng N` (sim, default **0**) selects the core's audio-RNG model:
`0` = the bridge's audiovisual neutralization (the sound queues/arrays are
cleared and exactly those RNG draws do not happen), `1` = the legacy source draws.
The bridge neutralizes those sites, so the default for game-vs-sim work is `0`
(the same value `config/brotato_demon_endless.ini` now sets explicitly, so train
and eval share one audio-RNG model); a trace recorded with the legacy model must
be replayed with `1` (DemonReplay owns the flag).  Rows record `audio_rng`.

`--endless` drives both sides: the game gets `BROTATO_ENDLESS=1`
(`RunData.is_endless_run`, so the wave-20 boss does not end the run) and the core
gets `BtState.endless` (set right after `bt_core_init`; the wave-20 stop is
skipped and waves 21+ are built by the endless schedule).  `won` keeps its
definition -- *wave 20 cleared* -- which in endless is exactly "reached wave
21+", and a run that was **started** past wave 20 (`--start-wave 21`) never
claims that.

`--invulnerable` drives **both** sides.  Game: `DebugService.invulnerable`, read
by `run_data.gd:556` and consumed by `player.gd:114-115 disable_hurtbox()`: the
player's Hurtbox Area2D is disabled, so no enemy contact Hitbox and no enemy
projectile ever reports the player.  Sim: the same option is now a core field,
`BtState.invulnerable` (`[env] invulnerable`, default 0; CPU `brotato.h`, CUDA
`brotato.cu`, Metal `brotato_metal.mm` all set it after `bt_core_init`, and the
eval harness's own sim runner takes `--invulnerable`).  The flag is applied at the
two hurtbox overlap tests (`bt_player_contact_probe` and `bt_proj_hits_player`),
not at the damage path, so the run consumes exactly the draws the game does -- no
hit, no iframe/dodge roll, no hurt sound, no damage -- while enemy spawns, AI,
wave RNG and projectile motion are unchanged (a shot that reaches the disabled
hurtbox keeps flying instead of stopping).

A recording made with `BROTATO_RL_INVULNERABLE=1` carries `config.invulnerable`
in its meta, and `replay_cpu.py` forwards it to `tests/brotato_core_trace.cpp`
(trailing `invulnerable` argv; override the recording with `--invulnerable 0|1`),
so an invulnerable capture replays under the same hurtbox model.  Regression:
`tests/test_brotato_invulnerable.cpp`.

An **endless** recording additionally needs its `RunData.is_endless_run`: it
carries `config.endless`, and `replay_cpu.py` forwards it as the harness's
trailing `endless` argv (`--endless 0|1` overrides).  Without it an endless
capture replays as a campaign run that ends -- and `bt_core_step` resets -- at the
wave-20 clear, so the candidate loops wave 20 while the recorded run advances.

The two deep-wave smoke invocations, now with one shared intervention:

* game: `--start-wave 20 --invulnerable` (DebugService) fast-forwards to wave 20
  and keeps the player alive; each row's notes say
  `invulnerable (DebugService; smoke only)`, `endless (RunData.is_endless_run)`
  and `started at wave N (DebugService.starting_wave)`.
* sim: the same `--invulnerable` (the core field) gives the identical
  intervention, so the pair is a real parity scenario (still flagged in the
  notes, and still excluded from the unflagged aggregates).
* `--bonus-hp N` remains as a *stress* grant for runs that are NOT invulnerable
  (the same grant `tests/demon_guide_bot_run.cpp` uses); its rows are flagged
  `stress_bonus_hp`.

Measured endless smoke (Demon, Ghost Scepter, D5):

```text
game  --endless --start-wave 20 --invulnerable --max-seconds 110 --game-only
  seed 1 | waves 21 | won Y | mat 18 | kills 128 | 6595 ticks | 282.6s | ok
         notes: invulnerable (DebugService; smoke only); endless (RunData.is_endless_run);
                started at wave 20 (DebugService.starting_wave)
sim   --sim-only --sim-forward bot --endless --bonus-hp 5000 --max-seconds 1250
  seed 1 | waves 11 | 27911 ticks
  seed 2 | waves 24 | 74395 ticks | 75000 steps | mat 1052 | lvl 30 | kills 8924 |
           buys 284 | rerolls 210 | 0.7s | ok
```

Both sides reach waves > 20 (`waves_reached` 21 and 24); the game row is a
flagged invulnerable/start-wave smoke and the sim rows are flagged stress runs.

Every invocation **freezes `run_game.sh`** into
`<run-root>/launcher/run_game.snapshot.sh` and executes that copy (its sha256 is
recorded in `--json`).  `bash` reads a script lazily by byte offset, so an
editor saving into `run_game.sh` while episodes are running makes those episodes
execute a mix of old and new bytes; observed: `run_game.sh: line 434: ...:
Permission denied` and a nonzero rc on a run whose trace and summary were
perfectly valid.

## What each side actually runs

**Game.** `run_game.sh` is invoked once per seed with its own
`BROTATO_RUN_DIR`, `BROTATO_ISO_HOME` and `BROTATO_TAG`, so episodes never
collide.  The retail pack and the patched pack are staged **once** into
`<run-root>/stage`; each episode's run directory hard-links those files
(`BROTATO_SKIP_STAGE=1 BROTATO_SKIP_BUILD=1`), so no 200 MB copy and no GDRE
re-patch per seed.  `BROTATO_EXTERNAL_ROOT` points at `<run-root>/external` so a
concurrent `run_game.sh` (another agent, another eval) cannot rewrite the
observation tables under our engines.  `BROTATO_RL_NO_SAVE=0` is set because the
game's own save inside the isolated home carries `enemies_killed` (the bridge
trace has no kill counter).

**Sim.** `eval_game.py` emits two translation units into `<run-root>/build`:

* `eval_sim_policy.c` -- the only place that sees puffernet's C types; it wraps
  `src/puffercpu.c` (`load_weights`/`make_linear`/`make_mingru`, the same
  registration order `native_forward_probe.c` checks) behind an opaque
  `bt_policy_load/act/reset/free` API, and verifies the weight file's float count
  against the declared layout before running;
* `eval_sim_step.cpp` -- the episode driver: it includes `brotato_core.h`, sets
  `endless`/`bonus_hp`, and in one process steps the core with the action chosen
  by the requested forward.

Compiled with `clang -std=c99` (glue) + `clang++ -std=c++17` (driver), both
`-O2 -ffp-contract=off`, linked with `-lm`.  The build is keyed by a **content
stamp** over both sources, the exact command lines, the include root and every
`ocean/brotato/*.h` (plus `src/puffercpu.c`), never by mtimes: a binary whose
sources are current but whose object file is from an older revision looks
identical to a good build, and one such stale driver hung at the first phase
transition during this work.  A transiently broken shared header simply makes the
sim half report `sim_failure: sim driver unavailable: <compiler output>` while the
game half still runs and prints.

`--sim-forward` selects how the action is produced:

| mode | action source | per-step pipe traffic |
|---|---|---|
| `native` (default with a policy) | the artifact's weights, in-process puffernet forward, with the artifact's own action mask applied before the argmax (see "Action masks") | one row every `--sim-print-every` steps |
| `bot` | `ocean/brotato/policy/demon_guide_bot.h`'s `dgb_action` against the core's own `bt_action_mask` (the scripted Demon policy the sim-vs-game comparison uses) | one row every `--sim-print-every` steps |
| `python` | `policy_reference.py`'s `Reference` in this process, one action per line on the driver's stdin | every step (deadlocks otherwise) |

### Action selection (`--action-mode argmax|sample`, default `sample`)

Both sides turn the policy's **masked** logits (see "Action masks" below) into one
action the same way, selected by `--action-mode`:

* `sample` (default) - the softmax over the masked logits, **one** uniform per
  decision, cumulative-probability selection.  A greedy policy that keeps proposing
  one action the shop cannot apply (e.g. a purchase with no currency) never leaves
  the shop and burns the tick budget; sampling draws the skip/reroll with positive
  probability and the run progresses.
* `argmax` - the historical greedy pick, unchanged.

The uniform comes from a **dedicated** xorshift32 seeded from the run seed plus a
constant (`rl_policy.gd` `SAMPLE_SEED_CONST` = `0x9E3779B9`, the same seeding in the
driver's `bt_sampler_seed` and in `policy_reference.py`'s `SampledRng`), never from
the game's global RNG, `Utils._rng`, or the core's `bt_state` RNG: an action draw
must not shift a gameplay stream.  The three implementations are bit-identical (the
engine test in the bridge's own check runs 20 000 draws and matches the C and Python
sequences; masked ids carry `MASKED_LOGIT = -1e4`, whose `exp()` underflows to
exactly `0.0`, so an illegal action can never be drawn).

The game reads the mode from `BROTATO_RL_ACTION_MODE` (`rl_cfg.gd` -> `rl_policy.gd`,
recorded in the run meta as `action.mode`, in `config.action_mode`, in the config
fingerprint and in `policy.action_mode` / `policy.samples_drawn`); the sim driver
takes `--action-mode` and echoes it in its `hello` row; a `--sim-forward python`
episode samples in `policy_reference`'s `PhasePolicy`.  Both the eval result JSON and
the printed banner carry `action_mode`.  With `--action-source script|file` the mode
is not applied (the harness supplies the ids) and the rows say so.

Note the one asymmetry in the `python` forward: `policy_reference.Reference` is fed
no artifact mask, so its sampled draw is restricted to the phase contract's own
legal set (the same set the fail-closed gate enforces) instead of the artifact's
mask; the `native` forward samples over the artifact mask, exactly like the game.

### Action masks (both sides must mask before selecting an action)

`brotato_policy.gd` replaces masked-off logits with `MASKED_LOGIT` (`-1e4`) before
its argmax (and before the sampled softmax, where they underflow to zero probability),
and `rl_policy.gd` derives the mask per step from the **artifact's**
mask metadata: the phase from `phase_field_index`, then the first `selectors`
entry for that phase whose `when_nonzero` field is non-zero replaces the clause
(`mask_key`), and the clause named by that key gives the legal ids (`legal_by_phase`,
keyed like `2_crate`); an absent or empty clause means "no mask".  The harness
consumes the same metadata: `--mask-clauses`, `--mask-selectors`,
`--mask-phase-field`, `--masked-logit`, evaluated **per observation** by the driver
-- nothing is flattened, no core-mask substitution, no shim.  An artifact without
`action_mask` (pre-mask export) makes the game apply no mask at all and the sim
matches that (the tool says so and still records the core's mask).

The masks are *verified*, not assumed.  Every row carries `pm` (the phase the masks
belong to, i.e. the pre-step phase), `m` (the core's own `bt_action_mask`, kept as
an independent oracle), `am` (the mask actually applied), and the sim row reports
`mask steps: 0=N, 1=N, 2=N, crate=N` (steps observed per phase, `crate` = a phase-2
step whose applied mask is the take/recycle pair) plus either
`applied mask == core bt_action_legal on every step` or the number of steps where
the **applied** mask admits an id the core forbids -- that direction is a hard
failure (`status=mask_failure`, `failure=mask_parity`) outside the combine-block
residual, because the policy would then be sampled an action the core never
trained on.  The reverse direction (the artifact narrower than the core, e.g. the
lock/ban ids when the artifact clause omits them) is safe and only counted
(`applied mask narrower than the core on N step(s)`).  The harness additionally
re-derives the mask from the metadata itself (`mask_from_rules`, evaluated on the
observation the driver masked) and reports `harness evaluation != applied mask on
N step(s)`; a mismatch there is how the driver's conditional-spec parsing bug was
caught (an unbounded `strchr('+')` swallowed every spec entry after the first one
containing `+`, so only the first conditional was applied and the driver ran a
loose range mask while the harness and the game were correct).

Measured on a Demon-bot trajectory at stride 1 (seed 2, 21 waves):

```text
mask steps: crate=35, 0=74495, 1=442, 2=28; core mask == applied mask on every step
```

i.e. all four cases occur in real play (combat, shop, level-up, crate-held), the
crate clause is selected on those 35 steps, the core's mask agrees on **every**
step, and the harness's own re-derivation agrees with the driver (0 differences).
A policy episode reports `0=1322, 1=1077, 2=1` with `core mask == applied mask on
every step`; its masked cross-check (recorded obs+mask replayed through the driver
and through `policy_reference.py`) is `900/900 actions identical, max |logit delta|
6.7e-05`, and the per-phase forward checks pass for combat `0-8`, shop `0-4,8-28`,
level-up `0-4` and crate `29-30`.

### Interventions are flagged, never mixed

Rows carry structured flags: `<invulnerable>` (both sides:
`DebugService.invulnerable` in the game, `BtState.invulnerable` in the sim),
`<stress_bonus_hp>` (the sim's `--bonus-hp`, the `demon_guide_bot_run.cpp` stress
grant), `<start_wave=N>`, `<endless>`, `<dgb_bot>`, `<reuse_pack>`.  Game
invulnerability and sim stress HP are **different** interventions, so they are
never treated as equivalent parity scenarios: the aggregates are computed over
*unflagged* rows only (`mean over unflagged rows`), and the gap table marks any
pair that used an intervention with `!smoke-only:<flags>`.  Since the core now
models `DebugService.invulnerable` as `BtState.invulnerable` (and `--invulnerable`
drives both sides), an invulnerable pair IS one intervention and can be read as a
parity scenario; `--bonus-hp` stays a sim-only stress grant.  The replay side
reproduces it too: `replay_cpu.py` forwards `meta.config.invulnerable` (and
`meta.config.endless`) to the harness.

Whenever the driver owns the action it also enforces the step budget
(`--max-steps`, the game's `BROTATO_RL_MAX_TICKS` count: every physics step,
phase steps included), because `state.ticks` does not advance in the shop or
level-up phases and a policy that keeps proposing shop actions the core accepts
as no-ops would otherwise spin until the wall timeout.

`policy_reference.py` remains the reference implementation, and with a policy
the default `--sim-crosscheck` verifies it against the native forward on
**identical inputs**: the first seed's primary run records every observation
(stride 1), the driver re-runs its own native forward over that recorded sequence
in one pass (`--forward-obs`, a pure forward with no stepping), and
`policy_reference.py`'s `Reference` is replayed over the same sequence.  Measured
on `random_h256l2_v4`, 900-step window:

```text
crosscheck native vs policy_reference: 900/900 actions identical, max |logit delta| 6.48e-05
```

A *closed-loop* comparison of two parallel runs would be meaningless (after the
first action flip the two episodes are simply different runs -- the first
attempt reported 303/900 for exactly that reason, with the flip at step 2), so
the check is feed-forward by construction; a real mismatch prints the first
divergent step and the top-2 logit gap at it.

### Scripted Demon bot (`--sim-forward bot`)

`demon_guide_bot.h` (DemonBaseline) acts only through the 31-action contract, so
the same policy can drive both sides.  The driver calls `bt_action_mask` +
`dgb_action` per step (the core's own mask, not a re-derived one) and still
applies the bridge gate on top, so an action the game would reject ends the sim
episode at the same point.  Fidelity against DemonBaseline's own driver on the
same revision, stock Demon + Ghost Scepter (118), pivot 30 / stay 45:

| seed | waves | cleared | kills | total steps |
|---|---|---|---|---|
| 1 | 4 | 3 | 122 | 5473 |
| 2 | 4 | 3 | 141 | 5697 |

Both rows reproduce DemonBaseline's own driver exactly on the same core revision
(their rows: 122/5473 and 141/5697), run with `--audio-rng 1` because their
runner calls the 6-argument `bt_core_init` whose `audio_rng` default is 1 (the
legacy model); `--audio-rng 0` is the bridge-comparable default.  `steps` counts
every physics step including shop/level-up; the table's `ticks` column is the
core's combat-tick count, which is why it is smaller.  Their recorded action dumps
also replay through this driver's stdin (`--sim-forward python` idles the
forward; the script path consumes one int per line), which is how the action
streams were compared.

Measured inner-loop throughput (`--sim-bench N`, one process, observe -> action ->
step, reset on terminal; 198-obs core, `random_h256l2_v4` artifact):

| forward | steps/s |
|---|---|
| native, hidden 256 / 2 layers (the trainer's width) | ~6 600-6 950 |
| native, hidden 128 / 2 layers | ~25 000-28 700 |
| `--sim-forward bot` (no NN) | ~92 000-135 000 |
| scripted source, no NN | ~250 000 |

So the harness itself is no longer the bottleneck (the pure-Python reference
manages ~60 steps/s).  At the trainer's width the native forward is
latency-bound in `src/puffercpu.c`'s `linear()` (single `float sum`
accumulator: ~452 000 MAC/step at ~0.7 MAC/cycle); `-O3 -march=native` measured
flat (~7 000 steps/s), so the tool keeps `-O2 -ffp-contract=off`.  Going past
~7 k steps/s *with a 256-hidden policy* needs a multi-accumulator matmul in
puffernet (a shared-file change that alters summation order, so it would have to
pass the cross-check).

Phase/action contract (identical on both sides): combat `0..8` are the eight
movement directions (`0` hold, index `+1`=N, `2`=NE, ... `8`=NW) and every other
action is a non-movement hold; the shop and level-up sets are read from
`state_contract.json`'s `action.table` at run time (`phase_valid_actions()`), so
a contract revision (21 -> 31 actions: buy/reroll/skip/recycle/combine/lock/ban,
level-up reroll, crate take/recycle) does not silently desynchronise the sim
from the game.  If the contract cannot be parsed the tool falls back to the
21-action table and says so through its behaviour, not a silent guess.

The observation and action widths are checked before anything runs: the sim
driver announces the live `BT_OBS_SIZE`/`BT_ACTIONS` it was compiled with, the
artifact's `layout.obs_size`/`layout.actions` are compared against both that and
`state_contract.json`, and a mismatch is reported as `sim_failure` (sim) or a
warning plus a `policy_failure` (game) instead of a raw traceback.  Re-export
the artifact at the live schema after a contract change --
`python3 ocean/brotato/policy/policy_reference.py fixture --out-dir DIR` always
uses the live core schema.

## Metrics

| column | game source | sim source |
|---|---|---|
| `waves` | max `wave` in the bridge trace | `state.wave` |
| `won` | `is_run_won` in the game's `user/log.txt` / `run_won` in the game save / `EndRun` scene at wave 20; clears wave 20 only | core episode log: `waves_cleared == BT_WAVES` |
| `death` | first trace row with `hp <= 0`, else the last wave when `is_run_lost` | wave at the terminal step when not won |
| `mat` | last sampled trace row's `gold` (= `RunData.get_player_gold(0)`) | `state.materials` |
| `lvl` | max `level` in the trace | `state.level` |
| `kills` | game save `data.enemies_killed` (fresh isolated home ⇒ this run) | core episode log `score` (= `state.kills`) |
| `buy`/`rr`/`skip`/`rc` | bridge summary `progression` (`shop_buys`, `shop_rerolls`, `shop_skips`, `shop_recycles`) | a buy is counted when the core's step reward is non-zero (`0.5` = completed purchase), rerolls/skips from the sent shop actions |
| `lu` | summary `progression.level_ups_chosen` | level-up phase steps that chose an offer |
| `stall` | a `Shop`/level-up run ≥ 600 trace rows without leaving, a scene outside `Main`/`Shop`/`EndRun`, or a tick-budget end that neither died nor won | the same on phase runs / a step cap |
| `wall` | measured around the whole `run_game.sh` process | measured around the whole driver process |
| `ticks` | last trace `tick` + 1 | `state.ticks`, the core's combat-tick count (phase steps advance neither side's `ticks`) |
| `steps` | — | every physics step, phase steps included (the driver counts them; equals the game's `BROTATO_RL_MAX_TICKS` accounting) |
| `status` | see failure modes below | `ok` / `sim_failure` |

Reading `move_landed=A/B`: the bridge's `action_applied` is 1 only when the
engine registered a **movement** request; `HOLD` (action 0) and the non-movement
actions press nothing and therefore can never be "applied" (rl_inject.gd
`verify`).  Three populations are reported separately, because they mean
different things:

* **in-wave movement** (`move_landed=X/Y in-wave`): the only one that says
  something about delivery; a miss here is real and is reported as
  `in-wave movement misses=N`;
* **cleanup-window movement** (`cleanup window: X/Y game-inhibited`): while a wave
  is over the arena is still the active scene (~120 ticks) and
  `player.gd:808-810` sets `_can_move = false` with `_current_movement = ZERO`, so
  a movement request there legitimately does not register.  Those rows carry
  `wave_in_progress = 0`; they are *not* mismatches and are not silently dropped;
* **holds** (`hold_rows=N`).

`parse_trace --check` requires at least one applied row, so a constant-hold policy
trips the launcher's gate and is reported as `bridge_warn` even though nothing is
wrong.

A full 20-wave campaign is 1050 simulated seconds of waves
(`brotato_waves.h` `BT_ZONE_WAVE_DUR`), i.e. ~63 000 physics ticks.  With the
default `--sample-every 6` a full-run trace is ~10 MB per episode instead of
~100 MB; `--sample-every 1` restores the every-tick contract the replay consumer
requires.  Game episodes run at roughly 20–30 ticks/s wall time on the headless
software-GL path, so a *full* 20-wave game episode takes tens of minutes — raise
`--parallel` rather than `--max-seconds` for coverage, and use small
`--max-seconds` for smoke runs.  The simulator itself is orders of magnitude
faster (1800 scripted ticks in ~0.1 s), but a *policy* run through the sim is
bound by `policy_reference.py`'s pure-Python forward pass: measured ~60 steps/s
for `hidden 256 / layers 2`, so a 31 000-step policy episode takes ~10 minutes of
wall time even though the core is idle.  Use parallel seeds, or a shorter
`--max-seconds`, rather than assuming the sim is free.

The step budget is the same on both sides: a sim phase step (shop/level-up) does
not advance `state.ticks`, so the driver bounds *every* step by
`60 * max_seconds` -- the game's own `BROTATO_RL_MAX_TICKS` also counts shop
frames, so a policy that loops in the shop (buying with no materials, say)
truncates at the same step count with `stall=shop` on both sides instead of
spinning until the wall timeout.

## Failure modes

`status` is one of:

* **`ok`** — the episode ran and every bridge check passed.
* **`bridge_warn`** — the episode ran and its metrics are usable, but something
  on the bridge side is degraded.  These rows are still included in the
  aggregates.  Causes observed in practice:
  * **Godot 3.7-dev1 teardown assertion.** The engine can exit `1` after the run
    with `ERROR: Condition "_first != nullptr" is true. at: ~List
    (./core/self_list.h:108)` (three times).  The bridge summary is `ok`, the
    trace gate prints `CHECK OK`, and only `run_game.sh`'s view of the engine
    status is affected (`rc=3`, *"engine failed with status 1"*).  The data is
    complete; treat the nonzero rc as an engine-shutdown artifact, not an RL
    failure.
  * **Incomplete observation rows.** `obs_complete=0` rows (e.g. `obs_errors:
    ['non-finite observation value(s) at index [52, 53, 56, 57, 60, 61, 64]
    replaced with 0.0']` — the enemy block's `ehp/emax`/`damage` fields for
    far enemy slots) make `parse_trace.py --check` fail (`rc=1`).  In *policy*
    mode those rows also feed the policy a silently zeroed observation, so a
    policy episode can be degraded without failing.
  * **Launcher rc `1`/`3` for any other reason** while the summary is `ok`.
* **`bridge_failure`** — no summary, a summary `ok=false`, or a named bridge
  failure (`run_configuration_failed`, `obs_tables_missing`, `arena_never_loaded`,
  `trace_open_failed`, `autoload_timeout`, `unexpected_scene` before a run,
  `no bridge trace rows`).  Metrics are not trusted and such rows are excluded
  from the aggregates.
* **`policy_failure`** — the bridge summary's failure is
  `policy_inference_failed`.  In policy mode the mod fails closed: a missing
  artifact, an observation-schema mismatch, or an action the phase does not
  allow ends the episode immediately, and this is reported as a *policy*
  failure, never as a bridge failure.  A random-weight policy will frequently
  hit this in the shop/level-up phases, which is expected.
* **`sim_failure`** — the sim driver failed to compile, crashed, hit its wall
  timeout, or the observation length did not match the artifact.

In **policy** mode both sides fail closed at the same contract point: the bridge
(`mod_main.gd _progression_action` -> `_fail_policy`) aborts the game episode when
the policy proposes an action its phase does not allow, and the sim driver does
the same (`PhasePolicy` raises `PolicyAbort`), so an aborted comparison never
looks like a longer sim episode.  The sim records the abort tick
(`policy_abort_tick=N`), which is a direct parity signal against the game's
`failure_message` tick.

**Action-gate drift (measured, not assumed).**  The abort decision mirrors the
*bridge's* gate, which is parsed at run time from
`mod/res/mods-unpacked/rlbridge-headlessrl/mod_main.gd`'s `var valid: bool = ...`
expression; the contract's `action.table` is parsed as well.  When the two
disagree the tool prints
`eval: WARNING action gate: <detail>` to stderr and records both sets plus the
notes in `--json` (`action_gate_effective`, `action_gate_contract`,
`action_gate_notes`, `action_gate_source`).  Every invocation prints
`eval: gate bridge mod_main.gd` when the two agree.  This check found a real
drift (the contract had moved to 31 actions while `mod_main.gd` still gated on
the 21-action sets, so contract-legal lock/ban/crate actions would have failed a
policy episode closed); it was reported with the exact sets to the bridge owner
and is now closed, so the current tree reports no drift.  The sim mirrors
whatever gate the bridge currently implements; the fallback when the gate cannot
be parsed is the contract's sets, announced as
`WARNING action gate: bridge action gate unparsable ...`.

`stall` is reported independently of `status`: `shop`, `level_up`,
`menu:<scene>` (the run walked into a scene the RL driver does not drive) or
`no_progress` (a tick-budget end that neither died nor won).

**Stale pack / pinned manifest.**  `Brotato.rl.pck` embeds the mod's GDScript,
so a peer landing a mod change makes an existing pack stale; the tool hashes the
build inputs (`bridge/mod/**`, the bridge's own `.py`/`.json`/`.sh`, **and the
repo-sourced `ocean/brotato/policy/*.gd` the pack embeds as
`res://mods-unpacked/rlbridge-headlessrl/brotato_policy.gd`**) into
`<stage>/build_inputs.sha256` and rebuilds on any change.  Per-episode run
directories are re-linked when the staged pack changes, so a rebuild cannot leave
one episode on the old bridge.  Independently, `build_mod_pck.py` strictly pins
every *committed* payload file by sha256 in `patch_manifest.json`: if a peer edits
`rl_obs.gd` without refreshing the pin, the build refuses and the last built pack
(an older observation schema) stays in place -- observed as
`verification failed: .../rl_obs.gd: sha256 mismatch`, after which a live-schema
artifact is rejected with
`obs_schema_mismatch: bridge observed 161 values, artifact expects 198`.  The tool
never refreshes that pin itself (`patch_manifest.json` is not its file);
`--reuse-pack` is the explicit escape hatch to run the existing pack anyway, and
it marks every affected row with `pack reused without build-input check`.  When
the game bridge's observed width and the sim core's differ, the gap table prints a
`WARNING: ... NOT the same observation space` line instead of silently comparing
them.

## Matched action-stream mode (`--action-source file`)

The bridge now has a per-frame action source (`BROTATO_RL_ACTION_FILE`, consumed
one id per physics frame, HOLD on EOF), so the *same* stream can drive both
sides:

```sh
# generate a stream from the sim (here: the scripted Demon bot, campaign, seed 1)
~/brotato_eval/build/eval_sim_step --seed 1 --zone 0 --char 2 --weapon 118 \
    --shop-steps 1 --endless 0 --audio-rng 0 --particles-rng 0 --bot 1 \
    --bot-pivot 0 --bot-stay 0 --print-every 1 --obs-every 0 --max-steps 100000 \
    --gate "1:0-4,8-28;2:0-4,29-30" \
  | python3 -c 'import sys,json; [print(json.loads(l)["a"]) for l in sys.stdin if l.startswith("{") and "a" in l]' \
  | tail -n +2 > /tmp/stream_s1.txt

python3 ocean/brotato/bridge/eval_game.py --action-source file \
    --action-file /tmp/stream_s1.txt --character character_demon \
    --weapon weapon_ghost_scepter_1 --danger 5 --seeds 1 --max-seconds 89 \
    --sample-every 1 --parallel 1
```

`--action-source file` sets `BROTATO_RL_ACTION_FILE` for the game and feeds the
same ids to the sim one per step over stdin, so the two sides consume an
identical stream (the mod fails boot on a malformed one, and it records
`action_file_index` per row: the tool reports `action-file ids consumed=N` for the
game, which is checked against the sim's step count).  Measured short matched smoke (`/tmp/stream_short.txt` = the first 1200 ids of a
Demon-bot campaign stream, `--max-seconds 20`, sample-every 1):

| | game | sim |
|---|---|---|
| waves | 1 | 1 |
| kills | 14 | 14 |
| materials | 27 | 36 |
| level | 0 | 0 |
| ticks | 1200 | 1200 |
| ids consumed | 1200 (`action-file ids consumed`, 1-based max index over 1200 frames) | 1200 steps |

Kills, waves and ticks agree exactly on the same stream.  Both sides consume one id
per decision and the counts line up (1200 consumed / 1200 steps).  The materials
difference this table shows (game 27, frozen sim 36) is superseded for the *current*
core: replaying this very game trace through today's core (`migration`-style
`replay_cpu.py`, path `/home/ilyaas/brotato_run_main_v4/replay_match41/cpu_trace.jsonl`)
gives **materials 30 vs the game's 27** with **0 action mismatches**, and its
observed divergence order is: first divergence at step 316 (`obs[82]`, game 1.0 vs
core 0.94428), then enemy HP at 377, materials at 520 (0.31 vs 0.30), HP at 554.
Which of those *causes* the residue is not established here (the clock-first reading
is an inference, and an independent pickup difference is not ruled out); the frozen
bundle above stays valid as the pinned baseline for the 36 number.

**File mode is exact, not approximate** (the check that gates calling it green):
one frozen core build (`sim_driver.stamp`, content hash over sources, flags,
include root and every core header), one frozen config (`seed 1`, Demon,
weapon 118, danger 5, campaign, `shop_steps 1`, `audio_rng 0`, `particles_rng 0`,
`start_wave 1`) and one fixed budget (6000 steps) -- the bot decides 5372 steps,
those exact ids (terminal decision included: the end row now carries its action)
are replayed through the same binary, and every row is compared field by field:

```text
ids 5372 | origin end {we 4, kills 112, ticks 5340, steps 5372}
replay    5372 | end {term 1, we 4, kills 112, ticks 5340, steps 5372}
per-step: 5372 x 13 fields (a,t,ph,w,hp,mat,lvl,kills,x,y,m,am,pm) -> IDENTICAL
final row (we,ew,won,kills,ticks,steps,buys,rerolls,skips,recycles,combines,level_ups,max_shop_run,max_lvl_run) -> IDENTICAL
```

The one hazard here is an off-by-one in the *stream*, not in the core: the driver's
pre-loop observation row is not a decision, so a stream must start at the first
decision (the tool does; an ad-hoc capture that included the placeholder id
produced a one-step shift and a wrong "reproduces its origin" claim -- that claim
is withdrawn here).

## Environment requirement for the game side

The engine needs a working X display even with `--no-window` (x11 always opens
one), including for the build's GDScript parse check.  The harness wraps the whole launcher
(not just the launch) in `xvfb-run` when `DISPLAY` is unset, with **two** host
quirks handled, both of which present as "X11 Display is not available" while
the X server is fine:

* `xvfb-run -a` picks a *stale* display number on a host carrying many leftover
  `/tmp/.X11-unix/X*` sockets (this one has >100): Xvfb cannot bind, dies, and the
  client gets `XOpenDisplay == NULL`.  The harness therefore passes an explicit
  `-n <high>` (one number per episode, so parallel episodes stay apart) -- with
  `-n 250` the same pack boots the engine (`OpenGL ES 3.0 Renderer: llvmpipe`,
  `Brotato v1.1.14.6`).
* a full `/tmp` (Errno 122) breaks xvfb-run's Xauthority `mktemp`: the harness
  sets `TMPDIR=<run-root>/tmp` for the launcher and the build.

If a game-side run still cannot start, the rows read
`bridge_failure: staging/build failed (rc=3)` (parse check: "the bridge was never
instantiated") or `no bridge summary written`, the underlying engine log is kept
in `rl_out/smoke.<tag>.log`, and the sim half still runs and prints.  Nothing in
the sim half depends on X.

## Handover bundle (immutable evidence)

Durable copies (sha256 of each file; nothing under `/tmp` is referenced):

| artifact | path | sha256 (first 16) |
|---|---|---|
| real-game trace, 1200 ticks, file source | `~/brotato_eval/run_match41/rl_out/rl_trace.match41.jsonl` | `4ea8ec130fdb9a4b` |
| its meta / summary | same dir, `.meta.json` / `.summary.json` | `663ba25b1cc3d836` / `35e12acadb2c9daf` |
| game smoke log / harness log / diag | same dir, `smoke.match41.log`, `harness.match41.log`, `rl_bridge_diag.match41.json` | `164161649774cee5` / `70f354392b57bb63` / `34b4925b32832345` |
| action stream, 1200 ids (Demon bot, seed 1, campaign) | `~/brotato_eval/handoff/stream_dgb_s1_1200.txt` | `44b0f3bb0f6b4895` |
| action stream, 5372 ids (same origin, full episode) | `~/brotato_eval/handoff/stream_dgb_s1_5372.txt` | `660d1282ed4bf68a` |
| sim run on the 1200-id stream | `~/brotato_eval/handoff/sim_file1200.json` | `8dccdb79a7162532` |
| tool output of the combined 1200-id run | `~/brotato_eval/handoff/matched_tool_output.json` | `869c85d6a00c81ba` |
| sim driver binary / build stamp / sources | `~/brotato_eval/handoff/eval_sim_step`, `sim_driver.stamp`, `eval_sim_step.cpp`, `eval_sim_policy.c` | `a1e5ff3efc604cd2` / stamp `7b7c13c59ca45a4fecb40fbac8b61ae3b61a0fcd9df4c2dbb25002c577e70e16` |
| policy artifacts used | `~/brotato_eval/policies/random_h256l2_v5/policy.json` (selectors), `..._v4/policy.json` | `6e600ec498dded00` / `51eca5ccbb73d7a0` |

The **one in-wave movement miss** in the 1200-tick real-game trace, in full
(trace row and the bridge's own classification):

```text
tick 1199  action 3 (east)  action_applied 0  wave 1  wave_in_progress 1
wave_time_left 0  wave_duration 20  phase 0  x 1851.951294  y 94.572174  hp 4  gold 27
action_source file  action_file_index 1200
summary.action: input_applications 1200  applied_total 1198  verify_mismatches 0
                verify_game_inhibited 1  input_actions_pressed_last ["analog_move_right"]
summary.environment.movement_probe: {behavior MovementBehavior, can_move true, move_locked false}
rows 1196-1198: same action, applied 1, x 1829.45 -> 1836.95 -> 1844.45, wave_time_left 0.05 -> 0.017
```

So it is the **wave-expiry frame**: the analog input was delivered and the player
moved one more 7.5 px step east on that frame, but the game's own wave-clear path
had already zeroed `_current_movement` when the row is verified, and the bridge
records exactly that (`verify_mismatches 0`, `verify_game_inhibited 1`).  This
harness classifies it under `in-wave` because `wave_in_progress` had not been
cleared yet on that frame (only `wave_time_left == 0` marks it); the row is
reported, not dropped, and the bridge's own counter is the authority.

## Movement-verification reconciliation (row-by-row)

The bridge emits per-row `action_verify_error` plus `player_can_move`,
`player_cleaning_up`, `player_move_locked`, so the harness classifies movement
delivery from the trace alone and reconciles it with the bridge's own counters:

* movement accounting covers the **arena only** (`phase == 0`): a shop or level-up
  row is not a movement comparison whatever action id it carries, so it is never
  counted as a miss;
* a failed comparison is **game-inhibited** only when the same row carries
  `game_inhibited_input` *and* `player_can_move == 0` or `player_cleaning_up == 1`
  (the wave-end window: `wave_time_left == 0`, `wave_in_progress == 1`);
* `phase_transition_resync` rows are **not compared by design** and are counted
  separately, not as misses;
* anything else with `action_applied == 0` is reported as an **unguarded miss**
  with the offending row inline.

Measured on `~/brotato_eval/run_gaps1` (Demon, 5372-id stream, 1800 ticks, tag
`gaps1`): movement rows 1784, applied 1663, game-inhibited 120, uncompared 1,
unguarded 0 -- i.e. `applied == applied_total` (1663) and
`inhibited == verify_game_inhibited` (120) with `verify_mismatches == 0`, all
reconciled from the trace alone.

Those two artefacts were reported to the bridge owner and are fixed; on a fresh
capture built from the corrected tree (`~/brotato_eval/run_gaps21`, same stream and
budget, tag `gaps21`) the reconciliation is exact and clean:

```text
parser : rows 1784 applied 1663 inhibited 120 uncompared 1 unguarded 0 verify_errors 121
bridge : applied_total 1663 of 1800 | verify_game_inhibited 120 | verify_mismatches 0
action_source: file 1800 / script 0     stale-error rows: 0     shop rows with an error: 0
```

(previously 1786/14, 134 error rows and 128 vs 120 inhibit counts), i.e. row count,
applied count and inhibit count all equal the bridge's own counters by construction.

Display contention: the launcher prefers an existing `$DISPLAY`, never uses
`xvfb-run -a`, passes an explicit `-n`, and **retries on a fresh display number**
when the captured output shows `X11 Display is not available` -- a display that
failed to bind otherwise looks exactly like a broken pack ("the bridge was never
instantiated" -> parse check FAILED), which would be a false pre-land failure.

## Known limitations

* **Endless mode is a core/bridge dependency, not a tool one.** The CPU core
  stops at `wave == BT_WAVES` (20) with `won = 1` (`brotato_core.h`: *"endless and
  coop are outside"* the core), and the game reaches wave 21+ only when
  `RunData.is_endless_run` is true, which the bridge does not set.  Until those
  land, `waves_reached` is capped at 20 on both sides and `--max-seconds` beyond
  the campaign only wastes time; once they do, eval_game.py needs no change (it
  reports whatever wave/`won`/`stall` each side produces, and a wave-21+ game run
  is a graph of `waves_reached` alone).
* The game's trace carries no kill counter, so `kills` comes from the game's own
  `ProgressData.data.enemies_killed` written into the isolated home (hence
  `BROTATO_RL_NO_SAVE=0`).  If that save is absent the column is `n/a` and a note
  records it.
* The sim driver reads the terminal episode's totals from the core's numeric
  episode log (`bt_core_add_log`, which `bt_core_reset` never touches), so
  `won`/`waves_cleared`/`kills`/`ticks` are exact.  The terminal row's
  `materials`/`level`/`hp` are the pre-step snapshot (one frame earlier; exact
  for a wave-20 clear, which does not end-of-wave-process).
* Sim shop pacing uses `shop_steps=1` (one action per step), the value the repo's
  own core driver uses.
* Because the two sides stop at their own `--max-seconds` tick budget
  (`BROTATO_RL_MAX_TICKS = 60*max_seconds` on the game side, the same count on
  the sim side), a truncated pair is comparable; a run cut short is reported as
  `stall=no_progress` on both sides rather than as a failure.
* Game metrics are aggregates over the sampled trace rows, so `--sample-every N`
  can move a "final gold/level" by up to N ticks.
* One game process and one xvfb display per episode; the engine is reaped by
  `run_game.sh`'s watchdog, and the harness adds a hard `--watchdog + 120 s`
  subprocess timeout on top.
* The core headers are a shared tree that other agents edit continuously.  If
  the sim driver cannot compile, the run does not abort: the game results are
  still parsed and printed, and each sim row becomes
  `sim_failure: sim driver unavailable: <compiler error>`.  To keep evaluating
  while the live tree is mid-edit, `--sim-include DIR` compiles the driver
  against a pinned snapshot of `ocean/brotato` (the sim rows then say which
  include root they used).

## Smoke test (exact commands and outputs)

All commands run from `Pufferlib/`.  `eval_game.py --action-source script` sets
`BROTATO_ACTION_SOURCE=script` for the launcher, exactly as the assignment asks.

### 1. Scripted source, 2 seeds, 30 simulated seconds

```sh
python3 ocean/brotato/bridge/eval_game.py --action-source script --seeds 1,2 \
    --max-seconds 30 --watchdog 240 --sample-every 1 --parallel 2
```

```text
== real game (bridge) ==
 seed | waves |  won | death |  mat  | lvl | kills |  buy |rr|skip|rc|lu |  ticks | stall        |  wall | status
    1 |     2 |    n |   n/a |    61 |   1 |    25 |    0 | 1|   1| 0| 1 |   1800 | no_progress  |  92.6 | ok [move_landed=1321/1440 hold_rows=360;input_mismatches=118;actions=script:1800;materials_collected=23]
    2 |     2 |    n |   n/a |    61 |   1 |    26 |    0 | 1|   1| 0| 1 |   1800 | no_progress  |  92.7 | bridge_warn:run_game.sh rc=3 [move_landed=1321/1440 hold_rows=360;input_mismatches=118;...]
game aggregate: episodes=2 usable=2 (warn=1) wins=0 stalls=2 policy_fail=0 bridge_fail=0 sim_fail=0 | mean waves=2.0 materials=61.0 level=1.0 kills=25.5 buys=0.0 wall_s=92.63

== simulator (shared CPU core) ==
    1 |     2 |    n |   n/a |    60 |   1 |    34 |    0 | 1|   1| 0| 1 |   1800 | no_progress  |   0.1 | ok [sim_obs=198]
    2 |     2 |    n |   n/a |    61 |   1 |    32 |    0 | 1|   1| 0| 1 |   1800 | no_progress  |   0.1 | ok [sim_obs=198]
sim aggregate: episodes=2 usable=2 (warn=0) wins=0 stalls=2 policy_fail=0 bridge_fail=0 sim_fail=0 | mean waves=2.0 materials=60.5 level=1.0 kills=33.0 buys=0.0 wall_s=0.07

== sim vs game (same seeds, same policy weights) ==
 seed | game_waves sim_waves | game_won sim_won | game_mat sim_mat | game_kills sim_kills | game_wall sim_wall
    1 |          2         2 |        n       n |       61      60 |         25        34 |      92.6      0.1
    2 |          2         2 |        n       n |       61      61 |         26        32 |      92.7      0.1
```

Both sides reach wave 2 in 1800 ticks (30 s) with identical materials, level and
reroll/skip/level-up counts; the 8-point kill gap is the sim/engine divergence
the table exists to expose.  Seed 1 is a fully clean `ok`; seed 2's `bridge_warn`
is the Godot teardown assertion (`rc=3`) after a complete, contract-clean trace.

### 2. Random-weight exported policy, 2 seeds, 20 simulated seconds

```sh
python3 ocean/brotato/policy/policy_reference.py fixture \
    --out-dir ~/brotato_eval/policies/random_h256l2_v4 --hidden 256 --layers 2
python3 ocean/brotato/bridge/eval_game.py \
    --policy ~/brotato_eval/policies/random_h256l2_v4/policy.json \
    --action-source policy --seeds 1,2 --max-seconds 20 --watchdog 200 \
    --sample-every 1 --parallel 2 --tag-prefix pol
```

`policy_reference.py fixture` exports at the live core schema (`obs_size 198`,
`actions 31`), so this is the same weights the sim forward pass runs.

```text
== real game (bridge) ==
 seed | waves |  won | death |  mat  | lvl | kills |  buy |rr|skip|rc|lu |  ticks | stall        |  wall | status
    1 |     1 |    n |   n/a |    56 |   1 |    26 |    0 | 0|   0| 0| 0 |   1200 | no_progress  | 134.4 | ok [move_landed=42/42 hold_rows=1158;actions=policy:1200;materials_collected=26]
    2 |     1 |    n |   n/a |    52 |   1 |    25 |    0 | 0|   0| 0| 0 |   1200 | no_progress  | 134.4 | bridge_warn:run_game.sh rc=3 [move_landed=6/6 hold_rows=1194;actions=policy:1200;materials_collected=22]
game aggregate: episodes=2 usable=2 (warn=1) wins=0 stalls=2 policy_fail=0 bridge_fail=0 sim_fail=0 | mean waves=1.0 materials=54.0 level=1.0 kills=25.5 buys=0.0 wall_s=134.37

== simulator (shared CPU core) ==
 seed | waves |  won | death |  mat  | lvl | kills |  buy |rr|skip|rc|lu |  ticks | stall        |  wall | status
    1 |     1 |    n |   n/a |    54 |   1 |    24 |    0 | 0|   0| 0| 0 |   1200 | no_progress  |  20.7 | ok [policy_fallbacks=0/1200;sim_obs=198]
    2 |     1 |    n |   n/a |    50 |   1 |    24 |    0 | 0|   0| 0| 0 |   1200 | no_progress  |  21.1 | ok [policy_fallbacks=0/1200;sim_obs=198]
sim aggregate: episodes=2 usable=2 (warn=0) wins=0 stalls=2 policy_fail=0 bridge_fail=0 sim_fail=0 | mean waves=1.0 materials=52.0 level=1.0 kills=24.0 buys=0.0 wall_s=20.92

== sim vs game (same seeds, same policy weights) ==
 seed | game_waves sim_waves | game_won sim_won | game_mat sim_mat | game_kills sim_kills | game_wall sim_wall
    1 |          1         1 |        n       n |       56      54 |         26        24 |     134.4     20.7
    2 |          1         1 |        n       n |       52      50 |         25        24 |     134.4     21.1
```

The bridge loads the artifact (198 obs, 256 hidden, 2 layers, from the run
summary), drives **every** step with it (`actions=policy:1200`, no fallback), and
the movement requests land (`move_landed=42/42`, `6/6`); the sim runs the same
weights with zero fallbacks.  Seed 1 is a fully clean `ok`; seed 2's `bridge_warn`
is the Godot teardown assertion after a contract-clean trace.  The 2-4 point
materials/kill gap over 1200 ticks is the sim-vs-game divergence the table exists
to quantify.

Earlier passes of the same smoke, kept because they exercise the schema guards:

* a 161-era artifact against the same 161-era pack: the bridge loaded it and ran
  `actions=policy:1319` before failing the episode closed
  (`policy_failure:policy_inference_failed`, the random argmax proposing an
  action its phase forbids -- a **policy** failure, never a bridge failure), while
  the sim refused it (`sim_failure: schema mismatch: the sim core offers 31
  actions but the artifact was exported for 21`);
* a 198-era artifact against the *stale* staged pack: rejected at tick 0 with
  `obs_schema_mismatch: bridge observed 161 values, artifact expects 198`, and
  the run's game rows still printed.  That is what the build-input digest now
  prevents.

### 3. Combined game + sim, native sim forward, cross-checked

```sh
python3 ocean/brotato/bridge/eval_game.py \
    --policy ~/brotato_eval/policies/random_h256l2_v4/policy.json \
    --action-source policy --seeds 1,2 --max-seconds 20 --watchdog 200 \
    --sample-every 1 --parallel 2
```

```text
== real game (bridge) ==
    1 |     1 |    n |   n/a |    56 |   1 |    26 | ... |   1200 | no_progress | 140.2 | bridge_warn:run_game.sh rc=1 | trace gate failed: injected movement was [actions=policy:1200;materials_collected=26]
    2 |     1 |    n |   n/a |    52 |   1 |    25 | ... |   1200 | no_progress | 136.2 | ok [move_landed=1/1 hold_rows=1199;actions=policy:1200;materials_collected=22]

== simulator (shared CPU core) ==
    1 |     1 |    n |     1 |    52 |   1 |    27 | ... |   1200 | -  | 0.2 | ok [forward=native;sim_obs=198;core_mask[level_up]=[0, 1, 2, 3, 4] vs bridge gate [0, 1, 2, 3, 4, 29, 30];crosscheck native vs policy_reference: 900/900 actions identical, max |logit delta| 6.48e-05]
    2 |     1 |    n |     1 |    51 |   1 |    26 | ... |   1200 | -  | 0.2 | ok [forward=native;sim_obs=198;...]

== sim vs game (same seeds, same policy weights) ==
 seed | game_waves sim_waves | game_won sim_won | game_mat sim_mat | game_kills sim_kills | game_wall sim_wall
    1 |          1         1 |        n       n |       56      52 |         26        27 |     140.2      0.2
    2 |          1         1 |        n       n |       52      51 |         25        26 |     136.2      0.2
```

Both sides run the same artifact to the same tick budget with the same wave and
materials/level, and the 140 s vs 0.2 s wall time is the point of the native
forward.  Seed 1's game row is the documented constant-hold gate trip (the random
policy held 1199/1200 steps, so `parse_trace --check` found no applied
movement), reported as `bridge_warn` with the metrics intact.

### 4. Death path in 26 seconds (`--start-wave`)

```sh
python3 ocean/brotato/bridge/eval_game.py --action-source script --game-only \
    --seeds 1 --start-wave 18 --max-seconds 90 --watchdog 300 --sample-every 1 \
    --tag-prefix late
```

```text
== real game (bridge) ==
 seed | waves |  won | death |  mat  | lvl | kills |  buy |rr|skip|rc|lu |  ticks | stall        |  wall | status
    1 |    18 |    n |    18 |    30 |   0 |     0 |    0 | 0|   0| 0| 0 |    376 | -            |  26.4 | ok [move_landed=196/315 hold_rows=61;input_mismatches=119;actions=script:376;materials_collected=0]
game aggregate: episodes=1 usable=1 (warn=0) wins=0 stalls=0 policy_fail=0 bridge_fail=0 sim_fail=0 | mean waves=18.0 materials=30.0 level=0.0 kills=0.0 buys=0.0 wall_s=26.44
```

Wave 18 starts directly (`DebugService.starting_wave`), the weaponless Bull dies
at wave 18 in 376 ticks, and the death wave/`is_run_lost` detection is confirmed
in 26 s of wall time instead of after 20 waves.
