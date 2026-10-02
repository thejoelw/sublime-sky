# Sublime Sky: planning overview

Status: **draft for discussion** (2026-10-01). Nothing here is decided until Joel says so. Every "decision" below is
a *proposal* with a reason. Each one links to the doc that argues it in depth. Open questions are collected in
[questions.md](questions.md). Its first section lists the 20 that matter most, in the order they block work.

**How to read this folder:**
- Start with this page: the glossary (§5), the proposed decisions (§6) and the working assumptions waiting for your
  answer (§6.2).
- Then read [questions.md](questions.md).
- Then read the docs in the order of the milestones you care about: 02, 03, 04 and 18 for M1; 05, 06, 12 and 14 for M2;
  08 and 09 for M3.

All the docs were checked against each other in one pass. Where they disagreed, the shared choice is written into
§6 and every doc follows it.

This folder plans the real game, built slowly and on purpose. The prototypes in the sibling repo `../blobby-test`
(called `blobby-test/` below) answered many "can it be done, and what breaks" questions. These docs turn those answers
into a foundation. Where a prototype measured something, the docs cite it rather than guess.

## 1. The game in one paragraph

You appear on a planet in a deterministic universe, identified by an id anyone can share. The world is made of
**blobs**: metaballs that merge smoothly. Blobs are to this game what cubes are to Minecraft. You explore. The world is
beautiful at every scale, from a sunset on a mountain top to a galaxy seen from orbit. It is alive at every scale too:
a storm you watched on the horizon floods your river a day later, beavers dam a creek, a volcano erupts, an asteroid
lands. Events follow a Zipf-like balance: something small is always happening, something big once in a long while.
You can jump to any planet or star you can see, or look one up by id, and embark anywhere. Almost everything that
happens comes from **plugins**: small WebAssembly modules that anyone can write. They define blob types, fields and
behaviours on top of a core that owns bodies, blobs, physics, rendering and the simulation schedule.

## 2. Pillars and constraints

**Pillars** (what every system is judged against):
1. **Exploration and discovery.** It is rewarding to go and look. Events and interactions have a Zipf balance, from
   common to very rare.
2. **Wonder and beauty.** It need not be realistic, but the view from a mountain top or of a sunset should inspire awe.
3. **Blobby, consistently.** Everything is blobs: terrain, water, creatures, clouds, debris. No polygons in the
   look.
4. **Plugins make the world.** The core supplies mechanisms. Plugins supply content and behaviour. It should be easy
   for the community to add things.
5. **A consistent, simulated world.** Large-scale processes run everywhere at a coarse scale and show up at the fine
   scale. Looking at a place does not change what happens there (the "observer effect" is kept small and measured).

**Hard constraints** (not all built now, but no decision may block them):
- Isotropy: no grid or preferred direction visible to the player.
- A spherical planet you can walk around.
- A big world with near-infinite view distance.
- Fast ray queries from any point, not only from the camera.
- Beautiful lighting: shadows and AO first, then fuller radiosity with several light sources.
- Rare events that keep things interesting.
- Large-scale simulation that stays consistent (the storm, then the flood).
- Runs in the browser. The core is WebAssembly, plugins are WebAssembly, and JS is only glue (plus the renderer's
  GPU calls).
- Multiplayer is out of scope now, but nothing may make it hard later.

**Nice to have:** several planets you can visit; physics with many loose objects (merging blobs is an acceptable
substitute for bouncing and rolling).

## 3. The systems, and where each is planned

