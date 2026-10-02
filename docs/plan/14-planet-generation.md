# 14. Planet generation

Status: draft · Depends on: 02 (deterministic maths, hashing), 03 (planet parameters, seeds), 05 (blob model,
keys), 06 (hierarchy, split protocol, cut), 08 (plugin ABI), 09 (field schemas) · Used by: 10, 11, 12, 13, 15, 16, 17

## 1. Purpose and scope

This doc covers how a planet's ground comes to exist at every level, from one blob for the whole planet down to
sub-metre plates under the player's feet. It also covers how that ground carries the fields the simulation runs on.

**In scope:**
- the spherical index that the ground hierarchy, its keys and its neighbour finding are built on;
- top-level generation: plates, uplift, erosion, priority flood, spin-up; its cost; caching;
- the lift for terrain fields: the smooth prediction, the continuous detail field, rivers as lifted structure, shape
  terms;
- realisation: how a cell's field state becomes blobs (plates), with representatives, bounds and error estimates;
- four kinds of content (field-derived, counted, shaped, placed) and how each relates to the lift and to edits;
- caves, overhangs and arches; oceans and lakes (the generation side); biomes and ground appearance; vegetation
  placement;
- other planet kinds; the far appearance of planets in orbit view;
- the boundary between core and plugins; generation throughput and workers.

**Out of scope** (owned elsewhere, interfaces stated here):
- universe and planet parameters, seeds and ids (03);
- the generic hierarchy machinery: restrict, projection, the cut, requesters (06);
- field schemas and composition (09); process scheduling and spin-up timing (10);
- persistence records and storage (15); hydrology, climate and vegetation *processes* (plugins, 10);
- how blobs are drawn (12); atmosphere and ocean shading (13); entity behaviour (16).

## 2. Requirements

