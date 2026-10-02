# 05. Blob model

Status: draft · Depends on: 02 (numerics, hashing, RNG), 04 (bodies and frames) · Used by: 06, 07, 08, 09, 11, 12, 13,
14, 15, 16

## 1. Purpose and scope

This doc defines what a blob *is*:
- its parameters and flags;
- the field function that turns a set of blobs into a surface;
- appearance (colour, materials) and the blob type registry;
- dynamics modes, at the level of "where a blob's position comes from";
- identity: handles and keys;
- storage, on the CPU and the GPU.

**In scope:** the blob record, the blend rule, carves and paint, materials as data, instance ids, handles and keys,
storage layouts and memory budgets.

**Out of scope:**
- the hierarchy (levels, split, merge, representative vs bound, the cut): [06](06-blob-hierarchy-and-cut.md);
- queries over blobs: [07](07-spatial-queries.md);
- field schemas and composition: [09](09-fields-and-composition.md);
- how blobs move: [11](11-physics.md);
- how the renderer stores and traces them: [12](12-rendering.md). This doc fixes the *contract* the renderer receives;
  12 owns the final GPU structures.

## 2. Requirements

**Must:**
- One primitive for everything: terrain, water, creatures, trees, debris, clouds (pillar 3).
- Order-independent blending, so that any spatial index, any worker split and any edit order give the same surface
  (D5, and `world/README.md` "Blending had to change").
- Blending decided by instance only (D10).
- A blob is addressable from plugins by a `u32` handle in batch arrays. A stable key identifies it on disk and over the
  network (D6).
- Positions precise to well under a millimetre anywhere on a 500 km–6000 km planet (D4).
- Appearance rich enough for the creatures' look (the creature atom's material terms) without per-blob shader code.
- Deterministic everything on the CPU side (D5). The same blob set gives bit-identical field values on every machine.

**Should:**
- A GPU record small enough that millions of blobs fit (the prototypes kept 1–4 M resident).
- Field-driven appearance (wet, snowy, burnt, glowing) without re-uploading geometry.
- Cheap validation of stale handles (the `planet/PLUGINS.md` stale-array hazard).

**Later:**
- Translucent or density blobs (canopy porosity, powder clouds: `M7.md` §6).
- Data-defined material graphs.

## 3. What the prototypes taught

| finding | source | consequence here |
|---|---|---|
| Oriented ellipsoids are worth much more than spheres; orientation + 3 radii is the cheapest big win | `README.md` (creatures) #2 | the primitive is an oriented ellipsoid |
| The per-blob blend radius `k` is the main artistic control: large for masses, tiny for eyes and claws | creatures #3 | `k` is per blob, not per type |
| Carve atoms are essential for faces; a separate colour-blend width avoids colour bleeding | creatures #4 | instance-scoped carves; a colour width per material |
| Surface detail is mostly material, not atoms (`shag` displacement, fur grain); the bear needs only 97 atoms | creatures #5 | a rich material table, not more blobs |
| "Cell" paint (nearest cell atom wins) makes giraffe patches from ~260 atoms | creatures #6 | paint and cell-paint roles |
| Ordered smooth-min doesn't survive a spatial index; the exp smooth-min is associative | `world/README.md` | exp smooth-min within an instance |
| Exp smooth-min with per-blob `k` is not a distance field: the gradient reaches K/k_min, and overlap inflates the surface by ~k ln N (1.1 m on the first terrain) | `world/README.md` #6 | a Lipschitz bound in every tracer; calibrated generators |
| Instances decide blending: same instance smooth, different instances a hard min; it fits in the 32-byte atom with a smallest-three quaternion | `BLOBMAP3.md` §1 | D10, and the GPU geometry record below |
| A 2-bit kind (ground, object, detail, water) routes blobs: keels for sheets, detail to the rasteriser, dropped from merged levels | `BLOBMAP3.md` §1 | a render-routing kind per blob |
| Digging by removing blobs is simpler than carve blobs and works with every LOD level and with splats | `M7.md` §7 | D15: world edits are removals and additions |
| Plugins kept arrays that the core reallocated, read garbage, and NaN spread down the rivers | `planet/PLUGINS.md` §1.2 | handles with generation checks; accessors, never raw arrays across calls |
| GPU cell lists filled by atomics came out in a different order each build, so float sums differed | `M7.md` §7 | canonical summation order on the CPU (key order) |
| Exact per-pixel evaluation runs at ~3–10 G blob visits/s on an M1 Max | `BLOBMAP.md` #4, `README.md` #1 | the CPU evaluator is for queries, never for drawing |

## 4. Design

### 4.1 The blob record (logical)