| # | doc | system | one line |
|---|---|---|---|
| 01 | [runtime-and-toolchain](01-runtime-and-toolchain.md) | runtime | core language, WASM features, threads and workers, JS glue, browser support, memory |
| 02 | [numerics-time-and-determinism](02-numerics-time-and-determinism.md) | numerics | precision and frames, units, time, deterministic maths and RNG, multiplayer readiness |
| 03 | [universe-and-identity](03-universe-and-identity.md) | universe | seeds, ids and search, lazy galaxy/star/planet generation, non-colliding systems |
| 04 | [bodies-and-orbits](04-bodies-and-orbits.md) | bodies | the body tree, Kepler rails, parent selection, reparenting, spin, drag, frames |
| 05 | [blob-model](05-blob-model.md) | blobs | what a blob is, the blend rule, materials, handles and keys, storage |
| 06 | [blob-hierarchy-and-cut](06-blob-hierarchy-and-cut.md) | hierarchy | levels, split and merge, the cut and who drives it, lift / restrict for geometry |
| 07 | [spatial-queries](07-spatial-queries.md) | queries | neighbours, rays, field and surface queries, the batch ABI, acceleration structures |
| 08 | [plugin-system](08-plugin-system.md) | plugins | manifest, isolation vs dynamic linking, ABI, lifecycle, services, workers, SDK |
| 09 | [fields-and-composition](09-fields-and-composition.md) | fields | field schemas, stored vs computed fields, composition order (the `@checkpoint` question) |
| 10 | [simulation-and-scheduling](10-simulation-and-scheduling.md) | simulation | ticks, processes and bands, slow and fast layers, catch-up, time warp, observer effect |
| 11 | [physics](11-physics.md) | physics | dynamics modes, parcels, rigid objects, contact with blob fields, detachment, drag |
| 12 | [rendering](12-rendering.md) | rendering | the renderer synthesised from M1–M7 and the ray cut, scales, LOD, GPU data path |
| 13 | [lighting-atmosphere-and-sky](13-lighting-atmosphere-and-sky.md) | lighting | suns, shadows, AO, atmosphere, clouds, water, night sky, GI roadmap |
| 14 | [planet-generation](14-planet-generation.md) | worldgen | top-down planet generation, lift priors, rivers, blobs from fields, other planet kinds |
| 15 | [persistence](15-persistence.md) | persistence | source of truth, residuals and edit logs, save format, storage, versioning |
| 16 | [entities-and-creatures](16-entities-and-creatures.md) | entities | blob types, entities, skeletons and animation, populations ↔ individuals, behaviour |
| 17 | [events-and-discovery](17-events-and-discovery.md) | events | Zipf design, emergent power laws, an event director, the discovery journal |
| 18 | [player-camera-and-navigation](18-player-camera-and-navigation.md) | player | galaxy / system / planet views, jump and search, embark, the player on a sphere, UI |
| 19 | [tooling-and-testing](19-tooling-and-testing.md) | tooling | repo layout, dev server, test pyramid, GPU tests, benchmarks, debug views, authoring |
| 20 | *audio (not written yet)* | audio | thunder, rumbles, wind, creatures. Events lose much of their impact without sound. Planned before M6 (Q17.8); event records carry sound hints from M5 |

Every doc has the same shape: **purpose and scope**, **requirements**, **what the prototypes taught**, **design**
(with the alternatives considered), **interfaces**, **risks**, **first experiments**, **build order**, and **open
questions** (numbered `Qnn.m`, collected in [questions.md](questions.md)).

## 4. How the pieces fit

```
 main thread ─ DOM UI (menus, search, journal), input capture
      │  input ring / UI messages
      ▼
 engine worker ─ owns the WebGPU device and an OffscreenCanvas (main-thread fallback designed in)
 ┌────────────────────────────────────────────────────────────────────────────────────┐
 │ core (WASM)                                                                        │
 │   universe + ids ─▶ body tree + orbits ─▶ blob store ─▶ hierarchy                  │
 │                                              │          ├─ sim tier ◀── avatars,   │
 │   spatial queries (CPU, sim tier only) ◀─────┘          │   bands, contacts, events │
 │   fields (schemas, phases)                              └─ render tier ◀─ renderer, │
 │   scheduler (ticks, stages, processes)                       cameras               │
 │   physics (parcels, objects, contacts)                                             │
 │   entity tables, event log                                                         │
 │                │ batch calls (sync)                                                │
 │                ▼                                                                   │
 │   plugin instances (WASM): types, split, fields, processes, services               │
 │                                                                                    │
 │ renderer (JS + WGSL): reads both tiers through heap views, uploads, draws          │
 └────────────────────────────────────────────────────────────────────────────────────┘
      │ jobs with a fixed tick latency (deterministic)      │ chunks to write
      ▼                                                     ▼
 plugin workers ─ replica instances of            persistence worker ─ OPFS saves
 the same plugins, for async processes            and the deletable cache
 and splits
```

**The data flow of one frame:**
- Input arrives as commands stamped with a tick, and the scheduler advances whole ticks.
- Each tick runs in fixed stages (10): physics and the processes that are due read one snapshot, and their writes
  are merged in a canonical order.
