# 16. Entities and creatures

Status: draft · Depends on: [05 blob model](05-blob-model.md), [06 hierarchy and cut](06-blob-hierarchy-and-cut.md),
[07 spatial queries](07-spatial-queries.md), [08 plugins](08-plugin-system.md), [09 fields](09-fields-and-composition.md),
[10 simulation](10-simulation-and-scheduling.md), [11 physics](11-physics.md) · Used by: [12 rendering](12-rendering.md),
[15 persistence](15-persistence.md), [17 events](17-events-and-discovery.md), [18 player](18-player-camera-and-navigation.md),
[19 tooling](19-tooling-and-testing.md)

## 1. Purpose and scope

This doc covers how plugin-defined *things* (animals, plants, structures that creatures build, and the player's
avatar) are built from the core's primitives: blobs, instances, bodies, fields and resources. It covers:

- what the core knows about entities, and what it leaves to plugins;
- skeletons, posing, and procedural locomotion;
- the creature asset format and creature LOD;
- populations at coarse levels and individuals at fine levels, and how one becomes the other;
- behaviour: perception, decisions, interactions;
- plants and growth;
- the beaver dam (canonical scenario 3) end to end;
- what entities cost, and what they ask of the renderer.

**Out of scope here:**
- how blobs blend and are stored (05);
- how contacts and rigid objects work (11);
- how creatures are drawn (12). This doc states the load; 12 owns the technique;
- the save format (15);
- the plugin ABI mechanics (08). This doc lists the calls it needs.

## 2. Requirements

| | requirement | traces to |
|---|---|---|
| must | Plugins define new kinds of creatures, plants and structures with no core change. | pillar 4 |
| must | Creatures are made of blobs and look like the blob creatures of `blobby-test/blobs/`. | pillar 3 |
| must | Populations exist everywhere, at a coarse scale. Individuals exist near observers, and refining or coarsening a region does not change how many there are. | pillar 5, constraint "consistent large-scale simulation" |
| must | An individual the player has interacted with keeps its identity when the player leaves and comes back. | scenario 5, pillar 1 |
| must | Everything that affects the simulation is deterministic, and authoritative state lives in core storage. | D5, D11 |
| must | Creatures walk on a sphere (local up is radial) and on any blob surface, including overhangs. | constraint "spherical planet" |
| should | ~20 creatures at full detail near the camera, ~200 at mid detail, ~2,000 as distant dots (estimates; see §4.11). | pillar 2 |
| should | Herds and flocks are visible from a mountain top (1–2 km) even where no individual is simulated. | pillar 2, constraint "near-infinite view" |
| should | Entities interact with each other and with the player through services, events and fields, and the chains are emergent (beavers → pond → dead riparian trees → wetland). | pillar 1 |
| later | Learned locomotion (RL policies), taming and breeding, procedurally generated species per planet, sound. | |

## 3. What the prototypes taught

**Creatures** (`blobby-test/blobs/`, `README.md`, `ITERATIONS.md`):
- **Atom counts.** Bear 97, giraffe 383, human 424, pterodactyl 955, lion 989. Of the lion's atoms, 815 are the mane.
  Counted from `blobs/creatures/*.py`.
- **Parts.** Every creature has 6–10 parts (bear: torso, neck, head, two front legs, two hind legs, a paint
  "grizzle"), and each part is a bone. Posing is rigid rotation of a part's atoms about a pivot (`blobs/poses.py`).
  "Animation is nearly free" because the smooth union re-blends every joint, every frame.
- **Implicit hierarchy.** The pose code expresses the bone hierarchy only through naming: `rotate(['neck', 'head'],
  …)` and then `rotate(['head'], …)`. A real skeleton needs an explicit bone tree.
- **Foot planting is a translation.** `plant()` moves a leg vertically so that its lowest atom sits on the floor.
  `swing_leg` rotates then re-plants. That is enough for a flat floor and nothing more.
- **Carves are rare but essential.** Each creature has 1–9 carve atoms (human 9, bear 6). They are eye sockets,
  nostrils, mouth lines and ear hollows.
- **Blend radii span 25×.** `k` runs from 2 mm (human details) to 8 cm (lion and giraffe masses). The median is
  1.4–6 cm. "The per-atom blend radius is the main artistic control" (finding 3).
- **What carries recognition.** Silhouette and a few signature features (finding 1). Oriented ellipsoids are worth
  much more than spheres (finding 2). Surface detail is mostly material, not atoms (finding 5).
- **Hair and manes.** They need structure: curved three-atom locks that bend toward gravity (finding 7).
- **Faces are the hardest** (finding 9), hence the stylised face kit the README proposes.
- **Blending was sequential.** It used a polynomial smooth-min applied in authoring order: "masses first, details
  later, carves after what they carve". The world moved to an order-independent exponential smooth-min per instance
  (`world/BLOBMAP3.md`). Creatures have not been re-authored under that rule yet.
- **Rendering cost.** Exact evaluation took 123 ms (bear) to 376 ms (lion) per creature at 1080p. That is roughly
  an order of magnitude too slow for a game with many characters. The README suggests baked per-bone distance bricks,
  a BVH over atom chunks, and LOD by merging ("about 100 well-placed atoms go a long way").

**Trees as entities** (`blobby-test/planet/DESIGN.md` §2.6, `planet/js/world.js`, `planet/js/plugins/vegetation.js`):
- **Counts and sites.** A real-valued tree count per 20 m entity cell becomes trees. Each cell has 8 candidate sites
  (`PL.ENT_SLOTS`) at fixed jittered positions with fixed hashed ranks.
- **Which trees live.** `n = floor(count + hash(cell))`. A rising count activates the best free site; a falling count
  removes the worst living tree (incumbency). The same count always shows the same trees.
- **Overrides.**
  - A stump blocks its site for 30 years.
  - A damaged tree gets a worse rank (`rank / health`), so it is the first to die.
  - A planted tree is activated with a better rank.
- **Growth.** Vegetation is logistic growth on ~157 m cells, with colonisation from neighbours. Its deltas are pushed
  down by room (births) and stress (deaths).
- **Two rules for priors.** A prior may only read persisted state: the tree prior that read capacity brought back the
  wrong number of trees after an unload. Coarse closures should be the expectation of the fine function under the
  lift prior. The unbiased land-fraction closure took the observer effect on trees from 31% to 15.5% at p90.
- **Measured observer effect.** Vegetation run on 157 m cells changes when observed: 3.1% median, 15.5% p90, 27% max.
  Run on top cells only, it doesn't change at all.