| | requirement | traced to |
|---|---|---|
| must | A spherical planet you can walk around, with no seams, poles or face edges visible. | constraint: sphere, isotropy |
| must | Every level comes from the same state. What you see at 50 km is what you walk into (no statistical far field that disagrees with the near one). | pillar 5; `BLOBMAP3.md` (clearings that didn't exist) |
| must | Fully deterministic: the same planet key gives bit-identical blobs on every client and in every visit order. | D5; `planet/DESIGN.md` §2.2 (order-dependent lifts: 1,054 spurious residuals) |
| must | Coarse work only for coarse views. A level is made from the level above, never from finer data. | near-infinite view distance; `RAYCUT.md` §2 |
| must | Large-scale structure that fine levels obey: rivers, coasts, mountain ranges, cones. | constraint: consistent large-scale simulation |
| must | Edits and events (digs, lava, dams, craters) change every level, and survive unloading. | D8, D15; scenarios 3, 4, 5 |
| must | Terrain is a plugin: the core knows no landform by name. | pillar 4; README M3 |
| should | Awe-inspiring landforms: ranges, cliffs, canyons, volcanoes, coasts, not only fBm hills. | pillar 2 |
| should | A new region becomes usable within about a second of being requested (coarse first). | `RAYCUT.md` (0.9 s after a 180° turn) |
| should | A new planet's top level is ready within the length of the jump animation. | scenario 1 |
| later | Caves and underground exploration; gas giants, ice and lava worlds, cratered moons. | nice-to-have: multiple planets |

## 3. What the prototypes taught

| lesson | source | consequence here |
|---|---|---|
| Generate only the top level; everything finer is lifted. No height function is ever evaluated at fine scale as the truth. | `planet/DESIGN.md` §3 | top-level generation is a one-off per planet; fine terrain is lift output |
| Lift = prior + exact projection, so `R(L(x)) = x` (2e-16 to 6e-15 relative). | `DESIGN.md` §2.3, §5 | the terrain plugin writes priors only; the core projects |
| A refined cell's 8 same-level neighbours must exist; otherwise lifts depend on visit order. | `DESIGN.md` §2.2 | neighbour invariant and 2:1 balance (06) |
| Priors read persisted state only. Lifted detail must be in equilibrium with its parent: white noise dug 35 km³ of closed depressions that swallowed the river. | `DESIGN.md` §2.3, §4 | priors of `z` route water through; only edits make lakes |
| Rivers must be lifted as structure (segments, bed profiles, valley floor, stream burning, segment-following receivers). Upward, routing is restricted (`outlets`). | `DESIGN.md` §2.5 | hydrology contributes river conditioning to the terrain lift |
| White noise per child with per-parent mean removal draws the parent grid (a staircase of about half the level's amplitude). A continuous detail field `N(x)` with rotated octaves, stored as cell averages, removes it (projection error 3.5–7% instead of ~50%). | `DESIGN.md` §6 | `N(x)` is the terrain prior's fine content |
| Shape terms (cones, lava lobes) compose with the detail; cells track the terms' part (`nzT`) so rivers can ignore it; a top-level river carved 600 m off a cone before that. | `PLUGINS.md` §1.2–1.3 | terms are an extension point of the terrain plugin |
| A sea painted from `z < 0` made square lakes; the ocean-connectivity mask is the truth. | `DESIGN.md` §6 | the ocean is decided by connectivity, not height |
| Plates per surface point per cell, thick enough to hold the band `H ± A`, give closed, blendable ground at every level; exact enclosure is possible (0 failures). | `RAYCUT.md` §2, `tdgen.js` | realisation uses plates; enclosure goes to the bound (D9) |
| Rendering the enclosing superset puts each level's surface 0.8–1.5 cells high; lift compensation brings it to 0.1–0.35 cells. | `RAYCUT.md` §2–3 | representatives sit on the surface; bounds enclose (D9) |
| Per-blob error, not per-node error, is needed for convergence; steep-slope error is underestimated (1–1.8 cells on 60–70° faces vs 0.3–0.45 estimated). | `RAYCUT.md` §2, §10 | per-blob error estimates; cliffs need their own treatment |
| Terrain lumps overlap ~9 deep; that overlap is what makes ground smooth. Coarse levels need volume (keels), or sampling falls through the shell. | `world/BLOBMAP.md` #7, `world/README.md` #4, `BLOBMAP3.md` (keels) | plate radius ~1.7 cells; thickness ≥ one render sample |
| Grid-cell merging drew a lattice on distant forests; jittered sites fix it for 2–7% of GPU time. | `BLOBMAP3.md` §3 | jittered plate centres and candidate sites |
| Generation, not the GPU, limits how fast a new view converges: ~5 ms per 400-blob family per level in JS, 1–2 k splits/s. | `RAYCUT.md` §9 | generation in WASM, in workers, coarse first |
| Generation of 9,126 top cells: `[terrain]` alone builds in 305 ms; with the 4-year spin-up the build is ~3 s. Refinement on a teleport: 15–40 ms (41–45 ms with the detail quadrature, 49 ms with snow). | `PLUGINS.md` §1.2, §7; `DESIGN.md` §5–6 | spin-up dominates first-visit cost |

## 4. Design

### 4.1 The pipeline

```
 planet key + profile (03)
   │
   ▼
 top-level generation (terrain plugin, worker, once per planet, cached)
   plates → uplift → implicit stream-power erosion + diffusion → priority flood
   → z, rough, plate ids, river seeds on the top cells (depth 6 for the reference planet)
   │
   ▼
 spin-up (every plugin's spin-up extension point + processes with large steps, 10)
   → climate, hydrology (rivers, lakes, ocean mask), snow, vegetation counts at t0
   │                                       (t0 state: cache, not save — 15)
   ▼
 the live top level (ℓ_sim_floor: simulated and saved in full once the planet is activated, 10)
   │   split (06) ──────────────────────────────────────────────┐
   ▼                                                             │
 lift of fields: owner priors (Harten + N(x) + conditioning)     │ merge = restrict (core)
   → core projection → residuals applied (15)                    │
   ▼                                                             │
 realisation: fields + terms → plates (representative, bound, error, material)
   ▼
 blobs in the cut ── used by the sim, physics, queries, the renderer
```

Every arrow is a deterministic function of persisted state (D5). The terrain plugin owns the boxes from top-level
generation to realisation. The core owns the index, the projection, residuals, the cut and storage.

### 4.2 The spherical index

The ground needs:
- a 2D index on the sphere that nests exactly (children partition their parent, so sums are exact);
- cheap parent, child and neighbour operations, with stable 64-bit ids that order nearby cells together on disk;
- cells close enough to equal in size that one depth means one level.

The planet's processes (hydrology, climate, snow, vegetation) are surface processes on a 2D graph, as in `planet/`.
So the index is 2D, with a radial dimension per cell column (§4.6), not a 3D octree.

| option | nesting | max/min cell area | neighbours | ids | notes |
|---|---|---|---|---|---|
| cube-sphere, linear (gnomonic) | exact 4:1 | 5.20 | 8 (7 at the 8 cube corners) | S2-style, Hilbert order | sizes vary too much: one depth spans 2+ size classes |
| **cube-sphere, tangent (equi-angular)** | exact 4:1 | **1.41** | 8 (7 at corners) | S2-style | one `atan`/`tan` per coordinate conversion |
| cube-sphere, quadratic (S2 default) | exact 4:1 | 2.08 | 8 (7 at corners) | S2-style | no trig; less uniform |
| HEALPix (nested) | exact 4:1 | **1.00** (equal area) | 8 (7 at some vertices) | nested index | cells are curved diamonds, distorted in the polar caps; centres on iso-latitude rings; neighbour code is intricate |
| icosahedral triangles | exact 4:1 | ~1.5–2 (projection-dependent) | 3 edge + 9 vertex | custom | triangles don't fit the 3×3 Harten stencil the lift was proven with |
| 3D octree in the planet frame (ray cut) | exact 8:1 | 1 | 26 | octree path | not a surface graph: areas vary with slope, a cell can hold several sheets; hydrology and area sums get awkward |

Area ratios for the cube-sphere options are from the [S2 cell statistics](https://s2geometry.io/resources/s2cell_statistics.html).

**Proposal: an S2-style cube-sphere quadtree with the tangent projection.**
- Ids are 64-bit: 3 bits of face, 2 bits per depth along a Hilbert curve, and a sentinel bit, which allows up to 30
  depths. That is 0.7 mm cells on a 500 km planet. Parent ids, child ids and the containing depth are bit
  operations. Hilbert order makes the cell id a good spatial sort key for storage records (15). Blob keys
  themselves are hashes (R3, §4.7).
- Areas are exact: each cell's area is computed in f64 (spherical quadrilateral), cached per loaded cell, and used by
  every area-weighted mean. Equal area (HEALPix) would simplify the arithmetic, but `planet/` already ran exact
  area weights with no trouble. Being able to reuse S2's well-specified geometry matters more.
- The 8 cube corners have 7 neighbours instead of 8. `planet/` handled this in its stencils (`world.js` `neighborhood`).
- **The layering (R4, pending questions.md C8).**
  - This cube-sphere is the **ground plugin's surface index**: cell ids, key ordinals, the hydrology graph, cell
    areas.
  - The index maths ships as a **core/SDK library** (§5.1), implemented once in deterministic f64 with our own
    `tan`/`atan` (02). So every plugin that reads ground cells (hydrology, vegetation, snow) agrees on ids,
    neighbours and areas.
  - The core's own spatial structure is 06's per-body **region octree**, in the body-fixed frame, used for
    splitting, balance and render nodes. Ground blobs belong to regions like any other blobs.
  - 06's 26-neighbour region invariant guarantees that the 3×3 cell stencil the ground's lift reads (§4.6) is
    loaded.
- **Isotropy does not come from the index.** It comes from three things, all measured in the prototypes:
  - the detail field `N(x)`, whose octaves are randomly rotated (`detail.js`);
  - plate centres jittered inside their cells;
  - the 9-deep overlap of plates.

  The grid is in the bookkeeping, never in the shapes.

### 4.3 Depths and levels on the reference planet

The reference planet for estimates (README) has R = 500 km. A cube face spans an arc of πR/2 = 785.4 km. The cell
edge at depth d is e_d = 785.4 km / 2^d (nominal; the tangent projection keeps edges within about ±19% of it). Plates
have an in-plane radius of ~1.7 e_d (`tdgen.js` `A = 1.7`), so their level is ℓ = ⌊log2(1.7 · 785.4 km) − d⌋
= **20 − d** on this planet.

| depth d | edge e_d | plate radius | level ℓ | cells on the planet | role |
|---:|---:|---:|---:|---:|---|
| — | — | 500 km | 18 | 1 | the planet blob: the root of the ground hierarchy |
| 3 | 98.2 km | 167 km | 17 | 384 | the first plates (orbit view) |
| 4 | 49.1 km | 83 km | 16 | 1,536 | (storage regions are 06's ℓ13 regions, 65.5 km; 15) |
| 5 | 24.5 km | 42 km | 15 | 6,144 | |
| **6** | **12.3 km** | 21 km | 14 | **24,576** | **top level, `ℓ_sim_floor`:** generated on entering the system; always simulated once the planet is activated (10) (planet/: 10 km) |
| 10 | 767 m | 1.30 km | 10 | 6.3 M | `ℓ_view_floor`: the orbit view's render cap (§4.13) |
| 13 | 96 m | 163 m | 7 | 403 M | vegetation band (planet/: 157 m) |
| 15 | 24 m | 41 m | 5 | 6.4 G | entity depth: tree candidates (planet/: 20 m) |
| 20 | 0.75 m | 1.27 m | 0 | 6.6 T | finest ground, the cheaper option (Q06.7) (planet/: 1.2 m) |
| **21** | **0.37 m** | 0.64 m | **−1** | 26 T | **finest ground plates, the default** (R5): the ray cut found its 0.5 m cells already coarse near the camera |

Notes:
- **Depths 0–2 are not realised.** Their plates would be as big as the planet. The planet blob's children are
  depth-3 plates. A split may skip levels (06 must allow ℓ18 → ℓ17 children from a depth-0 index root).
- **Depths 3–5 are restrictions of the top level.** They exist to be drawn from orbit and to give coarse processes
  (a global climate band) somewhere to run. They are inner nodes of the always-loaded hierarchy.
- **Level follows depth, not each plate's measured size.** The ±19% edge variation would put siblings in adjacent
  size classes. The terrain plugin declares ℓ per depth. (Requirement on 06: an owner may assign a nominal level to a
  family. Size classes stay comparable across plugins to within one level.)
- The observer ring is K = 8 blob sizes of each band's level, which is one region edge (06 owns it; R5). With a
  12.3 km top level, the sim tier around one player holds about 16 × 1,300 ≈ 20–25 k surface cells (depths 6–21)
  plus the 24,576 top cells. That is an estimate from planet/'s ~900–1,300 leaves per level.

By the size-class rule (largest radius in `[2^ℓ, 2^(ℓ+1))`), a 500 km planet's root is ℓ18, as in 06's single level
table (R5).
The named levels `ℓ_sim_floor`, `ℓ_view_floor` and the finest ground level are defined there too. The values above
are the reference planet's.

### 4.4 Top-level generation

This is a port of `planet/js/gen.js`, run by the terrain plugin in a worker on the top cells' graph:

1. **Plates.** n_P plates (12 by default) grow by randomised Dijkstra flood fill from seeds, with a noise wobble on
   edge costs. Each is continental or oceanic (42% continental by default), with an Euler pole and a rate of 0.4–1.4 ×
   4.5 cm/yr.
2. **Boundaries and uplift.** Measure the convergence speed across plate edges and spread it inland by distance.
   Choose the uplift profile by plate kinds:
   - continent–continent: collision ranges;
   - continent–ocean: coastal ranges and trenches;
   - ocean–ocean: island arcs;
   - divergent: rifts and mid-ocean ridges;
   - plus hotspots.
3. **Initial surface.** Plate base heights, shelf smoothing, broad noise, then a datum shift to the target ocean
   fraction (55% by default).
4. **Erosion.** 60 steps of 200 kyr: uplift, priority flood, flow accumulation, an implicit stream-power update
   (Braun & Willett 2013, K_f = 5e-6, m = 0.5) and explicit hillslope diffusion (K_d = 60). Then sediment infill of
   the remaining depressions (85%).
5. **Output per top cell:** `z`, `rough` (the detail amplitude from relief and uplift), plate id, and uplift rate (for
   volcano placement). Resources: the plate table and the river seeds (from the final flood's receivers).

**Determinism details** (requirements on the port, beyond 02):
- heap pops break ties by (value, cell id);
- the plate-growth `rng` is a counter-based stream keyed by (planet key, "plates", i), not a sequential generator;
- every reduction (the datum percentile, sums) runs in cell-id order.

**Parameters come from the planet profile (03).** Gen-relevant fields: radius, gravity, ocean target, plate count,
continental fraction, uplift scale, erosion constants, hotspot count, volcanism, water inventory and ice fraction.
The terrain plugin maps profile → gen options. Planet kinds (§4.12) change the mapping, not the code.

**Cost, estimated.** `[terrain]` alone built 9,126 top cells in 305 ms in JS (`PLUGINS.md` §1.2). The full build with
a 4-year spin-up took ~3 s. At 24,576 cells (2.7× as many, with an n log n flood):
- generation: ~0.9 s in JS; WASM, est. 2–4× faster: **~0.25–0.5 s**;
- spin-up: ~8 s in JS; **est. 2–4 s in WASM**. This is the dominant cost. It runs climate, hydrology and snow
  processes (10), and their steps can be spread over several workers only per plugin, not per cell.

These estimates are what experiment E14.1 measures.

**Policy: generation and activation are separate (R9).**
- **Generation** is a pure function of (seed, profile, generator pins). It produces the t0 top level and its spin-up.
  - It runs **on entering a star system**, for that system's planets, in background workers (nearest and largest
    first). The jump animation (18) then rarely waits.
  - The t0 state is **cached** (15). It is regenerable, so it is not part of a save.
  - It simulates nothing beyond the spin-up, and it doesn't make the planet live.
- **Activation** is defined once, in 10: by embark, or by an event.
  - Only an activated planet's top level (`ℓ_sim_floor`) is stepped by the scheduler.
  - Only an activated planet's live top level becomes save state.
  - A never-activated planet is the same pure function of (seed, T) that 10 describes, so generation never has to
    advance it.
- If a jump arrives before the top level is generated, the orbit view shows the impostor of §4.13 and cross-fades.

### 4.5 Terrain state: the fields the terrain plugin owns

| field | agg | meaning | persisted residual τ |
|---|---|---|---|
| `z` | mean | mean ground elevation over the cell's area, metres above the datum sphere of radius R | 400 yr (planet/) |
| `rough` | mean | relief amplitude of the unresolved detail (sets `N(x)`'s local amplitude) | none: regenerated |
| `nz` | mean | the cell's average of the continuous detail plus terms | none: regenerated |
| `nzT` | mean | the shape terms' part of `nz` (rivers condition the ground without it) | none: regenerated |
| `dev_lo`, `dev_hi` | min, max | the deepest and highest any descendant's surface lies relative to this cell's surface (bounds, §4.7) | stored absolutely with the record (15) |
| `sheets` | — | the number of extra surface sheets in the column (caves, overhangs; usually 0) | derived from terms |

Fields other plugins own also attach to ground blobs, through the field system (09): water, snow, temperature,
precipitation, discharge, trees, burnt, lava and so on. A ground blob is the graph node they live on.

### 4.6 The lift of terrain fields

On a split of cell P at depth d, the terrain plugin proposes priors for its four children. Following `planet/`, the
prior for `z` is:

1. **The smooth part.** u = z − nz on the 3×3 neighbourhood of P (all exist, by 06's 26-neighbour region invariant). Harten's
   cell-average prediction gives the children's u. In 1D, child averages are ū_i ± (ū_{i−1} − ū_{i+1})/8, which is
   exact for quadratics; 2D uses the tensor product (`world.js` `harten`).
2. **The detail.** Each child adds its own average of the continuous field, nz_c = avg_c(N + T). N(x) is
   gradient-noise octaves of wavelength λ_o = e_top/2^o, each with a random rotation and offset and amplitude
   K · rough · (λ_o / e_top)^0.75. Octaves much longer than the cell are point sampled, those near the cell size use
   2×2 to 5×5 quadrature, and much shorter ones average to zero (`detail.js`).
   - **In 09's terms (R17), the continuous field is the point-domain field `std:ground_height`:**
     - the terrain plugin supplies its `base` phase, the smooth reconstruction of u plus N;
     - other plugins' shape terms (cones, lava lobes, crater rims) are contributions to its `shape` phase (`add`,
       value plus gradient as `f64x4`, with a declared support);
     - snow adds to the `cover` phase, which the lift does not read.
   - So T(x) = `@shape − @base`, and the lift integrates `std:ground_height@shape`.
   - nzT, the terms' part of nz, is the cell average of T.
3. **Conditioning.** Contributions to the `lift_conditioning` extension point clamp children's u. Hydrology
   contributes river beds and valley floors:
   - bed profiles along segments (non-increasing downstream, identical from both sides of an edge);
   - valley-floor lower bounds within the 3×3 neighbourhood;
   - "no closed depressions" for lifted detail, unless next to a touched cell.

   The conditioning reads the ground without the terms, `std:ground_height@base` (u + nz − nzT). So a river never
   carves a cone flat (`PLUGINS.md` §1.2, hazard 2). Rivers stay an extension point of the lift, not a field phase:
   they bound the children's values rather than adding to a point field (R17).
4. **The core's projection.** It shifts children so that the area-weighted mean equals z_P exactly. Stored residuals
   (15) are added after that, decayed to now.

Because averages add up across depths (a parent's `nz` is the area-weighted mean of its children's, up to quadrature
error), the projection removes only 3.5–7% of a level's amplitude (`DESIGN.md` §6). There are no creases along parent
edges.

**The two hard rules for every terrain prior**, enforced by review and by experiment E14.3:
- *Priors read persisted state only.* That means P's 3×3 neighbourhood of persisted fields, the planet key, cell ids,
  shape terms, and resources (river segments, edit terms, placed content). Never regenerated process state such as
  discharge or snow.
- *Lifted detail is in equilibrium with its parent.* It holds no water the parent wouldn't, and makes no slope a
  process at the parent's band would release (snow uses the same rule, `PLUGINS.md` §3).

Other terrain-owned fields lift by their kind: `rough` by smooth prediction; `nz` and `nzT` are recomputed from the
field.

### 4.7 Realisation: from fields to plates

Once a family's fields are final (prior, projection, residuals), the owner **realises** each child as blobs. The
split protocol therefore has two phases. This is a requirement on 06 and 08 (§5.2):
1. the owner proposes keys and field priors;
2. the core projects and applies residuals;
3. the owner realises geometry from the final fields.

Owners whose geometry is primary (§4.8, kinds C and D) do it in one phase.

**One plate per surface sheet per cell** generalises `tdgen.js` to the sphere:

- **Centre.** Take a jittered direction p̂ inside the cell, uniform in the inner 60% of the cell in its (s, t) face
  coordinates, hashed from the key. The radius is r = R + h_d(p̂), where h_d is the level's surface:
  - the smooth part reconstructed from the 3×3 stencil;
  - plus the octaves of N and T with λ ≥ κ e_d, with κ = 4 as in `tdgen.js`;
  - plus the cell's mean correction, so that the mean of h_d over the cell equals z.
- **Orientation.** The plate's thin axis is the normal of the level's surface at p̂: the analytic gradient of the
  smooth part plus the resolved octaves, as in `planet/`'s `patch.js` (never finite differences).
- **Radii.** In-plane ~1.7 e_d (stretched along the slope by 1/cos θ on steep cells). Half-thickness
  max(0.7 e_d, 1 render sample, A_d). That is 1.8 e_d at most, so the plate stays inside its neighbourhood
  (`tdgen.js` `BMAX`).
- **Blend.** k = 0.05 e_d (`tdgen.js` `KB`). Ground plates of one body share one instance (D10), so they blend into a
  continuous surface.
- **The top sits on the surface, not above it.** The exp smooth-min of ~7–9 overlapping plates bulges the blended
  surface by up to k ln N (`world/README.md` #6). So the plate is lowered by a calibrated β · k · ln N along its
  normal, with β measured once per configuration against the CPU field (E14.3). This replaces the ray cut's lift
  compensation. It also makes the representative of D9 sit on the surface.

**The representative and the bound (D9).**
- **The representative is the plate above.** It approximates the level's surface (mean error 0.1–0.35 cells is the
  bar the ray cut reached with compensation).
- **The bound encloses every descendant.** That is the cell's prism between radii R + z + dev_lo − m and
  R + z + dev_hi + m, where m is the blend margin k ln 7 · 1.15 (`tdgen.js` `blendMargin`). It is given to 07 as
  whichever primitive 07 picks (a bounding sphere at least; a radial slab if 07 supports it, which is far tighter on
  flat ground). For **untouched** cells, dev_lo and dev_hi come from the analytic bound
  A_d = Σ (amplitudes of the octaves and terms unresolved at d) + ε_proj (projection slack) + c_river (the maximum
  conditioning depth). This recursion nests: A_d ≥ A_{d+1} + (what depth d+1 resolves). That is the ray cut's
  `A(L, mB)` rule. For **touched** cells, dev_lo and dev_hi are min/max restrictions of the real descendants, kept
  with the residual record. An edit can then make the bound looser but never wrong.

**Error estimates** feed the cut's render requester (06, 12). Per blob, not per node (`RAYCUT.md`):
- *expected*: the rms of the unresolved octaves at this cell's `rough`, √(Σ a_o²), plus the measured calibration
  residual;
- *max*: A_d.

The ray cut underestimated steep slopes by 3–4× (§6, risk 2). So the expected error is multiplied by 1/cos θ, and
E14.4 checks it on cliffs.

**Material and colour.** The plugin's `paint` function maps (biome, height, slope, noise, field overlays) → colour
and material id. This is the ray cut's `paint` generalised (§4.11). Overlays such as snow, burnt, wet and lava come
from other plugins' fields. They are computed fields (09), read at realisation and on change.

**Keys (R3).**
- A ground blob's key is H(parent key, ordinal) (05), with ordinal = (64-bit S2-style cell id, sheet index).
- Keys are hashes, so a parent is not derivable from a key. The ground plugin derives parents from cell ids, and the
  ordinal doubles as the spatial sort key for storage (15).
- A sheet keeps its index across levels, by nesting order from the outside in.
- Requirement on 05: an ordinal can carry a 64-bit cell id plus a small sheet number.

**Why boundary plates and no interior blobs.** The ground is a *boundary representation*:
- Blobs realise the surface sheets of an implicit solid: everything below the outermost sheet, minus caves.
- The interior is never blobs. The planet blob carries the body's mass and moments (04) for physics and orbits.

A deep dig or a tunnel does not expose a void, because it creates new sheets (§4.9) that get plates at every level.
This is the ray cut's "digs are generator input".

Volumetric interior blobs were considered and rejected:
- At depth 3 a plate is already 70–180 km thick, so the shell is closed from outside.
- Interior blobs would double the blob count for nothing a ray ever sees.
- World README #4 found coarse levels need *volume* in the shell; keels and thickness rules give that.

**Detachment (R6).** Physics needs to know when a chunk of ground comes loose. Anything connected through the ground's
sheets to the planet's root plates is anchored. The ground plugin answers which ground regions are detached, as a
service (§5.3) evaluated against the implicit solid minus edit terms. The mass of a detached chunk is integrated from
that solid, not from blobs (11).

### 4.8 Four kinds of content

06's geometric (moment) lift projection and D15 (edits as removals and additions) apply differently depending on where a
blob's geometry comes from. Every owner declares one kind per blob type.

| kind | examples | geometry is | consistency across levels | an edit is persisted as (15) |
|---|---|---|---|---|
| **A. field-derived** | ground plates, water surfaces, snow cover, lava lobes as terms | realised from fields + terms (§4.7) | the *field* projection (mean `z` etc.); no geometric projection | field residuals (`z`, water…) and 3D edit terms |
| **B. counted** | trees, bushes, boulders, burrows | one individual per active candidate site, generated from (candidate key, species, age) | count fields restrict and lift; ranked candidates with incumbency choose individuals (§4.10) | candidate overrides + living-set snapshots |
| **C. shaped** | rock outcrops, coral heads, cliff lumps, an arch | the owner's prior from the parent's shape | **the geometric projection (06):** the core projects children onto the parent's volume, centroid and second moments (translation + affine fit) | removals as prior input; per-key shape deltas (geometric residual) |
| **D. placed** | logs a beaver dragged, the player's hut, an impact's ejecta rocks | explicit; not produced by any split | its own small merge pyramid (an object's LOD, 06) | the whole record, with a minted key |

Why the split matters:
- **Kind A gets no geometric projection.** The field projection already makes the parent's mean surface equal the
  children's. A second, moment-based projection would fight it, since plate thickness and overlap don't conserve
  volume across depths.
- **Kind D must stay out of the lifted family.** Consider a placed log inside a kind-C family. The parent's moments
  would include the log. Re-lifting would inflate the natural children to carry its volume, and then the record
  would add the log again. Separating generated from placed content avoids that double count. The coarse view of a
  dam comes from the dam's own pyramid, merged with its surroundings only in the renderer.
- **Kind B never needs geometry residuals.** The count and the overrides regenerate exactly the same trees
  (`DESIGN.md` §2.6).
- **The general rule: discrete edits are prior input; continuous edits are residuals** (15 §4.1).
  - Removals, overrides, stumps and edit terms are persisted state that the owner's prior *reads*. So the
    projection never has to redistribute a removed object's volume over its siblings.
  - Shape changes and depth profiles are stored as differences from a fresh lift.

This refines D15 rather than disagreeing with it:
- At the level of the blob store, an edit still is "blobs removed and blobs added". That is what the renderer and
  queries see.
- For kind A, the *persisted* form of an edit is generator input: a `z` residual, or a 3D edit term. A list of
  removed plate keys would not survive a re-lift from a changed parent.
- "Objects removed whole" (`M7.md` §7) applies to kinds B–D: an instance goes as a unit.

### 4.9 Caves, overhangs, arches

**Generating them** (different landforms take different routes):
- **Tunnels, caves, shafts, and the player's tunnelling** are **3D edit terms**: subtractive SDF primitives such as
  capsule chains, ellipsoids and bowls, held as a resource of the terrain plugin (or of a cave plugin, through the
  extension point). A cave plugin generates its network per depth-6 region from (planet key, region), deterministic,
  with a bounded extent, worm-style capsule chains. The player's digs append terms in log order.
- **Arches, hoodoos, overhanging crags** are kind C rock instances: additive objects sitting on the ground, as in the
  `world/` arch.
- **Undercut cliffs** come from a subtractive term at the cliff foot.

**Realisation with terms.** A cell's column is intersected with the terms. Each crossing of the solid's boundary
along the cell's radial line, within the column's range, becomes a **sheet**, with outward plates for floors and
surfaces, and inward-facing plates for ceilings. This is `tdgen.js`'s "one plate per surface point" at creases,
generalised.
- **Hydrology and `z` see only the outermost sheet.** Water in caves is a later plugin.
- **Coarse levels drop sheets smaller than their cell.** A cavern wider than e_d keeps its sheets, so it shows from
  inside at that level. A tunnel narrower than e_d vanishes from the coarse level, which is correct: it is inside
  the solid and invisible from outside.
- **The cut refines where the camera is inside the solid's outer bound** (06: any requester inside a cave region
  refines it).

**The cost of terms.** Each realisation tests the terms near its column through a per-region list (bounded support,
like `shape.js`'s `centre` + `radius` test). A region with no terms pays nothing.

**Shape terms vs edit terms.**
- **Shape terms** are smooth heightfield additions: cones, lava lobes, crater rims.
  - They are contributions to `std:ground_height`'s `shape` phase (09, R17). They are averaged into `nz`, so they
    change the `z` that surface hydrology sees.
  - Realisation happens on the CPU, so terms no longer need a WGSL twin. That removes the double implementation that
    `PLUGINS.md` §1.3 called "a real cost".
- **Edit terms** are 3D SDF operations applied at realisation. They change sheets, not `z`, unless they break through
  the outermost sheet.
- **Following R7:**
  - a dig that lowers the surface is a `z` edit, which `std:ground_height@base` reflects, since the dig changed
    generator input;
  - a tunnel is an edit term;
  - an impact crater is 17's regenerable crater record, drawn as a `shape` contribution at every level, plus the
    touched detail of the explicit run.

### 4.10 Vegetation and other counted content (kind B)

This follows `DESIGN.md` §2.6, generalised to any counted type.

**Coarse levels hold counts.** A count field (`trees`, `boulders`, …) is a sum field. It is lifted by expected land
area and pushed down where there is room, and its `finest` depth is the entity depth (depth 15, 24 m, on the
reference planet).

**Entity cells hold candidates.**
- Each entity cell has 8 candidate sites with jittered positions and fixed ranks, both hashed from (cell key, slot).
- The candidate's species comes from the biome and a hash.
- n = ⌊count + hash⌋ sites are alive, chosen by rank with incumbency:
  - a rising count activates the best free site;
  - a falling count kills the worst living individual;
  - damaged individuals rank worse.

**Individuals are generated blobs.** A tree is an instance built from (candidate key, species, age, health) by the
vegetation plugin's builder: trunk tubes, canopy masses. It is a deterministic function, so it is never stored.

**Coarse appearance is a closure.** Where cells are not refined to the entity depth, the vegetation plugin realises
*canopy representatives* for each coarse cell: the expected merge (moments and coverage) of the individuals the lift
would produce, given the count, species mix and age distribution. This is the planet's closure rule (`DESIGN.md`
§2.8 item 4) applied to geometry. It must agree with what refinement shows: BLOBMAP3's statistical canopy did not,
and showed clearings where thousands of trees per km² stood. E14.5 checks this. When refined, the real individuals
replace the representatives.

**Render-tier splits** (06, R1) can show individual trees at a distance: the lift is deterministic, and it is never
simulated. This is how "what you see is what you walk into" holds beyond the sim tier.

**Grass, flowers and pebbles** are detail atoms, render tier only (R19). The renderer's detail pass generates them
from fields (biome, wetness, snow, burnt), and they are never candidates or sim blobs.

### 4.11 Oceans, lakes, rivers, biomes and ground appearance

**Generation-side responsibilities** (the processes belong to plugins, 10):
- **Sea level** is the datum (z = 0) unless the profile (03) says otherwise.
- **The ocean** is the set of cells below sea level *connected to the open sea*. The mask comes from hydrology's
  priority flood seeded at the sea (`gen.js` `priorityFlood` with `isSea`). It is a field hydrology owns (09),
  restricted by max, so a coarse cell is "ocean" if any child is. Inland cells below sea level are dry basins or
  lakes, never sea (`DESIGN.md` §6 lesson 1).
- **The ocean surface (R18, pending questions.md C20)** is an analytic sea-level sphere clipped by the mask, at every
  distance, plus dynamic water (fast-layer parcels for surf, splashes and surges; 11, 12, 13). There are no ocean
  water plates: an all-blob ocean would put millions of blobs where a sphere does the job. So generation produces
  only sea level and the mask for the sea.
- **Lakes and river water** are hydrology fields realised as water plates (kind A). The water plates' instance is
  separate from the ground's, so water meets ground with a hard contact (D10, `BLOBMAP3.md`).
- **Rivers** at generation time:
  - the top level's flood gives every river cell its segments: entry and exit points on edges (hashed from the edge
    itself) and a bed profile;
  - the lift then follows them (§4.6 step 3).
- **Biomes** are a computed field (09). The classification is Whittaker-style, from mean temperature and
  precipitation (climate fields), plus elevation, slope, soil wetness and distance to coast.
  - **Coarse cells use a closure:** biome *fractions* under the lift's expected sub-cell distribution of height,
    not the biome of the mean. The same land-fraction lesson as `DESIGN.md` §2.8 (1.97 M vs 1.29 M trees).
  - **Biomes drive** the vegetation species mix, ground paint and materials, and other plugins' priors.
- **Ground paint.** Colour and material are a deterministic function of (biome fractions, height above sea, slope,
  noise at three scales, overlays). That is the ray cut's `paint` (sand, grass, rock by slope and height, snow
  line), plus biome palettes.
  - **Coarse plates prefilter it:** their colour is the area-mean of what their children would show, using the same
    closure, not the colour at the centre. Otherwise far terrain changes hue as it refines (`M7.md` §1 measured
    per-level brightness drift; apply the same test).

### 4.12 Planet kinds

The profile (03) picks a kind. Each kind is a set of gen options plus the plugins that apply (`applies_to` in the
manifest, 08).

| kind | ground | notable plugins | embark |
|---|---|---|---|
| temperate rocky | this doc as written | climate, hydrology, snow, vegetation, volcano, fauna | yes |
| desert | low ocean target, high uplift, dunes as shape terms (aeolian plugin, later) | climate, hydrology (ephemeral rivers: planet/ seed 2) | yes |
| ice world | ice shell over ocean: `z` of rock plus an `ice` thickness field; ice plates as their own instance | snow, glacier (later), cryovolcano | yes |
| ocean world | ocean target ~0.95, island arcs and hotspots | climate, hydrology, reef (kind C) | yes |
| lava world | young surface, magma seas instead of water (the ocean mask with a lava material) | volcano dominant | yes, at your own risk |
| airless moon | no erosion: cratered surface from a crater plugin (shape terms with a power-law size distribution, which is also an event source, 17), regolith paint | crater, impact | yes |
| gas giant | **no ground**: the planet blob plus 2–4 levels of cloud-band blobs (a flow pattern in latitude bands, vortices as terms); no split below ~1,000 km | atmosphere (13) | no |
| star | not a planet: one emissive blob plus a corona (12, 13) | — | no |

What differs is gen options, plugins and paint palettes. The pipeline stays the same, so a new kind is mostly a
plugin and a profile mapping.

### 4.13 Far appearance and the orbit view

- **A planet you can see as a disc of more than about 200 px:**
  - drawn from its realised depths 3–6: the top level (generated on entering the system, live if activated) and its
    restrictions;
  - plus render-tier splits down to **`ℓ_view_floor` = ℓ10** (depth 10, ~0.77 km cells) in orbit view (18's "LOD
    floor"; 06's level table, R5);
  - the splits are generated in workers from the top level and stored records, so far views show the player's own
    craters.
- **Smaller discs and points** use an **impostor from the profile**: ocean fraction, a palette by kind, cloud cover
  from the climate state if known. It is drawn by the renderer's body pass (12).
- **The impostor is replaced by the real top level as soon as that exists,** with a cross-fade over ~1 s. At ≤ 200 px
  the difference is a few pixels of colour, not a different continent.
- **Orbit view's slow rotation needs little refinement over time.** The render requester stops at ~1 px footprints
  before reaching depth 10 when the whole disc is on screen: a 1,000 km disc at 1080p is ~0.9 km per pixel. So the
  visible disc stays within ~10⁵–10⁶ plates (an estimate). The floor binds only when the camera zooms onto the limb
  or a feature.

### 4.14 Core vs plugins

| responsibility | owner | why |
|---|---|---|
| spherical index maths: ids, parents, children, neighbours, areas, direction ↔ cell | **core/SDK library** (R4); the region octree stays the core's own structure (06) | every plugin reading ground cells must agree on ids, neighbours and areas |
| the split protocol, field projection, residuals, touched/halo, restrict per aggregation | **core** (06, 15) | consistency must not depend on plugin correctness (`PLUGINS.md`: "no plugin can break consistency") |
| the generic moment projection for kind C | **core** (06) | a plugin supplies only the prior |
| deterministic noise, the Harten stencil, priority flood, quadrature helpers | **SDK library** statically linked into plugins (02 freezes their numerics) | shared, deterministic code without core bloat; isolated instances (D11) can't call each other cheaply per cell |
| top-level generation, `z` / `rough` / `nz`, the `base` phase of `std:ground_height`, realisation of ground plates, paint, the `lift_conditioning` extension point, edit terms, `dig` / `raise` / `surface_at` / detachment services | **terrain plugin** (the body's *ground provider*) | landforms are content; a community plugin could replace it entirely (Q14.2) |
| rivers, lakes, the ocean mask, routing, outlets; contributes river conditioning | hydrology plugin | |
| cones, eruptions, lava lobes as `shape`-phase contributions to `std:ground_height` (09) | volcano plugin | |
| species, candidates, individuals, canopy closures | vegetation plugin | |
| caves | terrain, or a cave plugin through edit terms | |

**The ground provider.** Each body has exactly one owner of its ground, chosen by the profile's kind from the plugins
whose manifest says `provides: ground` and `applies_to` that kind. A requirement on 08: this is a role, with exactly
one holder per body. Two installed terrain plugins don't fight. The save pins which one a body uses (15).

### 4.15 Throughput, workers and determinism

**Work per split.** A split of a ground cell costs, roughly:
- the field priors: 4 children × fields;
- the `nz` quadrature for octaves near the cell size: the expensive part, 28 → 41–45 ms per teleport in `planet/`;
- conditioning;
- realisation: one plate per sheet, gradient evaluation, paint.

`RAYCUT.md`'s ~5 ms per 400-blob family per level in JS is the nearest measurement. Estimates for WASM in 8
workers: ~1–2 ms per family, so 4–8 k families/s. A sim tier of ~6 k families (25 k cells) then builds in ~1 s after
a teleport, coarse first. E14.6 measures this.

**Two kinds of split jobs**, one per tier (06, R1):
- **Sim-tier splits** are deterministic and due `L_split` = 3 ticks after the request (06, R1). The terrain plugin is
  stateless (D11, 08), so any worker's instance computes the same result.
- **Render-tier splits** are best-effort and asynchronous. They read the same inputs and stored residuals, and they
  are never simulated, saved or awaited by the sim. They may be dropped and regenerated at will.

**Rules:**
- Generation runs on the CPU only. GPU results never become ground state (GPU floats differ across vendors; 07, 12).
- The jump animation (18) registers the destination region as a requester ahead of arrival, so the first frames
  after embarking are at the right depth.
- Work is prioritised coarse first: a new view fills in by levels, as in the ray cut.

### 4.16 Alternatives considered

- **Evaluate a height function at every level (no hierarchy of state).** This was rejected in `planet/` §6. Edits,
  erosion, lakes and rivers live in the cells, and restrict, lift and persistence only work if the fine cells *are*
  the state.
- **Bottom-up pyramid from source blobs (`BLOBMAP3.md`).** Every level is merged from real level-0 blobs. It is the
  most faithful, but a coarse view costs fine work: 4,096 level-3 nodes and two minutes for a 16 km view. A 500 km
  planet is out of reach. The top-down lift keeps BLOBMAP3's rule ("coarse is what fine would show") through the
  projection and closures, without generating fine content first.
- **A 3D octree for the ground (ray cut).** Clean for rendering and caves, but the surface processes need a 2D graph
  with areas. The renderer builds its own 3D structure over the cut anyway (12), so the ground's index need not be
  3D.
- **Volumetric interior blobs.** See §4.7: they cost blobs, and nothing sees them.
- **Shape terms with GPU twins drawing detail inside coarse cells (`planet/` `patch.js`).** This is cheaper in CPU,
  but every plugin's terms must exist twice, and the GPU can't feed state. Render-tier splits do the job with one
  implementation.
- **HEALPix.** Its equal areas are attractive. It lost on neighbour complexity and polar-cap cell shapes. It is
  reconsidered only if exact-area sums prove to be a precision problem (they weren't in `planet/`).

### 4.17 Where this refines the README

None of these are disagreements, but each narrows a README proposal for the ground:
- **D9:** for kind A, the representative is the realised plate and the bound is the cell's radial prism from
  `dev_lo`/`dev_hi`. Neither comes from moment-matching.
- **The geometric (moment) projection (06):** used for kind C only (R7). Kind A gets its consistency from the field projection,
  and kind D stays outside lifted families.
- **D15:** holds at the blob-store level. Kind A edits persist as generator input (`z` residuals, edit terms), not
  as lists of removed keys.
- **The surface-shell-plus-volumetric-interior alternative:** rejected in favour of boundary plates over an implicit
  solid, with keels so every level is closed for rays (§4.7). This is the agreed working assumption (R6, pending
  questions.md C13). Detachment and mass come from the implicit solid through the ground's service (§4.7, 11).

### 4.18 Canonical scenarios

1. **Orbit to grass.**
   - Search an id. The system's top levels were already generated on entering the system (§4.4).
   - The orbit view draws depths 3–6 plus render-tier splits to `ℓ_view_floor` (depth 10).
   - Choosing a ridge makes a pending observer (sim state, R9) and activates the planet (10). Sim-tier splits from
     depth 6 to 21 arrive coarse first, during the zoom: about 25 k cells, est. ~1 s in WASM workers.
   - The orbit camera itself only ever makes render-tier requests.
   - The stream exists at every depth because hydrology's segments condition every lift.
2. **The storm and the flood.** Generation's part: the river is lifted structure from the top level down. The
   player's finest water sheets sit on the bed profile, the same river the 12 km cells route. Discharge is pushed
   down by hydrology (10).
3. **The beaver dam.**
   - Felled trees are kind B overrides (stumps), through vegetation's service.
   - Logs are kind D placed content, with their own pyramid.
   - The pond is hydrology's water field realised as water plates. Hydrology reads a blockage computed from placed
     content (persisted state, so the prior rule holds).
   - The terrain lift is unchanged: the dam does not alter `z`.
   - What persists is in 15 §4.19.
4. **The asteroid.**
   - The crater is 17's regenerable `impact.crater` record (R7). The impact plugin draws bowl and rim as a
     `shape`-phase contribution to `std:ground_height`, so the crater appears at every depth at once, through `nz`,
     whether or not anyone watched.
   - The explicit run's details on top (slumps, the exact ejecta blanket) are touched subtrees: `z` residuals, kind D
     rocks. An overturned flap or a cave-in would be edit terms.
   - Ejecta settle as kind D rocks (or as `z` deposits for fine material, 11).
   - The cut was forced down to the impactor's level first (06, 17).
5. **Dig, leave, return.**
   - A dig is `terrain.dig` writing `z` on the finest leaves (depth 21; a bowl at the dig's level), or edit terms
     for a tunnel (R7).
   - Restrict carries the removed volume to every ancestor at once.
   - The halo and decay are in 15.
6. **A community "hut" plugin.**
   - A hut is kind D (placed by the player or by the plugin's settlement process) or kind B (huts as counted
     candidates of a "settlement" field, generated per entity cell).
   - Either way it never touches the ground's realisation.
   - It sits on the ground through `terrain.surface_at` and contributes `flammability` (09).

## 5. Interfaces

### 5.1 Core: the spherical index (deterministic f64)

```c
typedef uint64_t cell_id;                       // S2-style: face(3) | hilbert path (2/depth) | sentinel
uint32_t   ss_cell_depth(cell_id c);
cell_id    ss_cell_parent(cell_id c, uint32_t depth);
void       ss_cell_children(cell_id c, cell_id out[4]);
uint32_t   ss_cell_neighbors(cell_id c, cell_id out[8]);      // 8, or 7 at cube corners; same depth
double     ss_cell_area(cell_id c, double radius);              // exact spherical quadrilateral
void       ss_cell_point(cell_id c, double s, double t, double out_dir[3]); // (s,t) in [0,1]^2 → unit dir
cell_id    ss_cell_from_dir(const double dir[3], uint32_t depth);
// batch forms take arrays, as everywhere (D6): ss_cell_neighbors_n(n, const cell_id*, cell_id* out, uint32_t* counts)
```

### 5.2 Terrain plugin exports (called by the core)

```c
// once per planet, in a worker; fills the top cells' fields and the plugin's resources
int  gen_top(const body_profile* p, uint32_t n_top, const cell_id* top, field_out* z, field_out* rough,
             resource_out* plates_and_river_seeds);

// split, phase 1: keys and field priors for children of parent handles (core then projects + applies residuals)
void split_propose(uint32_t n, const uint32_t* parents, uint32_t* child_counts, blob_key* child_keys,
                   field_out* priors /* one block per owned stored field */);
// split, phase 2: geometry from final fields; one or more blobs (sheets) per child
void realize(uint32_t n, const uint32_t* children, uint32_t* blob_counts, blob_params* out);
// bounds and errors for the cut and for queries (D9)
void bound(uint32_t n, const uint32_t* handles, bound_out* out);
void error(uint32_t n, const uint32_t* handles, float* expected, float* max);
```

### 5.3 Terrain services and extension points (called by other plugins)

```c
// services
void surface_at(uint32_t n, const double* dirs /*3n*/, const float* band_m, double* height, float* grad /*3n*/);
void dig(const edit_bowl* e);            // z edit on the leaves under it (touches them); or terms if undercut
void raise(const edit_bowl* e);
void add_edit_term(const sdf_term* t);   // 3D subtractive / additive term, appended in log order
void detached(uint32_t n, const uint32_t* regions, uint8_t* out);   // R6: 1 if the ground in the region has lost
                                                                     // its connection to the root plates

// the one extension point the terrain plugin defines
//   lift_conditioning:  per-child lower / upper bounds on u (river beds, valley floors, no-pit rules)  (R17)
```

- **Shape terms are not an extension point.** They are contributions to the `shape` phase of `std:ground_height`
  (09, R17). The terrain plugin supplies its `base`.
- **Overlays** (snow, burnt, wet, lava) are 09's appearance fields (`std:snow_cover`, `std:burnt`, `std:wetness`,
  `std:emission_temperature`; R12). They are read at realisation and on change.
- Service names above are descriptive. They are exposed through 08's service mechanism (`ss_call`).

### 5.4 What 14 needs from other docs

- **05:** H(parent, ordinal) keys whose ordinal can carry a 64-bit cell id plus a sheet number; blob params
  carrying instance, material and level.
- **06:**
  - the two-phase split for kind A owners, and a one-phase split for kind C;
  - families may skip levels near the root;
  - an owner-declared nominal level per depth (ℓ = 20 − d on the reference planet, R5);
  - the 26-neighbour region invariant that covers the ground lift's 3×3 cell stencil (R4);
  - render-tier splits that read residuals;
  - the moment projection for kind C;
  - an object pyramid for kind D.
- **07:** a bound primitive (a sphere at least; a radial slab if possible), and the surface query routed to the
  ground provider.
- **08:** the `provides: ground` role; `applies_to` predicates; static SDK libraries; stateless instances in
  workers.
- **09:**
  - min/max aggregation fields (`dev_lo`, `dev_hi`, ocean mask);
  - `std:ground_height` with phases `base` (terrain) and `shape` (terms), as `f64x4` with supports;
  - computed fields for biome and the appearance overlays;
  - closures.
- **10:**
  - spin-up as an extension point with large steps;
  - activation, defined once;
  - sim-tier split jobs due `L_split` = 3 ticks later (06, R1).
- **15:** residual records with absolute `dev_lo`/`dev_hi`; edit-term and placed-content records; the t0 cache.

## 6. Risks and unknowns

1. **Spin-up dominates first visits.** Est. 2–4 s in WASM at 24.6 k cells; more for bigger planets or more plugins.
   - *Mitigation:* generate on entering a system; cache.
   - *If too slow:* a coarser spin-up band (depth 5).
2. **Cliffs and steep slopes.**
   - One plate per radial column fits faces near 90° poorly. The ray cut's error was 1–1.8 cells on 60–70° faces.
   - Options:
     - stretch plates along the slope (in the design);
     - add plates per column on steep cells (several centres along the fall line);
     - make cliff faces kind C rock instances.
   - E14.4 decides.
3. **Generic-looking terrain.** fBm in the lift gives rolling hills between eroded top-level ranges (`DESIGN.md` §6:
   "erosion-shaped detail would need the lift's prior to be more than noise"). Awe needs canyons, spires, terraces.
   - *Mitigations:*
     - ridged and terraced octaves by biome;
     - landform plugins as shape terms;
     - later, a local erosion step inside the lift (allowed: it reads only the persisted 3×3 neighbourhood) — Q14.6.
4. **Cube corners.** 7-neighbour stencils at the 8 corners and the tangent projection's shear could show as a
   pattern at the coarsest depths (where each corner cell is ~100 km). Jitter and rotated octaves should hide it.
   E14.2 checks.
5. **Cross-plugin lift coupling.** Terrain's `z` lift depends on hydrology's conditioning, which depends on terrain's
   ground without terms. Order comes from `liftReads` (`plugin.js` sorts it, cycles fail at load). It works, but it is
   the most intricate coupling in the system, and a plugin bug here corrupts terrain everywhere.
   - *Mitigation:* conditioning can only clamp (bounds). It never writes values, and the core projection runs last.
6. **Closures that disagree with refinement.** Far forests, biome colours and snow lines. BLOBMAP3 and `PLUGINS.md`
   §7 (the snow edge at the refined/coarse boundary) show how visible this is.
   - *Mitigation:* run closures at a fixed band, and test every closure against forced refinement.
7. **Planet size.** Everything here scales with R²: top cells, spin-up, save size. Earth size (6,371 km) at the same
   12 km top level would be 165× the cells. That is not feasible for spin-up, so a large planet would need a coarser
   top level, which weakens the large-scale simulation (Q14.1).
8. **Determinism of the port.** Every heap, every reduction and every `Math.*` call in `gen.js` is a determinism
   hazard. Fingerprint tests (19) from day one.

## 7. First experiments

| id | experiment | success criterion |
|---|---|---|
| E14.1 | Port `gen.js` + a minimal climate/hydrology spin-up to WASM (Zig); generate the reference planet's 24,576 top cells in a worker | generation < 0.5 s, spin-up < 4 s on the M1 Max; bit-identical fingerprints in Chrome, Firefox and Safari, and in Node |
| E14.2 | Index check: tangent vs quadratic projection; render a flat-ish planet's coarse plates around a cube corner, with jitter on and off | no visible pattern at corners in side-by-side images at depths 3–9; conversion cost < 100 ns per id ↔ dir in WASM |
| E14.3 | Plates from fields on the sphere: realise depths 6 → 21 around a point from lifted fields; render with the ray-cut or M5 renderer; measure the blended surface vs the finest surface per level | mean error < 0.2 cells and max < A_d per level after β calibration; no parent outlines (`DESIGN.md` §6 staircase test) |
| E14.4 | A 60–80° cliff: one plate per column vs slope-stretched vs several plates per column vs kind C rock | error on the face < 0.5 cells at every level with a ≤ 1.5× blob count |
| E14.5 | Far forest closure vs forced refinement at 0.5–3 km | mean luminance within ±10% and coverage within ±10% (M7 reached −15% to +8% after its fixes) |
| E14.6 | Split + realise throughput in WASM workers | ≥ 4 k families/s on 8 workers; a 25 k-cell sim tier after a teleport in < 1.5 s, coarse levels within 0.2 s |
| E14.7 | A cave network as edit terms through a hillside: sheets per column, coarse-level dropping, cut refinement from inside | no holes from outside at any level; the cave interior is visible once inside, within ~1 s |

## 8. Build order

| milestone | work in this area |
|---|---|
| M0 | index math in the core with tests (ids, neighbours, areas, corners); deterministic noise in the SDK |
| M1 | planet profiles from 03 mapped to gen options (data only); impostor parameters for far planets |
| M2 | top-level generation ported (as core-internal code for now); realise depths 3–10 as plates for the orbit view; render-tier splits to `ℓ_view_floor`; E14.1–E14.3 |
| M3 | terrain moves into a plugin: `gen_top`, two-phase split, `realize`, `bound`, `error`; `std:ground_height` `base` + one `shape` contribution (09 M4 lands the point domain); the field projection in the core |
| M4 | refinement to the finest ground level near the player (depth 21 by default, 20 as the cheaper option, E14.3 decides); the detail quadrature; steep-slope handling (E14.4); `surface_at`; `dig` / `raise` |
| M5 | hydrology conditioning (rivers as lifted structure), the ocean mask, water plates, vegetation candidates and canopy closures (E14.5), biomes and paint |
| M6 | craters (impact), cones (volcano), deposits; edit terms and caves (E14.7) |
| later | other planet kinds; erosion inside the lift; a glacier plugin |

## 9. Open questions

- **Q14.1 How big is the reference planet, and its top cell?** (merged into questions.md C1 and C22)
  - *Why it matters:* the cell count, spin-up time, save size and how far the horizon is all scale with it. A 500 km
    planet has a 12.3 km top level and est. 2–4 s first-visit spin-up. Earth size would need a ~100 km top level
    and lose large-scale fidelity.
  - *Default:* R between 250 and 1,000 km (reference 500 km), top cells ~10–15 km. Gravity is set independently
    (03).
- **Q14.2 Should the ground provider be replaceable by community plugins** (a whole different world generator), or
  is terrain a fixed first-party plugin?
  - *Why it matters:* replaceable terrain fixes the `ground` role, its extension points and its services as a public
    API that has to stay stable.
  - *Default:* replaceable, but keep the first-party terrain plugin the only one until the API settles (after M5).
- **Q14.3 How important are caves and underground exploration?** (merged into questions.md C13)
  - *Why it matters:* sheets, edit terms and inside-the-solid refinement add cost to every realisation path, and are
    easier to design in than to retrofit.
  - *Default:* design sheets in from M3 (one sheet everywhere), and build caves after M6.
- **Q14.4 Realism vs drama in landforms.** Should mountains be exaggerated (taller and steeper than erosion gives)
  for awe, and are non-physical forms (floating islands, giant arches) in style?
  - *Why it matters:* it decides whether top-level erosion is the main source of shape or one input among landform
    plugins.
  - *Default:* physical erosion for the large scale, plus exaggeration through a profile parameter and landform
    plugins. Floating islands, if wanted, are bodies, not ground.
- **Q14.5 Is a short wait acceptable on a first jump to a never-visited system?**
  - *Why it matters:* it decides whether spin-up can stay faithful (years of climate) or must be approximate.
  - *Default:* generate on entering the system, in the background; show the impostor if the player is faster; never
    block input.
- **Q14.6 Erosion-shaped fine detail.** Is it worth an experiment to run a small stream-power step inside each lift,
  reading only the persisted neighbourhood, so valleys branch at every scale?
  - *Why it matters:* fBm-only fine terrain may look generic up close.
  - *Default:* yes, as an experiment after M5. The equilibrium rule (no new pits) still applies.
- **Q14.7 Do the S2-style cube-sphere and its keys suit you,** or do you have a preference (HEALPix, an octree)?
  (merged into questions.md C8)
  - *Why it matters:* keys are baked into saves (15). Changing the index later breaks every save.
  - *Default:* S2-style cube-sphere, tangent projection, as the ground's surface index, with 06's region octree as
    the core's structure (R4). The 64-bit Hilbert cell ids are frozen as key ordinals and storage sort keys.