- The **sim tier** is updated from deterministic requests only: observers, bands, contacts and events. Splits arrive
  a fixed `L_split` ticks after they are asked for.
- The **render tier** is updated from the renderer's requests of the previous frame, under a per-machine budget. It
  is never simulated or saved.
- The renderer then draws both tiers, interpolating between the last two ticks, and writes requests for the next
  frame: rays that found a blob too coarse.

## 5. Glossary (use these words, with these meanings)

**Bodies and space**
- **Body**: a node of the body tree. It has a mass (possibly 0), a frame, and a motion: on **rails** (a Kepler
  ellipse, exact and periodic) or **integrated** (forces, drag, contacts). It is drawn and touchable only if blobs are
  attached to it. Galaxies, stars, planets, moons, a thrown rock and the player are all bodies.
- **Root**: the one body without a parent.
- **Parent body**: the body whose gravity a body feels. It is chosen by **sphere of influence (SOI)**, not by
  strongest force (see D13).
- **Frame**: a body's coordinate system: inertial axes, origin at the body, f64. A spinning body also has a rotating
  **body-fixed frame**, which its surface blobs live in.
- **Switch radius**: the radius around a body (the top of its atmosphere, or just above an airless surface) inside
  which dynamic bodies are integrated in the body-fixed frame. Outside it they go back on rails (04).
- **NCA**: the nearest common ancestor of two bodies. Their collision is detected and resolved in its frame.
- **Binary angle**: an unbounded phase (orbit, spin) stored as a wrapping integer, so it never drifts (02).

**Blobs**
- **Blob**: an oriented ellipsoid, a blend radius `k`, appearance (colour, material) and metadata (level, instance,
  owner, type, flags). Blobs attach to a body.
- **Level ℓ**: an absolute size class. Blobs of level ℓ have their largest radius in `[2^ℓ, 2^(ℓ+1))` m, and a
  merged blob is one level above its children. So a whole 500 km planet is level 18, a 1 m boulder level 0, and a
  creature's eyelid level −6. (This replaces "L0 = atom" from the first sketch. See D-levels below.)
  - Named levels on the 500 km reference planet (06 owns the table):
    - `ℓ_sim_floor` = ℓ14: the coarse level always simulated on an activated planet, 24,576 cells of ~12 km;
    - `ℓ_view_floor` = ℓ10: the orbit view's render cap;
    - the finest ground level, ℓ−1 by default (~0.4 m cells).
  - **Contact level**: the level at which a body touches the world. A creature's eyelid doesn't refine the ground
    under it (11).
- **Atom**: a blob its owner marks unsplittable. **Coarse blob**: one that can split.
- **Split**: replace a blob by its children (blobs of lower levels that it approximates). **Merge**: the reverse.
- **Representative** and **bound**: every coarse blob has a *representative* shape (it approximates its descendants:
  it is what is drawn, simulated and touched at that level) and a *bound* (it encloses every descendant: it is what
  culling, ray traversal and broad phase use). See D9.
- **Instance**: a blend group. Blobs of the same instance blend smoothly. Blobs of different instances meet with a
  hard min. (This was settled in `blobby-test/world/BLOBMAP3.md`.)
- **Object**: a connected group of blobs that moves rigidly. Every object is a body (usually a child of a planet).
  Blobs of different objects never blend; one object can hold several instances.
- **Owner**: the plugin that defines a blob's type. There is exactly one, and it is the one asked to split the blob.
  Other plugins create its blobs only through its services.
- **Blob type**: a plugin-defined kind ("granite", "oak trunk", "beaver fur"). It is identified by (plugin u16,
  type u16).
- **Content kind**: how a family of blobs is lifted and persisted (14):
  - **A, field-derived**: ground, water sheets, snow;
  - **B, counted**: trees, populations, as ranked candidates;
  - **C, shaped**: objects split from a parent shape;
  - **D, placed**: logs, dams, built and settled things, never part of a lifted family.
- **Entity**: a plugin-level thing (a tree, a beaver), realised as blobs, bodies and plugin state. The working
  assumption is that the core keeps thin **entity tables**: key, owner, anchor, plugin-declared columns, with no
  behaviour (16, pending C10).
- **Model instance**: a creature's or the avatar's appearance. It is a shared blob asset posed by bones, kept outside
  the blob store and reached by queries through proxies (16, pending C10).