- **Services from the start.** Lava kills trees through `vegetation.killTrees`, not through a raw field write. Only
  vegetation knows where the trees of a coarse cell are (`PLUGINS.md` §1.2).

**World blobs** (`world/BLOBMAP3.md`, `M7.md`):
- **Stable tree ids.** Tree instance ids are hashed from the generator cell, so a tree has the same id whichever chunk
  or level produces it. That is the stable-key idea of D6.
- **Trees merge into canopies.** Instances smaller than 4 cluster sizes merge as "aggregates" into canopies, with the
  coverage cap of M7. The far forest is the real forest (the sandbox rule from the prior sessions).
- **Moving objects were never built.** `BLOBMAP3.md` proposed them as "a small extra level, rebuilt every frame, with
  the same instance rule".

## 4. Design

### 4.1 What the core knows: a thin entity record, no entity behaviour

Three options were considered:

| option | what the core holds | verdict |
|---|---|---|
| A. no entity concept | blobs, instances, bodies, fields, resources | every plugin would reinvent four things: anchoring state to a region so it loads and saves with it; migrating it when the thing moves; telling picking and the journal "what is this"; and (later) replicating it to peers. The planet prototype got away with this only because trees never move. |
| B. a full ECS in the core | components, systems, queries | the schema would churn with every plugin. Plugins differ too much: a tree is a ranked site, a beaver is a skeleton with goals, a herd is a density field. It duplicates the field system (09). |
| **C. entity tables (proposed)** | per kind: a table of rows with a stable key, an owner, a spatial anchor, an optional body, flags, and plugin-declared columns | the core does the generic work (storage, keys, anchoring, persistence, interest management, picking) and has no idea what a beaver *does* |

**Proposal: option C.** An **entity kind** is declared in a plugin's manifest with typed columns. The core stores each
kind as a struct-of-arrays table. Rows are addressed by `u32` entity handles (0 = none), which are transient like blob
handles. Each row has:

| column | type | meaning | written by |
|---|---|---|---|
| `key` | entity key (the key space of 05) | stable identity: what disk and network see | core, at creation |
| `kind` | u16 | entity kind id | core |
| `anchor` | blob key | the cut leaf, at the kind's **entity band**, that contains the entity. It loads, unloads and saves with that region. | core (from `body`), or the owner for body-less entities |
| `body` | body handle or 0 | the body that moves it, if any (an animal, a felled log); 0 for static things (a living tree, a dam) | owner |
| `flags` | u32 | `touched`, `dormant`, `named`, `display_only`… | core and owner |
| plugin columns | declared types | e.g. `hunger: f32`, `action: u8`, `target: key`, `gait_phase: f32`, `home: key` | owner (assignments are fine: a row has exactly one writer) |
| `blob_state` | variable bytes (optional) | a behaviour-tree blackboard, a small memory | owner |

This fits D11 and planning idea 4: an entity's authoritative state is in core storage, so a plugin instance holds
only static tables (species parameters, model assets) and scratch. The same beaver plugin can run on the engine
thread and in a worker, and can be hot-reloaded.

