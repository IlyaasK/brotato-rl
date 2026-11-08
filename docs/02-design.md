# Environment design

Brotato splits into three different-timescale problems. They are trained separately.

| Phase | Freq | Nature | Winning component |
|---|---|---|---|
| Combat | ~10–15 Hz | continuous control | movement (weapons auto-fire) |
| Level-up | 0–few per wave | 4 stat cards + reroll | small discrete |
| Shop | 1 per wave | 4 slots, lock, reroll, combine | draft/sparse |

**No single policy over all of it.** Two nets (combat + draft), or two heads on a shared
trunk only after each is proven. Different timescales, different credit assignment.

## Characters: curriculum, not flattening

62 characters change the *optimal* policy (Soldier can't move-attack; Streamer/Speedy
want movement; Pacifist wants live enemies; Bull has no weapons; Demon pays HP; Baby
has 24 slots). Do not train all at once.

1. **Ranger** (or Well Rounded), Danger 0, fixed starting weapon. Scripted shop.
2. Same character D1 → D5.
3. Add a family: Ranger/Hunter/Soldier (ranged) or Crazy/Gladiator/Knight (melee).
   Character identity as a **learned embedding** (8–16d), not a 62-d one-hot.
4. Hold out freaks (Pacifist, Bull, Demon, Baby, Cryptid, Fisherman) until the
   generic policy is solid.

Randomize one dimension at a time: starting weapon (within legal set) only after
step 1 is stable; character + danger + shop RNG all at once will not converge.

## Combat observation (every decision)

Privileged structured state (we have decompiled sources). No pixels.

- **Player ~20 f**: HP/max, invuln timer, speed, dodge, armor, pickup range,
  lifesteal, knockback, materials on ground nearby, standing-still flag, range,
  damage/atk-speed (collapsed stat vector), map-relative position.
- **K=16 nearest enemies**: dx, dy, vx, vy, radius, hp_frac, is_elite, is_boss,
  is_horde, is_tree. Distance-sorted, padded + mask.
- **K=8 nearest threats**: enemy projectiles + charging tells.
- **K=8 nearest pickups**: material / crate / consumable / tree + value.
- **16×16 player-centric occupancy grid** (enemy density) — prevents
  “surrounded but K-nearest looks fine” blindness.
- **Wave context**: wave/20, time left, enemies alive, danger, elite/horde flags
  (known from D2+).
- **No items, no shop** in combat obs. Yes: compact stat vector (that's what items
  already collapsed into). Normalize to [-1,1]; HP as fractions.

## Shop / level-up observation

Draft state (~200 floats), not thousands:

- **4 offers ×** item feature vector (rarity, tags multi-hot, stat deltas, unique
  /cursed/structure/weapon flags, weapon class + tier, char-tag match) + price /
  materials + can_afford + would_auto_combine + locked.
  Feature vectors > 200-way one-hot: generalizes to new items/DLC.
- **Inventory**: weapon slots (type embed, tier, class bits); item bag as
  counts/tag-bag + same collapsed stat vector.
- **Economy**: materials, harvesting, reroll price, shop-price multiplier, wave,
  danger, next elite/horde.
- **Character embed** + current stats. Demon flag (“buying costs HP”).

No leakage: item *names*, wiki tiers, future rolls, future crate contents.

## Actions

- **Combat**: `Discrete(9)` (8-dir + idle), frame-skip 4–6 physics frames.
  No aim head — weapons auto-fire. (Box(2) is a later refinement.)
- **Shop**: 0–3 buy slot i, 4–7 toggle lock i, 8 reroll, 9 next wave, 10 combine.
  Recycle later (second-order eco trick).
- **Level-up**: 0–3 pick card, 4 reroll.
- **Mask invalid actions** (can't afford, broke, wrong phase). Two net heads, masks
  by phase. Don't make the net learn the UI.

## Reward

### Combat (dense)

```
r = -2.0 * damage_taken / max_hp
  + 0.01 * dt
  + 0.02 * materials_picked / wave_scale
  + 1.0 * wave_cleared
  - 5.0 * died
  + 20.0 * won_run
```

- **No per-kill reward** (dives into packs). Add tiny “enemies dying nearby” only
  if the agent turtles.
- Reward **damage taken**, not HP delta (else lifesteal/regen chars farm the reward).
- Pacifist/Harvesting chars get a specialist wave-end term later.

### Shop (sparse)

True outcome is 15–20 min delayed. Order of sanity:

1. **Script the shop** (tag match, same-class weapon, combine, don't dump gold
   wave 1, max 2 rerolls) while combat trains — this is how a D5 movement agent
   exists at all.
2. Freeze combat → train shop on **Monte Carlo return** = remaining waves survived
   + win bonus. ~20 decisions/run; small MLP + many runs suffices.
3. Light shaping only: +0.05 tagged buy, +0.1 combine, -0.05 reroll below 20% gold.
   **Never** reward spending gold.

## Env shape

- Episode = one run; truncate at curriculum cap (wave 5 → 10 → 20).
- `reset`: character (curriculum or sampled), danger, seed. Skip menus; start wave 1.
- `step`: combat advances N physics frames; on wave end / level-up / shop, switch
  phase and wait for a draft action (or run the scripted shop without asking the net).
- Death or wave 20 → `terminated`.
- Seed game RNG from env seed; eval = 100 seeded runs, **win rate + mean wave**, not reward.