# 07. Spatial queries

Status: draft · Depends on: 02, 04, 05, 06 · Used by: 08, 09, 10, 11, 12, 14, 16, 17

## 1. Purpose and scope

Everything that asks "what is near here, what does this ray hit, where is the surface" goes through this doc.
- Plugins ask: hydrology for its graph, beavers for trees, vegetation for room to grow.
- Physics asks: contacts, the character controller, detachment.
- Events and the engine ask: impact points, picking on the CPU, placement.

**In scope:**
- the query catalogue and its exact semantics;
- the batch ABI (numpy-style, D6);
- the CPU acceleration structures that answer sim queries;
- determinism, tiers, frames, level clamping, work budgets;
- performance targets.

**Out of scope:**
- the GPU structures that draw the world (bricks, lists, splats, feedback): [12](12-rendering.md). This doc only says
  what GPU queries may be used for (§4.9).
- Body-level broad phase across the body tree: [04](04-bodies-and-orbits.md) supplies it; this doc uses it.
- What physics does with contacts: [11](11-physics.md).

## 2. Requirements

**Must:**
- Batch, index-based calls on `u32` handles (D6): `neighbors(dst, src, size, k, radius)` from the original sketch,
  with rows zero-filled beyond the neighbours that exist.
- Fast ray queries from any point, not only from the camera (README constraint 4), for gameplay, AI, physics and
  placement.
- Deterministic results: the same state gives bit-identical outputs on every machine, whatever the thread count,
  handle assignment or render tier (D5). So outputs are sorted canonically, and the sim never sees GPU results.
- Answers come from the current cut, with no synchronous refinement. A query reports when the cut was too coarse for
  the caller's need, and the caller can register a refinement request (06).
- Isotropy: no query may carry a grid's directional bias into results (README constraint 1).

**Should:**
- Work across levels and across bodies (a rock on a planet, a moon seen from a mountain).
- Clamp to a coarser view on request ("feel the terrain at my own scale": `PLUGINS.md` §1.3).
- Per-plugin deterministic work budgets, so a runaway query can't stall a tick unpredictably.

**Later:**
- GPU-backed batch queries for cosmetic plugin features (AI vision effects, audio occlusion).
- Rotating sweeps and continuous collision for fast spinning objects; 11 decides whether they are needed. Translational
  sphere and capsule sweeps are in v1 (`ss_sweep`).

## 3. What the prototypes taught

| finding | source | consequence |
|---|---|---|
| "The same index serves physics": the waterfall found neighbours through the dynamic grid used for rendering, and collided with the baked bricks; the one bug was colliding with last frame's water | `world/README.md` #8 | one source of truth per tick; queries read the state of the current tick only |
| Physics could query the same structures: exact lists for contact, samples for broad phase | `BLOBMAP.md` next steps | the CPU structures here mirror what the renderer uses, at sim resolution |
| A "nearest blob" term over whatever blobs are in a list (including zero-weight ones) printed list-shaped seams into AO | `BLOBCUT.md` fix 1 | query results may depend only on weights, never on which candidate list was scanned |
| GPU lists filled by atomics came out in a different order each build; float sums differed | `M7.md` §7 | candidates are summed in key order (05 §4.9) |
| Rays walk the tree up and down to find their leaf; exit points pushed across faces; whole subtrees skipped when their content top is below the ray; over-relaxation (ω = 1.5) cut steps 58 → 46 | `RAYCUT.md` §3 | the CPU ray traversal (§4.6) |
| Exp smooth-min isn't a distance field: steps must be divided by `K/k_min`, overshoots bisected | `world/README.md` #6 | every field march here uses the Lipschitz bound |
| `surfaceAt(dir, band)`: a 10 m parcel feels 10 m slopes inside a 10 km cell | `planet/PLUGINS.md` §1.3 | level clamping is a parameter of every field query (§4.5) |
| CPU and GPU surfaces differed by metres in coarse cells: parcels floated or sank, and the camera needed 3 m clearance | `planet/PLUGINS.md` §7 | sim queries define the truth; the renderer must stay within `err` of it (06, 12) |
| A fire-spread bug read a graph-node index as if it were a cell index | `planet/DESIGN.md` §4.9 | APIs never mix index spaces: graph rows contain handles, never graph-local indices |
| 15,000 hash probes per parcel: the resting store's cell size was set by 2 m bombs | `planet/PLUGINS.md` §2 | grids are per size class, so small blobs never set the cell size for large ones |
| Readback is async (`mapAsync`, ~2 ms for 4 MB with JSPI) | `wasmgpu/README.md` | GPU query results arrive frames later; fine for cosmetics, never for the sim |

## 4. Design

### 4.1 Principles

1. **The CPU answers sim queries.** GPU float results are not bit-identical across vendors and drivers. So nothing a
   GPU computes may feed sim state (idea 3, adopted). The sim's queries run in the core on the CPU, with the
   deterministic libm of 02.
2. **Queries see one tier.** Sim contexts (plugin processes, physics, events) see the **sim tier only** (06 §4.8).
   Render-only blobs are invisible to them, so where the camera looks can't change a sim result. Engine-side cosmetic
   code may ask for the render tier explicitly.
3. **Queries answer from the current cut.** They never split. Every row says which level answered it. If the caller
   gave a needed resolution, the row also says whether the answer was too coarse; the caller then calls
   `ss_refine_request` (06 §5), which takes effect at `T + L_split` (3 ticks).
4. **The field is the union of the tier's leaves.**
   - At a point, each instance's field sums every leaf blob of that instance whose reach covers the point, whatever
     its level.
   - At a 2:1 boundary a coarse leaf and its neighbour's finer leaves overlap and blend. That is continuous, with a
     bulge of at most the usual `k ln N` mechanism.
   - Interior (split) blobs are *not* part of the field; their representatives serve clamping (§4.5) and the
     renderer's morph.
   - The ray cut's alternative, "each region's own level only", makes the field step at region faces. Physics would
     feel those steps.