- **Handle**: a loaded blob's `u32` slot index; `0` means none. It is transient: slots get reused.
- **Key**: a blob's or body's stable, deterministic identity, 128 bits: a hash of (parent key, semantic ordinal).
  Keys go to disk and over the network. Handles never do.

**The hierarchy and the cut**
- **Cut**: the set of loaded leaf blobs. Every potential blob is either loaded or has exactly one loaded ancestor.
  (Also "the loaded hierarchy".) It has two tiers:
  - **Sim tier**: the deterministic part. It is driven only by observers, process bands, contacts and events, and
    has a fixed budget `B_sim`. It is what the sim runs on, what queries see, and what is saved.
  - **Render tier**: a per-machine extension of the sim tier driven by the renderer's requests. Its
    **render-only blobs** are regenerated from the sim tier's leaves, never simulated, never saved, and dropped by
    eviction (06).
- **Region**: a node of the per-body octree that splitting, balance and the renderer's nodes are organised by. A
  region of level ℓ has an edge of 2^(ℓ+3) m (06).
- **Requester**: anything that asks for refinement:
  - in the sim tier: observers, a sim process's band, physics contacts, an event, later a network peer;
  - in the render tier: the renderer and cameras.
- **Restrict (R)**: children → parent. **Lift (L)**: parent → children, a prior followed by an exact projection, so
  that `R(L(x)) = x`. **Push-down (P)**: a delta on a coarse blob shared out to its leaves. **Residual**:
  `x − L(R(x))`, what a fine state has that a fresh lift would not. (All from `blobby-test/planet/DESIGN.md`.)
- **Touched**: a subtree holding state that cannot be regenerated (an edit, lava). Only touched subtrees, plus a halo,
  persist anything.

**Fields and simulation**
- **Field**: a named per-blob quantity with a schema (type, default, unit, aggregation). A **stored field** is state
  (water, temperature, snow): restricted, lifted, persisted. A **computed field** is derived on demand
  (flammability, ground density).
- **Phase**: a named point in a computed field's composition order (the formal version of `@checkpoint`; doc 09).
- **Process**: a plugin's periodic computation, declared in its manifest. It has a cadence, a **band** (the finest
  level it resolves), and the fields it reads and writes. A process changes shared state in one of three ways:
  - a **delta** (add, max, min);
  - a **transfer**: a conserved move between fields, scaled so no source goes negative;
  - an **owned** set, of the plugin's own private fields only.

  It never assigns shared state.
- **Service**: a function a plugin exposes for others to call (e.g. `vegetation.kill_trees`). **Extension point**: a
  hook a plugin defines and others contribute to. Contributions are summed, so order doesn't matter.
- **Resource**: plugin state that is not per blob (a river network, a list of volcanoes). The core stores it.
- **Slow layer**: fields on the cut (matter at rest or in steady motion). **Fast layer**: **parcels** (blobs moving in
  physical time: avalanches, lava, bombs, floods). Both from `blobby-test/planet/PLUGINS.md`.
- **Observer**: whatever drives the sim tier. That is the player's **avatar** (or a **pending observer** at an embark
  destination), and every player's avatar later. Observers are sim state. A camera is never an observer: it only
  requests render detail (18).
- **Activated planet**: a planet that has been embarked on or touched by an event. Its top level is simulated on the
  canonical schedule (10). Generating a planet's top level when the player enters its system (14) is separate.
- **Tick**: the fixed simulation quantum (provisionally 1/30 s, with 2 physics substeps; doc 02). Rendering
  interpolates between ticks.
- **Stage**: a phase of a tick (10). Every process in a stage reads the same snapshot, and the writes are merged at
  the stage's end in a canonical order.
- **Notability class**: an event's rarity class, 0 (ambient) to 5 (legendary), each with a target interval between
  witnessed events (17). **Director**: the bounded part of the event system that may shift timing and placement
  toward observers (17, pending C12).

## 6. Proposed cross-cutting decisions

These touch many docs, so they are stated once here. Each is a proposal until confirmed. The cross-doc review amended
some of the first versions (marked *amended*) and added D18–D23, which the docs converged on independently. The docs
sometimes cite "idea N" or "R N"; [integration.md](integration.md) defines those and maps them to the ids here.

### 6.1 Decisions

