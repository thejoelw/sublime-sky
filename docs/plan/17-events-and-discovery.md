# 17. Events and discovery

Status: draft · Depends on: [04 bodies and orbits](04-bodies-and-orbits.md), [06 hierarchy and cut](06-blob-hierarchy-and-cut.md),
[07 spatial queries](07-spatial-queries.md), [08 plugins](08-plugin-system.md), [10 simulation](10-simulation-and-scheduling.md),
[11 physics](11-physics.md), [16 entities](16-entities-and-creatures.md) · Used by: [12 rendering](12-rendering.md),
[13 lighting](13-lighting-atmosphere-and-sky.md), [15 persistence](15-persistence.md), [18 player](18-player-camera-and-navigation.md),
[19 tooling](19-tooling-and-testing.md)

## 1. Purpose and scope

This doc covers how the game produces a stream of happenings with a Zipf-like balance between common and rare that
the player can actually witness, and how what the player witnessed is recorded. It covers:

- what an event is, as data;
- the frequency design: a notability ladder, and how type and magnitude combine;
- emergent power laws (self-organized criticality) as the main source of the balance;
- an event taxonomy;
- an **event director** that biases *when* and *where* within limits, so that rare events are seen;
- the event lifecycle through the core (refinement, fast layer, time governor, persistence);
- the asteroid impact (canonical scenario 4) end to end;
- discovery: the journal, rarity, landmarks, calendar events, sharing.

**Out of scope here:**
- the physics of each phenomenon. That belongs to the plugin that owns it (snow, volcano, fire…) and to 10 and 11;
- how plumes, glow and aurorae are drawn (12, 13);
- the save format (15);
- UI layout (18);
- audio. No doc owns it yet (see Q17.8).

## 2. Requirements

| | requirement | traces to |
|---|---|---|
| must | Something is always happening nearby, and something rare happens rarely, on a ladder the designers can tune and measure. | pillar 1, constraint "rare events" |
| must | Events come from plugins. The core supplies the mechanism (records, scheduling hooks, refinement, the director) and knows no event by name. | pillar 4 |
| must | Events are deterministic: the same seed, state and inputs give the same events. Observers count as state. | D5 |
| must | Looking at a region changes what happens there by no more than a declared, measured amount. The director's bias is bounded and can be turned off. | pillar 5 |
| must | Big events read from far away: a storm on the horizon, a plume over the range, a fireball across the sky. | constraint "you can see a storm a long way away" |
| must | Events seen and unseen produce the same records and the same lasting outcomes, statistically. | pillar 5, scenario 4 |
| should | Rare events announce themselves (precursors), so they become places to go. | pillar 1 |
| should | A journal records what the player discovered, with a rarity that the world's own statistics estimate. | pillar 1 |
| should | Astronomical events (eclipses, comets) are the same for every player and can be predicted. | pillar 1, D5 |
| later | Sharing links, an almanac, sound cues, multiplayer witnesses. | |

## 3. What the prototypes taught

All from `blobby-test/planet/` (`DESIGN.md`, `PLUGINS.md`, `js/plugins/*.js`):

- **Events emerge from processes.** Nothing in the prototype is an "event system". The happenings come from the
  processes themselves:
  - **Volcanoes.** Each volcano recharges magma pressure over 2–8 years and erupts past 1 (`volcano.js`:
    `pressure += h / recharge`). Lava flows at 20–100 m³/s for half a day to 3.5 days, and the eruption opens with
    bombs at 80–250 m/s.
  - **Storms** spawn over the sea at `stormRate` 40 per year, drift ~300 km per day and decay (`climate.js`).
  - **Lightning** ignites fires at a rate per km² of fuel per year (`fire.js`).
  - **Avalanches.** Snow load above a release quantum slides within ~6 h (`snow.js`).
- **Threshold-and-cascade systems need caps.** It took three attempts to bound the snow cascade:
  - every parcel re-eroding the path grew one event to 10⁸ m³;
  - triggers taking whole quanta of a 10 km cell gave 9,689 triggered slabs;
  - only visible releases draining the load, at 6 per step planet-wide, left load piling up everywhere.

  The final rules: each 20–40 m patch is eroded once, triggers are limited to 40 per event, and at most 6 releases
  happen per step.
- **Watched avalanches released too often.** After a storm, about 36 per day kept coming, "more than a natural
  avalanche cycle". The cause was not isolated (debris loading steep cells, or the 6 h load time). This is a tuning
  lesson for any threshold system.
- **Explicit (watched) and implicit (unwatched) give different outcomes.** `tools/snowobserver.mjs` measured it per
  10 km top cell:
  - relative difference 0 / 5% / 200% (median / p90 / max) after 1 day, 0 / 33% / 200% after 10 days;
  - 1.9% of the snow ended in a different top cell after a day;
  - planet totals within 0.2%.

  Watched runouts cross cell boundaries, and the closure never does.
- **Volcanoes resolve skipped time themselves.** Lava skipped by a fast-forward is laid as lobes along its path by
  `source.onSkip`, "exactly as an unwatched eruption". Before that fix, 4,847 hot parcels piled up at the vent after
  a one-week skip. Moving parcels still lose their dynamics in skipped time.
- **The time governor works.** While parcels move, the clock runs no faster than lets the 5th-fastest parcel cross 2%
  of its distance to the camera per real second, and never slower than real time:
  - an avalanche from 3 km runs at real time;
  - lava from 4 km runs at 10–15×.

  The fast layer gets 8 ms per frame, and the clock slows when it falls behind.
- **One shared random stream is fragile.** Every process draws from one sequential stream (`sim.rng()`). Adding a
  plugin, or one more draw in climate, changes every later lightning strike and storm. D5's keyed RNG fixes this,
  and the director depends on it (§4.5).
- **Scenarios were the design tool.** Storm, dam, wildfire, volcano, drought, diversion and outburst flood were each
  a scripted trigger plus measurement. That is the shape of the event test harness here (§7).