5. **Canonical outputs.** Rows are independent. Within a row, results are sorted by (distance, key); ties are broken
   by key, never by handle. Float accumulations run in key order. So outputs don't depend on handle assignment
   (which differs between machines: 05 §4.8), thread count or arrival order.
6. **Frames.** Every query names a **query frame** (a body handle; positions in f64). Cross-body queries use the body
   tree's broad phase (04) to find the bodies involved. Each is queried in its own frame, with the ray or shape
   transformed in f64, and the results transformed back. This is the "nearest common ancestor" rule of 04 in
   practice: the transform composition runs through the NCA.
7. **Two residences, tagged hits** (R10, 05 §4.16, 16 §4.2; pending `questions.md` C10). Model instances (creatures,
   the avatar's appearance) are not world blobs.
   - Queries reach them through their **proxies**: one or two posed ellipsoids per bone, in a per-body structure
     (§4.4).
   - Every result row is **tagged**: either a world blob (`handle`), or a model hit (`body`, `bone`, `atom`). The
     atom index is filled only when the caller asks for exact atoms (`EXACT_MODELS`), which evaluates the asset's
     atoms under the pose.
   - Sorting keys for model hits are the entity key, then (bone, atom).

### 4.2 The catalogue

These are the canonical plugin-facing names (R2; 08 §5 lists the signatures, this doc owns the semantics).

| query | answers | typical callers |
|---|---|---|
| `ss_neighbors` | for each source blob, up to k leaf blobs within a radius, by centre distance or surface gap, with filters | plugins' ad hoc needs (beaver finds trees, plants find room), entity perception |
| `ss_neighbors_at` | the same from arbitrary points instead of source blobs | placement, events |
| `ss_graph_rows` | the cut graph: for each source leaf, its adjacent leaves at a band, with interface weights | processes doing mass balance or fronts (hydrology, fire, heat: `DESIGN.md` §2.4) |
| `ss_raycast` | first hit (or up to m hits) along rays from any origin, in field, hard-union or conservative mode | AI vision, projectile prediction, CPU picking, placement |
| `ss_sweep` | first contact of a moving **sphere or capsule** along a segment | the camera's spring arm, standing spots, the character controller's sweeps (11, 18) |
| `ss_visibility` | batched line-of-sight scores between observers and target points (with target heights): horizon plus a coarse ray | the event director, witnessing, AI awareness (17, 16) |
| `ss_field_eval` | the field value, gradient, **a Lipschitz bound**, the winning instance and the heaviest blob at points | physics contact depth (`d = −f / L`, 11), buoyancy, "am I inside rock?" |
| `ss_surface_project` | the nearest surface point, normal and signed distance from points | the character controller, snapping, resting parcels |
| `ss_surface_along` | the topmost surface along a direction (radial by default) within a range, at a clamped level | `planet/`'s `surfaceAt`: grounding parcels, placing trees, the player's spawn |
| `ss_overlap` | leaf blobs whose reach (or bound) intersects a sphere, ellipsoid, box or capsule | explosions, area effects, broad phase |
| `ss_overlap_pairs` | all pairs among a set of blobs whose grown ellipsoids overlap | 11's contact graph for connectivity |
| `ss_components` | connected components of a set of leaves, with anchoring | detachment (11) |
| `ss_cut_level_at` | the cut's level and leaf at points | 06 (listed there), LOD-aware code |

### 4.3 Semantics in detail

**`ss_neighbors`** (and `ss_neighbors_at`, from points).
- *Distance modes:*
  - `CENTER`: `|cᵢ − cⱼ|`.
  - `GAP`: an estimate of the surface-to-surface distance between the two representatives:
    `|cᵢ − cⱼ| − ρᵢ(u) − ρⱼ(−u)`, where `u` is the unit direction between centres and `ρ(u) = 1 / |diag(1/r) Rᵀ u|`
    is the ellipsoid's radial extent along `u`.
    - It is cheap, deterministic and symmetric.
    - It is exact for spheres and conservative enough for overlapping ellipsoids.
    - "Touching" means `GAP ≤ max(kᵢ, kⱼ) · ln 2`, where blending would join them.
- *Filters* (any combination): same instance or not; a set of owners; a set of types; a level range; kinds; exclude
  the source's own instance.
- *Candidates:* sim-tier leaves; plus atoms of any level; plus parcels, small rigid bodies and model-instance proxies
  if the filter allows (they live in separate indexes, §4.4). Model hits are tagged (§4.1, principle 7).
- *Output:* `dst[n × k]` handles, zero-padded, sorted by (distance, key). Optional `dist[n × k]` (f32) and `count[n]`.
  - A row with more than k results returns the k nearest and sets `TRUNCATED` in the row status.
  - The radius cap is mandatory. There is no unbounded k-NN, because an unbounded k-NN on a planet is unbounded work.

**`ss_graph_rows`** (the cut graph, `planet/`'s neighbour graph in blob form).
- *Nodes* are sim-tier leaves **clamped at the band β**: a leaf finer than β is represented by its ancestor at level β
  (06 §4.14). A process with band β therefore sees one node per β-blob near observers, and coarser nodes elsewhere.
  This is `planet/`'s "its graph is the cut clamped at that depth".
- *Edges* join nodes of the same instance (or the same owner, by flag) whose representatives touch (`GAP` mode).
- *Interface weight* is a fixed deterministic formula: the chord where the two footprints' power-diagram bisector
  cuts them.
  - With footprint radii `ρᵢ, ρⱼ` along the joining direction and centre distance `d`, `x = (d² + ρᵢ² − ρⱼ²)/(2d)`.
  - The interface length is `2√(max(0, ρᵢ² − x²))` for sheets (measured in the sheet plane).
  - The interface area is `π (ρᵢ² − x²)` for volumes.
  - It plays the role of `planet/`'s "side neighbours carry the shared edge length".
- *Output:* CSR for the requested source rows: `offsets[n + 1]`, `cols[]` (handles), `weights[]` (f32), sorted by key
  within a row.
- *Caching:* the core caches the graph per (body, β, edge filter). It is invalidated per region on cut changes and
  rebuilt incrementally. Processes pay only for the rows they ask for.

**`ss_raycast`.** Each ray has: origin (f64 × 3, query frame), direction (f32 × 3, unit), `t_max` (f64), cone (f32
radians: the footprint grows as `cone · t`; 0 means exact), and a needed resolution (`min_feature`, metres; 0 means
don't care).
- *Modes:*
  - `FIELD`: the blended surface (the truth: the zero set of §4.1's field), found by sphere tracing with the Lipschitz
    bound and over-relaxation (`RAYCUT.md`'s ω = 1.5), and a bisection to a root tolerance of `max(ε₀, cone · t)`.
  - `HARD`: the first hit against the hard union of representatives, each grown by a per-kind growth (`M7.md`'s 0.6 k
    for sheets and 0.7 k for objects). Analytic: no march. It is at most a few `k` inside the true surface. Good for
    line of sight and AI.
  - `CONSERVATIVE`: the first hit against **bounds** (06 §4.5): "could anything be here, at any finer level". A miss
    proves the ray is clear, whatever the cut. A hit only says "maybe". Bounds are ellipsoids, or **radial slabs**
    for the ground (§4.4).
- *Multiple hits:* up to m per ray, sorted by t. The `FIELD` mode reports each entry into the solid.
- *Filters:* as for neighbours, plus `ignore_instance` (don't hit yourself).
- *Output per ray:* `t` (f64), a **tagged target** (a world blob handle, or body + bone + atom for a model instance),
  instance, level, normal (f32 × 3), and flags:
  - `HIT`, `MAXED` (`t_max` reached);
  - `TOO_COARSE`: the answering leaf's `err` exceeds `max(min_feature, cone · t)`;
  - `BUDGET`: the work cap was reached (the row is zeroed).

**`ss_sweep`** (sphere and capsule casts; 18's spring arm and standing spots, 11's controller sweeps).
- Each row: a shape (`SPHERE` radius r, or `CAPSULE` radius r and a segment in the shape's frame), a start pose
  (f64 × 3 position, plus a quaternion for capsules), a direction and a length.
- Returns the first `t` at which the shape touches the surface, the contact point and normal, the tagged target, and
  the level.
- `FIELD` mode marches the field offset by the shape: `F(p) − r` for a sphere, the segment's closest-point field for
  a capsule. It uses the Lipschitz bound, as rays do. `HARD` mode tests the grown representatives analytically
  (sphere–ellipsoid; capsule–ellipsoid by a fixed-iteration closest-point solve).
- Deterministic in the same sense as `ss_raycast`. Rotation during the sweep is not supported: sweeps are
  translations.

**`ss_visibility`** (17's director and witnessing, 16's AI awareness).
- Each row: an observer point (f64 × 3, plus eye height) and a target point (plus a target height: a plume's top, a
  creature's head).
- Returns a score in [0, 1] and flags:
  - `BEYOND_HORIZON`: the geometric horizon of the body, `d > √(2R·h_obs) + √(2R·h_target)`;
  - `OCCLUDED`: a `HARD` ray, clamped at a coarse level (by default the level whose blobs are ~1% of the distance),
    hits something before the target;
  - `PARTIAL`: the target's top clears the occluder, its base doesn't. The score is the fraction of the target height
    that is visible, sampled at 3 heights.
- Coarse by design: a few `HARD` rays at a clamped level, never the full field. Weather and light are not part of
  it; 17 multiplies them in.

**`ss_field_eval`.**
- For each point: `F` (f32), `∇F` (f32 × 3, unnormalised), **`L` (f32), a Lipschitz bound** of `F` near the point (the
  accumulator's `K/k_min`, at least 1; `wgsl_lib.js` `lipschitz()`), the winning instance, and the handle of the
  heaviest blob of the winning instance. The heaviest blob is the one with the largest weight; ties go by key.
- `L` is what converts a field value into a distance bound: `|F| / L` is no more than the true distance to the
  surface. 11 uses `d = −F / L` as the contact depth.
- A level clamp and a body filter apply as everywhere ("against this body's static set at this scale", 11).
- An optional single-instance mode evaluates `F_I` for a named instance only, for example "how deep is this point
  inside the beaver?".

**`ss_surface_project`.**
- From each point, Newton steps `p ← p − F ∇F / |∇F|²` (at most 8, with backtracking), from the field of the clamped
  cut.
- Returns the surface point (f64 × 3), the normal, the signed distance from the start, and the handle.
- Fails with `NO_SURFACE` when no blob reaches within a cap.

**`ss_surface_along`.**
- For each (direction from the body centre, or a point plus an "up" vector): the first `FIELD` hit coming *down* from
  `r_max` to `r_min` along the line.
- Returns the height (f64, distance from the body centre or along "up"), the normal, the handle and the level.
- With a level clamp, it is `planet/`'s `surfaceAt(dir, band)`.
- With overhangs, "topmost" is the semantics. Callers wanting the surface nearest a point use `ss_surface_project`.

**`ss_overlap`.**
- *Shapes:* sphere, ellipsoid, oriented box, capsule.
- *Tests:*
  - `REACH`: the blob's reach (`3.5 k` beyond its ellipsoid) intersects the shape. This is for field contributions.
  - `SURFACE`: the shape intersects the blob's ellipsoid grown by `k ln 2`. This is for "touching".
  - `BOUND`: conservative, against bounds.
- Ellipsoid–ellipsoid tests use a fixed-iteration separating test, with a bounding-sphere early-out.
- *Output:* CSR.

**`ss_overlap_pairs`** (11's connectivity graph).
- Input: a set of blobs (handles) and a growth: each blob's ellipsoid grown by `min(kᵢ, kⱼ) + margin` (11 uses 1 cm).
- Returns every unordered pair (i, j) among the set, *plus* their sim-tier neighbours if requested, whose grown
  ellipsoids overlap, regardless of instance (support is physical: a boulder resting on the ground is connected to
  it). Mixed levels are allowed.
- Output: pairs as two handle arrays, sorted by (key of i, key of j) with i < j by key. Capped, with `TRUNCATED`.

**`ss_components`** (detachment; R6, pending `questions.md` C13).
- For a set of seed leaves (usually the leaves near a removal), it labels the connected components of the
  `ss_overlap_pairs` contact graph (union-find in key order), across instances.
- For each component, it reports whether it reaches an **anchor** within a work cap.
- **Anchors** (R6):
  - Anything connected through the ground's sheets to the planet's root plates is anchored. A component that
    touches anchored ground, or another anchored component, is anchored.
  - **The ground itself is not judged by blob connectivity.** It is a boundary representation (plates over an
    implicit solid, 06 §4.12), so a BFS over plates can't tell rock from a hollow shell.
  - Whether a piece of ground has been cut loose is the ground plugin's **detachment service** (14 §5.3, `detached`):
    it evaluates the implicit solid minus the edit terms for the edited regions. `ss_components` treats ground plates
    in regions the service has not reported detached as anchors.
  - 11 calls the service after any edit that touches the ground, and `ss_components` for everything else (kinds
    B–D).
- If the cap is reached, the component is reported `ANCHORED_ASSUMED`. Large pieces don't fall off on a guess; 11
  queues the async full check.
- The algorithm is a bounded BFS outward from the seeds.

### 4.4 Acceleration structures (CPU, sim tier; the same code serves the render tier)

| structure | what | updated | answers |
|---|---|---|---|
| **region tree + bounds** (06 §4.3) | per region: the union of its blobs' bounds and its blobs' reaches; skip data like the ray cut's `yTop` (the highest content along the body's up) | on split, merge, touch | ray traversal, overlap, conservative queries |
| **level grids** | one hashed grid per level ℓ over that level's sim-tier leaves and atoms; cell size `2^(ℓ+2)` m (about a blob diameter); a blob is inserted into every cell its reach overlaps | incrementally on split and merge (insert and remove, O(1) per blob) | neighbours, field candidates, overlap |
| **level mask per region** | a bitmask of which levels have leaves in or near a region | with the region | skipping empty level grids |
| **parcel grid** | one per body with parcels; cell size per parcel size class (log₂ bins), so 2 m bombs don't set the cell for 40 m lava (`PLUGINS.md` §2) | rebuilt every tick by counting sort | everything involving parcels |
| **small-body BVHs** | per small rigid body (a thrown rock, a dragged log), a BVH over its blobs in body-local coordinates | on change | everything involving loose objects |
| **model proxies** | per model instance (creatures, the avatar), its posed proxy ellipsoids (1–2 per bone) in a per-tick flat list per body, binned in the parcel-style grid by size class; exact asset atoms evaluated under the pose only for `EXACT_MODELS` rows | per tick, from the bone poses | tagged model hits (§4.1, principle 7) |
| **radial slabs** | kind A ground bounds (06 §4.5): the cell's footprint (its S2 cell polygon, conservatively a spherical cap) between two radii. Ray–slab and sphere–slab tests are a sphere-shell intersection plus a cone (cap) test, both closed-form | with the bound | conservative rays and overlaps over the ground: far tighter than ellipsoids on flat ground (14 §4.7) |
| **body broad phase** | from 04: the body tree with SOI-bounded spheres plus each body's blob bound | per tick | which bodies a query can touch |
| **graph cache** | the CSR cut graph per (body, band, filter) | per region on cut changes | `ss_graph_rows` |

Why per-level grids and not one grid or a BVH over all leaves:
- 2:1 balance means a blob's same-surface neighbours are within one level of it. A radius query at level ℓ checks
  grids ℓ−1, ℓ, ℓ+1, plus any levels the region masks say have atoms nearby.
- Each grid's cell size suits its blobs, so there is no probe explosion.
- Updates on split and merge are local hash inserts. A BVH would need refits or rebuilds.
- Rays use the region tree instead, where bounds give large skips.

**Candidate lists for field evaluation.** At a point p:
- gather the leaves whose reach covers p from the relevant level grids, the parcel grid, the small bodies and (if
  requested) model proxies;
- sort them by key (lists are short, typically 10–30: `BLOBCUT.md` measured 15–30 candidates per step for terrain);
- accumulate per instance with the exp smooth-min (05 §4.2), using four instance lanes like `world/`'s accumulator,
  and an overflow lane taking the min of its own distances.

A query that doesn't need a point's full field (a `HARD` ray, `ss_neighbors`, `ss_visibility`) never builds candidate
lists.

**Precision.**
- Positions are f64. For each candidate, `p − cᵢ` is computed in f64, then converted to f32 for the blob-local maths.
  Offsets are at most a few region edges, so f32 is ~10⁻⁷ relative.
- Rays accumulate `t` in f64. Each march step is evaluated relative to the step's start point.
- The conversion and every operation are deterministic IEEE operations (02).

### 4.5 Level clamping

Every field-based query takes an optional `clamp_level`. The cut is then evaluated as if leaves finer than it were
replaced by their ancestors at `clamp_level`, using the representatives interior blobs keep (06 §4.6).
- A 40 m lava parcel feels 40 m slopes, not 1 m pebbles (`PLUGINS.md` §2: "a large parcel also feels the terrain only
  at its own scale").
- A process at band β evaluates its geometry at β.
- AI far from the player can query coarse terrain cheaply.

Clamping never refines. Clamping *finer* than the cut has the effect of no clamp, plus `TOO_COARSE` flags.

### 4.6 Ray traversal on the CPU

1. **Bodies.** The ray (in the query frame) is tested against the body broad phase. For each body hit, the ray is
   transformed into the body's frame in f64. Bodies are processed in order of entry `t`, and the walk stops at the
   first confirmed hit.
2. **Regions.** Walk the region tree front to back from the root, visiting only regions whose bound union the ray
   crosses, and skipping regions whose content lies entirely on one side (the ray cut's `yTop` skip, generalised to
   the bound union).
   - Leaves are found by walking up and down, as the ray cut did. The exit point is pushed across the face along its
     axis, so near-parallel rays still cross.
3. **Candidate intervals.** In each region, every leaf whose reach the ray crosses contributes an interval `[t₀, t₁]`
   (analytic ray–ellipsoid with the reach-grown radii). `HARD` and `CONSERVATIVE` stop here: the first analytic hit
   wins. The ray–ellipsoid solve uses `M7.md`'s closest-approach form, because the textbook `B² − AC` cancels every
   digit at range.
4. **March** (`FIELD`). Within the merged intervals, sphere-trace `F` with step `ω · F / L`:
   - `L` is the accumulator's Lipschitz estimate, `K/k_min` (`wgsl_lib.js` `lipschitz()`).
   - ω starts at 1.5 and drops to 1 on overshoot.
   - Stop at `F < max(ε₀, cone · t)`, then bisect to the root.
5. **Answer.** Report the normal from `∇F`, the heaviest blob, and the level. Set `TOO_COARSE` from that blob's `err`.

Typical work (estimates): a short ray near the ground takes 20–60 march steps over 10–30 candidates each, so
200–1,800 blob evaluations. That is ~5–40 µs on one core. Sim callers are expected to send hundreds to a few thousand
rays per tick, not millions. Anything screen-sized belongs on the GPU (12) and is cosmetic.

### 4.7 The batch ABI

All queries follow one pattern. In the core's Zig-like terms:

```zig
fn query_x(
    frame: u32,              // query frame (a body handle); positions are in it
    tier: Tier,              // .sim (forced for plugin sim contexts) or .render (engine cosmetic code only)
    n: u32,                  // rows
    src: [*]const u32,       // or points: [*]const [3]f64, rays: [*]const Ray, …
    params: *const XParams,  // radius, k, filters, clamp_level, min_feature, mode, work cap
    dst: [*]u32,             // n × k handles, zero-padded
    aux: ?[*]f32,            // optional: distances, weights, normals …
    row_status: ?[*]u8,      // optional per-row flags
) Status;
```

**Conventions:**
- **Row-major** outputs, `n × k`. `0` is never a blob, so zero padding is unambiguous. Per-row counts are optional.
- **Status** for the whole batch: `OK`, `PARTIAL` (some rows truncated or over budget), `STALE` (some source handles
  were stale and their rows are zeroed), `INVALID` (bad parameters; nothing written).
- **Row flags:** `HIT`, `TRUNCATED`, `TOO_COARSE`, `BUDGET`, `STALE`, `NO_SURFACE`, `MAXED`, `ANCHORED_ASSUMED`.
- **Index spaces never mix.** Every output column holds handles (or keys, on request), never positions in some
  internal array. This is the `planet/` fire bug's lesson.
- **Memory.**
  - Core callers pass pointers into core memory.
  - Plugins call the same functions through the 08 glue. Their `src`, `params` and output pointers are in the
    plugin's own memory: the glue copies inputs in, runs the query on core scratch, and copies outputs back.
  - Costs are O(n) copies plus ~100–200 ns per call (`wasmgpu/README.md` measured ~4 ns per boundary crossing and GB/s
    copies). This favours big batches, which the API encourages anyway.
- **Sorting** within rows: by (distance, key). Graph rows: by key.
- **Work caps** are deterministic work units: candidate evaluations, plus march steps × candidates for rays.
  - Each call carries a cap (default: the calling plugin's remaining per-tick quota, 08).
  - Rows are processed in input order. Rows past the cap get `BUDGET`.
  - Work counts are deterministic, so the same rows hit the cap on every machine. Wall-clock limits would not be.
- **Parallelism.** Rows are independent, so the core may split a batch across its worker threads (01) without
  changing any output.

### 4.8 Determinism checklist

- The candidate order for every accumulation is key order.
- f64 → f32 conversions happen at fixed points (§4.4).
- `exp` and `ln` come from the core's libm (02), and SIMD lanes use the same polynomial in the same operation order as
  the scalar path, so tails match. No relaxed SIMD.
- Sorting uses a total order: distance, then key, with NaN impossible by construction. Inputs are validated: a NaN
  position makes the row `INVALID`.
- Query structures are read-only during the query phase of a tick. Updates (splits, merges, parcel rebuilds) happen
  at phase boundaries (10).
- Results never depend on the render tier, handle values, thread count or timing.

### 4.9 GPU queries (cosmetic only)

The renderer's world-space structures (12) can answer screen-scale query volumes: millions of rays for shadows, AO,
probes and picking. Uses outside the frame:
- **Engine cosmetics:**
  - picking from the rendered frame (`M7.md` §7 read the pick pixel), which is exactly what the player saw;
  - audio occlusion later;
  - camera collision smoothing.
- **Plugin cosmetics (later, Q07.1):** a batch API that returns results 1–3 frames later, to code paths that cannot
  write sim state (visual-only hooks: a creature's eyes tracking the player, particles avoiding walls).

**Picking for gameplay** (the player clicks a tree to fell it) works in two steps:
1. The GPU pick gives the point the player saw.
2. The *action* is issued as a command with that point and ray (02, 10). The sim re-resolves it with a CPU
   `ss_raycast`.

The two agree within `err`. If they disagree (render-only detail), the sim's answer wins: the player can only act on
sim-tier geometry, which near the player is the finest level anyway.

### 4.10 Isotropy

- Exact metric queries (centre distances, gaps, field values, rays) carry no grid bias. Grids and regions only decide
  *which candidates are examined*, never the answer.
- Forbidden shortcuts: "neighbours = blobs in the same or adjacent grid cells" without a distance test (square
  neighbourhoods), and Manhattan or Chebyshev distances.
- Graph processes on `ss_graph_rows` see an irregular graph, because blob positions are jittered by generators (14).
  Fronts spread at physical speeds (10). `planet/`'s scale-free forms (mass balance, front propagation with m/s,
  metric budgets) don't inherit the graph's shape.

### 4.11 Performance targets

These are estimates and targets, not measurements. E1–E3 measure them. Single core, WASM SIMD128.

| query | target | basis |
|---|---|---|
| `ss_neighbors` (k ≤ 16, ~30 candidates scanned) | 0.3–1 µs per row (≥ 1 M rows/s) | hashed grids, f32 local maths |
| `ss_field_eval` (~15 candidates) | 0.3–0.6 µs per point | E1 of 05: ≥ 50 M blob evaluations/s/core |
| `ss_raycast` `HARD`, short | 1–3 µs per ray | analytic tests over a few regions |
| `ss_raycast` `FIELD`, short | 5–40 µs per ray | §4.6 |
| `ss_surface_along` | 3–15 µs | a `FIELD` ray from above |
| `ss_sweep`, short | 5–60 µs | a `FIELD` march of the offset field; `HARD` is ~2× a `HARD` ray |
| `ss_visibility` | ≤ 10 µs per pair | a horizon test plus 3 `HARD` rays at a clamped level |
| `ss_graph_rows` cache rebuild | ~50–100 µs per changed region; 50–150 ms for a full rebuild of 0.3 M leaves (after a teleport) | |
| level grid update | ~50–100 ns per inserted or removed blob | |
| parcel grid rebuild, 20 k parcels | ~0.2 ms | counting sort; `world/`'s GPU dynamic grid took 0.3 ms for 18 k blobs |

**Per-tick budget.** At 30 Hz, the sim's queries should stay within ~3–5 ms per tick on the engine thread. Heavy users
(hydrology, fire) read cached graph rows, not fresh neighbour searches. Long-running plugin work runs in workers with
their own copies of what they read (08, 10).

### 4.12 Alternatives considered

| alternative | why not |
|---|---|
| GPU queries for the sim (they are faster) | not bit-identical across vendors; readback latency; the sim would depend on the GPU (idea 3) |
| One uniform grid for all levels | one cell size can't serve 0.5 m and 8 km blobs (`PLUGINS.md`'s probe explosion) |
| A BVH over all leaves, refitted | refits on every split and merge; a worse fit for radius queries than hashed grids; the region tree already gives rays their hierarchy |
| "Each region evaluates its own level only" (the ray cut's bake rule) as the sim field | steps at region faces; physics feels them |
| Returning neighbours unsorted (faster) | order would leak into plugin results (float sums, first-found choices) |
| Wall-clock work limits | nondeterministic; different machines cut different rows |
| k-NN without a radius cap | unbounded work on a planet |

### 4.13 Disagreement with README

None. Idea 3 (GPU results never feed the sim) is adopted here as a principle; the README doesn't state it yet.

### 4.14 Canonical scenarios

1. **Orbit to grass.**
   - Embarking uses `ss_surface_along` at the chosen point (clamped coarse first, then at the finest level once the ring
     lands) for the spawn height.
   - Walking uses `ss_surface_project`, `ss_field_eval` (with its Lipschitz bound) and `ss_sweep` (capsule) for the
     character controller (11). The camera's spring arm is a sphere `ss_sweep` (18).
   - A click on a flower uses the GPU pick, then a CPU `ss_raycast` confirms the action.
2. **The storm and the flood.** Hydrology runs on `ss_graph_rows` at its band, across levels: the cut graph of
   `planet/`. Water parcels of a flash flood use the parcel grid and `ss_field_eval` with a level clamp.
3. **The beaver dam.**
   - The beaver's perception is `ss_neighbors` with filter `type ∈ {trees}`, in `CENTER` mode within 30 m.
   - It checks line of sight with `ss_visibility` (or `ss_raycast HARD` for one target) and finds the creek with
     `ss_neighbors` on the water instance.
   - Dropping a log (kind D placed content) uses `ss_surface_along` to place it on the creek bed. Other creatures see
     the beaver through its proxy, as tagged model hits.
4. **The asteroid.**
   - The impactor's predicted contact is a `ss_raycast FIELD` along its trajectory (one ray per tick, from far out),
     clamped at its own level. 11 turns the hit into a refinement request (06 §4.11).
   - After impact, the crater is 17's crater record, applied as `z` changes and a `std:ground_height` term (kind A,
     R7). Whether a cliff edge the crater cut has come loose is the ground's detachment service (14 §5.3).
     `ss_overlap` with a sphere finds the trees, rocks and placed things in the blast, and `ss_components` checks
     whether they are still anchored.
   - The director's "was it seen?" uses `ss_visibility` from each observer to the plume's top (17).
5. **Dig, leave, return.**
   - The dig is generator input (R7): `terrain.dig` writes `z` on the finest leaves under the bowl (or appends an edit
     term for a tunnel). The blob store then shows the re-realised plates as removals and additions.
   - `ss_overlap` (`REACH`) with the dig volume finds the kind B–D content it hits (a shrub, a placed log), which the
     dig removes through its owners' services.
   - On return, nothing special happens: queries see the regenerated cut.
6. **A community plugin (hut).** The fire process asks `ss_neighbors` (filter: same owner = fire or any burning blob, by
   field) to spread. The hut's flammability is a field read through `ss_graph_rows` neighbours (09). No query is
   hut-specific.

## 5. Interfaces

The C ABI that plugins see, through 08's glue; the pointers are in the plugin's memory.

```c
typedef struct { uint64_t owners_mask_lo, owners_mask_hi; uint32_t type_set; /* 0 = any; else a registered set */
                 uint32_t instance;      /* 0 = any */
                 uint8_t same_instance, exclude_self_instance, kinds_mask,
                         include;        /* bits: PARCELS | SMALL_BODIES | MODELS | EXACT_MODELS */
                 int8_t level_min, level_max, clamp_level; uint8_t tier; } QueryFilter;

// a tagged target (principle 7): a world blob, or a model instance's bone (and atom, with EXACT_MODELS)
typedef struct { uint32_t handle;      /* world blob; 0 for a model hit */
                 uint32_t body;        /* model hit: the body handle */
                 uint16_t bone; uint16_t _pad; uint32_t atom; /* atom: 0xFFFFFFFF unless EXACT_MODELS */ } SsTarget;
#define SS_MODEL 0xFFFFFFFFu   /* in a handle array: "this row is a model hit; see the tagged output" (0 = padding) */

int ss_neighbors(uint32_t frame, uint32_t n, const uint32_t *src, uint32_t k, float radius, uint8_t dist_mode,
                 const QueryFilter *f, uint32_t *dst, float *dist /*nullable*/, uint8_t *row_status /*nullable*/,
                 SsTarget *tagged /*nullable; n×k; needed if include has MODELS*/);
int ss_neighbors_at(uint32_t frame, uint32_t n, const double *points /*n×3*/, uint32_t k, float radius,
                    const QueryFilter *f, uint32_t *dst, float *dist, uint8_t *row_status, SsTarget *tagged);
int ss_graph_rows(uint32_t frame, uint32_t n, const uint32_t *src, int8_t band, uint8_t edge_filter,
                  uint32_t *offsets /*n+1*/, uint32_t *cols, float *weights, uint32_t cap, uint8_t *row_status);

typedef struct { double origin[3]; float dir[3]; float cone; double t_max; float min_feature; } SsRay;
typedef struct { double t; SsTarget target; uint32_t instance; float normal[3]; int8_t level; uint8_t flags; } SsHit;
int ss_raycast(uint32_t frame, uint32_t n, const SsRay *rays, uint8_t mode /*FIELD|HARD|CONSERVATIVE*/,
               uint32_t max_hits, const QueryFilter *f, SsHit *hits /*n×max_hits*/);

// sphere and capsule casts (translation only)
typedef struct { uint8_t shape; /*SPHERE|CAPSULE*/ float radius; float half_len; /*capsule: segment along local z*/
                 double start[3]; float quat[4]; float dir[3]; double length; } SsSweep;
int ss_sweep(uint32_t frame, uint32_t n, const SsSweep *sweeps, uint8_t mode /*FIELD|HARD*/, const QueryFilter *f,
             SsHit *hits /*n; t = travel distance, normal at the contact*/, double *contact /*n×3, nullable*/);

// batched line of sight: horizon + coarse HARD rays at a clamped level; score in [0, 1]
typedef struct { double observer[3]; float eye_h; double target[3]; float target_h; } SsSight;
int ss_visibility(uint32_t frame, uint32_t n, const SsSight *rows, int8_t clamp_level /*INT8_MIN: auto*/,
                  const QueryFilter *f, float *score, uint8_t *row_status /*BEYOND_HORIZON|OCCLUDED|PARTIAL*/);

int ss_field_eval(uint32_t frame, uint32_t n, const double *points, const QueryFilter *f,
                  float *value, float *grad /*n×3*/, float *lipschitz /*n; |value|/L <= distance*/,
                  uint32_t *instance, uint32_t *heaviest);
int ss_surface_project(uint32_t frame, uint32_t n, const double *points, float max_dist, const QueryFilter *f,
                       double *out_points, float *normal, float *signed_dist, uint32_t *handle, uint8_t *row_status);
int ss_surface_along(uint32_t frame, uint32_t n, const double *origins, const float *dirs /*down*/,
                     double r_max, double r_min, const QueryFilter *f,
                     double *height, float *normal, uint32_t *handle, int8_t *level, uint8_t *row_status);

typedef struct { uint8_t kind; /*SPHERE|ELLIPSOID|BOX|CAPSULE*/ double center[3]; float quat[4]; float extent[3]; } SsShape;
int ss_overlap(uint32_t frame, uint32_t n, const SsShape *shapes, uint8_t test /*REACH|SURFACE|BOUND*/,
               const QueryFilter *f, uint32_t *offsets, uint32_t *items, uint32_t cap, uint8_t *row_status);
int ss_overlap_pairs(uint32_t frame, uint32_t n, const uint32_t *set, float margin, uint8_t include_neighbours,
                     uint32_t *pair_a, uint32_t *pair_b, uint32_t cap, uint32_t *n_pairs);  // sorted by (key a, key b)
int ss_components(uint32_t frame, uint32_t n, const uint32_t *seeds, float margin, uint32_t work_cap,
                  uint32_t *label /*per seed*/, uint8_t *anchored /*per label: NO|YES|ASSUMED*/, uint32_t *n_labels);
                  // anchors: ground plates outside regions the ground's detachment service reported (14 §5.3)

int ss_query_budget(uint64_t *remaining_work_units);   // this plugin's remaining per-tick quota
```

Bounds passed back by 06's `ss_blob_bound` are a tagged union: an ellipsoid (centre offset, radii, sharing the
blob's orientation) or a **radial slab** (`r_lo`, `r_hi` and the footprint's cell id, for kind A ground). Every
`BOUND`/`CONSERVATIVE` test here accepts both.

Every function also checks a per-call work cap: the plugin's quota, or an explicit `ss_set_work_cap`. They return
`OK`, `PARTIAL`, `STALE` or `INVALID`.

**Expected from other docs.**
- 04: the body broad phase and f64 frame transforms through the NCA.
- 05: the field function, keys, the canonical order and libm hooks.
- 06: the tiers, clamping representatives, bounds, `err`, region tree access and `ss_refine_request`.
- 08: argument copying, per-plugin quotas, and forcing `tier = sim` in sim contexts.
- 10: the query phase within a tick, when structures are read-only.
- 11: the semantics of contacts and anchoring built on these primitives.
- 12: the GPU pick and the cosmetic query path.
- 14: the ground's detachment service (`detached`, 14 §5.3) and its radial-prism bounds.
- 16: model assets, poses and proxies; entity keys for sorting model hits.
- 17: visibility scores feed the director; weather and light are multiplied in there.

## 6. Risks and unknowns

- **CPU ray cost.** `FIELD` rays at 5–40 µs mean gameplay can't afford thousands per tick. If AI perception needs
  more, it must use `HARD` rays, or coarser clamps, or run in workers on snapshots. This is unmeasured (E2).
- **Seam bulges.** The union rule makes level boundaries slightly lumpy for physics (the `k ln 2` order of the coarser
  level). It's unmeasured how visible it is in contact behaviour, for example a parcel hopping at a seam (E5).
- **Graph weights from a power-diagram chord** are a crude interface estimate. Processes that need accurate fluxes
  (diffusion) may want better. They are also only meaningful between similar-sized neighbours.
- **The candidate lists' "four lanes plus overflow"** accumulator approximates a point where more than four instances
  meet. That's rare, but deterministic. A sim result could differ slightly from the renderer's, which has the same
  limit.
- **Work-cap semantics** may surprise plugin authors, with partial results under load. The SDK must surface them.
- **Cross-body queries near SOI changes** (04): the transform chain must be the same one 11 uses for contacts.
- **Model proxies vs exact atoms.** A proxy of one or two ellipsoids per bone is coarse. A spear that "hits" a deer's
  proxy may visibly miss its atoms. `EXACT_MODELS` fixes it at extra cost (pending `questions.md` C10).
- **Radial slabs** assume ground bounds are radial prisms. On a planet's poles or edges of S2 faces, the footprint
  test must be conservative (a cap around the cell polygon), or rays could skip a cell's content.

## 7. First experiments

| # | experiment | success criterion |
|---|---|---|
| E1 | `ss_neighbors` and `ss_field_eval` in the core language over 0.5 M leaves (ray-cut terrain plus trees), with three level grids per query; run the same batch in Chrome, Firefox, Safari and Node with shuffled handle assignment | identical outputs (hash of dst + dist) everywhere; ≥ 1 M neighbour rows/s and ≥ 2 M field points/s on one core |
| E2 | CPU `ss_raycast` in all three modes against a GPU reference (the ray cut's march) on 10 k random rays per view, from the six `RAYCUT.md` views | `FIELD` hit distances within the root tolerance of the GPU's on > 99% of rays (the rest explained by GPU sampling); measured µs per ray per mode |
| E3 | `ss_graph_rows` for a hydrology band over a 0.3 M-leaf sim tier: full build, then incremental after 100 random splits and merges | incremental results identical to a full rebuild; a full build < 150 ms, incremental < 0.1 ms per changed region |
| E4 | Level clamp: the same parcel trajectory clamped at ℓ = 5 and unclamped on ray-cut terrain | the clamped run sees slopes within 5% of the level-5 representative surface; no row flagged `TOO_COARSE` when `min_feature` ≥ the level's `err` |
| E5 | Seams: roll parcels across 2:1 boundaries at levels 0–6 | no visible hop: vertical acceleration spikes < 10% of g at a seam |
| E6 | `ss_sweep` (sphere, capsule) against a brute-force reference (dense `ss_field_eval` sampling along the path) on ray-cut terrain with trees | contact `t` within the root tolerance on > 99.5% of sweeps; ≤ 2× the cost of a `FIELD` ray of the same length |
| E7 | `ss_visibility` for 1,000 observer–target pairs over 5–50 km on the reference planet | agrees with a fine `FIELD` ray reference on ≥ 95% of pairs; ≤ 10 µs per pair |

## 8. Build order

| milestone | what of this doc |
|---|---|
| M2 | `ss_field_eval` and `ss_raycast` (`HARD`, `CONSERVATIVE`) on the render tier, for CPU-side picking checks and tests; the region tree traversal |
| M4 | the sim-tier structures: level grids, the parcel grid, small-body BVHs, model proxies, radial slabs; `ss_neighbors`, `ss_field_eval` with the Lipschitz bound, `ss_surface_project`, `ss_surface_along`, `ss_raycast FIELD`, `ss_sweep` (the avatar and camera need it), `ss_overlap`; tagged hits; the batch ABI through plugins; work caps (E1, E2, E4, E5, E6) |
| M5 | `ss_graph_rows` and its cache, for hydrology and vegetation; `ss_overlap_pairs` and `ss_components` for detachment, with the ground's detachment service (E3) |
| M6 | `ss_visibility` for the director (E7); performance work (SIMD paths, parallel rows); cross-body queries for impacts |
| later | the cosmetic GPU query path for plugins (Q07.1) |

## 9. Open questions

- **Q07.1 Should plugins get GPU-backed cosmetic queries** (results a few frames late, usable only in visual-only
  hooks)?
  - *Why it matters:* it lets plugins do screen-scale effects, but it opens a second, non-deterministic API surface
    that must never leak into sim state.
  - *Default:* not in v1. Revisit after M6.
- **Q07.2 Neighbour distance by default:** centre distance (simple, cheap) or surface gap (what "touching" means)?
  - *Default:* both are offered. `CENTER` is the default for `ss_neighbors`, `GAP` for graphs and components.
- **Q07.3 Deterministic work caps per plugin per tick**, with partial results when exceeded, or no caps?
  - *Why it matters:* a runaway plugin could stall the tick. Caps make degradation deterministic, but plugins must
    handle `BUDGET` rows.
  - *Default:* caps, with generous defaults and a clear SDK warning.
- **Q07.4 Sim queries never see render-only detail.** Gameplay acts only on sim-tier geometry, and near the player
  that is the finest level. Acceptable? (merged into `questions.md` C7)
  - *Default:* yes. It is the price of a camera-independent simulation (06 Q06.1).
- **Q07.5 How many CPU field rays per tick should gameplay expect to afford** (AI vision for many creatures)?
  - *Why it matters:* it decides whether creature perception needs a cheaper model (`HARD` rays, coarse clamps,
    staggered updates) from the start.
  - *Default:* plan for ~1–2 k `HARD` and ~100–300 `FIELD` rays per tick, and stagger perception across ticks (16).