| id | proposal | why (short) | doc |
|---|---|---|---|
| D1 | **Core in Zig**: a pinned compiler, a minimal `std`, a vendored libm, and a hand-written C header (`sublime.h`) as the ABI source of truth. C++ is the fallback if the M0 spike fails. | data-oriented SoA, explicit allocators, `wasm32-freestanding` with no runtime; the C ABI keeps every plugin language open | 01 |
| D2 | **WebGPU only**, no WebGL2 fallback. | everything dynamic needs compute (`world/README.md` #7); two backends double the renderer | 01, 12 |
| D3 | **The engine runs in a dedicated worker** with an OffscreenCanvas; the main thread only does DOM UI and input. A main-thread fallback is designed in, because WebGPU in workers is unverified on Safari and Firefox. | no main-thread jank; the core and the renderer share one heap; GPU calls need one thread anyway (`wasmgpu/README.md`) | 01 |
| D4 | **f64 positions per body frame** on the CPU. Relative positions always go through the NCA. **The GPU uses integer node keys with per-level camera cells** (no rebasing). **Time is an integer tick count**; unbounded phases are binary angles; generated periods are whole ticks. | f32 has 0.5 m steps at Earth's radius and 3 cm at 500 km; integer phases never drift, so motion is exactly periodic | 02, 04, 12 |
| D5 | **Determinism by construction**: fixed ticks; a counter-based RNG keyed by (stream, key, tick, counter); canonical iteration and merge order; no relaxed SIMD; the core's own libm (never JS `Math`); plugins get no clock or ambient randomness, and are identified by content hash. | lockstep or state sync later, replays, bit-exact tests (the planet port was checked bit-identical by fingerprint) | 02 |
| D6 | **Handles vs keys.** Batch APIs use `u32` handles (0 = none). Keys are 128-bit hashes of (parent key, semantic ordinal), and are what disk and network see. Saves are chunked by spatial region and sorted by a spatial key, never by key order. | the numpy-style index API stays fast; identity stays stable and deterministic | 05, 15 |
| D7 *amended* | **One hierarchy, a two-tier cut.** The **sim tier** is deterministic: observers, bands, contacts and events, under a fixed `B_sim`; it merges when requests expire, with hysteresis. The **render tier** extends it per machine from the renderer's requests; it merges only by eviction and is never simulated or saved. The core balances both (2:1, neighbours exist). | the sim must not depend on where a camera looks (determinism, replays, multiplayer), yet the view needs view-driven detail (the ray cut, `RAYCUT.md`) | 06, 10, 12 |
| D8 *amended* | **Leaves own state; coarse levels are restrictions.** Lift = plugin prior + exact core projection. **An unload is lossy** (only touched state plus a halo is kept); **a save is exact** (the live top level and the cut are snapshotted). | consistency to 1e-15 and an unload/return round trip to 1e-12 m were measured in `planet/`; exact saves make replays and joins trustworthy | 06, 15 |
| D9 | **Representative ≠ bound.** Each coarse blob has a moment-matched representative (what is drawn and simulated) and a separate conservative bound (what queries trust). | the ray cut found that rendering an enclosing superset looks wrong (surfaces 0.8–1.5 cells high), while queries need enclosure | 05, 06, 07 |
| D10 *amended* | **Blending by instance only.** Same instance: exponential smooth-min. Different instances: hard min. Blobs of different objects never blend; one object can hold several instances (a creature's eyes are their own instance). | settled in `BLOBMAP3.md`; it also gives objects crisp contacts | 05, 12, 16 |
| D11 | **Plugins are isolated WASM instances by default**, talking through a C ABI of batched arrays. Trusted plugins can be linked statically with the same ABI. **Authoritative mutable state lives in core storage** (fields, resources, entity tables). **The owner of a blob is its type's definer.** **Everything structural is declared in the manifest.** | community plugins can't corrupt each other or the core; any language works; instances can be replicated in workers and reloaded; saves and networking stay core concerns | 08 |
| D12 | **Field composition by phases**: the field's definer declares named phases; commutative phases take operands in canonical plugin order; ordered phases use before/after; a load-time `(field, phase)` graph catches cycles. Base values come from type tables or owner dispatch. The schema gives the dtype and default, so no `@init` export is needed. | the formal version of `@checkpoint`; the planet port needed no ordering because writes were deltas | 09 |
| D13 *amended* | **Parent body by sphere of influence**, not by strongest force: Laplace SOI for planets and moons, the Roche-lobe (Eggleton) radius for binaries. | the Sun pulls on the Moon ~2.2× harder than Earth does, so "strongest force" would make the Moon orbit the Sun | 04 |
| D14 *amended* | **Generated systems are non-colliding by construction**: siblings' radial annuli `[periapsis − SOI, apoapsis + SOI]` are disjoint, with Hill spacing on top. **Stars are static in the galaxy.** **Planned impacts are the one deliberate exception.** | provable whatever the inclinations; stellar motion would be invisible over game time anyway | 03, 04, 17 |
| D15 *amended* | **At the blob-store level, an edit is blob removals and additions**; there are no carve blobs in the world. **The persisted form depends on the content kind**: A as a ground-input residual plus 3D edit terms, B as candidate overrides, C as removals plus residuals, D as placed records. | `M7.md` §7: one mechanism, every level re-derives; each kind keeps the cheapest exact record | 06, 14, 15 |
| D16 | **A slow layer of fields plus a fast layer of parcels** for everything that flows: water, snow, lava, debris, later clouds. | built and measured in `PLUGINS.md`; one integrator, rheology per material | 10, 11 |
| D17 *amended* | **Shared state changes only by deltas and transfers**; a plugin may set its own private fields. Every process in a stage reads one snapshot, and the stage's writes are merged in a canonical order. Coarse processes are rates on restricted state, at a declared band. | plugin load order can't change results; the planet's rules against the observer effect (`DESIGN.md` §2.4, §2.8) | 09, 10 |
| D-levels *amended* | **Levels are absolute size classes**: 2^ℓ ≤ r_max < 2^(ℓ+1) m, merged blobs at their children's level + 1, negative levels allowed. The 500 km root is ℓ18. "Atom" means unsplittable. **Interaction refines to a body's contact level**, not to its finest blob. | levels then compare like with like across bodies and plugins | 06, 11 |
| D18 *new* | **Deterministic async.** Work done in workers (splits, async processes) is applied a fixed number of ticks after it is issued (`L_split` = 3 ticks for splits; 10 sets the per-process rule). A late job stalls the engine instead of diverging. | parallelism without results depending on machine speed | 01, 06, 08, 10 |
| D19 *new* | **GPU results never feed sim state.** Every sim query runs on the CPU against the sim tier. The GPU may do cosmetic things: spray, splash particles, display-only detail. | GPU float results aren't bit-identical across vendors or drivers | 07, 11, 12 |
| D20 *new* | **Observers are avatars**, and they are sim state. Cameras only request render detail. "Activated planet" is defined once (10). | browsing in orbit view must not refine or simulate where it looks; multiplayer interest follows players, not cameras | 10, 18 |
| D21 *new* | **Content kinds A–D** (14) decide how a family splits and persists. Kind A (ground) splits in two phases: fields are lifted and projected first, then geometry is realised from them. Kind C gets the full geometric moment projection. Kind D never enters lifted families. | exactness and conservation for continuous media, without moving a tree because someone looked at it | 06, 14, 15, 16 |
| D22 *new* | **A save pins the exact plugin modules it was made with**, embeds them, and detects undeclared changes through baseline hashes. A newer generator is applied only on request (rebase). | residuals are relative to a regenerated baseline; a silent generator change corrupts every save | 08, 15 |
| D23 *new* | **Persistence runs in a dedicated worker** on OPFS sync handles. Content-addressed chunks with two alternating manifests mean a crash never loses the last save. A **cache is not a save**: generated and merged nodes are cached separately, keyed by generator version, and can be deleted. | no stalls on the engine thread; the far view needn't take minutes to rebuild cold (`BLOBMAP3.md`) | 01, 15 |

### 6.2 Working assumptions waiting for your answer

The docs had to assume something for each of these, so they all assume the same thing. Each one is a real choice for
Joel, asked in [questions.md](questions.md).

| id | working assumption | the alternative | question |
|---|---|---|---|
| W1 | **No centrifugal or Coriolis force below the switch radius**: radial gravity only in surface physics, with an exact frame conversion at the switch radius. Day length is then free (any day longer than ~24 min), even on a small planet. Working default: a ~1 real-hour day. | include them: then a 500 km planet needs days of at least ~2 h (a 40-minute day would mean ~35% of g outward at the equator) | C2 |
| W2 | **The ground is a boundary representation**: plates over an implicit solid, with keels so every level is closed for rays. Caves, tunnels and overhangs are 3D edit terms that add sheets. There are no interior volume blobs. | volumetric interior blobs (surface band, interior fill, deep fill) | C13 |
| W3 | **The hybrid renderer** (12): one integer-keyed octree of render nodes (= regions) over both tiers; baked bricks as a cache; exact thin blobs; far splats that morph into their parents; rasterised grass; moving parts under a per-frame top level. | M7's per-level camera windows; or the ray cut alone | Q12.1 |
| W4 | **Thin entity tables in the core**, plus **model instances** for creatures and the avatar's appearance. | no entity concept in the core; creature atoms as ordinary store blobs | C10 |
| W5 | **Two index layers**: the core's per-body octree of regions (splitting, balance, render nodes); the ground plugin's S2-style cube-sphere as its surface index (cell ids, ordinals, hydrology graph). | a single cube-sphere for everything; or the octree only | C8 |
| W6 | **A bounded event director**: it may shift an event's timing within plugin-declared windows, and choose its place among K = 4 candidates. It never creates mass or energy, and acts only on notability class ≥ 2. | events at their natural rates only: a wandering player then witnesses about one eruption per ~4,000 h of play (at the ~1 h day) | C12 |
| W7 | **Hardware**: an M1 Max at 1080p60 is the design point; an M1 base or recent integrated GPU at 720p30 is the minimum; no mobile. | a higher minimum, or mobile | C5 |
| W8 | **The ocean is an analytic sea-level sphere** plus dynamic water where it moves. | an all-blob ocean | C20 |
| W9 | **A 30 Hz tick** with 2 physics substeps. | 20 Hz or 60 Hz | C15 |

## 7. Canonical scenarios

These six scenarios exercise the whole architecture. Each doc checks its design against the ones that touch it, and
the first integration tests will be built from them.

1. **Orbit to grass.** Search a planet id, jump, watch it rotate, embark on a ridge, walk to a stream. This tests the
   ids, the body tree, scale transitions, the cut under a moving camera, the renderer at every distance, precision,
   and the zoom animation hiding refinement latency.
2. **The storm and the flood.** A storm crosses the top level 16 km upstream, and the player's 1 m water blobs rise
   days later. This tests slow processes, push-down, the always-simulated top level, and time warp. It was built in
   `planet/` (6.8 → 98 m³/s).
3. **The beaver dam.** A beaver plugin finds trees through queries, fells them through a vegetation service, drags
   the logs and drops them in a creek. Hydrology reroutes and a pond forms. The player leaves and comes back a year
   later. This tests entities, cross-plugin services, blob creation and motion, routing restricted upward, residuals,
   and populations ↔ individuals.
4. **The asteroid.** A body on rails enters the atmosphere, switches to integrated motion, glows, and strikes. The
   ground is forced to split down to the impactor's contact level. The crater is a regenerable crater record plus
   touched detail (the store sees removals and additions), ejecta are parcels, and the fires and dust are fields. The
   crater persists. This tests events, forced refinement, rails ↔ integrated,
   the fast layer, and persistence.
5. **Dig, leave, return.** The player digs a hole and fells a tree, flies to another continent for ten years of game
   time, and returns. This tests touched subtrees, the halo, decay, ranked tree candidates, and catch-up.
   (`planet/` measured 3.7 → 3.56 m deep, felled trees gone, the damaged tree standing.)
6. **A community plugin.** Someone adds a "hut" blob type that burns, using only the manifest and the public fields.
   This tests field composition (flammability), owner dispatch, plugin isolation, and loading.

## 8. Proposed build order

Each milestone starts by settling its open questions and ends with something to look at. The order front-loads the
things everything else rests on (numerics, ids, the body tree, the blob store and cut). It also produces a visible
result early.

| milestone | content | you can |
|---|---|---|
| **M0 foundations** | repo, toolchain (the Zig spike decides D1), dev server (COOP/COEP), engine worker + OffscreenCanvas + WebGPU with the main-thread fallback, a core WASM skeleton, heap-view glue, a Node determinism harness with cross-engine runs (V8, JSC, SpiderMonkey), OPFS persistence-worker probe, CI for non-GPU tests | see a cleared canvas driven from WASM; run `test` headless in three JS engines |
| **M1 the sky** | universe ids, one galaxy generated lazily (static stars), binaries, body tree on Kepler rails with binary-angle phases, stars as points with a blackbody table, systems as coarse blobs, engine-drawn labels, picking, search, camera transitions | fly around a galaxy, pick a star, see its planets orbit |
| **M2 a planet from orbit** | blob store, regions and the render tier (render requests only), top-level planet generation with a first-visit cache, terrain statically linked for render-only splits, renderer v1 (far-field splats), Hillaire atmosphere | watch a planet rotate with a sunset terminator |
| **M3 plugins and fields** | manifest, isolated instances, field schemas and phases, terrain as a plugin providing the two-phase `split`, one process (climate) ported, entity tables (if C10 says yes) | swap a terrain plugin and see the planet change |
| **M4 embark** | the sim tier and avatar observers, refinement to the finest levels near the player, near-field renderer, shadows and AO, spatial queries, player controller, physics v1 (character, parcels) | walk on the planet |
| **M5 a living world** | scheduler stages, slow and fast layers, hydrology and vegetation plugins, persistence v1 (exact saves, residuals, placed records), sky visibility for dark places | see rivers, forests, a flood; leave and return |
| **M6 events** | event framework and log, the director (if C12 says yes), avalanche, eruption, impact, discovery journal; the audio plan (doc 20) | witness something rare |
| **later** | creatures and behaviour, clouds, caves, GI, multiple planets you can visit, networking preparation | |

## 9. The prototypes, and what carries over

| prototype | what it settled | where it lands |
|---|---|---|
| `blobby-test/blobs/`, `viewer/` (creatures) | oriented ellipsoids beat spheres; per-blob `k` is the main artistic control; carves and a separate colour-blend width for faces; material richness over atom count; animation as rigid bone groups is nearly free; exact per-pixel evaluation is ~10× too slow for many characters | 05, 12, 16 |
| `world/` M1–M4 (`README.md`) | evaluating blobs while marching doesn't scale and baking does; fine detail (grass) is the cost, not blob count; coarse levels need volume; exp smooth-min is not a distance field (bound the gradient); same GPU, WebGL2 ~2× faster than WebGPU on fragment work | 05, 12 |
| `world/` M5 (`BLOBMAP.md`) | smooth blobs sampled, thin blobs exact; per-level camera windows make memory independent of world size | 12 |
| `world/` M6 (`BLOBCUT.md`) | projected-error cuts work; pixel-count feedback cuts churn, livelock and drop geometry; per-pixel overlap is the cost, not resident blobs | 06, 12 |
| `world/` M5 v2 (`BLOBMAP2.md`) | rasterise small local blends (grass); split march and shade into compute passes; exact grass shadows via a sun-space grid; toroidal windows | 12, 13 |
| `world/` M5 v3 (`BLOBMAP3.md`) | blending by instance; continuous LOD as a second pass; every level merged from real blobs (sandbox rule); jittered merge sites (isotropy) | 05, 06, 12 |
| `world/` M7 (`M7.md`) | prefilter merges by measuring first; far field as morphing ellipsoid splats; edits as removals; deterministic, origin-snapped merges keep edits local | 06, 12, 13, 15 |
| `world/` ray cut (`RAYCUT.md`, branch `worktree-vcut`) | top-down generation with exact enclosure; a ray-driven cut with feedback words; eviction as the only merge; prediction rays; the budget is memory, not frame time | 06, 07, 12, 14 |
| `planet/` (`DESIGN.md`) | restrict / lift / push-down / residual; leaves own state; the neighbour invariant; priors read persisted state only; structure (rivers) lifted and restricted; the observer effect measured; continuous detail inside the lift | 06, 09, 10, 14, 15 |
| `planet/` (`PLUGINS.md`) | fields as the seam; writes as deltas make composition order-free; services and extension points were needed at once; accessors not arrays; slow + fast layers; time governor | 08, 09, 10, 11, 17 |
| `wasmgpu/` | the wasm data path equals JS; no aliasing of GPU memory; stale views after `memory.grow` upload nothing silently; pass floats through memory; coalesce writes ≥ 4 MiB; one thread per GPU device | 01, 12 |

## 10. How we'll work

- **Docs first, then code, in small steps.** A doc's proposal becomes a decision when Joel confirms it. Record the
  decision in the doc's status line and in the decision table above.
- **Measure before deciding** where a prototype hasn't already. Every doc lists its first experiments.
- **The prototypes stay as a reference.** Code is ported on purpose, never copied wholesale.