## 4. Design

### 4.1 What an event is

The word has two meanings, kept apart:

1. **A happening.** An eruption, a storm or a fire front. It is plugin state: fields, parcels, entity rows and
   resources, owned and advanced by the plugin's processes. The core does not know it exists.
2. **An event record.** A notification, emitted by the plugin through the core, that a happening started, peaked or
   ended, with its type, place, time, magnitude and visibility. Records are core data: an append-only log per body.

The log is the single feed for:
- other plugins that subscribe (a fish plugin reacting to `hydro:flood`; animals fleeing `volcano:eruption`, 16);
- the director (§4.5);
- the journal and UI (§4.9, 18);
- persistence: history survives (15);
- replays and tests: the log is the fingerprint of a run.

**Record layout** (144 bytes; keys and frames per 05, 02, 04):

| field | type | meaning |
|---|---|---|
| `key` | event key (128-bit, R3) | `H(origin key, (plugin, type, start tick))`, 05's key form; stable, deterministic |
| `type` | u16 | registered from the manifest (`volcano:eruption`) |
| `plugin` | u16 | emitter |
| `phase` | u8 | `precursor`, `active`, `peak`, `settling`, `ended` |
| `notability` | u8 | class 0–5 (§4.2), from the type's rule and the magnitude |
| `flags` | u16 | `explicit` (ran in the fast layer near an observer), `directed` (the director shifted it), `calendar` (§4.9) |
| `t_start`, `t_end` | i64 ticks | `t_end = 0` while running |
| `body` | body key (128-bit) | usually a planet |
| `anchor` | blob key (128-bit) | the coarse blob at the **director band** containing it (§4.5) |
| `pos` | f64 × 3 | in the body-fixed frame (04) |
| `magnitude` | f32 | on the type's own log scale (VEI, log₁₀ m³, log₁₀ km² burnt, kt TNT…) |
| `visibility_m` | f32 | distance from which it can be seen in clear air, before horizon and occlusion (§4.4) |
| `payload` | 40 bytes | typed by the manifest's schema (vent, volume, footprint radius…) |

**Retention.**
- Class ≥ 3 records are kept forever. This is the planet's history: "this crater formed 40 years ago".
- Class 2 records go into a ring buffer of the last ~30 game days, plus every one the player witnessed (§4.9).
- Classes 0–1 are never logged one by one. Per region and day, the core keeps counters by type for statistics and
  for rarity estimates.

**Estimate.** ~20 class ≥ 3 events per game day planet-wide (avalanches, fires, floods, landslides) × 144 B ≈ 1 MB per
game year. To be measured in E1.

### 4.2 The notability ladder (the "Zipf balance", made measurable)

"Zipf balance" needs a definition that can be tested. Proposal: every occurrence gets a **notability class** 0–5. The
target is the rate at which a player exploring normally **witnesses** events of each class or higher. The ladder is
roughly one order of magnitude per class:

| class | name | examples | target interval between witnessed events of this class or higher (real time, proposed) |
|---|---|---|---|
| 0 | ambient | wind in the grass, clouds drifting, birds, fish jumping, leaves falling | continuous |
| 1 | common | a rain shower, an animal encounter, a fox hunting, flowers opening, pebbles falling, a tree falling, fog | ~1 min |
| 2 | notable | a thunderstorm, a rainbow, a herd crossing, an aurora, a beaver at work, a small landslide, a dust devil | ~10 min |
| 3 | rare | an avalanche, a wildfire, a flood, a large rockfall, a meteor shower, a dam breach | ~1–2 h |
| 4 | very rare | a volcanic eruption, a total solar eclipse, a bright fireball or airburst, a great storm | ~10–20 h |
| 5 | legendary | a ground impact nearby, a supernova in the sky, a great comet, a caldera collapse | ~50–100 h (once or twice per playthrough) |

**Why type and magnitude both matter.** Zipf's law over event *types* alone cannot produce the top of the ladder. With
30 types and a rank–frequency exponent of 1.5–2, the rarest type is only ~170–900× rarer than the most common. If the
most common happens every minute, the rarest happens every few hours, not once per playthrough. The bottom of the
ladder comes from the long tails *within* types: a small rockfall is class 1, a mountainside giving way is class 4.
So each type declares:

- a **magnitude distribution**: rate density `Λ_τ(≥m) = Λ_τ0 · 10^(−β_τ (m − m_τ0))` per km² per year (a power law,
  usually truncated);
- a **notability rule**: `n = base_τ + slope_τ · m`, rounded down and clamped to 0–5;
- a **visibility rule**: `V_τ(m)`, the distance from which it reads (§4.4).

**Visibility flattens the curve.** The expected witnessed rate of class ≥ c for a player at x is
`R(≥c) = Σ_τ ∫ λ_τ(m) · P(x sees e | m) dm`, where `P` comes from the visible area `π V_τ(m)²` clipped by the horizon,
line of sight and time of day. Bigger events are visible from farther away, so the *witnessed* size distribution falls
more slowly than the *occurring* one: with `V ∝ 10^(γ m)`, the exponent drops from `β` to about `β − 2γ`. That is
desirable, and it is why plumes, glow and clouds matter as much as the events themselves.

**Planets as personalities.** Event rates are planet parameters from the generator (03, 14): volcanism, storminess,
debris flux, aurora activity. A volcanic planet, a stormy planet and a quiet garden planet are reasons to travel. The
ladder targets apply to the *typical* planet. Each planet's measured ladder (E1) can be shown in the planet browser
(18).

### 4.3 Emergent power laws: the free source of balance

Many natural hazards follow power laws in size. They arise in systems with a slow **drive**, a **threshold** and a fast
**relaxation** that can cascade. This is self-organized criticality (SOC):