| parameter | type (logical) | meaning |
|---|---|---|
| `center` | f64 × 3 | in the frame the blob is attached to (§4.6): body frame, body-fixed frame, or bone frame |
| `orient` | unit quaternion | the ellipsoid's axes |
| `radii` | f32 × 3 | semi-axes, metres |
| `k` | f32 | blend radius, metres; `0` means a hard-edged blob within its instance |
| `color` | rgb8 (sRGB) | base albedo |
| `paint` | u8 | paint coverage for paint roles; otherwise 255 |
| `material` | u16 | index into the material table (§4.4) |
| `instance` | u32 handle | blend group (§4.3); `0` = none (forbidden for drawn blobs) |
| `level` | i8 | size class (06 defines it; roughly `largest radius ∈ [2^ℓ, 2^(ℓ+1))` m) |
| `type` | (u16, u16) | the global blob type: (plugin id, type id within the plugin), packed in a u32 (§4.5). The owner plugin is the first half |
| `role` | 2 bits | `add`, `carve`, `paint`, `cell_paint` (§4.2) |
| `kind` | 2 bits | render routing: `sheet`, `object`, `detail`, `fluid` (§4.7) |
| `mode` | 2 bits | dynamics mode: `attached`, `bone`, `rigid`, `parcel` (§4.6) |
| `atom` | 1 bit | the owner never splits it |
| `body` | u32 handle | the body it is attached to |
| `bone` | u16 | bone index within the body, or `0xffff` |
| hierarchy | — | parent, children, region, bound, error, true volume: owned by [06](06-blob-hierarchy-and-cut.md) |
| `key` | u128 | stable identity (§4.8) |

The owner plugin is not stored separately: it is the plugin half of `type`. A blob has exactly one owner, the plugin
that defined its type (R11; there is no ownership transfer), and only the owner is asked to split it.

