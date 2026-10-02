# 10. Simulation and scheduling

Status: draft · Depends on: 02 (ticks, RNG, determinism rules), 06 (cut, lift / restrict / push-down, requesters),
08 (plugin ABI, workers), 09 (field schemas) · Used by: 11, 14, 15, 16, 17, 18

## 1. Purpose and scope

This doc decides how the world changes over time. It covers:

- **The tick:** what happens in one fixed step, in which order, and why.
- **Processes:** how plugins declare periodic work, how multi-rate stepping works, how long work moves to workers.
- **Field writes:** how stored fields are written (`delta`, `transfer`, `owned`; the kinds are 09's) and merged deterministically.
- **The two layers:** the slow layer, the fast layer, and the exchange between them. The fast layer's mechanics are in
  [11-physics](11-physics.md).
- **Time:** the rate, the governor, warp and skip.
- **Coverage:** which bodies are simulated, and how much.
- **The observer effect** as engine rules, and the split between the sim tier and the render tier.
- **Triggers** for threshold-and-release processes (self-organised criticality), the slot events take in the tick, and
  the budgets.

Out of scope, owned elsewhere:
- Field schemas, aggregation rules and composition of computed fields: [09](09-fields-and-composition.md).
- The cut's mechanics: balance, eviction, split jobs, the lift projection: [06](06-blob-hierarchy-and-cut.md).
- Integrators, contacts, parcels: [11](11-physics.md).
- What events mean, the director, the journal: [17](17-events-and-discovery.md).
- The save format: [15](15-persistence.md).
- Orbits: bodies on rails cost nothing here ([04](04-bodies-and-orbits.md)).

## 2. Requirements

**Must**

| id | requirement | from |
|---|---|---|
| R1 | Every activated planet (§4.10) is simulated everywhere at `ℓ_sim_floor`, all the time, and finer only where the sim tier is finer. There is one state at every level. | pillar 5, constraint "consistent large-scale simulation" |
| R2 | The state at tick T+1 is a pure function of the state at T and the commands stamped T. Wall-clock time, frame time, thread timing and view direction never reach sim state. | D5, multiplayer readiness |
| R3 | Every slow scheme is stable for any step, so catch-up, warp and skip can't blow up. | `planet/` rule |
| R4 | The observer effect is bounded. *Being* somewhere may change outcomes only through declared mechanisms (bands, the explicit fast layer), measured by regression tests. *Looking* somewhere (view direction, render detail) changes nothing. | pillar 5 |
| R5 | Conserved quantities (water, snow, lava, ground volume) are exact to float rounding through transfers, parcels and level changes. | `planet/`: 1e-15 to 4.5e-15 measured |
| R6 | Sim work per frame is bounded. Long work runs asynchronously. When work overruns, the clock slows; the frame rate does not drop. | constraint "beautiful", 60 fps |
| R7 | Plugins can be combined in any subset, and their processes compose regardless of order. | pillar 4 |

**Should**

| id | requirement |
|---|---|
| R8 | Time control: real time, faster (×N), and skip ("rest until morning", "come back in ten years"). |
| R9 | A rare fast event is readable when watched: an avalanche runs in real time, lava sped up (the governor). |
| R10 | Planets the player has left keep living cheaply. |

**Later**

| id | requirement |
|---|---|
| R11 | Multiplayer: the same scheduler, inputs from several peers, interest sets per peer. |
| R12 | Closure calibration: coarse closures learn from observed fine runs. |

## 3. What the prototypes taught

**What carries over unchanged** (`planet/DESIGN.md`, `planet/PLUGINS.md`, `planet/js/plugin.js`):

- Processes are written once, in physical units, at their own scale. Each declares a band, and its deltas reach the
  leaves by push-down (`DESIGN.md` §2.4).
- "Write coarse processes as rates applied to the current restricted state, never as absolute targets." That way a
  player's edit is part of the state the coarse process sees, and it is never corrected away.
- Writes are changes, so composition is free. The port of five processes into plugins was bit-identical
  (`tools/fingerprint.mjs`). Several plugins write `z`, `snow` and `flow` with no ordering. Extension points sum their
  contributions (`hydroSource`).
- Every scheme was written to be stable for any step. That is what let the prototype skip Berger–Oliger subcycling
  (`DESIGN.md` §8).
- Three process shapes are scale-free: mass balance on graph edges, fronts with a physical speed (arrival time =
  distance / speed), and metric budgets. Explicit neighbour-to-neighbour transfer is not: it ran lava across 10 km cells
  in minutes.
- Non-negative intensive fields must be pushed down multiplicatively. Additive push-down drove precipitation negative,
  then discharge, then NaN.
- The fast layer:
  - parcels sized by distance to the observer: 3 m near, 40 m at 10 km;
  - a 3,000-parcel budget that coarsens new parcels;
  - a resting store;
  - coalescing;
  - CFL substeps;
  - semi-implicit resistance (`planet/js/mobile.js`).
- The time governor (`planet/js/app.js`):
  - for each moving parcel, the world may run no faster than lets it cross 2% of its distance to the camera per real
    second;
  - the governor takes the 5th smallest such limit, never below real time;
  - it slows further when the layer lags.
  - An avalanche seen from 3 km ran in real time; a lava flow from 4 km ran at 10–15×.
- Skipped time goes implicit: "nobody observes a fast-forward". Sources resolve skipped volume themselves (`onSkip`).
- Snow cascades needed caps: at most 6 releases per step and 40 triggered slabs per event. Without them one event
  grew to 10⁸ m³, or 9,689 slabs triggered.

**What has to change, because the prototype was not deterministic:**

| prototype | problem | engine |
|---|---|---|
| `advance(dt)`: steps stretched to `acc / maxSteps` when the rate outran `maxSteps` (`plugin.js`) | step size depends on frame time | canonical periods in ticks; skip commands are the only source of stretched steps (§4.5) |
| fast layer `msBudget`: breaks out of substeps on wall-clock time; lags the world clock; drops lag beyond `maxLag` (`mobile.js` `update`) | results depend on machine speed | every tick is completed; overload slows the clock (§4.11) |
| `w.restrictAll()` after every process | cost grows with the cut | restriction of dirty paths, once per stage (§4.2) |
| the observer is the camera position, read every frame | sim depends on rendering state | observers are sim state, set by commands (§4.9) |
| processes run immediately, in plugin order, each seeing the previous one's writes | results depend on load order | stage-start snapshot plus deltas merged in canonical order (§4.4) |
| plugins kept arrays from `create` time (the "stale arrays" hazard: NaN snow down the rivers) | dangling data | handles and accessor calls only (08) |

**Costs measured on the 250 km planet** (9,126 top cells of 10.1 km, M1 Max, JS):

| | cost |
|---|---|
| climate (every 12 h) | 5–8 ms per step |
| hydrology (every 3 h) | 4–11 ms per step; 24 ms with snow |
| vegetation (every 10 days) | 3 ms per step |
| fire, lava, idle | < 1 ms |
| refinement on a teleport | 15–49 ms |
| fast layer, 700–850 parcels | 1.2–1.9 ms per frame |
| fast layer, 745 lava parcels | 2.3 ms per frame |
| fast layer before its budget existed | ~1,200 parcels at 13× took 210 ms per frame |
| live page at 1 day/s | ~2 ms of sim per frame, ~60 fps |

## 4. Design

### 4.1 Time base

- **The tick** is the fixed quantum: Δ = 1/30 s, provisionally ([02](02-numerics-time-and-determinism.md)). Game
  time is `tick × Δ`, held as an `i64` tick count. Every period in this doc is a whole number of ticks.
- **The rate r** is game seconds per real second. It is chosen each frame (§4.11) and only decides *how many whole
  ticks* run that frame. It never changes what a tick computes.
- **Rendering interpolates** between the last two published ticks ([12](12-rendering.md)). The local avatar is drawn
  from a prediction so input feels immediate ([18](18-player-camera-and-navigation.md)).
- **One clock.** Everything runs on the same game clock: bodies on rails, physics, slow processes. Gameplay
  compression (rivers that answer a storm in hours, not days) is done with each process's rate parameters, not with a
  second clock. Working assumption (R8, pending `questions.md` C2): a ~1 real-hour day, with fictitious forces dropped
  in surface physics ([04](04-bodies-and-orbits.md), [11](11-physics.md) §4.2). See Q10.1.

### 4.2 The tick pipeline

A tick takes `(S_T, commands_T)` to `S_{T+1}` in nine stages. A **barrier** separates stages: between barriers the
blob set is fixed, so handles are stable, and the fields are a fixed snapshot.

| # | stage | reads | writes | why here |
|---|---|---|---|---|
| 1 | **commands** | commands stamped T | avatar intents, edit queue, time control, observer poses | everything in the tick sees the player's intent, so the tick is a function of `(S_T, commands_T)` |
| 2 | **structure** | edit queue; cut jobs due at T | blob removals and additions (D15; the persisted form follows the content kind, R7 in 14 and 15); connectivity → detached islands become bodies ([11](11-physics.md)); splits (plugin prior + core projection) and merges (restrict + residual), from jobs requested at T − `L_split` (3 ticks, owned by [06](06-blob-hierarchy-and-cut.md)) | discrete changes happen at one barrier, before anything integrates. Physics then collides with this tick's geometry, and every later stage sees one fixed cut |
| 3 | **dynamics** | geometry, fields (snapshot) | positions and velocities of integrated bodies, objects and parcels (owned by physics); field deltas (entrainment, melt, deposit) as transfers; rails ↔ integrated handoffs due at T | fast processes first, so slow processes see where things are now (lava over trees) |
| 4 | **processes** | fields (snapshot taken at the start of stage 4), positions | deltas from the sync processes due at T; deltas from async jobs whose results are due at T; snapshots for async jobs starting at T | slow processes run on the freshest geometry |
| 5 | **merge** | the deltas of stages 3–4 | fields at the leaves: transfers scaled, then summed in canonical order; push-down of coarse deltas; non-negativity projection; buffered blob creations and destructions committed | the only place fields change, so the order of the work in 3–4 can't matter |
| 6 | **restrict** | dirty leaves | ancestors along dirty paths, bottom-up | coarse values are consistent before anything reads aggregates |
| 7 | **triggers** | fields that changed this tick | event records; threshold releases queued | thresholds can be on aggregates (top-level snow load) |
| 8 | **requests** | observers, bands, contacts and predicted paths, events, (later) peers | the sim tier's wanted set → split and merge jobs due at T + `L_split` | needs final positions and this tick's events |
| 9 | **publish** | final state | a render snapshot (double-buffered); a persistence checkpoint if one is due (a frozen copy handed to a worker, [15](15-persistence.md)) | the renderer and the saver never see a half-done tick |

**Three invariants make this deterministic.** This doc owns the stage snapshots and the canonical merge order (2,
below); other docs refer to them here.

1. **Jacobi semantics.** Within a stage, every process reads the same snapshot, and all field writes are deltas
   applied at the barrier. One process never sees another's writes from the same stage. The order in which processes
   run (or which thread finishes first) can't change the result.
2. **Canonical order.** Wherever order matters (float summation of deltas, handle allocation, event emission), it is
   sorted by `(plugin id, process name, call index)`. Stable string ids are used, never load order or arrival order.
3. **Handles stable within a stage.** Plugins create and destroy blobs through buffered calls committed at barriers in
   canonical order. The handle a creation returns is allocated immediately and deterministically, because sync
   processes run in canonical order on the engine thread. Async jobs can't create blobs directly: they return creation
   commands (§4.6). The fast layer creates and removes its own parcels inside stage 3, since it owns them exclusively.

**Why not Gauss–Seidel** (each process sees the previous one's writes, as in the prototype)? It converges faster
for tightly coupled pairs. But it makes results depend on plugin order, which a community plugin set can't
guarantee. Where two processes really are coupled within a step (snowmelt feeding runoff), the coupling goes through
an extension point *inside* one process step. That is how the prototype's `hydroSource` hook already worked. The cost
of Jacobi is a one-step lag between separate processes, which their rate formulation tolerates (E10.2 measures it).

### 4.3 Processes

A process is declared in the plugin's **manifest** ([08](08-plugin-system.md) §4; there is no runtime registration,
R2). The resolver checks every declaration before any plugin code runs. 08's manifest entry carries these keys,
including `phase`, `scope`, `transfers`, `max_skip_steps`, `on_skip` and `cost_hint` (08 §4.5). Steps are never
stretched in normal play: `max_skip_steps` applies only to skip schedules (§4.5).

| key | meaning | example (hydrology) |
|---|---|---|
| `name` | unique within the plugin; `plugin:name` is its canonical id | `hydro:route` |
| `dt` → `period` | `dt` in game seconds, converted to whole ticks; the step size is `h = period × Δ` | 3 h = 324,000 ticks |
| `phase` | offset in ticks; default `hash(id) mod period`, which spreads processes over different ticks | |
| `band` | the finest level it resolves; it runs on the cut clamped at that level | finest (whole cut) |
| `scope` | `body` (the whole planet), `observers` (rings around each observer), `event` (a region an event registered) | `body` |
| `reads`, `writes`, `transfers` | fields read; fields written (the write kind comes from the field's schema in 09: `delta` or `owned`); conserved moves as `[from, to]` pairs (§4.4) | reads `std:ground_height, std:precipitation, std:snow`; transfers `[hydro:flow, hydro:flow]`; writes `hydro:discharge` (owned) |
| `where` + `latency_ticks` | `sync` (engine thread, stage 4) or `async` (worker, applied k = `latency_ticks` later; the rule for k is §4.6, owned here) | `async`, 30 |
| `skip` | `max_skip_steps` and an optional `on_skip` (§4.5) | 64 |
| `cost_hint` | declared µs per domain blob, for budgeting and for choosing skip schedules deterministically | 1.2 |

- **The domain** is the cut clamped at the band ([06](06-blob-hierarchy-and-cut.md)). For every leaf finer than the
  band, the domain holds its ancestor at the band level instead. That ancestor always exists, since inner blobs are
  caches. The core hands the process its domain as a handle array, in canonical (key-sorted) order.
- **Writes at the band level** reach the leaves by push-down, using the field's declared push rule (09):
  - uniform;
  - by amount;
  - by room or stress;
  - multiplicative, for non-negative intensive fields.
- **One process, one body.** A process step runs for one body. A planet-wide process on three active planets is
  three steps, each with its own phase.

The prototype's processes, re-expressed (cadences from `planet/js/plugins/*.js`):

| process | period | band | scope | affinity | note |
|---|---|---|---|---|---|
| climate | 12 h | `ℓ_sim_floor` | body | async(30) | tendencies pushed down |
| hydrology | 3 h | finest | body | async(30) | routing on the whole cut; owns discharge and receivers, transfers water |
| vegetation | 10 days | ~157 m cells | body | async(30) | the band avoids most of the observer effect (§4.10) |
| fire | 1 h | finest | body | sync | the front is a field of arrival times, so rendering shows it continuously between steps |
| snow release | 1 h | finest | body | sync | releases near observers spawn parcels (§4.7) |
| volcano | 1 h | finest | body | sync | magma budget, vent sources |

**Arrival-time fronts.** Fire is a good model for anything that must look continuous but is computed rarely. Each
step computes arrival times `t_arr` for the cells the front reaches before the next step, from
`distance / speed`. "Burning now" is then `t_arr ≤ t < t_arr + burn_time`, which the renderer evaluates every frame
from `t`. A 1 h step therefore draws a front that advances smoothly.

### 4.4 Writing stored fields: delta, transfer, owned

Stored fields have a schema ([09](09-fields-and-composition.md)): type, default, unit, aggregation, push rule,
non-negativity, and the `conserved` flag. 09 owns the three write kinds (R13); this section owns how they are
scheduled and merged (snapshots, canonical order, transfer scaling):

| kind | meaning | merge | for |
|---|---|---|---|
| `delta` (op `add`, `max` or `min`, from the schema) | a commutative change with no bound | combined in canonical order | sources and sinks with an outside: rain from climate, evaporation, relaxation toward a target (`dT = (T* − T)(1 − e^{−h/τ})`); `max` for fire intensity |
| `transfer` | moves an amount from `(F, a)` to `(G, b)`; G may be F, and b may be a parcel | scaled per source so that no source goes negative, then applied in pairs | everything conserved: melt (snow → water), entrainment (snow → parcel volume), routing (flow → receiver's flow), lava from the magma budget into parcels |
| `owned` | the defining plugin sets its own private field (one writer, checked at load) | written at the barrier | diagnostics recomputed each step (discharge, routing receivers), and fields one plugin fully controls |

**Scaling transfers.** For each source `(F, a)`, sum the outgoing amounts in canonical order, then compute

```
s(F, a) = min(1, stock(F, a) / Σ out(F, a))
```

and apply every transfer out of `(F, a)`, both its debit and its credit, multiplied by `s`. This conserves mass
exactly, needs no process ordering, and can run in parallel. Credits arriving at `(F, a)` in the same stage can't be
spent in it (Jacobi semantics), so `s` depends only on the snapshot. For a field like snow, with a few takers
(melt, entrainment, sublimation, lava), the cost is one pass over the transfer log.

**Why transfers rather than "take" calls** that see each other's results? A take must know what earlier takers
took, which serialises them across threads and couples results to their order. A transfer states intent, and the
core resolves it. It also makes conservation testable: the core keeps a per-field ledger of delta totals, and the
regression tests check that the field total changed by exactly that amount.

**Push-down of coarse writes.** A delta written to a blob above the leaves goes down through the field's push rule
in stage 5, in the same canonical order. Transfers between coarse blobs push both sides down. Rules from `planet/`:
non-negative intensive fields go multiplicatively; extensive fields go by amount or by room.

### 4.5 Multi-rate stepping, catch-up and subcycling

**The canonical schedule.** Process P steps at every tick T with `(T − phase_P) mod period_P = 0`, with
`h = period_P × Δ`. Steps are never stretched in normal play or in warp. If a sync process overruns its budget, the
clock slows instead (§4.11). The result of a tick never depends on how fast the machine is.

**Skip schedules.** A skip command ("advance to tick T₂") is an input like any other, so it may change the step
pattern deterministically:
- If `T₂ − T ≤ D_canon` (provisionally 30 game days), the skip runs the canonical schedule, as fast as it can, behind
  a progress indicator. Its result is identical to playing through, except for the fast layer and physics, which go
  implicit (§4.11).
- Longer skips give each process `n = min(⌈(T₂ − T) / period⌉, max_skip_steps)` steps of equal size
  `h = (T₂ − T) / n`. Every scheme is stable for any step (R3), so this is safe. It is also cheap: ten years of
  hydrology at `max_skip_steps = 64` is 64 steps instead of 29,200.
- `n` depends only on the skip length and on declarations, never on measured cost. So two machines skipping the same
  span get the same world.

**Subcycling (Berger–Oliger).** Not needed for the processes we know of. They are all unconditionally stable:
- linear reservoirs with closed-form solutions;
- arrival-time fronts;
- implicit relaxation;
- bathtub fills.

A future process with an explicit, CFL-limited scheme on the cut (heat diffusion or smoke advection at fine levels)
can declare `cfl(level)`. Then:
- The scheduler runs that process's finer levels `2^m` times per coarse step, with `h_fine = h / 2^m`.
- Fluxes crossing a coarse–fine boundary are accumulated as transfers during the fine substeps and applied to the
  coarse side at the coarse step. This is AMR refluxing, which keeps conservation exact.

This is built only when a plugin needs it, with one test: a diffusion process across a refinement boundary conserves
mass and matches the uniform-fine solution. The one place CFL matters today is the fast layer, which handles it per
island (§4.7).

### 4.6 Async processes and workers

A process declared `async(k)` is a pure function of a snapshot:

1. **At stage 4 of tick T,** the core gathers the process's declared read fields over its domain into a snapshot
   buffer and posts the job to a worker that hosts an instance of the plugin. Plugins are nearly stateless (D11), so
   any instance will do ([08](08-plugin-system.md)). Reads outside the declaration fail, which is how the
   declaration is enforced.
2. **The worker runs `step`.** Its writes go into a delta log: deltas, transfers, owned writes, blob creation commands,
   requests and events, addressed by handle together with the handle's generation.
3. **At stage 4 of tick T+k,** the log is applied. If the job isn't back yet, the engine thread **stalls** until it
   is. It never applies the log late or skips it. A stall shows up as a hitch, and the compute limit slows the clock
   for the following frames (§4.11).

**Deltas follow the blobs.** Between T and T+k the cut may have changed. When the log is applied:
- a delta on a blob that has since split is pushed down to its children by the field's rule;
- a delta on a blob that has since merged goes to its parent;
- a transfer scales against the mapped stocks.

Splits and merges happened at known barriers, so the mapping is deterministic. It needs handle generations
([05](05-blob-model.md)) so that a stale handle is detected, never misread. This was the "classic index bug" of
`DESIGN.md` §4, in a new form.

**Choosing k: the async latency rule.** This doc owns the rule; 06, 08 and the plugin docs refer to it (R1).
- `k` is fixed per process (`latency_ticks` in its manifest entry) and must be shorter than the period.
- The default is `k = min(period / 4, 1 s)`, which is 30 ticks at 30 Hz. Climate and hydrology are `async(30)`.
- A 3 h process lagging 1 s is invisible. A 1 h fire step must not lag at all, so fire is `sync`.
- The resolver rejects a declared `latency_ticks` that breaks the rule. Smaller values than the default are allowed
  when a worker can reliably finish sooner.
- Split jobs for the cut are not processes: their latency is `L_split` (3 ticks), owned by 06.

**What runs where** (initially):
- **The engine thread** runs stages 1–9, the sync processes, physics and the fast layer.
- **Workers** run async processes, plugin `split` jobs for the cut (06), and persistence serialisation (15).
- There is no parallelism inside a stage on the engine thread yet. The Jacobi and canonical-order rules allow it
  later without changing results.

### 4.7 The slow layer and the fast layer

From D16: the **slow layer** is stored fields on the cut. It holds matter at rest or in steady motion, uses
reservoir closures, and is restricted, lifted and persisted. The **fast layer** is parcels: blobs in physical time,
for matter in unsteady motion. [11-physics](11-physics.md) owns the integrator. This section owns the exchange, the
level of detail and the observer rules.

**From the slow to the fast layer** (a closure hands over mass when its own assumptions fail):
- snow load beyond slab stability → avalanche parcels (built in `planet/`);
- a lake above its sill by more than a surge height → dam-break parcels (built: lake surges);
- water leaving its channel (overbank), free fall (a waterfall), a lahar (to add);
- vents → lava and bomb parcels, via sources.

Every handover is a `transfer` from the field to the new parcel's volume. Mass is exact by construction.

**From the fast to the slow layer**, when the motion ends, the material's `deposit` hook (batched) chooses:
- back into a field: water to the routing's water in transit, melted debris to runoff;
- into the ground: cooled lava becomes ground. That is kind A content (R7): a deposit term contributing to
  `std:ground_height` (09's `shape` phase, R17), so every level re-derives it, and the store sees blob additions. This
  replaces the prototype's 2D deposit shape field (`PLUGINS.md` §6);
- a placed record (kind D) for discrete debris that keeps its own shape;
- a resting parcel: an attached blob with a slow process that melts or cools it.

**Level of detail.** Parcel size follows the distance to the nearest observer: in `mobile.js`,
`r = clamp(dist / 250, 3, 40)` m, with a 3,000-parcel budget that coarsens new parcels. Distance to an observer is sim
state (§4.9), so this is deterministic.

**Islands with their own clock.** Parcels group into islands: connected components of parcels within kernel reach
(an avalanche here, a lava flow there). Each island steps every `2^j` ticks, with `j` chosen from its CFL limit
(0.4 × the smallest kernel radius / the largest speed) and capped by the material's `dt_max`. `j` is recomputed only
at the island's own step boundaries. This is local time stepping without interpolation between islands, because they
don't touch. When two islands meet, they merge at the next common boundary, and the finer step wins. A 40 m parcel
flow 10 km away then steps every few seconds instead of 30 times a second.

**Explicit or implicit.** Inside the explicit radius of any observer (15 km for snow in the prototype), a release
becomes parcels. Elsewhere the closure moves the load itself. This is an observer effect *by construction*, and it was
measured (`PLUGINS.md` §5):
- after 10 days, the top cells were 0 / 33% / 200% different (median / p90 / max);
- 1.9% of the snow ended in a different 10 km cell after one day;
- the totals were within 0.2%.

Four measures keep it bounded:

1. **Closures move load across cells.** The implicit closure sends a calibrated share of released load to its
   downslope neighbours, as the explicit runouts do (7–10 km), instead of only onto its own flat ground. This is the
   same fix as the river outlets (`DESIGN.md` §2.5).
2. **Only being somewhere counts.** The radius is measured from observers (sim state), never from the camera's view.
   Watching an avalanche through a zoomed camera from 30 km shows the implicit outcome, drawn coarsely.
3. **Hysteresis at event boundaries.** An event that started explicit stays explicit until its parcels have all
   deposited, even if the observer leaves; budget pressure coarsens its parcels instead. An event that started implicit
   stays implicit. Switching mid-event would add a discontinuity the closure can't represent.
4. **Regression tests with budgets.** The snow observer experiment (`tools/snowobserver.mjs`) becomes a standing test
   per fast-layer material, with tolerances agreed per field (Q10.5).

### 4.8 The sim tier and the render tier

**Proposal: adopt the two-tier cut.**

- **The sim tier** is what the simulation runs on. It is driven only by:
  - observers' positions (rings per band, §4.9);
  - process bands and scopes;
  - physics contacts and predicted paths;
  - event regions;
  - later, peers.

  Each of these is sim state or a command, so the sim tier is a deterministic function of the state and the
  commands.
- **The render tier** is the sim tier plus render-only splits. The renderer refines further where its rays say a
  leaf is too coarse ([06](06-blob-hierarchy-and-cut.md), [12](12-rendering.md)):
  - render-only blobs come from the owner's deterministic `split` and carry lifted copies of the fields the
    renderer reads (`std:snow_cover`, `std:wetness`, `std:burnt`, …, 09's seed list);
  - they are never simulated or persisted, and may be dropped at any time;
  - when the sim tier later refines the same blob, the same lift produces the same children, so render-only blobs
    are promoted in place, not regenerated.
- **Edits happen on the sim tier only.** An edit command whose target is render-only first forces sim refinement
  there. In practice the player can only edit near themselves, where the sim tier is already fine.

What may and may not influence the sim tier:

| input | sim tier? | why |
|---|---|---|
| observer positions | yes | sim state, set by commands |
| process bands, event regions, contacts | yes | sim state |
| camera view direction, screen-space error, GPU ray feedback | **no** | would make the world depend on where someone looks, and differ between peers |
| GPU readbacks of any kind | **no** | GPU floats are not bit-identical across vendors (idea 3; [11](11-physics.md) §4.10) |

**Cost.** Render-only splits run the same lift in the same workers, and the renderer already needed them (the ray
cut's view-driven refinement). The new rule only forbids simulating them.

### 4.9 Observers and interest

- **An observer is sim state** (R9): a position (a body plus coordinates in its frame) and a policy (which bands and
  radii it drives).
- **The only sim observers are the player's avatar, or a pending observer during embark.** The pending observer
  stands at the chosen embark point while the sim tier refines there, before the avatar exists
  ([18](18-player-camera-and-navigation.md)). It is created by the embark command, so it is sim state too.
- **Cameras drive nothing in the sim.** An orbit or navigation camera, a free-fly camera and a photo-mode camera only
  make render-tier requests ([06](06-blob-hierarchy-and-cut.md), [12](12-rendering.md)). From orbit, the
  render tier refines what the camera needs; the sim tier stays at `ℓ_sim_floor`. See Q10.4 and `questions.md` C11.
- **Sim requesters per observer:**
  - **rings:** every blob of level ℓ within `K = 8` blob sizes of the observer (one region edge of that level) is
    refined. That also gives the 2:1 balance and the neighbour invariant. 06 owns the ring definition (R5);
  - **band floors:** a process's band can stop refinement from making it finer (vegetation at ~157 m);
  - **the explicit fast-layer radius** (§4.7).
- **Interest management** (idea 8). For multiplayer, each peer's observer is a requester. The union of all
  observers' rings is the sim tier. A peer's interest set is its own observer's rings plus the global `ℓ_sim_floor`. That is
  the subset of state a server would send it ([02](02-numerics-time-and-determinism.md) owns the networking rules).
  Because the sim tier is a function of observers and state, every peer computes the same cut from the same inputs.

### 4.10 Coverage: which bodies are simulated

**Activated planet: the one definition** (R9). A planet becomes *activated* the first time the player embarks on
it, or the first time an event touches it (an impact aimed at it, say). From then on it is simulated at
`ℓ_sim_floor` all the time. Generating a planet's top level when the player enters its system, to draw it from orbit
([14](14-planet-generation.md)), is separate: it creates blobs to look at, not a running simulation.

`ℓ_sim_floor` is the always-simulated coarse level (R5, 06 owns the level table). On the 500 km reference planet
it is ℓ14: S2 depth 6, 24,576 cells of ~12.3 km. The prototype's "top level" was the same idea at 10.1 km (9,126
cells on the 250 km planet).

| body | simulated | cost |
|---|---|---|
| the active planet (activated, with an observer on it) | `ℓ_sim_floor` everywhere, plus the sim tier; physics and the fast layer near observers | the frame budget (§4.15) |
| other activated planets | `ℓ_sim_floor` processes on their canonical schedule, async in a worker | see below |
| planets not activated (never visited, or only seen from orbit) | nothing (the generated top level, if any, is static) | 0 |
| all bodies' motion | on rails ([04](04-bodies-and-orbits.md)) | evaluated on demand |
| stars, galaxies | no processes | 0 |

**Activated planets in the background.** At 1:1 time this is nearly free. On the 250 km planet, the top level's
processes cost about 2 × 8 ms (climate) + 8 × 11 ms (hydrology) + 0.3 ms (vegetation) per game day, roughly 0.1 s
of CPU per game day. At 1:1 that is about 1 µs of CPU per real second per planet. At 1 day/s it is ~110 ms per real
second per planet, more with snow. That budget makes warp, not background planets, the expensive case.

**Planet size.** The 500 km reference planet has 24,576 `ℓ_sim_floor` cells, about 2.7× the prototype's 9,126,
so its processes cost about 2.7× as much (estimate).

**Planets not yet activated** are a pure function of `(seed, T)`. When a planet is first activated at tick T_a, its
top level is generated (unless 14 already did so on entering its system) and then spun up for a fixed span S ending
at T_a: four game years in the prototype, which took about 3 s including generation. The spin-up starts from the
generated state at `T_a − S`, so the result depends on T_a. That is an observer effect in time, and nobody can
detect it, because nobody simulated the planet before. In multiplayer, the authority activates a planet once.

One visible case remains. If the orbit view showed a planet's generated state (snow, lakes, forest) before
activation, the spin-up could change it visibly at embark. 14 can avoid that by running the spin-up when it generates
the top level, ending at the tick the system was entered; activation then catches up from that tick to T_a with
skip semantics (§4.5).

**The alternative, lazy catch-up** (an activated planet sleeps until revisited, then catches up with skip steps), saves
the background cost. But a planet's state would then depend on when it was revisited: ten years of hydrology is
58 s+ at canonical steps, so the catch-up must use stretched steps. Background stepping costs little at 1:1 and keeps
"the world went on without you" exact. Recommendation: background stepping for activated planets, with a cap on how
many stay activated (least recently visited ones go to lazy catch-up). See Q10.2.

### 4.11 Time control: rate, governor, warp and skip

**The rate of a frame** is `r = min(r_requested, r_governor, r_compute)`:

- **`r_requested`** comes from the player's time control (a command).
- **`r_governor`** keeps fast events readable. For each moving parcel `i` with speed `v_i > 0.05 m/s` at distance
  `D_i` from the nearest observer:
  ```
  lim_i      = 0.02 · D_i / v_i
  r_governor = max(1, 5th smallest lim_i)
  ```
  A source that hasn't emitted yet gives 20. These values are carried over from `planet/js/app.js`. Measured
  there: an avalanche from 3 km runs in real time, and lava from 4 km at 10–15×.
- **`r_compute`** is how many ticks the frame budget affords, from the last frames' measured tick cost.

The rate is wall-clock dependent, and that's fine: it only decides how many whole ticks run. The command log records
the number of ticks each frame advanced, so replays reproduce the same ticks.

**Modes:**

| mode | when | what changes |
|---|---|---|
| **explicit** | `r ≤ r_explicit` (provisionally 8×) | nothing: every tick is complete. Physics, the fast layer and processes run as in real time |
| **warp** | `r > r_explicit`, by command | on entry, awake objects settle (11) and characters freeze. The fast layer goes implicit: no new parcels, existing islands coarsen and finish, or resolve through `on_skip`. Slow processes keep their **canonical** schedule. The clock advances in macro-steps of M ticks, and nothing in physics needs per-tick work |
| **skip** | a command "advance to T₂" | warp semantics, plus the skip schedule of §4.5 when the span exceeds `D_canon` |

**Why warp keeps canonical process steps.** At 1 day/s that costs about 110 ms of CPU per real second on the
250 km planet. That fits in workers, and it makes slow-process results identical to playing through in real time.
Only fast-layer and physics outcomes differ, and they go implicit by a deterministic rule.

**The fast layer in warp and skip.** The prototype dropped parcel dynamics through skipped time. Lava already flowing
solidified where it was, and before the `onSkip` fix it piled at the vent: 4,847 parcels. Better, and still
deterministic: **coarsen instead of skipping.** On entering warp:
- each island merges its parcels to the coarse LOD (40–150 m);
- it takes large steps with the semi-implicit resistance, which is stable for any step;
- it gets at most `N_skip` steps (provisionally 64 per island per warp span).

Whatever is still moving after that is resolved by the material's `on_skip`, as the prototype did. This keeps runout
lengths roughly right for flows already under way, which the prototype lost (`PLUGINS.md` §7).

**Multiplayer.** The rate becomes shared state, decided by the authority. Warp and skip need consensus or are off
(Q10.3, `questions.md` C6).

### 4.12 Triggers and self-organised criticality

Many of the events in the README's Zipf list come from one shape:
- **slow loading** (snowfall, fuel growth, stress, pore pressure);
- **a threshold** (slab stability, ignition, failure);
- **local release that can trigger neighbours** (avalanche entrainment, fire spread, rockfall);
- **dissipation** (melt, burnout, runout).

When loading is much slower than release and something dissipates, event sizes come out power-law distributed:
Bak–Tang–Wiesenfeld sandpiles, the Drossel–Schwabl forest-fire model. That is Zipf balance for free (idea 7, adopted;
[17](17-events-and-discovery.md) uses it).

**What the scheduler provides for this shape:**

- **Watches.** A watch (field, comparison, threshold, quantum, callback export) is declared in the manifest (§5). It is evaluated in stage 7, only on handles whose value changed this
  tick. The core keeps a per-field dirty set from stage 5, so a watch costs nothing when nothing changed. The callback
  runs at stage 4 of T+1 with the crossing handles.
- **Release quanta.** A watch fires when a stock crosses a quantum, not on every increment, so releases are discrete
  events.
- **Caps in physical units, not counts.** The prototype capped releases at 6 per step and triggered slabs at 40 per
  event. Count caps truncate the tail of the size distribution, and the tail is the rare, big event the player
  remembers. Caps should be:
  - a per-event volume or energy budget, scaled to the level the event runs at, so a big event coarsens instead of
    being cut;
  - and a per-tick count cap only as a safety net, with a log entry when it bites.
- **Cascades inside the fast layer.** Entrainment and triggering are local to parcels (11). The cascade stays in one
  island with one clock.
- **Magnitude logging.** Every release records its magnitude (volume, area, energy) with the event (17). Then the
  size distribution can be checked against a target slope (E10.9).

### 4.13 Events in the tick

- **Emission.** Event records are emitted in stages 3–7 and dispatched at the end of the tick.
- **Handlers** are plugins' event callbacks ([17](17-events-and-discovery.md)). Their effects apply at stages 1–4 of
  tick T+1: they spawn sources, create bodies, register requests, and queue edits. The one-tick latency is fixed, so it
  is deterministic.
- **Requests.** An event that needs detail registers a requester for a region, a level and a duration. A vent needs
  the 40 m lava band. An impact needs the impactor's level along the predicted path, registered several ticks before
  impact (11).

### 4.14 Parallelism

- **The engine thread** runs stages 1–9 in order: physics, the fast layer, sync processes, merge, restrict.
- **Async processes** form the only job graph. A job depends only on its snapshot, and all jobs due at a tick apply
  together. The one constraint: a process must not have two jobs in flight (`k < period`, checked at load).
- **Later.** The engine thread can split stage 3 by island, and stage 4's sync processes by process. Jacobi semantics
  and canonical merges make that change invisible to results. WASM threads in the core are a separate decision
  ([01](01-runtime-and-toolchain.md)).

### 4.15 Budgets

The targets for a 60 fps frame (16.7 ms) on the engine thread, at 1:1 with one tick or less per frame:

| work | target | basis |
|---|---|---|
| stages 1–2 (commands, structure; applying split and merge results) | ≤ 1.5 ms | prototype refinement: 15–49 ms per teleport, spread over frames by the cut's job queue |
| stage 3 (physics + fast layer) | ≤ 4 ms | 700–850 parcels took 1.2–1.9 ms in JS; WASM with SoA should be faster (estimate) |
| stage 4 (sync processes; applying async logs) | ≤ 2 ms | fire, snow release, volcano each < 1 ms idle |
| stages 5–8 (merge, restrict, triggers, requests) | ≤ 1 ms | dirty paths only |
| render CPU (12) | ≤ 4 ms | |
| slack | ~4 ms | stalls, GC in glue, warp ticks |

**Workers:** async processes (climate, hydrology, vegetation), split generation, persistence. One worker per process
family to start with.

If stage 3 + 4 exceed their targets for several frames, `r_compute` drops below 1 and the clock runs slower than real
time. The frame rate holds, and the world visibly slows, as the prototype did under heavy avalanches.

### 4.16 Scenarios

**The storm and the flood** (scenario 2):
- Climate (async, 12 h) puts a storm over the catchment 16 km upstream. Its precipitation is an `add` at the top
  level, pushed down multiplicatively.
- Hydrology (async, 3 h, whole cut) routes runoff and `transfer`s water in transit along receivers. The player's 1 m
  water cells see their discharge rise over the following days. In `planet/` that was 6.8 → 25 → 75 → 98 m³/s,
  peaking on day 3.
- Nothing about this depends on the player being there, except the fine cells' exact path lengths (water in transit:
  36% median difference, `DESIGN.md` §5).
- With per-process rate tuning (Q10.1), the same flood can arrive within hours of play.

**The asteroid** (scenario 4):
- **T−k:** physics predicts the entry tick from the orbit ([04](04-bodies-and-orbits.md)) and registers a requester
  along the predicted ground track at the impactor's contact level.
- **T_entry:** stage 3 switches the body from rails to integrated.
- **T_impact:** stage 3 detects the contact and emits the impact event.
- **T+1:**
  - stage 1/2 applies the handler's edit queue: crater removals and rim additions;
  - stage 2 runs the connectivity checks, so overhang islands become bodies;
  - stage 3 spawns ejecta parcels;
  - stage 4: fire and climate get `add`s (heat, dust).
- **Afterwards.** The crater region is touched and persists. The requester expires when the parcels deposit, and the
  cut coarsens by eviction.

**The beaver dam** (scenario 3):
- The beaver plugin is sync, period ~1 s, band finest, scope observers. Its processes query neighbours, call
  `vegetation.fell(tree)` (a service; the felling is an edit plus a detached object, [11](11-physics.md)) and move
  logs with kinematic grabs.
- Hydrology sees the dam through the obstruction its blobs add to the routing elevation (14 and the hydrology
  plugin). The next 3 h step reroutes, and the pond fills by bathtub over days.
- While unobserved, the beavers become a population field at a coarse band (16). The dam persists as kind D placed
  records (R7): the beaver plugin places its own `dam_log` content and asks vegetation, through a service, to remove
  the felled log (R11).

**Dig, leave, return** (scenario 5):
- The dig is an edit command applied in stage 2. Ground is kind A content (R7): the dig changes generator input (a
  `z` residual plus 3D edit terms, so `std:ground_height` reflects it), and the blob store sees removals plus
  additions. The subtree is marked touched ([15](15-persistence.md)).
- When the player flies away, the observer's rings leave. Requests lapse, the cut coarsens by eviction, and merges
  store residuals (15).
- Ten years later, during which the planet was background-stepped at `ℓ_sim_floor`, the returning observer's rings
  request refinement. The lift of the *current* parent plus the decayed residual gives the hole, now 3.7 → 3.56 m deep
  in `planet/`.

**Orbit to grass** (scenario 1):
- In orbit view the camera is not an observer (§4.9). If the planet is activated, the sim tier is `ℓ_sim_floor`;
  if not, nothing is simulated. Either way, the render tier adds render-only detail where the camera needs it.
- The embark command activates the planet if needed and creates a pending observer at the embark point. Its rings
  request the finest levels there, and the zoom animation covers the split latency
  ([18](18-player-camera-and-navigation.md)). When the sim tier has refined there, the avatar replaces the pending
  observer.
- Most of the blobs the camera saw are promoted from render-only to sim, not regenerated.

### 4.17 Alternatives considered

| alternative | why not |
|---|---|
| a variable timestep driven by frame time | breaks R2 |
| immediate writes in plugin order (the prototype) | results depend on plugin order; fine for one author, not for community plugin sets |
| claims serialised by a take ledger | serialises takers across threads; transfers do the same with no order |
| a fully event-driven (discrete-event) scheduler | good for fronts and orbit handoffs, which use it locally (arrival times, predicted crossing ticks); a uniform tick is simpler for everything else |
| simulating the render tier (one tier) | looking changes the world; peers with different views diverge |
| lazy catch-up for every planet | the state depends on visit times; long catch-ups need stretched steps; background stepping is cheap at 1:1 |
| wall-clock budgets that truncate a tick (the prototype's `msBudget`) | results depend on machine speed |
| an ECS-style fine-grained parallel scheduler | premature; plugins are batch functions over arrays, and the work is in a few big steps |

## 5. Interfaces

Processes and watches are **declared in the manifest** (R2). There is no runtime registration. 08 §5.2 is the
canonical list of `ss_` functions; this section gives the semantics the scheduler adds. Handles are `u32`, with 0
meaning none (D6). Arrays are pointer plus count in the caller's memory.

A process entry, in 08's manifest shape, with the keys this doc proposes adding (`phase`, `scope`, `transfers`,
`max_skip_steps`, `on_skip`, `cost_hint`):

```json
"processes": [
  { "name": "route", "export": "proc_route", "dt": 10800, "phase": "auto",
    "band": "all", "scope": "body", "where": "async", "latency_ticks": 30,
    "reads":     ["std:ground_height", "std:precipitation", "std:snow", "hydro:flow"],
    "writes":    ["hydro:discharge", "hydro:receiver"],
    "transfers": [["hydro:flow", "hydro:flow"], ["std:snow", "std:water"]],
    "calls":     ["ext:hydro:source"],
    "max_skip_steps": 64, "on_skip": null, "cost_hint": 1.2 }
],
"watches": [
  { "name": "slab_load", "field": "snow:over", "cmp": "cross_up", "threshold": 0.5, "quantum": 0.5,
    "export": "on_slab_load" }
]
```

The step ABI and the calls a step makes:

```c
// ---- the exported step (signature fixed per kind, 08 §5.3) ----
typedef struct { u32 body; i64 tick; f64 h; u32 n_domain; u8 skipping; u32 event; } ss_step_ctx;
i32  proc_route(const ss_step_ctx* ctx);

// ---- inside a step (names per 08 §5.2) ----
u32  ss_domain(u32* dst, u32 cap);                                   // band-clamped cut handles, canonical order
i32  ss_field_read (u32 field, void* dst, const u32* h, u32 n);      // stage-start snapshot
i32  ss_field_add  (u32 field, const void* delta, const u32* h, u32 n); // write kind delta: op add/max/min from the schema
i32  ss_field_set  (u32 field, const void* src, const u32* h, u32 n);   // write kind owned: definer only
i32  ss_field_push (u32 field, const void* delta, const u32* h, u32 n); // delta on coarse handles -> leaves
i32  ss_field_transfer(u32 from_field, const u32* from, u32 to_field, const u32* to,
                       const f64* amount, u32 n);                    // declared pair; scaled per source at the barrier
i32  ss_blob_create(u32* out, const BlobDesc* d, u32 n);             // buffered, committed at the barrier
i32  ss_blob_destroy(const u32* h, u32 n);                           // buffered
i32  ss_refine_request(...);                                         // a sim-tier requester (06 owns the signature)
i32  ss_event_emit(...);                                             // 17 owns ss_event_*
u64  ss_call_seed(void);                                             // counter-based RNG seed per call (02)

// ---- watches: the exported callback, run at stage 4 of T+1 with the crossing handles ----
i32  on_slab_load(const ss_step_ctx* ctx, const u32* h, u32 n);

// ---- the fast-layer exchange (11 owns parcels; sources are runtime state, not registrations) ----
i32  ss_source_add(const ss_source_desc* s);                          // vents: rate, material, until, on_skip
```

The core side, for the engine and tests:

```zig
pub fn tick(world: *World, cmds: []const Command) void;          // stages 1–9
pub fn advanceFrame(world: *World, budget_ns: u64) FrameReport;  // picks r, runs whole ticks, records the count
pub fn skip(world: *World, to_tick: i64) void;                   // a command; warp/skip semantics
pub fn fingerprint(world: *const World, fields: []const FieldId) u64; // bit-exact regression hash
pub fn conservationLedger(world: *const World, field: FieldId) Ledger; // adds vs total change
```

**What this doc expects from other docs:**
- **06:**
  - `cut.request(region, level, priority, until)`;
  - split and merge jobs with the fixed latency `L_split` (3 ticks);
  - the band-clamped domain;
  - dirty-path restriction;
  - a deterministic cut planner given the requests.
- **09:**
  - schemas with `agg`, push rule, `nonneg`, `conserved`, default;
  - computed fields readable in processes through `ss_field_eval` (08 §5.2).
- **08:**
  - instances replicable on the engine thread and in workers;
  - exported `step` functions;
  - snapshot-in and delta-log-out for async jobs;
  - overrun detection, since WASM can't be preempted.
- **05:**
  - handle generations, for validating delta logs.
- **02:**
  - the tick length;
  - the command log;
  - the RNG;
  - canonical float-summation helpers.

## 6. Risks and unknowns

- **One-step lag between coupled processes.** Jacobi semantics delay one process's effect on another by one step of
  the slower one. Snowmelt to runoff is fine via a hook. A tight two-way coupling between two plugins would need to
  live in one process. E10.2 measures the effect on the prototype's plugin set.
- **Delta remapping across cut changes** (§4.6) is subtle code on a hot path. A bug here is a silent conservation
  leak. Mitigation: the conservation ledger is checked in every test run.
- **Background planets at warp.** About 110 ms of CPU per real second per planet at 1 day/s (estimate from the
  prototype's costs), ×2.7 for the 500 km reference planet. Ten activated planets at warp do not fit. The cap on activated planets,
  or lazy catch-up past it, is needed.
- **Plugins that overrun.** WASM has no preemption. A sync process that takes 50 ms stalls the frame. The core can only
  measure afterwards and then demote the process to async or disable it (08).
- **Warp semantics differ from play.** Physics settles and the fast layer goes implicit. Some outcomes (an avalanche's
  runout) differ between "watched at 1×" and "warped through". That is accepted and documented.
- **Fast layer in skipped time.** Coarsening helps, but long flows can still end early. It needs measuring (E10.5).
- **The observer effect from the explicit fast layer.** It won't reach zero. The tolerances are a product decision
  (Q10.5).
- **Cost scales with planet size** for every planet-wide process, roughly with the number of `ℓ_sim_floor` cells.

## 7. First experiments

| id | experiment | success criterion |
|---|---|---|
| E10.1 | **Determinism harness.** In Node, port `planet/` climate + hydrology to the tick model. Run the same command log twice with different random ticks-per-frame sequences. | bit-identical fingerprints after 60 game days |
| E10.2 | **Jacobi vs immediate writes.** Run the 7 prototype plugins with stage snapshots + transfers, against the prototype's order-dependent run (`fingerprint.mjs` scenario). | field totals within 1% after 60 days; conservation ledger exact; the snow → runoff lag quantified |
| E10.3 | **Async latency.** Hydrology in a worker with k = 30 ticks, at 1:1 and at 1 day/s. | no stalls at 1:1; stall rate at 1 day/s reported |
| E10.4 | **Observer regression suite.** `observer.mjs` and `snowobserver.mjs` rewritten against the engine. | the prototype's numbers reproduced (trees 3.1% median at 157 m, 0 at top only; snow 0 / 5% / 200% after a day) |
| E10.5 | **Skip.** A 10-year skip with `max_skip_steps` vs canonical steps; a lava flow in progress, skipped vs coarsened. | top-level fields within agreed tolerances; skip ≤ 5 s; runout length within 30% of the watched flow |
| E10.6 | **The governor.** An avalanche from 3 km and lava from 4 km. | real time and 10–15×, as in `planet/` |
| E10.7 | **Background planets.** Ten activated planets at 1:1 and at 1 day/s. | CPU per real second measured; it informs Q10.2 |
| E10.8 | **Hysteresis.** The observer walks away mid-avalanche. | no visible discontinuity; mass exact to 1e-12 |
| E10.9 | **Self-organised criticality.** A year of snow release with volume caps instead of count caps. | the release-size histogram follows a power law over ≥ 2 decades (slope reported) |

## 8. Build order

| milestone | scheduler work |
|---|---|
| M0 | the tick loop, the command log, `advanceFrame`, fingerprints, the Node harness (E10.1 with an empty world) |
| M2 | the requests stage with the render requester only; the render tier (render-only splits) |
| M3 | manifest process declarations; sync processes; `delta` and `owned` writes; stage snapshots; merge; dirty restriction; climate ported (E10.2 partial) |
| M4 | the dynamics stage (character, objects); observers as sim state; ring requesters; the structure stage with edits and connectivity |
| M5 | transfers; the conservation ledger; async workers (E10.3); the fast layer with islands; the governor (E10.6); hydrology, vegetation and snow ported; watches; background planets (E10.7); warp and skip (E10.5); the observer suite (E10.4) |
| M6 | event dispatch in the tick; event requesters; caps in physical units (E10.9); hysteresis (E10.8) |

## 9. Open questions

**Q10.1 One clock, and how fast does the world move?** (merged into `questions.md` C2)
- *Why it matters:* real physical rates make a storm's flood arrive days later, and a volcano recharge in years. The
  prototype was played at 1 day/s to see anything.
- *Options:*
  - (a) one clock at 1:1, a short planet day (04), and per-process rate parameters that compress slow processes
    (faster wave speeds, faster growth);
  - (b) two clocks: physics at 1:1, world processes accelerated;
  - (c) player-controlled warp only.
- *Recommended:* (a) plus (c). Two clocks break consistency where they meet: parcels handing water to the routing,
  bodies leaving the atmosphere onto rails.
- *Working assumption until C2 is answered:* R8. A ~1 real-hour day, with fictitious forces dropped in surface
  physics, so day length doesn't depend on planet size; plus (a) and (c).

**Q10.2 Activated planets: background stepping or lazy catch-up?**
- *Why it matters:* cost under warp vs a world that is exact while you're away.
- *Recommended:* background stepping for the N most recently visited planets (N ≈ 8), lazy catch-up beyond.

**Q10.3 Time control for the player.** (merged into `questions.md` C6)
- *Options:* warp up to what rate? Skip ("rest") allowed? In multiplayer?
- *Recommended:*
  - explicit up to 8×;
  - warp up to ×600 on the surface, higher in orbit view (18), with implicit fast physics. (The cost figures quoted
    "at 1 day/s" are the prototype's 24 h days, i.e. ×86,400.)
  - skip any length;
  - multiplayer: authority-controlled, off by default.

**Q10.4 Who is an observer?** (merged into `questions.md` C11)
- *Options:* the avatar, certainly. The camera in orbit or galaxy view? A photo-mode free camera?
- *Recommended (now the working assumption, R9):* the avatar, or a pending observer during embark, is the only sim
  observer. Every camera only makes render-tier requests.

**Q10.5 How much observer effect is acceptable?** (merged into `questions.md` C12)
- *Why it matters:* it sets the regression tolerances and how much closure work is needed.
- *Recommended starting tolerances,* per `ℓ_sim_floor` cell after 10 days:
  - conserved totals per planet within 0.5%;
  - per-cell p90 within 10% for slow-layer fields;
  - per-cell p90 within 35% for fast-layer deposits (today: 33%), tightened as closures move load across cells.

**Q10.6 Tick length.** (merged into `questions.md` C15)
- *Why it matters:* 30 Hz is the provisional value (02). Physics wants 60 Hz substeps for characters (11).
- *Recommended:* a 30 Hz tick with 2 fixed physics substeps. Confirm with 02.