**Working assumption (R10, pending `questions.md` C10): the core knows entities only as these tables.** That means
identity, anchor and opaque columns, with no behaviour, no schema of its own, and no update loop. The reasons are
persistence (15) and later networking (planning idea 8: the anchor is what puts a row in a peer's interest set).
Without core anchoring, both have to be solved again in every plugin.

**What is *not* an entity.** Grass, flowers and ground cover are fields that the renderer turns into tufts (the
rasterised detail layer of `BLOBMAP2.md`). They are render-tier only (R19). Rocks and boulders are just world blobs
with stable keys. Neither needs a row until something touches it, and maybe not even then: a moved boulder is an
object (a body, 11) whose blobs are touched (15).

### 4.2 Two residences for blobs: world blobs and model instances

Creatures should not put their atoms into the world blob store:
- A lion has 989 atoms, and 200 animals would be ~100k blobs re-posed every tick.
- They would churn the spatial structures of 07 and 12 for no simulation benefit.
- A creature's atoms are not split or merged by the cut; their hierarchy is a fixed asset.

So there are two places a blob can live:

| | world blobs | model instances |
|---|---|---|
| what | terrain, water, plants, structures, debris, parcels, rigid objects (a felled log) | animals; the player's appearance (the avatar is a dynamic body whose look is a model instance, R10); later vehicles |
| storage | the core blob store (05): one handle and key per blob, per-blob fields | an immutable **model asset** (atoms, bones, materials, a LOD pyramid), shared by every individual of a species, plus per-individual **pose** and **variation** |
| hierarchy | the cut (06): split by the owner, restricted by the core | fixed in the asset, chosen per frame by projected size (render tier) |
| fields | per blob | per entity (an entity-table column): the same `std:` quantities (09's names: `std:wetness`, `std:burnt`), plus plugin columns such as `health` |
| queries (07) | indexed in the world structures | indexed as dynamic bodies through a coarse **proxy** (a few ellipsoids per bone); exact atoms only on demand |
| render (12) | the world structures | per-instance dynamic structures rebuilt from bone transforms |

**Conversions** between the two are explicit plugin calls:
- A dead creature that the world should absorb (a carcass that decays into soil, a creature burnt by lava) can be
  **materialised**. Its plugin reads the pose and places the posed atoms as **kind D placed content** of its own
  carcass type (`ss_place`, 15; R7). The plugin defines that type, so it owns the blobs (R11). The entity's `key`
  becomes the placed instance's creator key.
- The reverse is not needed.

**Requirements this imposes:**
- **05:** distinguish a model asset (a static atom list plus a bone index per atom) from world blobs.
- **07:** queries must return a tagged hit: a world blob handle, or (body handle, bone, atom index).
- **12:** draw model instances.

**Objects and instances (R11).** Blobs of different objects never blend, and one object may hold several instances.
Creatures need that: eyeballs, teeth and claws meet the face with a hard contact, which is also what reads best
(§4.4).

### 4.3 Skeletons and posing

**Data.** A model asset carries a skeleton:

```
bone[i] = { parent: u8 (255 = root), rest: (quat f32x4, vec3 f32) relative to parent, name_id: u16 }
atom[j] = { bone: u8, bone2: u8 (255 = none), w2: unorm8, ... atom fields of 05, in bone-local coordinates }
```

- 6–10 bones were enough in the prototypes. Allow up to 64 per model, so bone indices fit a u8 with room for tails,
  ears, jaws and wing fingers.
- Coordinates inside a body are small (≤ ~50 m), so bone transforms are f32. The body frame itself is f64 (D4).

**Two-bone atoms.** Rigid parts pinch or gap at large bends. An atom near a joint can name a second bone and a
weight. Its centre and orientation are blended (lerp, then nlerp) between the two bones' transforms. This is linear
blend skinning applied to ellipsoids instead of vertices. It costs one extra transform for perhaps 10–20% of atoms,
and keeps the "smooth union re-blends the joint" property. To be tested (E2) before it goes in the format.

**What is sim state and what is cosmetic.** This split matters for cost, determinism and networking:

| state | where it lives | why |
|---|---|---|
| body root transform and velocity (f64, in its parent frame) | body (04, 11) | it moves the entity and collides |
| `gait_phase`, `gait_kind`, `heading`, `action`, `look_target` | entity columns | small, deterministic inputs to posing |
| the full pose (every bone transform) | **not stored** | a deterministic function `pose(model, root, velocity, gait, action, look, terrain)` |

The pose is recomputed wherever it is needed:
- the renderer, every frame, interpolated between ticks;
- physics, when an attach point matters (a beaver holding a log in its mouth bone);
- the player's camera, for the head bone.

It is never persisted or sent over the network. Two consequences:
- **No posing nobody sees.** Off-screen creatures skip posing entirely.
- **The physics proxy follows the pose.** It is the coarse LOD's ellipsoids (one or two per bone), posed. Feet do
  not push the creature. Root motion comes from the controller (11), so the proxy only resolves contacts and
  overlaps.

**Procedural locomotion v1** (target-driven, not clip-driven, per the prior sessions' direction):
1. **Gait.** Behaviour sets a desired velocity and heading. The locomotion step turns them into root motion, through
   the character controller of 11 with species limits (speed, turn rate, slope limit). It also advances
   `gait_phase` by `speed / stride_length`. Gait patterns are phase offsets per limb: a lateral-sequence walk (LH, LF,
   RH, RF a quarter cycle apart, as `bear_walk`), trot, gallop, bound, hop, and flap and glide for the pterodactyl.
2. **Foot targets.** For each limb in swing, predict its landing point:
   `hip + velocity × (stance_time / 2)`, projected onto the surface by a **surface query along local up** (07). On a
   sphere, local up is radial in the planet's body-fixed frame. On an overhang it is the query's own normal, not up.
3. **Two-bone analytic IK** per leg (hip → knee → foot), with a pole vector from the rest pose. Three-bone legs
   (digitigrade lions, giraffes) use a fixed ankle-to-knee ratio heuristic.
4. **The body follows the feet.** Pelvis height is the mean of the stance feet heights plus the rest height. Pitch and
   roll come from fitting a plane to the stance feet. The spine bones distribute the pitch.
5. **Secondary motion.** Heads track `look_target` with limits. Tails, ears and manes get a damped spring per bone.
   These are cosmetic only, and are evaluated in the pose function from an analytic spring response of the last
   ticks' root acceleration, so they need no stored state.
6. **Swimming and flying.** Buoyancy and drag come from the water and air fields (11). The gait becomes a body wave
   (fish, swimming beaver) or a flap cycle.

**Later: learned policies.** An RL policy is a small MLP in the plugin's WASM, with weights as an asset. It maps
(target velocity, terrain samples) to bone targets. It stays deterministic because WASM float arithmetic is (D5).
Training happens offline. The interface above doesn't change: the policy replaces steps 1–4 inside `pose` and the
locomotion step.

**Pathfinding.** There is no navmesh on blob terrain, and building one would be a polygon idea in a blob world.
Proposal:
- **Local steering** on surface queries: probe slope, water depth and obstacles in a fan of directions.
- **Coarse routes** by A* on the cut's adjacency graph at a band a few cells coarser than the creature. The graph is
  the blob neighbour graph of 07, weighted by slope and by fields such as water depth and snow.
- Routes are cached per (start cell, goal cell) for a few seconds of game time.

This is enough for animals, which wander, forage and flee. Doc 07 should expose the adjacency graph at a chosen
band.

### 4.4 The creature asset format

The prototype's authoring vocabulary (`blobs/builder.py`) stays:
- `add`, `sub` (carve), `paint` and `cell` paint;
- `tube` (atoms along a Catmull–Rom spline, stretched along the tangent);
- mirrored parts (`sym`, `mirror=True`);
- named parts, which become bones;
- the material model `Mat`: `col`, `col2`, `rough`, `spec`, `fur`, `patch`, `tscale`, `fuzz`, `trans`, `wrap`,
  `shag`;
- a separate colour-blend width `ck`.

Doc 05 owns the runtime material and blob layout. This doc owns what a *model* adds on top.

```
model asset (binary; a JSON form for diffs and debugging)
  header:      name, version, scale (m), up axis, bounds per bone
  skeleton:    bones (§4.3)
  materials:   table of material records (05's material model; the model refers to them by index)
  instances:   sub-instance table, e.g. 0 = body, 1 = eyes, 2 = claws; blobs blend only within one
  atoms:       per atom: bone, bone2, w2, instance, kind (add | carve | paint | cell), shape, k, ck, material
  lod:         per bone, merged levels 1..L with a parent index per atom (for the morph, M7)
  proxy:       1–2 ellipsoids per bone for physics and queries (= the coarsest per-bone LOD)
  sockets:     named attach points (mouth, hands, back) as (bone, local transform)
  variation:   parameter ranges an individual can take: size scale, colour shifts per material, morph weights
```

**Blending inside a model is order-independent**, to match the world rule (D10, `BLOBMAP3.md`):
1. All `add` atoms of a sub-instance are combined with the exponential smooth-min, each with its own `k`.
2. Then all `carve` atoms of that sub-instance are applied with a smooth subtraction.
3. Different sub-instances meet with a hard min.
4. `paint` and `cell` atoms only change colour, with weights from `ck`.

The prototype relied on order in one place: carving a socket, then adding an eyeball into it. With "carves after all
adds" the eyeball would be carved away. Making eyes, teeth and claws **separate sub-instances** solves this, and they
then meet the lids with a crisp contact line, which is how real eyes read. Experiment E1 checks that the five
prototype creatures survive the change. If a case remains that needs ordering, the fallback is up to two **carve
layers** per sub-instance (adds of layer 0, carves of layer 0, adds of layer 1…). It costs a second accumulator in
12's shader, so it is a fallback, not the default.

**World edits never carve** (D15). Carves exist only inside model assets, and only within their own sub-instance.
Doc 05 should state this scope.

**A stylised face kit.** Stock sub-models (eyeball with an iris paint and lids; three or four mouth shapes; noses)
are parametrised by size, spacing and colour, and placed by sockets. "Faces are by far the hardest thing to get
right" (README), so this is the cheapest quality lever for the community. It ships with the SDK (19).

**Authoring path.**
- Near term: the Python builder in `blobby-test/blobs/` gets an exporter to this format (19 owns the tool). It
  already has every primitive.
- Later: an in-browser editor (open question Q16.3).

**Budgets** (proposed; the bear's 97 atoms set the tone):

| tier | atoms | example |
|---|---:|---|
| hero (the player, rare creatures) | ≤ 1,000 | lion with mane |
| typical animal | 100–400 | bear, deer, beaver |
| small animal, bird, fish | 20–100 | |
| swarm member (insects, small fish) | 1–10 | |

**Variation per individual** comes from parameters hashed from the entity key, not from copies of the atoms:
- a size scale (±10%);
- per-material colour shifts in a perceptual space;
- up to 3 morph targets (per-atom offsets, blended).

One asset can then yield a herd where no two deer look identical, at the memory cost of one deer.

### 4.5 Creature LOD

A creature's LOD is a **fixed pyramid inside the asset**, built offline by the merge of 06 (moment-matched, true
volumes, the M7 coverage cap for aggregate parts such as manes). Merges **never cross bones**, so every level still
animates with the same skeleton:

| level | content | e.g. a 300-atom deer |
|---|---|---|
| 0 | all atoms | 300 |
| 1 | per bone, groups merged at ~4× the median atom size | ~60 |
| 2 | per bone, 1–3 ellipsoids (= the physics proxy) | ~25 |
| 3 | the whole creature as 1–3 ellipsoids | 2 |
| dot | one splat | 1 |

- **Choosing a level** uses the renderer's projected-error rule (12), per individual per frame. It is a render-tier
  choice (R1), so it never touches the sim.
- **Switching** uses M7's continuous morph through parent indices. They come free from the offline merge.
- **Carves and paint at coarse levels.** Carves are dropped at level ≥ 1: an eye socket is sub-pixel by then, and
  the merge of the carved shape's moments already includes the hollow. Cell paints (giraffe patches) are dropped at
  ≥ 2, and their mean colour goes into the merged blobs' colour.
- **Fur, shag and fuzz** are material properties (05). Per finding 5 they carry the look at a distance more cheaply
  than atoms, so 12 should keep them at levels 0–1.

### 4.6 Populations and individuals

**The planet prototype's tree mechanism, generalised.** A species' plugin declares:
- a stored field `<plugin>:pop_<species>` (count, sum aggregation, persisted; named per 09 §4.8, so `fauna:pop_deer`,
  or `beaver:pop` for a one-species plugin), optionally with structure fields (`<plugin>:pop_<species>_juvenile`,
  biomass);
- a **population band** ℓ_P, where coarse dynamics run (logistic growth to a habitat capacity, predation coupling,
  migration flow). They are written as rates on restricted state and pushed down (D17);
- an **entity band** ℓ_E, where counts become individuals. Trees used 20 m cells. Proposed defaults (estimates, by
  home range):

| species | entity band (cell size) | candidates per cell | why |
|---|---:|---:|---|
| tree | 16–32 m | 8 | as `planet/` |
| beaver | 64 m (along rivers) | 4 | colonies of 2–8 animals over ~1 km of creek |
| deer | 128–256 m | 8 | herds |
| songbird | 64 m | 16 | dense, small |
| fish | river reach at 32–64 m | 16 | |

**Lift (split of an entity-band blob, owner = the species plugin).**
1. `n = floor(count + hash(cell_key, epoch))`, where `epoch = tick / T_turnover`. Trees use a fixed hash. Animals
   re-draw the fractional part once per turnover period (~a season) so the count's rounding is not frozen forever.
2. **Candidates.** The cell has `C` candidate slots. Each slot has a hashed rank, hashed traits (sex, age offset,
   variation parameters) and a hashed **home point**: habitat-weighted, sampled from the cell's surface by the
   plugin, e.g. near water for beavers.
3. **Dormant individuals first.** Touched individuals anchored in this cell take slots first (below). The remaining
   `n − dormant` slots are filled by rank among free slots, with incumbency.
4. **Positions and states** are lifted as a deterministic function of (slot key, tick):
   - Positions are a wander around the home point: a sum of two or three slow sinusoids, scaled by the species'
     home range.
   - The action is drawn from the species' time budget for that hour (asleep, foraging, travelling).

   So a herd seen at different times is in different places, and an observer arriving at a random moment sees the
   statistically right picture. This is the "lifted detail is in equilibrium with its parent" rule of `DESIGN.md`
   §2.8.

**While refined (leaves own state, D8).**
- Individuals move, eat, reproduce and die under their behaviour processes.
- When an individual crosses into another entity cell, the plugin pushes `−1` and `+1` deltas to `<plugin>:pop_<species>` in
  the two cells. The core updates its `anchor` from its body.
- A birth takes a free slot (best rank). A death frees one.
- So the count field is always exactly the number of live individuals plus the fractional part. Restricting it is
  exact.

**Restrict (merge of an entity-band blob).**
- Anonymous individuals are forgotten: their count is already in the field.
- Touched individuals become **dormant**: their row stays, anchored to the coarse ancestor that the region merged
  into, with `flags.dormant`.
- Nothing else is stored. Natural turnover is regenerated, as for trees.

**Dormant individuals.**
- A plugin may export `dormant_step(handles, n, dt)`, called at the population band's cadence: ageing, survival
  draws (keyed RNG, D5), staying within the home range.
- They still count in `<plugin>:pop_<species>`.
- If the coarse count falls below the number of dormant individuals in a cell (a drought), the lowest-ranked dormant
  individual dies.
- Every death of a `named` or `tamed` individual emits an event (17), so the journal can tell the player.

**Touched individuals.**
- *Touched* means the player fed, tamed, named, hurt or tagged it, or it is salient (the beaver that built the
  player's favourite dam).
- Touched individuals that are not `named` lose the flag after a decay time (proposed: one game year of no
  interaction). This bounds how much the saves grow (15).

**Herds and migration while unobserved.**
- A migration is a population process that moves density between population-band cells along a seasonal flow field.
  It is written as deltas, so it conserves the count.
- On refinement the herd appears where the density is. Its velocity is lifted from the flow field, so the animals are
  already walking the right way.
- Observed, the individuals' behaviour (flocking toward the herd's centroid and along the flow) keeps them coherent.

**Display-only individuals** (the render tier of 06's two-tier cut, R1). The sim tier reaches the entity band only
within a few hundred metres of an observer (06, 10; an observer is an avatar, never a camera: R9). A deer is one
pixel at ~1.5 km at 1080p (a 60° field of view gives a pixel ~1 mrad), and a herd reads at 2 km. So between the sim
tier's edge and ~2 km the renderer asks for **display individuals**:
- the same lift (§4.6 steps 1–4), evaluated read-only at the entity band;
- never simulated, never persisted, posed with the idle or walk cycle that their lifted action implies;
- created in render-tier splits and dropped freely.

When the sim tier arrives, the real lift produces the same individuals at the same positions at that tick, so there
is no pop. Beyond ~2 km, coarse population cells are drawn as nothing, or as a faint texture on the ground (12's
choice).

**The observer effect, measured.**
- Individual behaviour (predation, grazing, breeding) is nonlinear, so refined regions will drift from coarse ones,
  as trees did at 157 m.
- The rule from `DESIGN.md` §2.8 #4 applies: coarse rates must be the expectation of the individual-level rates under
  the lift prior.
- Each species plugin ships an observer test (E4). The test harness is 19's.

### 4.7 Behaviour

**Where it runs.** Behaviour runs as plugin **processes** (10) over entity tables, in batches: "all beavers in the sim
cut".
- **Decisions** run at 1–4 Hz. They are staggered by `hash(key) mod period`, so the load is flat and stays
  deterministic.
- **Locomotion** (root motion) runs at the tick rate for every simulated individual.
- **Posing** is not sim (§4.3).

**Perception** uses only core queries (07) and services, batched over individuals:
- neighbours by type within a radius;
- line-of-sight rays (`ray_cast` batch);
- surface queries;
- field reads (discharge, temperature, snow, fire);
- observers' positions (avatars are sim state, R9);
- species-specific services, e.g. `vegetation.trees_near` below.

All of it runs on the CPU (planning idea 3): no GPU result may feed a decision.

**Decisions: utility AI by default.**
- Each action is scored from needs (hunger, fatigue, safety, a building urge) and perceptions. The best score wins,
  with ties broken by key.
- It batches well (score every action for every beaver as columns) and is data-driven (curves in the manifest).
- Plugins may use **behaviour trees** for scripted sequences. The beaver's fell → take a piece → carry → place is one. The
  current node and timers are columns. A blackboard goes in `blob_state`.
- GOAP and learned policies are the plugin's business. The core only needs the state to live in rows.

**Interactions:**

| with | mechanism | example |
|---|---|---|
| another plugin's structured state | **service** call | `vegetation.fell(tree_key, dir)`, `hydrology.discharge_at(points)` |
| anyone who cares | **event** (17) | `tree_felled`, `animal_died`, `dam_breached` |
| shared physical quantities | **fields** (09), written as deltas | grazing writes `−biomass`; a wallowing animal writes `+mud` |
| the world's shape | blob creation and removal (05, 15), object creation (11) | logs, burrows, nests, dams |
| the player | the player is an entity whose position is sim state; verbs are commands (18) | flee, curiosity, feeding |

### 4.8 Plants

**Trees** are world blobs: they are static, there are tens of thousands per km², and they must merge into canopies
for the far field. They are generated by the vegetation plugin's split of an entity-band blob (06).
- **Trees are kind B counted content** (R7, 14's taxonomy; pending `questions.md` C21). Their lift turns the count
  into a choice of candidates (ranked, with incumbency, as in §4.6).
- **There is no geometric projection** of whole trees onto a canopy blob's moments.
- **Coarse canopy blobs** are restrictions (merges) of the trees, not a target the trees are projected onto.

- **A tree's blobs** are a deterministic function of (candidate key, species, `age`, `health`, local up and slope):
  a procedural blob tree (tubes for trunk and limbs, canopy masses, foliage aggregates).
- **Age of an anonymous tree** is derived, not stored: `age = (tick − birth_tick) / year`. `birth_tick` is hashed
  from the candidate key, consistent with the species' turnover rate, for trees present at generation. It is
  recorded (as in `planet/`'s `births` map) only for trees born while observed.
- **Growth is visible as stages.** Seedling, sapling, young, mature, old and snag are discrete shape stages. Within a
  stage, shapes scale continuously. A tree's blobs are regenerated when its age crosses a stage, or when its scale
  changes by more than ~5%. That is a few re-generations per tree per game year, cheap enough for time warp to show
  a forest growing.
- **Health and overrides** follow `planet/` exactly: damaged trees rank worse; stumps block their site for 30 years;
  planted trees rank better. A sick tree's material shifts, e.g. browner foliage (09: a computed colour field).
- **Felling.** Vegetation's `fell(tree_key, dir)` does three things:
  - It sets the stump override.
  - It removes the trunk-base blobs. 11's connectivity then finds the trunk and crown an island, which becomes an
    **object** (a body, a child of the planet), falls as a rigid body, and comes to rest as a **log**.
  - It emits `tree_felled`.

  The log is **kind D placed content** (R7) owned by vegetation. It has a `vegetation:log` entity row with `decay`
  and `moisture` columns. Its settle policy is `never` while beavers may want it (11), and it is persisted as a
  placed record (15).
- **Logs decay** by a vegetation process over years. Their blobs shrink and soften (`k` up, colour darker), then
  dissolve into soil, which is a field delta on the ground.

**Ground cover** (grass, flowers, moss, undergrowth) is not an entity. Coarse fields (cover, height, flowering,
colour) are turned into rasterised tufts by the renderer (12, `BLOBMAP2.md`), as render-tier detail only (R19). Blooming is a
seasonal change of a field, and becomes an event only when it is notable (17).

### 4.9 Scenario 3, end to end: the beaver dam

**The players.**
- **The `beaver` plugin.** It owns:
  - the species and its population field;
  - an entity kind `beaver:beaver`;
  - a structure kind `beaver:dam` with columns `site: key`, `integrity: f32`, `logs: u16` and `crest: f32`;
  - blob types `dam_log` and `dam_mud`;
  - a coarse field `beaver:dams` (expected dams per cell; sum, persisted).
- **`vegetation`.** It owns trees, logs, and the `fell`, `trees_near` and `take_log` services.
- **`hydrology`.** It owns the discharge and routing fields (`hydro:` namespace), lakes, the computed `hydro:blockage`
  (15), and the `hydro:permeability` field it reads.

**0. Unobserved, for years before the player arrives.**
- Beaver populations live on river cells with suitable discharge (a creek, not a river: a range per species
  parameter) and riparian trees. They grow logistically at the population band.
- A coarse **damming closure** runs at the same band:
  `d(dams)/dt = build_rate · pop · suitability − decay · dams · (1 − maintenance(pop))`.
- Hydrology's coarse routing reads `beaver:dams` as extra storage. It is a linear reservoir per dam with a ponded
  volume from the reach's slope and a crest height, so it lengthens τ.
- Vegetation's capacity reads the resulting wetness: wetland willows thrive, upland trees in the pond die.
- So, unobserved, the planet has beaver ponds wherever they make sense. This is the "structure must be lifted"
  lesson of `DESIGN.md` §2.5, applied to a biological structure.

**1. The player arrives; the region refines.**
- **Rivers first.** Hydrology's lift places river segments (14).
- **Dams.** The beaver plugin's lift places `round(dams)` dams at **candidate dam sites**: narrow, shallow points
  along the lifted segment path, ranked by a suitability score plus a hash, as tree sites are. These *natural* dams
  are kind B counted content (R7).
- **Dam geometry** is generated from the site key and an age: logs and mud blobs across the channel. Natural dams
  are regenerable and not persisted. A dam beavers build or change while watched becomes kind D placed content
  (steps 4–5): its site gets an override, and its placed records replace the generated geometry, as a felled tree's
  stump and log replace a candidate.
- **Beavers** are lifted from the population (§4.6), with home points at their lodge near a dam site.

**2. Perception and decision.**
- A beaver's utility scores rise for `build` when its dam's integrity is low, or when no dam holds its candidate
  site.
- Perception calls `vegetation.trees_near(home, 40 m, species ∈ {aspen, willow, alder}, max = 8)`. It returns tree
  keys, positions and trunk diameters, nearest first. The beaver picks the nearest suitable one.

**3. Felling.**
- The beaver walks there (steering + coarse A*, §4.3) and gnaws.
- The gnawing is a timer column, plus repeated `vegetation.damage(tree, amount)` calls. Each one lowers `health`,
  and with it the tree's rank, so if the coarse forest thins meanwhile, this tree is first to go.
- At zero, `vegetation.fell(tree, dir_away_from_beaver)` sets the stump, lets the trunk fall as an object (11), and
  emits `tree_felled`. The resting trunk is vegetation's log (§4.8).

**4. Taking a piece and carrying it.** There is no ownership transfer (R11): a plugin owns only the types it defines.
- **The beaver gnaws a piece off.** It calls `vegetation.take_log(log, length = 1.2 m)`. Vegetation shortens its
  log, or removes it (`ss_unplace`, 15), and returns the piece's shape.
- **The beaver makes its own copy.** It creates a `dam_log` object of that shape (`ss_object_from_blobs`, 11). It
  defines the type, so it owns it, and from now on the piece's decay and its place in the dam are the beaver
  plugin's concern.
- **Carrying** is `ss_object_hold(piece, beaver_body, mouth_bone, pose)` (11): the piece's blobs follow the `mouth`
  socket of the beaver's pose. (§4.3: the pose is computed sim-side here because a socket matters.) The piece's
  body-tree parent stays the planet: holding is a physics constraint, not a gravity relation (04).
- **Swimming.** The beaver swims with the piece. Buoyancy and drag come from the water parcels and fields (11).

**5. Placing.**
- At the site, the beaver releases the piece (`ss_object_release`, 11). It lodges against the others, sleeps, and
  settles under 11's `keep_instance` policy, so the dam keeps its log shapes. Settling turns it into a **kind D placed
  record** (R7, 15).
- The plugin places `dam_mud` around the logs as kind D content (`ss_place`, 15, with a `join_ground` look), and
  increments the dam row's `logs` and `crest`. Placing content touches its region (15).
- Two cases differ:
  - If the site held a *natural* (counted) dam, the site gets an override and the placed records replace the
    generated geometry.
  - If it was a free candidate site, the row and its placed records are new state.

  Both persist.

**6. Hydrology reroutes.**
- In refined cells, hydrology's computed `hydro:blockage` (the top of the placed solids in each cell, 15) raises the
  effective ground for routing. The ground's `z` is unchanged.
- The priority flood finds a depression, and a pond fills upstream. This is a bathtub fill; `planet/`'s dam scenario
  did it with a 14 m wall, dropping discharge from 6.9 to 0.14 m³/s 5 km downstream.
- **Leaks.** Beaver dams leak. Hydrology reads a computed field `hydro:permeability`, and the beaver plugin provides
  it for `dam_log` and `dam_mud` by owner dispatch (09). That is the same pattern as canonical scenario 6. A leaky
  dam passes `Q_leak = permeability · head · width`, so the creek below keeps a trickle.
- **Upward.** While the region is loaded, each touched coarse cell records its new outlet and storage, as `planet/`
  restricted routing (`outlets`). So the pond persists in the coarse cell after unloading.

**7. Consequences** (all emergent, none scripted by the beaver plugin):
- vegetation sees waterlogged ground and kills upland trees in the pond;
- wetland capacity rises;
- fish populations (if a fish plugin is installed) gain habitat;
- the journal records the first time the player saw a beaver dam being built (17).

**8. The player leaves for a year** (scenario 5's pattern).
- **Anonymous beavers** are restricted into the count.
- **The dam stays as an entity row.** It is a touched structure: its row and its placed records persist (15),
  anchored to the coarse cell, and it counts in `beaver:dams`.
- **The dam keeps changing while dormant.** The beaver plugin's `dormant_step` updates its `integrity`:
  - maintained (`integrity → 1`) while `beaver:pop` in the cell supports a colony;
  - otherwise it decays, with τ of a few years;
  - below a threshold it breaches: a `dam_breached` event, and hydrology releases the pond. If someone is near, the
    release is a surge in the fast layer (`PLUGINS.md` §4b).

**9. The player returns.**
- The region refines. Rivers and natural dams regenerate.
- The built dam's placed records are realised (15). Lost integrity has already removed logs (`ss_unplace`) in rank
  order (key hash), in proportion to the loss, so the same logs go first. 15 calls this step `catch_up` when it runs
  on return instead of during `dormant_step`.
- If the colony survived, beavers are lifted at its lodge and resume maintenance.

**Consistency checks this scenario must pass** (E5):
- dam count and pond storage per coarse cell agree, watched against unwatched, after N years;
- `beaver:pop` is exactly conserved through refine and coarsen;
- a dam built while watched appears where an unwatched dam *could* have been (same candidate site list).

### 4.10 Other scenarios

- **1. Orbit to grass.** Herds are display individuals between the sim tier and ~2 km. During the zoom animation
  (planning idea 6), the pending observer at the embark point (R9) brings the entity-band cells into the sim tier
  early, so creatures are simulated by the time the camera lands. Display individuals guarantee there is no pop at the handover.
- **4. The asteroid.** The impact event calls services:
  - `vegetation.kill_trees(region, fraction)` and `vegetation.fell(radial)` for the blast;
  - each species' `kill(region, fraction)`, which pushes negative deltas at the population band and kills
    individuals in the refined part, worst rank first.

  Animals in the refined region get a `threat` perception from the event (17), and their utility for `flee` spikes.
  Panic stampedes are emergent.
- **5. Dig, leave, return.** The felled tree is a stump override (30 years), plus a log row that decays while the
  player is away. The damaged tree is an override with a worse rank. Both behave as in `planet/`'s 15-year test.
- **6. A community plugin (a hut).** A hut is a static structure kind with world blobs. It declares
  `flammability` for its blob types by owner dispatch (09). If it has animals living in it, they anchor to it through
  their `home: key` column. Nothing in this doc needs to change for it.

### 4.11 What entities cost

**Simulation** (estimates, to be measured in E3–E5):
- The entity band is refined within a few hundred metres of an observer (06, 10), so ~0.5–1 km² is simulated around
  each observer.
- Plausible densities in that area: deer 10–50/km², a beaver colony or two per km of creek, songbirds ~100s/km², fish
  100s per km of river.
- That is 50–500 simulated individuals per observer.
- At 2 Hz decisions × 500 individuals × ~10 µs per decision ≈ 10 ms per real second, about 0.3 ms per frame.
  Locomotion at 30 Hz × 500 × ~2 µs is about the same.
- This fits comfortably. The risk is perception queries, which must be batched (07).

**Rendering load** that 12 must handle (the requirement; 12 owns the technique):

| class | count on screen (estimate) | LOD level | atoms | per-frame update |
|---|---:|---|---:|---|
| near (< 20 m) | ~20 | 0 | up to 1,000 each, ~10k total | bone transforms (~20 × 30 bones); every frame |
| mid (20–200 m) | ~200 | 1–2 | ~30–60 each, ~10k total | bone transforms |
| far (200 m–2 km) | ~2,000 | 3–dot | 1–3 each | root transform |

- **Updates are transforms, not blobs.** Atoms are static per asset, so 12 can keep them on the GPU and apply bone
  transforms there.
- **Exact evaluation of near creatures is the open cost.** 123–376 ms per creature filling the screen today. The
  candidates are per-bone baked bricks (re-posed by rigid transforms; the README's "Claybook / Dreams" suggestion) or
  M5 v2's rasterised local blends (it worked for grass because each group was 3–5 blobs; a bone with 15–90 atoms is
  at its edge).
- **Contacts and shadows.** Creatures meet the ground with a hard min (different instances), and they need shadows
  on the ground. A small per-frame sun-space grid would do, as for grass (`BLOBMAP2.md`).

**Memory.** A 400-atom asset with its LOD is ~400 × 32 B × 1.5 ≈ 20 KB, shared by every individual. An individual is
an entity row (~64–128 B plus columns) and a body. So 10,000 individuals cost ~2 MB.

## 5. Interfaces

Pseudocode in the C-ABI style of 08. Handles are `u32`; `0` means none (D6). Batch arrays are pointer plus count.

**Manifest additions** (08 owns the format):
```json
"entity_kinds": {
  "beaver:beaver": {
    "band": 6, "body": true, "model": "assets/beaver.blobm",
    "columns": { "hunger": "f32", "fatigue": "f32", "action": "u8", "target": "key",
                 "home": "key", "gait_phase": "f32", "carrying": "body" },
    "blob_state": 256,
    "touched_decay_years": 1.0
  },
  "beaver:dam": { "band": 6, "body": false,
                  "columns": { "site": "key", "integrity": "f32", "logs": "u16", "crest": "f32" } }
},
"populations": { "beaver": { "field": "beaver:pop", "pop_band": 12, "entity_band": 6, "candidates": 4 } }
```

**Core → plugin calls** (imports the plugin can use; the `ss_` names follow R2, and 08 §5.2 holds the canonical
list):
```
// entity tables (this doc)
ss_ent_create(kind: u16, n: u32, keys_in: *const Key128, out_handles: *mut u32)  // keys from the plugin's lift
ss_ent_destroy(handles: *const u32, n: u32)
ss_ent_read(kind: u16, col: u16, handles: *const u32, n: u32, dst: *mut u8)      // gather a column
ss_ent_write(kind: u16, col: u16, handles: *const u32, n: u32, src: *const u8)   // owner only
ss_ent_set_flags(handles, n, set: u32, clear: u32)                                    // touched, named, ...
ss_ent_in_cells(kind: u16, cells: *const u32, n: u32, out: *mut u32, cap: u32) -> u32
ss_ent_by_key(keys: *const Key128, n: u32, out_handles: *mut u32)                // 0 if not loaded

// model instances (this doc; 04 and 11 own bodies)
// models are declared in the manifest's `models` entry (08: no runtime registration); ids come from ss_id
ss_body_create_model(model_id: u16, n: u32, roots: *const Xform64, out_bodies: *mut u32)

// owned by other docs, used here
ss_character_create / ss_character_intent       // 11: controller with species limits (root motion)
ss_object_from_blobs, ss_object_from_placed   // 11: a new dam_log piece; picking up a settled thing
ss_object_hold, ss_object_release               // 11: carrying by a socket bone
ss_object_settle_now + settle policies          // 11: keep_instance, join_ground, never
ss_place, ss_unplace                            // 15: kind D placed content (dam logs, mud, carcasses)
ss_touch                                        // 15
ss_surface_along, ss_neighbors, ss_raycast, ss_graph_rows   // 07: perception, foot targets, A*
```
There is no `transfer_owner` (R11). Materialising a carcass is the creature plugin reading the pose and calling
`ss_place` for its own carcass type (§4.2).

**Plugin → core exports** (what a species plugin implements):
```
lift_individuals(cells: *const u32, n: u32)        // on split of entity-band blobs: create rows (dormant first)
restrict_individuals(cells: *const u32, n: u32)    // on merge: drop anonymous rows, mark touched ones dormant
dormant_step(handles: *const u32, n: u32, dt: f64) // optional, at the population band's cadence
display_individuals(cells: *const u32, n: u32, out: *mut DisplayInd, cap: u32) -> u32
                                                   // read-only lift for the renderer (no rows, no state)
pose(bodies: *const u32, n: u32, alpha: f32, out_bones: *mut XformF32)
                                                   // deterministic; called by renderer and physics
describe(handles: *const u32, n: u32, out_name_ids: *mut u32)   // for picking and the journal
// processes (10): e.g. "beaver:decide" at 2 Hz on band 6, "beaver:locomote" every tick
```

**Services named in this doc** (vegetation): `trees_near`, `damage`, `fell`, `take_log`, `kill_trees`, `plant`.
All are batch calls keyed by tree keys or positions.

**Requirements on other docs:**
- **05:** model assets (static atoms with bone indices and sub-instances) alongside world blobs; carves scoped to a
  model's sub-instance.
- **06:** entity bands; `lift_individuals` and `restrict_individuals` called on split and merge of entity-band blobs;
  render-tier splits able to call `display_individuals`.
- **07:** tagged hits (world blob or body + bone + atom); bodies indexed through proxies; the adjacency graph at a
  chosen band for A*; a batched surface query along a direction.
- **08:** entity kinds and populations in the manifest; core-stored tables (idea 4).
- **11:** objects from removed blobs (felling) and from new blobs (`dam_log` pieces), holding by socket bones,
  settle policies producing kind D placed records, a character controller with species limits, buoyancy.
- **12:** model instances with per-frame bone transforms, per-individual variation, the LOD morph, creature shadows.
- **15:** dormant rows and touched structures persisted with their anchor; touched-flag decay.
- **17:** `animal_died`, `tree_felled` and `dam_breached` events; threat perception from events.

## 6. Risks and unknowns

- **Rigid parts at large bends.** Necks, shoulders and tails may pinch or gap. Two-bone atoms (§4.3) are the cheap fix.
  If they fail, per-bone baked bricks would have to blend across joints, which is harder.
- **Order-independent blending may break faces.** Sub-instances and the carve-layer fallback (§4.4) are the plan.
  E1 decides.
- **The two-residence model** (world blobs and model instances) adds paths through 05, 07, 11 and 12. The alternative,
  every creature atom in the world store, costs ~100k re-posed blobs per tick plus structure churn. The duality is
  the cheaper risk.
- **Pathfinding without a navmesh** may leave animals stuck in concave terrain. Mitigation: a stuck detector, plus
  re-planning at a coarser band.
- **The observer effect for animals** is unmeasured. Predator–prey dynamics are strongly nonlinear, so closures may
  need calibration as trees did (31% → 15.5% p90 with an unbiased closure).
- **Touched-entity growth.** A player who interacts with everything could fill saves. The decay of the touched flag
  and a per-region cap (oldest unnamed rows decay first) bound it.
- **Near creature rendering** is unsolved (12). Until it is, the near budget may have to be ~5 creatures, not 20.
- **Plugin-authored AI quality varies.** Bad AI is a content problem, but the core must protect frame time:
  per-plugin budgets (08, 10).

## 7. First experiments

| # | experiment | success criterion |
|---|---|---|
| E1 | Re-author the bear and the human with order-independent blending (exp smooth-min per sub-instance, carves after adds, eyes and teeth as sub-instances). Render in the prototype harness, compare with the iteration-10 images. | ≤ 5% of pixels differ by more than 16/255 in the close-up views; Joel judges the faces "the same character" |
| E2 | Two-bone atoms at the bear's shoulders and hips and along the giraffe's neck; sweep joint angles to ±60°. | no visible gap or pinch at 60° in a close-up; cost ≤ +20% atom transforms |
| E3 | Procedural walk with foot IK in Node + headless renders, on `planet/` terrain: a bear crossing 0–35° slopes and a 1 m step. | stance feet within 2 cm of the surface; foot slide < 5 cm per step; no IK pops |
| E4 | Population ↔ individual round trip: a deer population; refine and coarsen a region 100 times at random ticks; tame one deer. | the count field is exact at every restrict; the same anonymous deer for the same count and epoch; the tamed deer persists and ages |
| E5 | The beaver observer test (headless, 10 game years, with and without an observer on the creek). | per coarse cell, dam count within ±1 and pond storage median difference ≤ 10% (an initial target; tighten after calibration) |
| E6 | The creature LOD pyramid for bear, lion and giraffe (merged per bone, 4 levels); render with switching by projected size. | the 16/255 pixel difference to level 0 stays below 5% at each level's switch distance |

## 8. Build order

| milestone | entities and creatures work |
|---|---|
| M0–M2 | none (the asset format can be drafted alongside 05's blob layout) |
| M3 | entity tables and anchored rows in the core API (with fields, 09); the vegetation plugin with tree candidates as world blobs; `trees_near` |
| M4 | the player as the first model instance: skeleton, `pose`, procedural walk on the planet (E2, E3); the creature exporter from the Python builder (with 19); E1 |
| M5 | tree growth stages, felling and logs (with 11); one animal species (deer) with populations ↔ individuals, utility AI, display individuals (E4); persistence of touched rows (with 15) |
| M6 | threat and flee responses to events; `animal_died` and `dam_breached` events |
| after M6 | the beaver plugin as the integration test for scenario 3 (E5); more species; creature LOD at scale (E6); later RL locomotion, taming |

## 9. Open questions

**Q16.1 (merged into `questions.md` C10): Should the core have thin entity tables (§4.1), or no entity concept at
all?**
It decides whether persistence, anchoring and future replication are solved once or in every plugin.
*Default:* thin tables (option C).

**Q16.2 (merged into `questions.md` C3): What can the player do with creatures?** Watch only? Follow, feed, tame,
name, ride, hunt? This decides how many touched individuals exist, whether creatures need to react to the player at
all, and whether death is part of the player's verbs.
*Default:* watch, follow, feed and name. No hunting at first.

**Q16.3: Creature authoring.** Keep the Python builder as the tool and add an exporter, or start an in-browser editor
early (so community creators don't need Python)?
*Default:* the exporter in M4, an editor later (19).

**Q16.4 (merged into `questions.md` C18): Which creatures?** Plausible stylised animals (beavers, deer, bears) or
invented blob creatures? Both are cheap to support; it is a theme decision.
*Default:* mostly plausible animals, stylised, with a few invented ones.

**Q16.5 (merged into `questions.md` C18): Same species everywhere, or per-planet ecologies?** Authored species per
planet, or procedural variation (colour, size, proportions), or procedurally generated species? The last is a large
project.
*Default:* authored species from plugins, procedural variation per planet; generated species later.

**Q16.6: Do named animals age and die while the player is away?** It is more alive, but it can feel like a loss.
*Default:* yes, through `dormant_step`, and the journal tells the player what happened.

**Q16.7 (merged into `questions.md` C10): Is the two-residence split OK?** World blobs for terrain, plants and
structures; model instances for creatures and the player's appearance (R10). It shapes 05, 07, 11 and 12.
*Default:* yes.

**Q16.8: How are death and injury shown?** It is a tone question with implementation weight (materialising
carcasses, decay).
*Default:* stylised. A dead creature goes still, then slowly sinks into soft ground blobs over a game week. No gore.