There is no per-blob "second colour". Patterned looks come from the material (`col2`, grain, patch) and from paint
blobs. Per-blob col2 was used by the giraffe, but cell-paint atoms reproduced it better (creatures #6). See Q05.2.

### 4.2 The field function

All blobs attached to one frame contribute to one implicit field. The surface is its zero set.

**Per blob.** Let `q = R⁻¹ (p − center)` be the point in the blob's frame and `r` its radii. The distance estimate is
iq's ellipsoid approximation, with a box bound for far points (as in `world/js/wgsl.js` `atomEval`):

```
k0 = |q / r|,  k1 = |q / r²|,  d = k0 (k0 − 1) / k1
d  = max(d, |max(|q| − r, 0)|)                 (box lower bound: keeps far estimates conservative)
w  = exp(−d / k) · (1 − smoothstep(CUT − 1, CUT, d / k)),   CUT = 3.5
```

The weight is cut off smoothly at `d = 3.5 k`. A blob's **reach** is its ellipsoid grown by `3.5 k`. That is what
spatial indexes bin and what the bound in 06 must contain.

**Per instance.** For the add-role blobs of instance `I`:

```
S_I = Σ w_i,   K_I = Σ k_i w_i / S_I,   F_I = −K_I ln S_I
```

This is the exponential smooth-min. It is a sum, so it is associative and commutative: any order, any partition into
lists, any number of workers gives the same value, up to float summation order, which is pinned in §4.9.

**Across instances.** `F = min_I F_I`: a hard min. The surface belongs to the instance with the smallest value.

**Carves (instance-scoped).** An instance may also hold carve-role blobs. They form their own exp smooth-min union
`C_I`. They are subtracted from that instance only, with a polynomial smooth-max of width `K_C` (the carves' weighted
`k`):

```
F_I ← smax(F_I, −C_I, K_C)
```

This is the decision on ordering. The creatures applied atoms *in order*: masses, details, carves, and adds after
carves (an eyeball in its socket). The world needs order independence. The rule that keeps both:
- Adds within an instance are order-free.
- Carves within an instance are order-free among themselves and apply after the adds.
- Anything that must sit *inside* a carve is its own instance. For example, the eyeball is an instance and meets the
  carved head with a hard min. This is what eyes should look like anyway: a crisp contact.
- Carves never cross instances. A carve on the head does not cut the hat.
- **World edits are never carves.** Digging removes blobs and adds new ones (D15). Carves are a *modelling* tool
  inside one instance: eye sockets, nostrils, ear hollows, mouth lines.

Carves pass through merges only while their instance is resolved at their level. A carve is detail, and it is dropped
when its instance is restricted past it (06 §4.6).

**Paint (instance-scoped, colour only).**
- A `paint` blob contributes colour within its instance, but no distance.
- A `cell_paint` blob joins a Voronoi partition among the cell-paint blobs of its instance: the nearest wins, with a
  line of base colour where two are nearly equidistant (creatures #6, the giraffe).
- Paint is commutative: a weighted sum for `paint`, a min for `cell_paint`.

**Properties to design around** (all measured in the prototypes):

| property | value | consequence |
|---|---|---|
| a lone blob | `F = d`: exactly its ellipsoid at the surface | atoms look like what they are |
| N blobs with weight ~1 at a point | the surface moves out by `K ln N` (terrain lumps 9 deep: ~2.2 K) | generators calibrate against the CPU field; 06's error estimates include it |
| gradient magnitude | up to `K / k_min` where blobs of different `k` meet | tracers divide steps by the Lipschitz bound (`lipschitz()` in `wgsl_lib.js`) and bisect overshoots |
| iq's estimate off the surface | not exact; exact on it | marches step at ≤ 0.75–1 × the estimate; queries refine roots by bisection |
| `shag` displacement (§4.4) | not a distance field | renderer only; physics ignores it |

**Normals** are the analytic gradient, as in the prototypes: `∇F_I ≈ Σ (w_i / S_I)(K_I / k_i) ∇d_i`, with `∇d_i`
taken as the ellipsoid normal. The `∇K` term is dropped. It is small, and the prototypes never needed it.

**Colour.**
- Colour uses the same per-blob distances with a colour width `ck_i = k_i · material.color_sharpness`:
  `c = Σ exp(−d_i / ck_i) col_i / Σ exp(−d_i / ck_i)`, over the winning instance.
- Eyes and lips get a small `color_sharpness`: a crisp colour edge on a soft geometric join (creatures #4).
- Putting the width on the material instead of each blob costs no bytes, and the creatures never varied it within a
  material.
- Paint then mixes in by its coverage, and cell paint by its Voronoi rule.

### 4.3 Instances

An instance is a blend group, and nothing more (D10).
- At runtime an instance is a `u32` handle (`0` = none).
- Each handle maps to a stable **instance key** (u128), with the same handle/key split as blobs.
- The GPU only ever sees handles.

**Who allocates.** Owners call `instance_new(key)`. Typical choices:

| instance | key | allocated by |
|---|---|---|
| a body's ground (blends across all regions) | `H(body key, "ground")` | the terrain plugin, once per body |
| a body's water | `H(body key, "water")` | the hydrology plugin |
| one tree, rock, hut, creature | `H(entity key, "inst")` | its owner |
| a forest canopy at coarse levels | the owner's choice: one per canopy blob (then the canopy blobs meet each other with a hard min, as trees would), or one shared per region | the vegetation plugin |

**Rules.**
- Blobs attached to different bodies never blend. They are different objects, so they are different instances by
  construction.
- An object (a rigid connected group that moves as one, i.e. a body) is one instance or several. Several only when its
  owner models it from parts that should meet crisply, like eyes in a head.
- The `BLOBMAP3.md` special id "aggregate" (`AGG`) is *not* core magic here. Coarse canopies are closures generated
  by their owner (kind B, 06 §4.4, 14 §4.10). Placed content (kind D) keeps its own instance at every level of its
  pyramid. The renderer merges it with its surroundings far away; the sim never does (06 §4.6).
- **Contact blends between instances** (the 3–18 cm fillet experiment in `BLOBMAP2.md`) are not supported. Different
  instances meet with a hard min, which `BLOBMAP3.md` preferred for rocks on ground. See Q05.5.

### 4.4 Materials

A material is a row in a core-owned table. Plugins register rows **as data** by name (`"myplugin:oak_bark"`) and get a
`u16` id. There is no plugin shader code (Q05.1).

The parameters come from the creature atom (`README.md`, "The atom"), plus what the world needed:

| group | parameters | from |
|---|---|---|
| surface | roughness, specular F0, `color_sharpness` | creatures; §4.2 |
| soft light | `wrap` (diffuse wrap), `translucency` + thickness scale, `fuzz`/sheen amount + tint | creatures (fur, velvet, ears, membranes) |
| texture | `grain` (fbm colour noise: amount, scale), `patch` (Voronoi toward `col2`: amount, scale), `col2` (rgb8) | creatures (fur grain, giraffe) |
| geometry | `shag`: displacement amplitude and tuft scale (renderer only) | creatures (furry silhouettes) |
| emission | emissive rgb × intensity; `glow_from_heat` (emission from the modulation word's `heat` slot: a blackbody from `std:emission_temperature`) | lava, fire, embers |
| behaviour flags | `water_like` (refraction, footprint-prefiltered spec: `M7.md` water), `wettable`, `snowable`, `charrable`, `foliage` (canopy porosity hints) | world, M7 |

A material is about 48 bytes on the GPU (f16 and u8 fields), so 65 k materials fit in 3 MB. Ids are `u16`, so the
`BLOBMAP3.md` 5-bit material id is gone (see §4.7).

**The modulation word** (09 §4.10, R12). Every drawn blob carries 4 or 8 `u8` slots, in appearance words 2–3
(§4.11).
- **Four standard slots, from `std` fields:**
  - `snow` ← `std:snow_cover`;
  - `wet` ← `std:wetness`;
  - `char` ← `std:burnt`;
  - `heat` ← `std:emission_temperature`, through a curve.
- **Up to four custom slots**, claimed by plugin fields, whose effect the material's parameters say (tint toward a
  second colour, roughness, emissive). That is data, not shaders.
- The core re-encodes the word from the commit's dirty lists, never per frame, and plugins never write it directly.
- It lets the planet prototype's per-pixel snow (`PLUGINS.md` §3), burnt ground, wet rock after rain and glowing lava
  work without touching geometry.
- The material decides how each slot shows. For example, `snow` adds a white layer on surfaces facing up, using the
  snowline law per pixel; `heat` drives emission.
- 09 owns the slot mapping, and 12 and 13 own the shading. The other appearance fields of R12 (`std:emitted_power`,
  `std:sediment`, `std:aerosol`) reach the GPU as emitter and atmosphere inputs (13), not through this word.

### 4.5 The blob type registry

A blob type is `(plugin, name)`, registered at plugin load. Its runtime id is a `u16` **per plugin**; the global type
is `(plugin u16, type u16)`, packed in a u32 (R12). The stable identity is the string `"plugin:name"`.

The registry holds only what the **core** needs:

| type constant | used by |
|---|---|
| owner plugin | split dispatch (06), field owner dispatch (09) |
| default material, default colour | creation shortcuts |
| `kind` (sheet / object / detail / fluid) | renderer routing; keels for sheets |
| default dynamics mode | 11 |
| **content kind** (A field-derived, B counted, C shaped, D placed) | 06 §4.4, §4.7, 14 §4.8, 15 |
| atom (never split), level range | 06 |
| projection, for kind C (`full` by default, or `centroid`) | 06 §4.4 |
| render-path hint (raster, march, thin lists; the renderer decides) | 12 |
| `detail_max_level` (detail atoms exist only at and below it, render tier only) | 06, 12 (grass) |

Everything else about a type (flammability, density, hardness, nutrition, how beavers feel about it) is a **field**
(09). Other plugins read it by name, and the owner answers for its own blobs (owner dispatch). The user's sketch already
said that plugins store the properties of the types they created.

### 4.6 Dynamics modes (where a blob's position comes from)

| mode | position is | moved by | examples |
|---|---|---|---|
| `attached` | fixed in its body's frame (the rotating body-fixed frame for a spinning planet); the body is resting (04 renames its body mode `attached` to `resting`, R12) | nothing: it moves only with its body | terrain, trees, a settled rock, a placed hut |
| `bone` | fixed in a bone frame of its body; the bone's transform is owned by the owner plugin | the owner, per tick (16) | swaying branches, a materialised carcass's limbs; creatures themselves are model instances (§4.16) |
| `rigid` | fixed in the frame of a **dynamic** rigid body (an object in flight, rolling or being dragged) | physics integrates the body (11) | a thrown rock, a log being dragged, a toppling pillar |
| `parcel` | its own position and velocity in its body's frame, integrated per tick | the fast layer (10, 11) | avalanches, lava, bombs, floods, splashes |

- When the connectivity check (or the ground's detachment service, 14 §5.3) finds a detached group, it becomes a new
  dynamic body and its blobs become `rigid` members of it. Their keys don't change. When the body comes to rest, it
  settles (11): its blobs become `attached`, as kind D placed content with `keep_instance`, or they `join_ground`.
- Parcels sit outside the region hierarchy (06 §4.10): no parent, no split. The fast layer has its own spatial index
  (07 §4.4).

### 4.7 Render routing kind

The 2-bit kind generalises `BLOBMAP3.md`'s kind:
- `sheet`: closed surfaces such as ground, cliffs and ice. They are realised plates (kind A, 14 §4.7) with keels
  where thinner than their level needs, so every level stays closed.
- `object`: everything solid that isn't a sheet (trunks, rocks, bodies of creatures).
- `detail`: small local blends that the renderer rasterises (grass tufts, flowers, pebbles; `BLOBMAP2.md`). **Detail
  atoms are render tier only** (R19). They are generated from fields (grass counts, ground material) at and below the
  type's `detail_max_level`, never simulated and never persisted.
- `fluid`: water and other liquids, which shade differently and are prefiltered far away (`M7.md` water).

The kind never affects blending (D10). It is a type constant copied into the GPU record.

### 4.8 Identity: handles and keys

**Handles** (D6).
- A handle is a `u32` slot index into the core's blob columns. `0` is never a blob.
- Batch APIs take and return handles: `neighbors(dst, src, n, k, radius)`.
- Handles are **transient**. A slot is reused after the blob is freed.
- Handles are **not deterministic across machines.** Render-only splits (06 §4.8), async job timing and a late join
  all change slot assignment. So:
  - **No sim computation may depend on handle values or handle order.** The canonical order is key order (§4.9).
  - Handles never go to disk or over the network.

**Stale handles.**
- Every slot has a `u16` generation, bumped on free.
- In debug builds, every batch call checks a parallel generation array. Plugins receive `(handle, gen)` pairs in
  debug and plain handles in release. A mismatch zeroes the row and sets `STALE` in the row status.
- Freed slots are quarantined for 2 ticks before reuse. A plugin holding a handle across one tick boundary therefore
  reads a zeroed row, never another blob.
- Plugins must not keep handles across ticks. Keys are what they keep (08). This is the `planet/PLUGINS.md` "accessors,
  not arrays" rule, applied to handles.

**Keys.** A key is a 128-bit hash, computed once at creation and never changed:

| how the blob came to be | key |
|---|---|
| a body's root blob | `H(body key, "root")` |
| a child from a split | `H(parent key, ordinal)`. The ordinal is **semantic**: a child-cell index, a candidate-site index, a part number. It is never "the n-th emitted", so the same child has the same key even when the state changes which siblings exist |
| a ground plate (kind A) | `H(parent key, (cell id, sheet))`: the ordinal is the S2-style 64-bit cell id plus the sheet index, nested from the outside in (R3, 14 §4.7). So the surface sheet keeps its key when a cave adds sheets below |
| a blob created at runtime (a lava parcel, a log the beaver placed) | `H(creator key, tick, seq)`: creator = the blob, entity or event that spawned it; `seq` counts within that tick |
| placed content (kind D: a player's hut, settled ejecta) | minted the same way, from the placing actor or event: `H(actor key, tick, seq)` |

Choices made:
- **Hashed, not a path.** A path of child ordinals from a planet's root to ℓ = −6 is ~25 steps. At 5–8 bits a step
  that is 125–200 bits, and it grows with depth. A 128-bit hash is fixed size.
  - Collisions: for 10¹² keys ever, the probability is ~10¹²·²/2¹²⁹ ≈ 10⁻¹⁵.
  - What a hash loses (prefix = subtree) isn't needed: persistence is applied top-down at split time, by key lookup
    (15), and every loaded blob's ancestors are loaded, so the tree gives ancestry.
  - A debug build keeps a side table, key → path, for printing.
- **Keys don't include the body.** A rock that detaches from the planet keeps its key. Keys are unique per universe,
  because every derivation starts from a body key, and body keys are unique (03).
- `H` is the deterministic 128-bit hash of 02 (defined on little-endian bytes, the same on every platform). The RNG
  streams of 02 are keyed by blob keys: `rng(seed, key, tick, stream)`.
- Ordinals are up to 64 bits plus a 16-bit sub-index (`BlobSpec.ordinal`, `ordinal_sub`, §5), so a ground cell id
  fits.

**Instance keys, entity keys and event keys** follow the same rules, and are 128-bit too.

### 4.9 Canonical order

Wherever a result depends on order (float sums, tie-breaks), the order is **ascending u128 key**. In practice:
- Field evaluation sums blobs in key order. The core keeps candidate lists sorted by key; for a short list, sorting at
  query time is cheap.
- Batch query outputs are sorted by (distance, key) (07).
- Batches handed to plugins are in key order unless the plugin declares the batch order-free.

This is the CPU version of the `M7.md` fix ("the bake now sorts each cell's list"), and it is required by D5. The GPU
may sum in any order: its results are cosmetic (07 §4.9, 12).

Key order is the order of **computation**. Saves are chunked by spatial region, and their records are sorted by a
spatial sort key carried with them, never by key order (15, R3).

### 4.10 Storage on the CPU (SoA, in the core's memory)

Columns are indexed by handle. Family blocks keep a blob's children in contiguous slots (06 §4.9), as the ray cut kept
children in blocks of 8.

| column | bytes | notes |
|---|---:|---|
| `cx, cy, cz` f64 | 24 | in the attachment frame |
| `quat` f32 × 4 | 16 | full precision on the CPU: the smallest-three 32-bit form is 0.08° per component, which is 1.4 m at the rim of a 1 km blob |
| `rx, ry, rz, k` f32 | 16 | |
| `instance` u32 | 4 | |
| `level` i8, `role/kind/mode/atom` u8, state flags u16 | 4 | state: touched, render-only tier, pending split, … (06) |
| `color` rgb8 + `paint` u8 | 4 | |
| `material` u16, `bone` u16 | 4 | |
| `type` (plugin u16, type u16) | 4 | the owner is the plugin half |
| `body` u32 | 4 | |
| `key` u128 | 16 | |
| `gen` u16 + spare u16 | 4 | |
| hierarchy (06): `parent`, `first_child` u32; `n_children`, `rank` u16 (rank within the family; the full semantic ordinal only feeds the key); `region` u32 | 16 | |
| bound (06): centre offset f32 × 3 + radii f32 × 3 (shares `quat`) | 24 | |
| `err` f32, true volume `vol` f32 | 8 | |
| **total, core columns** | **~148** | |

Plus, per blob:
- the stored fields (09) for sim-tier blobs. For example 8 fields × 4 B = 32 B;
- each plugin's private columns (registered with the core, D11);
- a side table with the moment covariance (6 × f32) only for kind C and D blobs whose representative is not their
  moment ellipsoid (aggregates with a coverage cap, 06 §4.4).

**Option, not taken:**
- Positions as f32 relative to the region origin (12 B instead of 24) would save 12 B a blob. The region origin is an
  exact f64 per region, and f32 at ≤ 8 region edges is ~10⁻⁶ relative.
- It complicates parcels (they cross regions) and every query.
- Revisit if memory binds (Q05.6, `questions.md` C8).

### 4.11 Storage on the GPU (the contract with 12)

Two streams, so the march reads only geometry:

**Geometry record, 32 bytes** (the `BLOBMAP3.md` layout, with the colour word moved out):

| word | content |
|---|---|
| 0–2 | position f32 × 3, relative to the blob's **anchor**: the region (render node) it belongs to (06 §4.3). Anchors are **not rebased**: 12 places each node from its integer region key and per-level camera cells, exactly in integers (R15, 02, 12 §4) |
| 3 | half2(rx, ry) |
| 4 | half2(rz, k) |
| 5 | quaternion, smallest-three: 2-bit index + 3 × 10 bits (≤ 0.24° error, `BLOBMAP3.md`) |
| 6 | instance handle |
| 7 | role (2), kind (2), level (8, i8 + 128), atom (1), **volume flag** (1: a density blob for smoke, dust, powder snow; 12), **render-path hint** (2: raster / march / thin lists, resolved by 12), spare (16) |

**Appearance record, 16 bytes** (read at shading only):

| word | content |
|---|---|
| 0 | rgb8 colour + paint coverage u8 |
| 1 | material u16 + f16 volume density (used when the volume flag is set; otherwise spare) |
| 2–3 | **the modulation word** (§4.4, 09 §4.10): standard slots `snow`, `wet`, `char`, `heat` in word 2; up to four custom slots in word 3 |

**Notes.**
- f16 radii have 11 significant bits: a 2 km blob is represented to ~1 m. That is fine at the distances where 2 km
  blobs are drawn (12's pixel-footprint rule), and the prototypes drew 4 km merged nodes with it.
- 48 bytes against `BLOBMAP3.md`'s 32. M5's blob pools were 274 MB of its 1.21 GB (`BLOBMAP.md`). The appearance
  stream is touched only once per shaded pixel. E2 in §7 measures the cost.

### 4.12 Budgets

| prototype | resident blobs | notes |
|---|---:|---|
| `world/` M5, 16–64 km streamed | 2.6–3.8 M | 1.21 GB GPU (`BLOBMAP.md`) |
| `world/` M5 v3 pyramid | — | 1.70 GB GPU + 0.9 GB CPU node cache (`BLOBMAP3.md`) |
| ray cut | 0.25–2 M (1 M default; the 16 km panorama needs 1.6 M) | 441 MB of bricks at 2 M (`RAYCUT.md` §7) |
| `planet/` | ~21–23 k leaf cells | cells, not blobs |

**Estimates for the game** (06 §4.15 has the per-level counts):

| | blobs | CPU (148 B + ~40 B fields) | GPU (48 B records only) |
|---|---:|---:|---:|
| sim tier around one observer (incl. the ℓ14 sim floor) | 0.1–0.15 M | 20–30 MB | 5–7 MB |
| render-only extension (view-driven; grass and pebbles included) | 0.3–1.5 M | 45–225 MB (no fields) | 15–75 MB |
| **total** | **≤ ~1.7 M** | **≤ ~255 MB** | **≤ ~80 MB** + 12's structures |

Model instances (creatures, the avatar's appearance; §4.16) are not in these counts: assets are shared, about 20 KB
per species (16).

That fits a wasm32 heap (01 owns the memory budget), with room for plugins, fields and caches. The render structures
(bricks, lists) dominate GPU memory, as in every prototype.

### 4.13 Alternatives considered

| alternative | why not |
|---|---|
| Ordered smooth-min plus world carves (the creature model) | not order-independent; breaks spatial indexes and edits (`world/README.md`) |
| Polynomial smooth-min within an instance | not associative; the result depends on the order of blobs in a list |
| Layer-based blending (terrain / objects / detail with fixed radii, M1–M5 v2) | replaced by instances in `BLOBMAP3.md`; layers gave aprons between rocks and grass |
| Per-blob material parameters (the creature's 8 × RGBA32F atom) | 128 B a blob; materials are shared by thousands of blobs |
| Plugin-provided shader functions per material | security, compile cost, a GPU hang risk, and every plugin would maintain WGSL (the planet's "implementing each shape twice" cost) |
| Path keys | grow with depth; 128-bit hashes are fixed-size and good enough |
| Handles as keys (stable slot ids) | slots must be reusable; keys must survive unload, regeneration and other machines |
| One 32-byte GPU record with a 5-bit material | 32 materials can't serve a plugin ecosystem; the colour word has no room for dynamic channels |

### 4.14 Disagreement with README

None on substance. Two clarifications:
- D15 says "never carve blobs in the world". This doc keeps **instance-scoped carves** for modelling (eye sockets).
  They never cross instances and are never used for edits. I read D15 as allowing this, but flag it (Q05.3).

### 4.15 Canonical scenarios

- **Orbit to grass.**
  - Every blob drawn from orbit to the grass uses the same record and blend rule, and 06 picks the level. The
    planet's root blob is one `sheet` blob of radius 500 km in the ground instance.
  - Grass tufts are `detail` blobs: render-only, generated from fields near the camera and rasterised (12, R19).
  - Precision: centres are f64 in the body-fixed frame. On the GPU they are f32 relative to their region, placed by
    integer node keys (12).
- **The beaver dam.**
  - The beaver is a model instance (§4.16). It asks vegetation, through a service, to fell a tree and remove the
    felled log. There is no ownership transfer (R11).
  - The beaver plugin then creates its own `dam_log` content, keys `H(beaver key, tick, seq)`. While dragged, a log
    is a small dynamic body of `rigid` blobs. Its instance is the log's, so it meets the beaver and the ground with a
    hard min.
  - Dropped in the creek, it settles as **kind D placed content** (`keep_instance`, 11) and persists as a placed
    record (15). Its key stays.
- **The asteroid.**
  - The impactor is a body with its own blobs (an instance per body).
  - On impact, the crater is 17's regenerable crater record (a `std:ground_height` term plus `z` changes: kind A).
    The blob store sees it as removals and additions of re-realised plates, in the ground instance.
  - Ejecta are `parcel`-mode blobs with keys `H(impact event key, tick, seq)`. They settle as kind D rocks.
  - Fires show through the modulation word's `char` and `heat` slots.
- **A community plugin.** The "hut" type registers a material row (`hut:thatch`, high `fuzz`, `charrable`) and a type
  (`object`, content kind D for built huts, or B for huts at counted settlement sites). Its blobs are one instance per
  hut. Its flammability is a field answered by owner dispatch (09). No core change is needed.

### 4.16 A second residence: model assets and instances

Working assumption, pending `questions.md` C10 (R10, 16 §4.2). Creatures, and the avatar's appearance, don't put
their atoms in the blob store above. A lion has 989 atoms; 200 animals would be ~100 k blobs re-posed every tick,
churning every spatial structure for no simulation benefit.

| | world blobs (§4.1–4.11) | model instances |
|---|---|---|
| storage | the core blob store: a handle and key per blob, per-blob fields | an immutable **model asset** shared by every individual of a species (atoms with a bone index each, sub-instances, materials, a fixed LOD pyramid built offline by 06's merge), plus per-individual **bone poses** and variation |
| hierarchy | the cut (06) | fixed in the asset; the level is chosen per frame by projected size (render only) |
| fields | per blob | per entity, in 16's thin entity tables (`std:wetness`, `std:burnt`, health) |
| queries (07) | indexed in the world structures | through a coarse **proxy** (one or two posed ellipsoids per bone); exact atoms only on demand |
| hits | a blob handle | a **tagged hit**: (body handle, bone, atom index) |

- **Assets are shared, instances are poses.** An instance costs a pose (bone transforms) and a variation record, not
  blobs. The atom format is this doc's: the same field function (§4.2), roles, carves, paint and materials. Carves
  live only inside assets, within their own sub-instance (16).
- **Hits are tagged.** Every query result and pick says which residence it came from (07 §4.3).
- **Materialisation.** A dead creature the world should absorb (a carcass that decays, a creature caught in lava) is
  copied into the world store as one instance of kind D, owned by its plugin (16). The reverse is not needed.
- The avatar is a dynamic body for physics (11), with a model instance for its appearance.

## 5. Interfaces

Core-side Zig-like pseudocode. Plugins see the C ABI of 08, with batch arrays copied into their own memory.

```zig
// creation (owner plugins, or the core for edits): one batch, one family per parent or free-standing
const BlobSpec = extern struct {
    center: [3]f64, quat: [4]f32, radii: [3]f32, k: f32,
    color: [3]u8, paint: u8, material: u16, role_kind_mode_atom: u8, level: i8,
    type: u32 /* (plugin u16, type u16); own types only */, instance: u32, bone: u16, ordinal_sub: u16,
    ordinal: u64,                 // semantic ordinal (split; the ground: S2 cell id, sheet in ordinal_sub)
                                  // or seq (runtime creation): feeds the key
};
// plugin-facing names (08 §5): ss_blob_create, ss_blob_destroy, ss_blob_get_*, ss_blob_find, ss_instance_new, …
fn blob_create(body: u32, creator_key: u128, specs: []const BlobSpec, out_handles: []u32) Status;
fn blob_destroy(handles: []const u32) Status;              // world edits go through 15's calls instead

// accessors: gather columns for a batch (the "accessors, not arrays" rule)
fn blob_get_geometry(src: []const u32, out_center: [][3]f64, out_quat: [][4]f32, out_radii_k: [][4]f32) Status;
fn blob_get_meta(src: []const u32, out: []BlobMeta) Status;   // level, type, instance, role/kind/mode, body, bone
fn blob_get_keys(src: []const u32, out: []u128) Status;
fn blob_find(keys: []const u128, out_handles: []u32) Status;  // 0 where not loaded
fn blob_set_geometry(src: []const u32, center: [][3]f64, quat: [][4]f32, radii_k: [][4]f32) Status; // owner only

// instances, materials, types
fn instance_new(key: u128) u32;
fn material_register(name: []const u8, params: *const MaterialParams) u16;
fn type_register(name: []const u8, consts: *const TypeConsts) u16;   // per-plugin id; global = (plugin, id)

// the CPU field (07 builds its queries on it; plugins call 07's ss_field_eval, which adds the Lipschitz bound)
fn field_eval(points: [][3]f64, frame: u32, out_value: []f32, out_grad: [][3]f32, out_instance: []u32) Status;

// model assets (§4.16; 16 owns the format): registered once, shared by instances
fn model_asset_register(name: []const u8, bytes: []const u8) u32;
```

**Status.** `Status` is `OK`, `PARTIAL`, `STALE` (some rows), `NOT_OWNER` or `INVALID`. Row-level problems zero the
row and set a bit in an optional per-row status array (07 §5 has the same convention).

**Expected from other docs.**
- 02: `H128`, `rng(seed, key, tick, stream)`, a deterministic `exp` and `ln` in the core's libm.
- 04: body handles and keys, frame transforms in f64.
- 08: glue that copies batches between plugin memory and core memory, and that maps `(handle, gen)` in debug builds.
- 09: the modulation word's slot mapping (`std:snow_cover`, `std:wetness`, `std:burnt`, `std:emission_temperature`,
  plus custom slots), re-encoded from commit dirty lists.
- 12: render nodes (06's regions) with integer node keys; the final GPU structures built from the two records;
  volume blobs; render-path resolution; drawing model instances.
- 16: the model asset format, poses, and the entity tables that hold per-entity fields.

## 6. Risks and unknowns

- **Surface inflation and gradient bounds.**
  - `k ln N` inflation and the `K/k_min` gradient are properties of the blend. Every generator has to calibrate
    against them; the first terrain was 1.1 m high.
  - If plugins ignore this, their content floats or sinks against other plugins' content.
  - Mitigation: a calibration tool in the SDK (19), plus the generator error estimates of 06.
- **Instance-scoped carves vs LOD.** A carve that is dropped when its instance is restricted makes the coarse version
  slightly fatter (eye sockets filled in). Invisible at the distances involved, but unmeasured.
- **48-byte GPU records** may cost more than expected in bandwidth-bound passes. See E2.
- **u16 material ids.** Plugins registering per-variant materials (every tree species × season) could exhaust 65 k.
  Unlikely, but the registry should report usage.
- **Handle non-determinism** is a trap for plugin authors: sorting by handle "works" locally and diverges across
  machines. The SDK should make the deterministic path the easy one: batches arrive in key order, and a lint for
  handle comparisons.
- **The f64 CPU footprint** (24 B a position) is the largest single column.

## 7. First experiments

| # | experiment | success criterion |
|---|---|---|
| E1 | CPU field evaluator in the core language (scalar and SIMD128; f64 centre offset, then f32 local maths), with the 02 libm. 1 M random evaluations, fingerprinted | bit-identical fingerprints in Chrome, Firefox, Safari and Node; throughput ≥ 50 M blob evaluations/s per core with SIMD (a target, not a measurement) |
| E2 | `world/` M7 with a 32 + 16 B split record instead of 32 B (dummy appearance stream) on the 16 km world | frame time within +5% in all six views |
| E3 | Rebuild the bear and the human with instance-scoped carves (eyeballs, teeth as their own instances) | ≤ 1% of pixels differ by > 16/255 from the ordered-smin renders of `renders/iter10` |
| E4 | Handle churn stress: 10⁶ random create/split/merge/destroy operations with plugin-side handle retention across ticks | zero reads of a different blob through a stale handle (debug generations); release build overhead of the quarantine < 1% |
| E5 | Key derivation on a ray-cut terrain descent: regenerate a region 100 times in random arrival orders | identical key sets every time; no collisions in 10⁹ generated keys |

## 8. Build order

| milestone | what of this doc |
|---|---|
| M0 | blob columns, handles + generations, keys (`H128` from 02), `field_eval` on the CPU with Node tests (E1, E5) |
| M2 | GPU geometry + appearance records, the material table v1 (a few built-in materials), instances, `kind` routing (E2) |
| M3 | type registry and material registration from plugins; batch accessors through the plugin glue; key-order batches |
| M4 | dynamics modes (`rigid` for thrown and detached objects, `parcel` for the fast layer), the avatar as a model instance, the modulation word |
| M5 | carves and paint roles (for model assets, E3), the modulation word driven by fields; model instances for creatures (16) |

## 9. Open questions

- **Q05.1 Plugin shaders?** Should plugins ever supply GPU code for materials (a WGSL function, a node graph)?
  (merged into `questions.md` C4)
  - *Why it matters:* it decides how exotic plugin visuals can get, and whether the renderer has to sandbox and
    compile third-party shaders.
  - *Default:* no. Data-only materials plus the modulation word's slots now. Revisit with a data-defined material
    graph (no code) after M6.
- **Q05.2 A per-blob second colour?**
  - *Why it matters:* 4 bytes on the GPU (a spare word exists) and a little CPU.
  - *Default:* no. Material `col2`, paint and cell-paint blobs cover the giraffe case.
- **Q05.3 Instance-scoped carves for modelling while world edits stay removals: is that within the spirit of D15?**
  - *Default:* yes.
- **Q05.4 GPU records of 32 + 16 bytes instead of `BLOBMAP3.md`'s single 32 bytes?**
  - *Why it matters:* materials beyond 32 and field-driven appearance need the room.
  - *Default:* yes, pending E2.
- **Q05.5 Should different instances ever blend with a small contact radius** (a rock settling into soil with a 3 cm
  fillet)?
  - *Why it matters:* a per-instance "contact k" changes the cross-instance rule from a hard min to a polynomial
    smooth-min with the ground only. It costs a little in every accumulator.
  - *Default:* no (hard min), as `BLOBMAP3.md` preferred. Revisit if contacts look pasted-on.
- **Q05.6 f64 positions on the CPU (24 B) or f32 relative to region origins (12 B)?** (merged into `questions.md` C8)
  - *Default:* f64. Simpler, and the 12 B only matter above ~5 M resident blobs.