| system | statistic | source |
|---|---|---|
| sandpile model | power-law avalanche sizes | Bak, Tang, Wiesenfeld 1987 |
| forest fires (USA, Australia, paleo records) | frequency–area power law, exponent β = 1.3–1.5 | [Malamud, Morein, Turcotte 1998, Science 281:1840](https://www.researchgate.net/publication/13541368_Forest_Fires_An_Example_of_Self-Organized_Critical_Behavior) |
| earthquakes | Gutenberg–Richter, b ≈ 1 (10× fewer per magnitude step) | standard |
| Earth impacts | cumulative N(>E) = 3.7 E^−0.9 per year (E in kt); ~5 kt per year; Tunguska-class ~once per 1,000 years | [Brown et al. 2002, Nature 420:294](https://www.nature.com/articles/nature01238) |
| power laws in geoscience generally | a review of where they hold and where they don't | [Corral & González 2019](https://agupubs.onlinelibrary.wiley.com/doi/abs/10.1029/2018ea000479) |

**This architecture already has the right shape.** The slow layer is the drive (D16): snow load, fuel accumulation,
magma pressure, dam decay. The fast layer is the relaxation: parcels, fronts and cascades. So the guidance to plugin
authors is:

1. Model the **loading** in the slow layer, as a stored field, in physical units.
2. Release it past a **threshold**, locally.
3. Let the release **redistribute** load to neighbours (entrainment, triggering, ignition of neighbours), so cascades
   can grow.
4. **Cap** every cascade explicitly (the snow plugin's 40 triggers, erode-once patches) and count how often the cap
   binds. If more than ~1% of events hit a cap, the cap is shaping the distribution and the plugin needs re-tuning.
5. **Measure** the size distribution in a long headless run (E2), and fit the log–log slope over at least two
   decades.

**Limits of SOC.**
- It needs a separation of time scales: slow drive, fast relaxation. Time warp compresses the drive, which is fine.
  Skipping the relaxation (the prototype's fast-forward) is not; doc 10 owns that problem.
- **Watched cascades (explicit) and unwatched closures (implicit) have different size distributions**, as the snow
  measurement showed. Closures must be calibrated so that event *counts* and *total sizes* per coarse region match
  the explicit runs. It is the same rule as for rivers and snow, applied to statistics.

### 4.4 Taxonomy

Visibility distances are the geometric horizon on the 500 km reference planet, `d = √(2R·h_obs) + √(2R·h_target)`, in
clear air. Haze and aerial perspective (13) shorten them. A player on the ground (2 m) adds 1.4 km; a player on a 3 km
summit adds 55 km.

| type (owner) | class | spatial scale | duration | visible from (ground observer) | needs | SOC? | precursors |
|---|---|---|---|---|---|---|---|
| wind gusts, swaying canopy (climate → render) | 0 | m–km | s | in view | render only | – | – |
| birds, fish jumping, leaves (species, render) | 0 | m | s | ~100 m | display individuals (16), cosmetic particles | – | – |
| rain shower (climate) | 1 | 1–10 km | min–h | ~50 km (cloud base ~2 km) | fields, clouds (13) | – | darkening sky |
| animal encounter, predation (species) | 1–2 | 10–100 m | s–min | ~0.3–2 km | entities (16) | – | tracks, calls |
| tree falling, small rockfall (vegetation, geology) | 1 | 1–10 m | s | ~1 km | fast layer, object (11) | yes (rockfall) | creaking, cracks |
| thunderstorm with lightning (climate) | 2 | 10–50 km | h | ~110 km (anvil 12 km) | fields, clouds, light flashes (13) | – | towering cumulus |
| rainbow (render, from fields) | 2 | – | min | local | render: sun behind, rain ahead | – | sun and showers |
| herd migration (species) | 2 | km | h–days | ~2 km | population flow, display individuals (16) | – | seasonal |
| aurora (sky; star activity, planet field) | 2–3 | planet | h | ~320 km (100 km altitude) | sky rendering (13) | – | star flare |
| landslide, large rockfall (geology) | 2–4 | 10 m–1 km | s–min | 5–50 km (dust) | fast layer, forced refinement | yes | rain loading, cracks |
| avalanche (snow) | 3 | 100 m–10 km runout | min | ~57 km (powder cloud on a 3 km peak) | fast layer, forced refinement | yes | storm load, cornices |
| wildfire (fire) | 2–4 | 1 ha–1,000 km² | h–weeks | ~71 km (smoke at 5 km) | fields, smoke parcels, light | yes (β 1.3–1.5) | drought, lightning |
| flood, dam breach (hydrology, beaver) | 3 | km | h–days | local–km | fields, fast-layer surges | – | upstream storm, dam decay |
| meteor shower (sky, calendar) | 3 | sky | h | hemisphere | sky rendering | – | almanac |
| volcanic eruption (volcano) | 4 (3–5 by VEI) | km–100 km | days | ~120 km (plume 15 km) | fast layer (lava, bombs), forced refinement, plume, light | (magma: threshold) | tremor, steam, swelling (days) |
| total solar eclipse (sky, calendar) | 4 | umbra path | min | the path only | ray queries against the moon body (13) | – | almanac |
| bright fireball or airburst (impact) | 4 | sky + blast | s | ~245 km (60 km altitude) | integrated motion (04), light source, parcels | power law | a moving "star", meteors |
| ground impact (impact) | 5 | crater 0.1–10 km | s + weeks of after-effects | ~245 km entry, ~120 km plume | everything in §4.8 | power law | a wanderer with a tail, weeks ahead |
| supernova (universe, calendar) | 5 | galaxy | weeks | sky-wide | sky rendering, the star's body (03) | – | none, or variability |
| great comet (sky, calendar) | 5 | sky | weeks | sky-wide | small body on rails (04), tail rendering | – | almanac |

**Who owns the sky events.** Some of these are not owned by planet plugins: aurora, meteor showers, eclipses,
supernovae and comets come from the universe and the body tree. Proposal: a first-party `sky` plugin that reads the
body tree (04) and the universe generator (03) and emits their records. Then the core still knows no event by name.

### 4.5 The event director

**The problem.** Natural rates make rare events invisible. Worked example on the 500 km reference planet:
- A ground observer sees an eruption plume within ~122 km. That is a cap of ~47,000 km², or ~1.5% of the planet's
  3.1 million km².
- Say the planet has 30 volcanoes with a mean recharge of 5 years (the prototype's range is 2–8). That is ~6
  eruptions per game year.
- An observer wandering at random witnesses ~0.09 eruptions per game year. With a 365-day year that is one eruption
  per ~4,000 game days.
- With the working default of a ~1 real-hour day (`questions.md` C2, R8), that is one eruption per ~4,000 hours of
  play. The year's length comes from the orbit (04), so it scales this number too.

Raising rates everywhere would make the world implausibly violent and expensive to simulate. The director instead moves
events **toward observers in time and within regions**, under strict rules.

**The rules** (the boundary between "directed" and "faked"):
1. **The director never changes how much.** It does not change mass, energy, or the number of events per region per
   window. The size of an event is whatever the plugin's state holds when it fires. An eruption that fires early is
   smaller, because less magma has accumulated. One that fires late is larger.
2. **It may shift timing**, only within a window that the owning plugin declares.
3. **It may choose the location** among plugin-validated candidates, only within one **director region**: a coarse
   blob at the director band ℓ_D, provisionally ~16–32 km (an estimate; tune in E3).
4. **It never fires an event whose preconditions are not met** (pressure, load, fuel, dryness).
5. **It is deterministic.** Its decisions are functions of sim state, including observers' positions, and of keyed
   random draws (D5). With several players, every observer counts.
6. **It is logged and switchable.** The `directed` flag and the shift go into the record. A world setting turns it off
   ("natural"), and tests run both ways (E3).
7. **It only acts on classes ≥ 2.** Ambient and common events are frequent enough on their own.

**Two kinds of trigger:**

*A. Threshold triggers* (volcanoes, slab release from a quantum, dam breach, caldera collapse):
- The plugin offers a window `[ready, deadline]`. For example, a volcano is ready at pressure 0.9 and must erupt by
  1.1, or by `ready + 10%` of its recharge time.
- **With no observer**, it fires at the nominal threshold (pressure 1.0), exactly as without a director.
- **With observers**, it fires at the first tick in the window where the **expected observation** `O` (below) exceeds
  a type threshold. Failing that, it fires at the deadline.
- The bias is bounded by the window. Because rule 1 conserves what is erupted, long-run mass rates are unchanged.
  Counts move by at most the window's fraction of the recurrence.

*B. Rate triggers* (lightning ignitions, storms, rockfalls, fireballs, impactors):
- The plugin registers a hazard density `λ_τ(x, t)` per region, and a window length `W_τ` (lightning ~1 game day,
  storms ~1 game week, impactors ~1 game month).
- **The core fixes the count.** At the start of each window, region r gets
  `n = floor(μ_r + carry_r + u)`, where `μ_r = ∫∫ λ dA dt` over the window and `u` is a keyed uniform
  `rng(seed, region key, window index, type)`. `carry_r` keeps the fractional remainder, so counts per region track
  `μ` exactly in the long run. (A keyed Poisson draw is the alternative if Poisson variance matters.)
- **The director picks where and when.** For each of the n events it asks the plugin for K candidate places drawn
  from λ, and picks the one with the largest `O`, with ties broken by key. It fires at the tick in the window where
  `O` is high. Failing that, it fires at a keyed uniform time.
- **The local boost is bounded by K.** The chance that the chosen candidate lies in an observed patch of probability
  `p` is `1 − (1 − p)^K ≤ K p`. **Proposed K = 4**, so no place sees more than 4× its natural rate.

**Expected observation `O(event candidate)`:**
- Observers are the avatars, or a pending observer during embark, which are sim state (R9). Cameras (orbit,
  free-fly, photo mode) don't count: a player sweeping the orbit view over a volcano does not hold its eruption.
- Sum over observers of `visible(observer, pos, V_τ(m))`:
  - horizon distance (§4.4);
  - a coarse line-of-sight test: a batched visibility query on the coarse terrain (07);
  - a time-of-day factor (an aurora needs night, a plume reads best by day).
- It is *not* weighted by where the camera is looking. That would make the sim depend on the view, which differs per
  frame and per player, and the two-tier cut (06, R1) keeps view direction out of the sim.

**Precursors make waiting purposeful.** Rare events emit *precursor* records first: tremors and steam for days, a
growing cornice, a dam's integrity dropping, a wanderer with a tail. Precursors are ordinary class 1–3 events. A player
who sees steam on a distant mountain and walks there arrives while the eruption is still in its window. The director
then fires it. The event becomes a destination, not luck.

**The observer effect this creates is deliberate, and measured:**
- Per region and window, counts and totals are exact.
- Within a region, outcomes shift toward observed places, bounded by K. Over years, an area a player frequents may
  burn more often than its natural rate (up to K×). That is the honest cost.
- E3 measures it against the natural run, on the same coarse cells `planet/`'s observer experiments used.

**Disagreement with README: none.** Pillar 5 says looking at a place does not change what happens there, or changes it
by a small, measured amount. The director is the one place where this doc chooses a larger, *declared* effect for
pillar 1. It is bounded by rules 1–3 and switchable by rule 6. Joel should confirm the trade (Q17.2).

### 4.6 The event lifecycle through the core

```
precursor ──▶ (director window) ──▶ active ──▶ peak ──▶ settling ──▶ ended
    │                                  │                      │            │
 record (class 1–3)          record; refine request;  governor (10)    outcome record;
                             fast-layer parcels                        touched subtrees (15);
                             if explicit                               refine requests released
```

1. **Trigger.** A plugin process (10) decides to fire, either by itself (no director) or through the director's fire
   export. It calls `ss_event_begin`, which returns the event key.
2. **Explicit or implicit.** The plugin asks the core whether any observer's sim tier covers the footprint, or will
   once refinement arrives.
   - **Explicit:** the plugin requests refinement (`ss_refine_request`, 06 and 08) for the footprint, at the
     **interaction level** the event needs (D-levels: lava parcels of ~3 m force the ground near them down to that
     size). Refinement takes effect after `L_split` (R1). It also spawns fast-layer parcels (10, 11).
   - **Implicit:** it runs its closure. Lava becomes lobes along the path (as `onSkip` in `volcano.js`); load moves
     to the flat ground of its cell; a crater is stamped as a regenerable record (§4.8).

   Observers are sim state, so this choice is deterministic.
3. **Priority in the cut.** Event refinement requests compete with the renderer and sim bands for the cut budget (06).
   Proposed: an event request outranks render requests within its footprint, but is capped at a fraction of the
   budget (e.g. 25%) so that an eruption cannot starve the view.
4. **Time governor.** While the event's parcels move, 10's governor slows the clock by itself (the 5th-fastest
   parcel crosses 2% of its distance to the nearest observer per real second; never slower than real time). This doc
   asks 10 for one more term: a new class ≥ 3 event within an observer's visibility caps the rate at ~10× until the
   player has had a chance to look. Whether that is shown in the UI is Q17.5.
5. **Settling.** Parcels deposit into the slow layer and the terrain (11). Salient changes mark subtrees touched (15),
   e.g. new rock, a crater, a breached dam.
6. **End.** `ss_event_end` writes the outcome: volume, area, trees killed, footprint. The refine requests are
   released, and the region coarsens later by eviction.

**Subscriptions.**
- Plugins list event types in their manifest (08's `"handles"` entries, e.g. `volcano:eruption`, `*:died`) and receive
  records in their handler export at the **next tick**, in canonical order (tick, type, key). Delivery is
  deterministic, and no plugin sees another's event within the same tick, which avoids order dependence (D5).
- Records sent to plugins running in workers follow 10's per-process async latency rule (R1).

### 4.7 Calendar events and the sky

Some events depend only on the body tree's rails (04) and the universe generator (03). They are the same for every
player in the same universe, whatever their history:

- **Eclipses** (solar and lunar), transits and conjunctions. Rails are exact and periodic (04), so the `sky` plugin
  can search forward in time for shadow cones that intersect the planet. It emits records with the umbra path. 13
  draws them by casting the sun's visibility ray against the moon body, so a blobby moon casts a blobby umbra.
- **Meteor showers.** They come from debris streams along comet orbits: at fixed dates each year, as Earth's
  Perseids do.
- **Periodic comets.** Small bodies on rails with tails (12, 13).
- **Supernovae.** A star's lifetime from the generator (03) ends at a seed-determined epoch, and the star becomes a
  new bright point for weeks. This needs 03 to give stars a lifetime and an end state.

Calendar records carry the `calendar` flag. They are predictable, so an **almanac** (later) can list upcoming ones:
"total eclipse on planet X, day 312, visible along this path". It turns them into planned trips. They need no
director.

### 4.8 Scenario 4, end to end: the asteroid

**Rates.**
- **Physically:** Earth sees ~5 kt of impact energy per year, and a Tunguska-class impact every ~1,000 years (Brown
  et al. 2002). The 500 km reference planet has (500/6371)² ≈ 0.6% of Earth's cross-section (ignoring gravitational
  focusing), so physical rates would make impacts about 160× rarer still.
- **So the game's impact flux is a design parameter per system** (the "debris" planet parameter). It is tuned to the
  ladder:
  - a witnessed fireball or airburst (class 4) every ~10–20 h of play;
  - a ground impact witnessed nearby (class 5) once or twice per playthrough.

  The power-law *shape* (N(>E) ∝ E^−0.9) is kept, so impacts still span sizes.

**1. Planning (weeks of game time before).**
- The impact plugin is a rate trigger with a one-month window and a whole-planet region. At the window start, the
  core fixes the count (§4.5).
- For each impact the director picks a site among K = 4 candidates: uniform over the sphere, excluding Q17.4's
  protected places. It picks an impact tick and an energy from the power law (keyed).
- **Impactors are spawned bodies.** D14 makes the generated system non-colliding, so impacts cannot come from the
  generated rails. The impactor is an explicit exception, created by the plugin.
- The plugin calls 04's `plan_impact` with an `ImpactRequest` (04 §4.13): site, impact tick, size, v∞, entry angle
  ~45°, and lead time.
  - 04 shoots the atmospheric segment, allowing for the planet's spin, then propagates backwards on rails. It
    continues in the star's frame if the conic leaves the planet's SOI first.
  - The impactor is created as a dynamic body at `t_impact − lead` (lead ≈ 2–4 game weeks), with a key derived from
    the event key.
  - Planning an impact activates the target planet (10, R9).

**2. Forewarning.**
- **Realistically there is none.** A 30 m asteroid is about magnitude 15 at 0.01 AU, invisible to the eye
  (estimate).
- **Proposed: a stylised wanderer.** Legendary impactors come as a slowly brightening point with a faint blobby coma
  and tail. It is visible at night for the last 1–2 weeks and is listed by its body id, so the player can look it up
  (18). Fireball-class impactors get a sporadic meteor shower in the nights before. Q17.6 asks whether this departure
  from realism is wanted.
- Precursor records (`impact:wanderer_sighted`, class 2–3) let the journal and the player's curiosity work.

**3. Entry.**
- **Rails to integrated.** Crossing the top of the atmosphere switches the body from rails to integrated motion
  (04: drag `½ ρ v² C_d A`, with ρ(h) from the planet's atmosphere model).
- **Ablation and light.** The energy lost to drag heats and ablates the body (mass loss ∝ energy deposited / heat of
  ablation). It glows: an **emissive light source** (13) bright enough to cast moving shadows for a few seconds.
  Smoke and plasma parcels trail it (fast layer, `smoke` material, 11).
- **Breakup.** When the ram pressure `ρ v²` exceeds the body's strength (~1–10 MPa for stony bodies; estimate), it
  fragments.
  - Small stony bodies **airburst**: a flash, a blast wave, no crater. Tunguska-style tree flattening comes from
    `vegetation.fell(radial)` within the blast radius and `kill_trees` beyond it (16).
  - Iron or large bodies reach the ground, with fragments as ballistic parcels.
- **The time governor** holds real time from ~1 minute before impact (10).

**4. Impact (explicit, because an observer is within visibility).**
- **Refinement.** The event requests refinement of the crater footprint and the ejecta blanket (~3 crater radii) down
  to the interaction level of the ejecta parcels and rim detail (06).
- **Crater size** from the scaling of Collins, Melosh and Marcus (2005, *Earth Impact Effects Program*):
  `D_tc = 1.161 (ρ_i/ρ_t)^(1/3) L^0.78 v^0.44 g^−0.22 sin^(1/3) θ`, final simple crater `≈ 1.25 D_tc`, depth
  ≈ D/5.
  - *Estimate:* a 30 m iron body at 15 km/s and 45° on a g = 9.8 m/s² planet gives D_tc ≈ 0.9 km and a final crater
    of ~1.1 km. (Barringer Crater is 1.2 km, from a ~50 m iron body.)
  - A smaller planet's lower g makes it larger, by g^−0.22.
- **Geometry the store shows is removals and additions** (D15; the dig mechanism of `M7.md` §7 at scale):
  - **Removals:** the bowl.
  - **Additions:** a raised rim, a breccia lining, and the ejecta blanket where parcels land.
  - **Ejecta:** ballistic parcels (the volcano plugin's `bomb` material, 11), with a size distribution, landing
    within ~2–3 crater radii.

  What persists follows the content kinds (R7): see "Persistence" below.
- **Fields:**
  - thermal radiation ignites fires within `r_th` (the fire plugin's `ignite` service);
  - the blast fells and kills trees (`vegetation`);
  - species take kill fractions and flee (16);
  - dust goes into `std:aerosol` (09's name, R12; stored, sum). Climate reads it as reduced sunlight, and 13 reads it
    for weeks of red sunsets.
- **The body tree.** The impactor body is destroyed: its blobs become ejecta and crater lining. Ejecta faster than
  escape velocity would become new bodies (04), which is unlikely at these sizes.
- **Persistence.**
  - The crater is a **regenerable crater record** (below). This is the canonical form (R7).
  - On top of it, the explicit run's details persist as touched subtrees (15). The ground's extra shape is a `z`
    residual plus edit terms (kind A), with fine ejecta as `z` deposits. Settled ejecta rocks are kind D placed
    records.
  - The record (class 5 if witnessed close, class 4 otherwise) stays in the history forever.
- *Later:* ocean impacts and tsunamis need fast-layer water at a scale not yet planned (11).

**5. The same impact, unobserved.**
- The trigger is the same and the director is not involved, because no observer is near.
- The plugin writes an `impact:crater` **record**: an entity row (16's thin tables) holding centre, diameter, depth,
  rim height, age and seed.
  - The record contributes a shape term to `std:ground_height` in 09's `shape` phase (R17), so every level
    re-derives the crater, like volcano cones in `planet/` (14).
  - Fires, blast and aerosol go through the same services as in the explicit path, as coarse deltas.
- The crater is regenerable, so it is not touched unless someone changes it later.
- **Watched and unwatched craters share the row.** The explicit run *also* writes the row first and adds its parcel
  details on top. So a crater looks the same from orbit whether anyone saw it form. This is pillar 5's "same lasting
  outcome", in the form `planet/` learned for lava (`onSkip`).

### 4.9 Discovery

**What counts as a discovery** (per player, saved with the player, 15):

| kind | how it is detected | key |
|---|---|---|
| species first seen | an individual (or display individual) drawn ≥ ~20 px tall for ≥ 2 s, unoccluded | species id |
| event witnessed | the player's camera (not a sim observer; this is render-side) within `visibility_m` and the horizon, with line of sight to the event's visible part, during `active` or `peak`, on screen for ≥ 2 s | event key |
| landmark found | within view distance and line of sight of a landmark record | landmark key |
| body visited | embarked on, or orbited closer than a few radii | body key |
| calendar event seen | as "event witnessed" | event key |
| firsts | first snow, first rainbow, first aurora on a planet | (type, body) |

**Witness detection is not sim state.** The journal does not change the world, so it may use the renderer's id buffer
and screen-space tests (planning idea 3 allows GPU results for anything that never feeds the sim). It runs per client
and needs no determinism. If discoveries ever unlock gameplay, they need a deterministic test. Q17.7 asks.

**Landmarks** are records that generator plugins emit at generation (14): the highest peak of each continent, the
biggest waterfalls, volcanoes, craters, caves, natural arches. Each has a name (Q14 owns naming) and a key. They are
deterministic, so they are the same for every player of the universe.

**Rarity is estimated by the world itself.**
- The log's counters (§4.1) give each type's planet-wide rate per magnitude bin.
- Combined with the type's visibility rule, that gives the expected witness rate per player-hour.
- **Rarity** = `log₁₀` of the expected interval between witnesses, in hours, shown as tiers:

| tier | expected interval |
|---|---|
| common | < 0.1 h |
| notable | 0.1–1 h |
| rare | 1–10 h |
| very rare | 10–100 h |
| legendary | > 100 h |

- No hand-tuned rarity table can drift from the simulation, because rarity is measured from it. Before enough history
  exists, the plugin's declared rates (manifest) are the prior.

**Sharing.**
- **Calendar events and landmarks** are fully shareable as text (`planet id @ lat, lon @ day`): anyone can go and
  see them.
- **Emergent events** depend on history: the player's edits and the director. They are shareable as records,
  screenshots and (later) replays, but are not reproducible on someone else's world. One refinement:
  - In a fresh world with the director off, the *natural* schedule is seed-determined: "volcano V on planet X erupts
    between day 30 and day 50".
  - With the director on, it fires within that same window.
  - So a natural window can be shared, and other players can plan for it.
- **Multiplayer (later):** players in one world share its log. Each player keeps their own journal.

## 5. Interfaces

C-ABI style per 08. Handles are `u32`. Keys are 128-bit (`Key128`, 05, R3). The plugin-facing calls use the
`ss_event_*` names (R2); 08 §5.2 holds the canonical list.

**Manifest** (08 owns the format: its `events` block has `emits` and `handles`; each `emits` entry carries the
per-type schema below):
```json
"events": {
  "emits": [
    { "type": "volcano:eruption",
      "magnitude": { "unit": "VEI", "min": 0, "max": 8 },
      "rate_prior": { "per_km2_per_year": 2e-6, "beta": 1.0 },
      "notability": { "base": 2.5, "per_magnitude": 0.5 },
      "visibility": { "kind": "plume", "top_m": [2000, 5000, 10000, 15000, 25000, 30000] },
      "trigger": "threshold", "directed": true,
      "precursors": ["volcano:tremor", "volcano:steam"],
      "payload": { "vent": "f64x3", "volume_m3": "f64", "explosivity": "f32" } },
    { "type": "fire:ignition", "trigger": "rate", "window_days": 1, "directed": true,
      "magnitude": { "unit": "log10_km2" } }
  ],
  "handles": [ { "event": "volcano:eruption", "export": "on_events" },
               { "event": "*:died", "export": "on_events" } ]
}
```

**Records:**
```
ss_event_begin(type: u16, body: u32, pos: *const F64x3, magnitude: f32, visibility_m: f32,
               payload: *const u8, len: u32, out_key: *mut Key128)   // anchor and notability computed by the core
ss_event_update(key: *const Key128, magnitude: f32, visibility_m: f32, phase: u8)
ss_event_end(key: *const Key128, outcome: *const u8, len: u32)
ss_event_emit(...)   // a one-shot record (begin and end in one call) for instantaneous events; 10 sketches this name
// handler export (named in the manifest's "handles"), called at the next tick in canonical order:
on_events(records: *const EventRecord, n: u32)
// read API for UI and tools (main thread, through the engine worker; 18, 19):
ss_event_log(body: *const Key128, since_tick: i64, min_class: u8, out: *mut EventRecord, cap: u32) -> u32
```
19's `ss_events(dst, cap, since)` is this read API.

**Director** (core side, `ss_event_*`):
```
// threshold triggers: one offer per pending event
ss_director_offer(type: u16, ready_tick: i64, deadline_tick: i64, nominal_tick: i64,
               cand_pos: *const F64x3, n_cand: u32, magnitude_est: f32) -> OfferId
ss_director_withdraw(offer: OfferId)
// rate triggers: registered once; the core fixes counts per region and window
ss_director_register_rate(type: u16, band: i8, window_ticks: u32)
```
The plugin exports the director calls are export kinds whose names the manifest binds (08 §5.3):
```
hazard(type: u16, regions: *const Key128, n: u32, window: i64, out_mu: *mut f64)    // expected counts
sample(type: u16, regions: *const Key128, n: u32, k: u32, out_pos: *mut F64x3)      // K candidates per region from λ
fire(type: u16, offers_or_slots: *const u64, chosen: *const u16, n: u32)            // fire now, here
```

**Cross-doc calls** (owned elsewhere):
- `ss_refine_request(body, reqs, n)`: a sim-tier requester with a priority, effective after `L_split`. 06 owns the
  semantics, 08 the signature.
- The time governor (10 §4.11) follows the parcels itself. This doc asks 10 for one extra term (§4.6 step 4).
- A batched visibility query `(observers, points, heights) → score`, horizon plus coarse line of sight: requested
  from 07.
- `plan_impact(ImpactRequest)`: 04 §4.13.
- The `sky` plugin needs from 03 and 04: moons' shadow geometry, comet orbits, star lifetimes.

**Requirements on other docs:**
- **03:** star lifetimes and end states (supernovae); per-planet event-rate parameters (volcanism, storminess, debris,
  aurora); comet populations.
- **04:** `plan_impact` (§4.13 there), with lead times of weeks (it continues into the star's frame); rails →
  integrated at the atmosphere top with drag and ablation hooks; spawned bodies as an explicit exception to D14.
- **06:** event requesters with a priority and a budget cap.
- **07:** a batched visibility query (horizon plus coarse line of sight).
- **10:** the event term in the governor; next-tick event delivery; the fixed-latency rule for workers.
- **11:** fast-layer materials `smoke` and `ejecta` (`bomb` exists); settle policies for ejecta rocks (kind D).
- **12, 13:** plumes, smoke columns, glow, aurorae, meteor streaks, comet tails, eclipse shadows, red-sunset aerosol,
  emissive light sources. These are the visibility rules' other half.
- **15:** persisting the log (classes ≥ 3), crater records and touched detail (R7), journals per player.
- **16:** threat perception from events; `kill` and `fell` services.
- **18:** the journal UI, notifications (Q17.5), looking up a wanderer by id.

## 6. Risks and unknowns

- **It may feel staged.** A player who always arrives just in time may notice. Mitigations:
  - windows are short relative to recurrence;
  - K is small;
  - precursors make the arrival feel earned;
  - many events stay undirected (classes 0–1, calendar events).
- **The ladder may be impossible to tune by hand.** Rates come out of coupled simulations. The answer is measurement,
  not intuition: E1's audit is a standing test, run whenever a plugin changes.
- **Cascades can run away.** The snow prototype's 10⁸ m³ event and 9,689 triggered slabs show it. Cap every cascade,
  and count how often the caps bind (§4.3).
- **Watched and unwatched statistics differ** (snow: 1.9% of the snow in a different top cell after a day, 33% p90 per
  cell after 10 days). Closures need calibration per plugin, and that is work.
- **Forced refinement spikes the frame.** An eruption near the player asks for a lot of refinement at once. The
  budget cap (§4.6), refinement ahead of time during precursors, and the governor are the mitigations.
- **Legendary events may never be seen** by a player with a few hours of play. That is acceptable for "legendary".
  A guaranteed early class-4 event per new player is a design option.
- **The director depends on observers being sim state.** This is fine for one player. With many players, everyone's
  events shift toward everyone's locations, which is still deterministic, but the boost per region is then shared.
- **No audio plan.** Thunder delayed by distance, a sonic boom after a fireball, the rumble of an avalanche: half of
  what makes these events land is sound (Q17.8).

## 7. First experiments

| # | experiment | success criterion |
|---|---|---|
| E1 | **Rate audit.** A headless planet with all M5–M6 plugins for 10 game years, plus an explorer bot walking a plausible route. Count events by type and magnitude; fit power laws; compute witness rates per class. | after tuning, witnessed rates per class within 2× of the §4.2 targets on the reference planet |
| E2 | **SOC check.** Avalanche and fire size distributions from E1. | a power law over ≥ 2 decades; caps bind in < 1% of events |
| E3 | **Director A/B.** The same seed and bot, director on and off. | witnessed class ≥ 3 events per hour ≥ 3× with the director; per-region event counts identical between runs; coarse-cell outcome differences (burnt area, snow, lava volume) bounded by K and reported |
| E4 | **Eclipse almanac.** A planet with a moon; predict the next 10 total solar eclipses from rails; render each at its predicted time and place. | umbra within 1 km and one tick of prediction; identical predictions in Node and Chrome |
| E5 | **Impactor planning.** Back-solve a trajectory to a chosen site and tick; propagate forward on rails, then integrate through the atmosphere. | impact within 100 m and 1 s of the target; bit-identical across runs and runtimes |
| E6 | **Visibility rule vs rendering.** From a 3 km summit, 50 random volcano positions within 200 km: the CPU horizon + line-of-sight test against a rendered plume pixel count. | agreement ≥ 95% |

## 8. Build order

| milestone | events and discovery work |
|---|---|
| M1–M4 | none, except that 04's rails must support future eclipse search; keep the keyed RNG (D5) from day one |
| M5 | the event log core API (`ss_event_begin`, `update`, `end`, handlers, retention, counters); flood and fire records from hydrology and vegetation; E1's harness skeleton (19) |
| M6 | the director (threshold offers, rate quotas, K choice, logging, the "natural" switch); lifecycle integration (refine requests with budget cap, the governor's event term); avalanche, eruption and impact (including the `impact:crater` record and E5); the journal v1 (witness detection, rarity tiers from the log, landmarks); E2, E3 |
| later | the `sky` plugin (eclipses, meteor showers, comets, supernovae, E4) and the almanac; sharing links; multiplayer witnesses; sound cues once audio exists |

## 9. Open questions

**Q17.1: The notability ladder.** Are the target intervals right: ambient continuous, common ~1 min, notable ~10 min,
rare ~1–2 h, very rare ~10–20 h, legendary ~50–100 h? This sets every plugin's tuning target.
*Default:* as proposed, revisited after E1.

**Q17.2 (merged into `questions.md` C12): The director.** Is a bounded, deliberate observer effect acceptable
(timing within plugin windows, placement among K = 4 candidates per region, classes ≥ 2 only, avatar observers only,
a "natural" switch)? It trades pillar 5 against pillar 1, and it is the one place where this plan chooses a larger
observer effect on purpose.
*Default:* yes, on by default, with K = 4.

**Q17.3 (merged into `questions.md` C2): Game time scale.** How many game days pass per real hour, and how long is a
year? It multiplies every rate in this doc. The volcano example (one witnessed eruption per ~4,000 game days without
the director) becomes playable only with the director *and* a fast clock or an active planet. Docs 02, 04 and 10
own the clock; this doc needs the number.
*Default:* the working default of a ~1 real-hour day (R8), with the year length from the orbit (04).

**Q17.4 (merged into `questions.md` C3): Can big events hurt what the player made?** Can an eruption, impact or flood
destroy the player's structures, or land near them?
*Default:* yes nearby, never directly on touched player structures, and always with precursors.

**Q17.5: Notifications.** Diegetic only (you notice the plume, the light, the sound), or UI hints ("something is
happening to the north-east")? Diegetic is purer; hints catch the rare events.
*Default:* diegetic, plus the journal, plus an optional subtle direction hint for class ≥ 4 within visibility.

**Q17.6: Forewarning realism.** Should legendary impactors appear weeks ahead as a stylised wanderer with a tail
(legible, not realistic), or be realistic (invisible until the fireball)?
*Default:* stylised.

**Q17.7: Does discovery unlock anything?** Achievements, progression or abilities, or is the journal its own reward?
If discoveries unlock gameplay, witness detection must become deterministic sim state.
*Default:* the journal with rarity tiers, and no unlocks at first.

**Q17.8: Audio.** No doc owns audio, and events lose much of their impact without it: thunder, booms, rumbles. Should
an audio doc be planned now?
*Default:* yes, a short `20-audio.md` before M6. Event records carry sound hints (type, magnitude, position) from M5.

**Q17.9: An almanac and sharing links.** Should there be an in-game almanac of calendar events, and should landmarks
and events be shareable as links?
*Default:* yes, after M6.
