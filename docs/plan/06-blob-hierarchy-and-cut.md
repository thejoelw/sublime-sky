# 06. Blob hierarchy and the cut

Status: draft · Depends on: 02, 04, 05 · Used by: 07, 08, 09, 10, 11, 12, 14, 15, 16, 17, 18

## 1. Purpose and scope

This is the heart of the engine. It decides:
- how blobs of different sizes relate (levels, parents, children);
- how a coarse blob turns into finer ones (split) and back (merge);
- how geometry and state stay consistent across levels (restrict and lift);
- which blobs are loaded at any moment: **the cut**;
- who decides the cut, and how it stays deterministic while the renderer still gets view-driven detail.

**In scope:**
- levels, the region tree, the blob tree, the split protocol, merges;
- the four content kinds and how each stays consistent across levels (field projection, moment projection, counts,
  placed content);
- representatives, bounds and error;
- the two tiers of the cut, requesters, budgets and eviction;
- forced refinement;
- the ground's vertical structure (a boundary representation);
- structure lifting;
- the numbers.

**Out of scope:**
- what a blob is: [05](05-blob-model.md);
- field schemas and their aggregation rules: [09](09-fields-and-composition.md); this doc applies them;
- process bands and scheduling: [10](10-simulation-and-scheduling.md);
- the GPU structures that draw the cut, and the feedback marks: [12](12-rendering.md);
- the terrain generator's algorithm: [14](14-planet-generation.md);
- the persistence format: [15](15-persistence.md); this doc defines what a residual *is*.

## 2. Requirements

**Must:**
- The user's model: a non-atom blob can be broken into blobs of the level below, and the parent is a good
  approximation of its children. The same loaded hierarchy serves simulation, physics and rendering.
- Every potential blob is loaded or has exactly one loaded ancestor (the README's "cut").
- A split's result is a pure function of persisted state. So the same region regenerates identically whatever order
  the player arrived from (the `planet/` neighbour-invariant lesson: 1,054 spurious residuals → 0).
- Consistency: restrict ∘ lift = identity for every stored field (D8). The same holds for geometry, as far as it is
  defined (§4.4).
- Splits and merges are local: an edit or a split changes nothing beyond a bounded halo (`M7.md` §7, "edits were not
  local").
- The sim's view of the world doesn't depend on where the camera looks (D5; pillar 5's observer effect).
- Near-infinite view distance with memory independent of world size (`BLOBMAP.md`: 1.2 GB for 4–64 km).
- Full 3D: overhangs, caves, digging below the surface shell (`world/README.md` #4; `BLOBMAP3.md` "digging is
  shallow").
- Isotropy: no grid visible, from blob placement or from merging (`BLOBMAP3.md` jittered sites).

**Should:**
- A new view arrives in about a second (`RAYCUT.md`: 0.9 s after a 180° turn with prediction).
- The budget decides memory and generation time, not frame time (`RAYCUT.md` §7).
- Plugins can't break consistency (`PLUGINS.md`: "no plugin can break consistency").

**Later:**
- Network peers as requesters (idea 8).
- Closure calibration (`DESIGN.md` §8).

## 3. What the prototypes taught

| finding | source |
|---|---|
| Leaves own the state; inner cells are caches recomputed by restrict | `planet/DESIGN.md` §2.2 |
| Lift = prior + exact projection, so `R(L(x)) = x` to 1e-15; residuals lie in the null space of `R`, so they survive any later parent change | `DESIGN.md` §1, §2.3 |
| A refined cell's same-level neighbours must exist, or the lift depends on arrival order (1,054 spurious residuals → 0); this implies 2:1 balance | `DESIGN.md` §2.2, §4.1 |
| A prior may read only persisted state; a prior must agree with what the coarse level assumes (lifted depressions dug 35 km³ of lakes) | `DESIGN.md` §2.3 |
| Structure (rivers) must be lifted, and restricted (outlets), not just numbers | `DESIGN.md` §2.5 |
| Discrete things: ranked candidates with fixed sites and ranks; incumbency; overrides for edits | `DESIGN.md` §2.6 |
| Only touched subtrees plus a halo store residuals; untouched neighbours of a touched cell regenerate "leaning into the hole" unless stored | `DESIGN.md` §2.7, §4.8 |
| White noise with per-parent mean removal draws the parent grid; continuous detail inside the lift fixes it | `DESIGN.md` §6 |
| Merging real blobs (moment-matched, true volumes) keeps every level a function of the source: an edit is just more source | `BLOBMAP3.md` §3, §5 |
| Grid-cell merging draws a lattice on far forests; jittered sites fix it for 2–7% | `BLOBMAP3.md` §3 |
| Coarse levels need volume: shells sample through; keels, not domes | `world/README.md` #4, `BLOBMAP3.md` §3 |
| Merged scattered trees became ground-covering plates; a coverage cap fixes it | `M7.md` §1 |
| Merge origin snapped to 256 m plus canonical group order make edits local (9,815 → 185 changed pixels) | `M7.md` §7 |
| Projected-error cuts work; pixel-count feedback cuts churn, livelock and drop geometry that only children would show | `BLOBCUT.md` |
| A per-node view-dependent cut re-bakes ~7× more than fixed levels | `BLOBMAP2.md` |
| Top-down generation with exact enclosure (0 failures); any cut is a superset of level 0 | `RAYCUT.md` §2 |
| Rendering the enclosing superset looks wrong (surfaces 0.8–1.5 cells high) and needs "lift compensation" | `RAYCUT.md` §2–3 |
| One error per node didn't converge; per-blob errors spread over bricks did | `RAYCUT.md` §2 |
| Eviction is the only merge; unseen first, prediction-only next, then by the parent's expected error; a group is evictable only after a readback has seen its children; rays mark the leaves they pass through | `RAYCUT.md` §4–5 |
| Capacity must be sized for the largest budget, and eviction must make room for slots and bricks too, or subdivision stalls | `RAYCUT.md` §11 |
| Generation throughput limits convergence (1–2 k splits/s, ~5 ms per 400-blob family in JS), not the GPU | `RAYCUT.md` §9 |
| Processes written as rates on restricted state, at their band, avoid observer effects; parcels sized by observer distance are an observer effect by construction | `DESIGN.md` §2.4, `PLUGINS.md` §5 |

## 4. Design

### 4.1 Overview: one hierarchy, two structures, two tiers

```
 body ─ region tree (spatial octree, body-fixed frame)          blob tree (logical, owner-defined)
          R(ℓ=17) ─ ... ─ R(ℓ) ─ 8 × R(ℓ−1) ─ ...                root blob ─ ... ─ B(ℓ) ─ children(ℓ−1) ─ ...
          holds the blobs of its level; the unit of               parent/child links; restrict, lift, keys,
          splitting, balance, neighbourhoods, persistence          morph parents, residuals

 cut = the loaded part, in two tiers:
   sim tier     deterministic: observers' positions, physics contacts, events, (later) peers    → simulated, persisted
   render tier  a render-only extension: GPU feedback, prediction, camera transitions          → drawn only, droppable
```

- **The blob tree** is what the user sketched. Every blob except a body's root has one parent one level coarser. The
  owner decides how a blob splits. Restrict, lift, keys, residuals and the renderer's morph all follow it.
- **The region tree** is a spatial octree per body. A region of level ℓ holds the blobs of level ℓ whose centres it
  contains.
  - Regions are the unit that splits: when a region splits, every owner with splittable blobs in it splits them, in
    one batch.
  - Regions carry the neighbour invariant and 2:1 balance, and they define "the neighbourhood" a split may read.
  - Every prototype split groups, never single blobs: cells in `planet/`, nodes in M7 and the ray cut.
- **The cut** is the set of loaded regions and blobs. It has two tiers (§4.8):
  - a deterministic **sim tier** that everything simulated and persisted lives on;
  - a **render tier** that only refines further where the camera needs detail.

### 4.2 Levels

**Definition.** A blob's level ℓ is an integer size class. **Generated blobs and atoms satisfy
`2^ℓ ≤ r_max < 2^(ℓ+1)`**, where `r_max` is the largest semi-axis, in metres.
- Why the largest radius:
  - level of detail decides by projected size, and a thin plate 10 m wide is seen as 10 m;
  - "interact at the level of the finest participant" (§4.11) compares extents;
  - the cube root of the volume would put a 10 m × 10 m × 1 cm grass sheet with 1 m pebbles.
- Thinness is a separate property. The renderer's "thinner than a sample" test (`BLOBMAP.md` #3) uses the smallest
  radius.
- **Merged and restricted blobs** inherit `ℓ = child ℓ + 1` even when their computed `r_max` drifts. The core accepts
  `r_max ∈ [2^(ℓ−1), 2^(ℓ+2))` and logs anything outside as a plugin quality metric.
- **Nominal levels per family.** An owner may declare a family's level instead of letting each blob's size decide it.
  The ground does this: plates at S2 depth d have ℓ = 20 − d on the reference planet ([14](14-planet-generation.md)
  §4.3), even though the tangent projection makes cell edges vary by ±19%. The same tolerance band applies.

**Branching.** One level is a factor 2 in length. A split therefore yields about 8 children for volumes, 4 for sheets
and 2 for lines. Branching is the owner's choice.
- Children are **exactly one level finer**, except atoms, which may be any finer level (a level-5 blob may have a
  level-1 pebble atom as a child).
- Families above 64 children are allowed but warned about: they defeat the batch economics of a split.

**The level table** (the single one; other docs refer here). Reference planet R = 500 km. Ground plates sit on 14's
S2-style cube-sphere: depth d has a nominal cell edge `e_d = 785.4 km / 2^d` and plates of in-plane radius ≈ 1.7 e_d,
so **ℓ = 20 − d** ([14](14-planet-generation.md) §4.3).

| ℓ | r_max | ground depth d | cell edge | ground cells on the planet | named level / role |
|---:|---|---:|---:|---:|---|
| 18 | 262–524 km | — | — | 1 | the planet blob, root of the ground hierarchy (the README glossary said ~19) |
| 17 | 131–262 km | 3 | 98 km | 384 | the first plates (orbit view); depths 0–2 are not realised |
| 16 | 66–131 km | 4 | 49 km | 1,536 | storage regions (15) |
| 15 | 33–66 km | 5 | 24.5 km | 6,144 | |
| **14** | 16–33 km | **6** | **12.3 km** | **24,576** | **`ℓ_sim_floor`**: the generated top level, always loaded and simulated on activated planets (`planet/`: 10 km) |
| 11 | 2–4 km | 9 | 1.5 km | 1.6 M | |
| **10** | 1–2 km | **10** | 0.77 km | 6.3 M | **`ℓ_view_floor`**: the orbit view's render cap (18) |
| 7 | 128–256 m | 13 | 96 m | 403 M | the vegetation band (`planet/`: 157 m) |
| 5 | 32–64 m | 15 | 24 m | 6.4 G | entity depth: tree candidates (`planet/`: 20 m) |
| 0 | 1–2 m | 20 | 0.75 m | 6.6 T | finest ground level, cheaper option (Q06.7) |
| **−1** | 0.5–1 m | **21** | **0.37 m** | 26 T | **finest ground level, default**: the ray cut found 0.5 m cells already coarse near the camera |
| −3 | 12–25 cm | — | — | — | grass tufts and pebbles: detail atoms, render tier only, generated from fields (R19) |
| −6 | 1.5–3 cm | — | — | — | creature details: inside model assets, not world blobs (§4.10) |

- From `ℓ_sim_floor` (ℓ14) down to ℓ−1 is 15 levels of surface refinement. `planet/` had 14 levels, covering half
  the radius.
- The observer ring is **K = 8 blob sizes of the band's level**, which is one region edge `E_ℓ` (§4.3, §4.8).
- `ℓ_sim_floor` applies to **activated** planets (10 defines activation: an embark or an event). Generating a planet's
  top level when its system is entered (14) is separate from activation.
- Working assumption pending `questions.md` C22: ℓ14 floor. Pending Q06.7: ℓ−1 finest.

### 4.3 The region tree

**Shape.**
- Each body with blobs has a region tree: an octree in its **body-fixed frame** (04).
- A region of level ℓ is a cube of edge `E_ℓ = 2^(ℓ+3)` m, about 8 cells of the level across. So a surface passes
  through ~64 cells, giving ~64 sheet blobs per family per region.
- The root region is the smallest region centred on the body that contains the body's bound. A 500 km planet's root
  region is ℓ = 17 (edge 1,049 km).
- **Regions are also the render nodes.** 12 adopts this edge and states its brick sample spacing relative to it, and
  uses the region key as its integer node key (12 §4, R4, R15). One structure serves splitting, balance, persistence
  grouping and drawing.

**Region key.** `(ℓ: i8, x, y, z: i32)`, integer cube coordinates relative to the root. Neighbours are coordinate
arithmetic. Keys are packed in a u128 with the body handled separately.
- Coordinates fit easily: ℓ = −1 under a level-17 root needs 18 bits per axis.

**Membership.** A blob belongs to exactly one region.
- **Children of the blobs in region R go to the child region of R that contains the child's centre.** If the centre
  lies outside R (a plate reaching over the edge, a crown overhanging the region), the child is clamped into R's
  nearest child region. Membership is logical. Spatial queries use bounds, not membership (07).
- **Atoms stay in their own region when it splits.** An atom of level ℓ in a split region is still a leaf of the cut.
  So a split region can hold atoms.
- **Placed content (kind D)** is attached to the region containing its centre at its pyramid root's level. Where
  that region is split, the pyramid's finer levels show in the child regions, down to the cut's level (§4.6).
  Generated content (kinds A–C) created in an already split region (a plugin spawning a kind C rock near the player)
  is split on creation, down to the cut's level there, by the owner's split as usual.

**The cut invariant, on regions:**
1. If a region is loaded, so are all its ancestors, and they are split.
2. If a region is split, all 8 of its children are loaded.
3. **Neighbour invariant:** if a region of level ℓ is split, its 26 same-level neighbours are loaded (leaf or split).
   This is `planet/`'s invariant in 3D. It makes a split's inputs independent of arrival order, and it implies 2:1
   balance: adjacent leaf regions differ by at most one level.
   - **It guarantees the ground's 3×3 cell stencil.** The ground's lift reads a plate's 8 S2 neighbours at the same
     depth (14 §4.6). Their plate centres lie within ~2 · 2^ℓ of the plate's own: ≤ 1.5 e_d, times the tangent
     projection's ±19%. That is a quarter of a region edge (`E_ℓ = 8 · 2^ℓ`). So they lie in the plate's region or
     one of its 26 neighbours, all of which are loaded at level ℓ.
4. A blob in a split region is an atom or has all its children loaded. A blob in a leaf region is a leaf of the blob
   tree.

**Splitting a region** therefore may first split neighbours' parents, recursively. Prerequisites are added to the same
batch in a deterministic order (§4.8). **Merging a region** (dropping its children) is allowed only if none of its
children is split and no split region of level ℓ−1 has one of its children as a neighbour (`planet/`'s merge rule).

**Two spatial structures, two jobs** (R4; working assumption pending `questions.md` C8):
- **The core's octree of regions** (this section) is generic. It does splitting, balance, the neighbourhoods a split
  reads, render nodes and persistence grouping, for every body.
- **The ground's S2-style cube-sphere** (14 §4.2) is the ground plugin's *surface index*: cell ids, semantic ordinals
  and keys (R3), the hydrology graph, cell areas. Its maths ships as a core/SDK library, so any surface-bound plugin
  can use it. Ground plates live in regions by their centres, like any blob.

Why the core's structure is an octree and not `planet/`'s cube-sphere quadtree:
- **Full 3D.** Caves, overhangs, sheets at several radii in one column, floating rocks and detached bodies need
  volume regions. A quadtree over the surface would need a second structure for the vertical.
- **One structure for every body.** An asteroid, a moon and a detached kilometre-sized rock use the same code.
- **Grids don't show.** Region boundaries are logical; what the player sees is blobs. Blob positions are jittered by
  the generators (14), merges use jittered sites, and level transitions morph (12).
- **Cost.** About half the regions near a surface are mostly air or mostly solid, and they are cheap (few or no
  blobs). Measured on the ray cut's octree: ~0.13 node slots per blob converged (`RAYCUT.md` §11).

**Region record** (core-owned, ~64 B): key, state (leaf / split / pending), tier, first-child index, per-owner blob
lists (handles), bound (the union of its blobs' bounds), flags (touched, halo, pinned), the sim requester's expiry
tick, the render tier's last-seen frame and q values.

### 4.4 Geometry across levels: four kinds of content

Where a blob's geometry comes from decides how its levels stay consistent. 14 §4.8 defines four **content kinds**;
every owner declares one per blob type (05 §4.5). Working assumption pending `questions.md` C21.

| kind | examples | geometry comes from | consistency across levels | the parent of a touched family | an edit persists as (15) |
|---|---|---|---|---|---|
| **A. field-derived** | ground plates, lake and river water sheets, snow cover | realised by the owner from its fields plus terms (14 §4.7) | the **field** projection (mean `z` etc., §4.14); no geometric projection | re-realised from its restricted fields | field residuals (`z`, water…) plus 3D edit terms |
| **B. counted** | trees, bushes, boulders, burrows, grass counts | one individual per active candidate site, from (candidate key, species, age) | count fields restrict and lift exactly; ranked candidates with incumbency (`DESIGN.md` §2.6); no geometric projection | coarse canopy representatives re-realised by the owner's closure from the counts (14 §4.10) | candidate overrides plus living-set snapshots |
| **C. shaped** | rock outcrops, arches, cliff lumps, coral heads, blob models split from a parent shape | the owner's prior from the parent's shape | the **moment projection** below (idea 5) | moment restrict of its children | removals (read by the prior) plus per-key shape residuals |
| **D. placed** | logs, dams, player-built things, settled objects, ejecta rocks | explicit records; never produced by a split | its own small merge pyramid (the object's LOD) | — (never in a lifted family) | the whole placed record, with a minted key |

**Kind D never enters a lifted family.** A placed log inside a kind-C family would put its volume into the parent's
moments. A re-lift would then inflate the natural children to carry that volume, and the record would add the log
again: a double count. So placed content has its own pyramid. The renderer merges it with its surroundings at far
distances; the sim never does (§4.6).

The rest of this section is the machinery for **kind C** and for **kind D's pyramids**. Kind A gets its consistency
from the two-phase split (§4.7), kind B from its counts.

**Moments.** For kinds C and D, the geometric state of a blob is its moments:
- the true volume `V` (sum-aggregated);
- the centroid `c`;
- the covariance `Σ` (the central second moment per unit volume).

For an atom, `V` is its ellipsoid volume `4/3 π r₁r₂r₃` and `Σ = R diag(r²/5) Rᵀ`: the solid ellipsoid's moments, as in
`world/js/lod.js`.

**Restrict (children → parent)** is exact pooling, as `lod.js` does it. It is computed about the parent's centroid for
precision; `lod.js` used a 256 m-snapped local origin:

```
V_p = Σ V_i
c_p = Σ V_i c_i / V_p
Σ_p = Σ V_i (Σ_i + (c_i − c_p)(c_i − c_p)ᵀ) / V_p
```

For moments, this *is* the "sum" aggregation of 09. Volume is extensive, and `(V c, V(Σ + c cᵀ))` are extensive
too. Geometry is therefore restricted exactly like any stored field.

**The representative** is what is drawn, simulated and touched at a blob's level. For kinds C and D it is **derived**
from the moments, by a transform chosen by the blob's render kind (05 §4.7). The transform is never fed back into the
moments: it is the `lod.js` "output transform, never fed back" rule.

| render kind | representative | reason |
|---|---|---|
| `object` (solid) | the moment ellipsoid (radii `√(5 eig Σ)`), scaled down to at most 2.5 × `V` (`lod.js` `fluff`) | sparse parts must not become a slab |
| aggregate (an `object` family the owner marks porous: rock fields, a pile of logs) | the moment ellipsoid with the **coverage cap** of `M7.md` §1: each axis's projected area set to what the parts cover, `C = S(1 − Π(1 − aᵢ/S))`, never above the moment radii | merged scattered parts otherwise cover the ground |
| `detail` | dropped above the type's `detail_max_level`; its volume still pools into the parent's moments | sub-pixel at coarse levels (`BLOBMAP2.md`) |

The coverage cap needs per-axis sums over the parts, taken along the *parent's* axes. So it is computed when the
children exist (as in `lod.js`), stored as the parent's representative radii, and kept in a side table (05 §4.10).

For **kind A**, the representative is 14's realised plate (14 §4.7). Its top sits on the level's surface after a
calibrated `β · k · ln N` correction for the blend's bulge. Its thickness and keels keep every level closed. This
replaces the ray cut's lift compensation. For **kind B**, the coarse representatives are the owner's closure: the
expected merge, moments plus coverage, of the individuals a lift would produce.

**Lift (parent → children) for kind C = owner prior + core projection** (idea 5).
1. The owner proposes children: the prior (§4.7).
2. The core projects them so that their pooled moments equal the parent's moments exactly. Order matters: weights,
   then translation, then the affine fit.
   - **Volume:** `Vᵢ ← Vᵢ · V_p / Σ Vⱼ`. This rescales weights only; centroid and covariance are unchanged, because
     they are volume-weighted averages.
   - **Centroid:** `c_u = Σ Vᵢ cᵢ / V_p`. Translate every child by `c_p − c_u`.
   - **Covariance:**
     - `Σ_u = Σ Vᵢ (Σᵢ + (cᵢ − c_p)(cᵢ − c_p)ᵀ) / V_p`.
     - `A = Σ_p^{1/2} Σ_u^{−1/2}`, with symmetric positive square roots. This is the closest-to-identity choice; any
       `Σ_p^{1/2} Q Σ_u^{−1/2}` with `Q` orthogonal also works.
     - Apply it: `cᵢ ← c_p + A (cᵢ − c_p)`, and each child's shape matrix `Sᵢ = Rᵢ diag(rᵢ²) Rᵢᵀ ← A Sᵢ Aᵀ`, then
       re-diagonalise to get the new `Rᵢ, rᵢ`. The `Vᵢ` stay: a child's porosity changes by `det A ≈ 1`.
     - Result: pooled centroid = `c_p` and pooled covariance = `A Σ_u Aᵀ = Σ_p`, exact up to float rounding.
   - A single child gets the parent's moments directly.
3. So **restrict(lift(B)) = B** for kind-C geometry, exactly as for fields. Residuals (§4.6) live in the null space of
   restrict.

**Projection modes by kind** (a type constant, 05 §4.5):

| kind | projection | steps |
|---|---|---|
| A | the **field** projection, then realisation | two-phase split (§4.7): fields lifted and projected first (§4.14), then geometry realised from them; never a geometric projection, which would fight the field one (plate thickness and overlap don't conserve volume across depths, 14 §4.8) |
| B | none on geometry | counts are exact through ranked candidates; positions are authoritative (a tree that moves when you look at it is worse than a canopy that's 2% off) |
| C | `full` (default), or `centroid` | `full`: volume, centroid, covariance. `centroid`: volume and centroid only, for shapes whose spread is authored |
| D | none | not in lifted families at all |

**Diagnostics.**
- For kind C, the core records `‖A − I‖`, `|c_p − c_u| / r_p` and `V_p / ΣVᵢ` per plugin and type. A good prior keeps
  them near 0, 0 and 1. These are the geometric counterpart of the ray cut's clamp counts, and the SDK reports them
  (19).
- For kind A, it records the field projection's correction relative to the level's amplitude. `planet/` measured
  3.5–7% (`DESIGN.md` §6).
- A large correction means the generator's coarse levels disagree with its fine levels: `DESIGN.md`'s "the prior must
  agree with what the coarse level assumes".

**The general rule for generators:** a generated coarse blob predicts what its children restrict to:
- moments for kind C (a coarse lump's moments include the variance of the finer detail it leaves out);
- field averages for kind A (the `nz` cell averages that add up across depths, 14 §4.6);
- expected merges (closures) for kind B (14 §4.10).

### 4.5 Bounds and error (D9)

Every blob carries, beside its representative:

**The bound:** an ellipsoid sharing the representative's orientation (centre offset + three radii, 05 §4.10). For
kind A ground it is instead the cell's **radial prism**: the cell's footprint between two radii,
`R + z + dev_lo − m` and `R + z + dev_hi + m` (14 §4.7). That is far tighter on flat ground, and 07 supports it as a
radial-slab primitive.
- It contains the **surface envelope** of every *potential* descendant: each one's ellipsoid grown by `k ln 8`, the
  most a blend of up to 8 overlapping blobs bulges. The ray cut used `k ln 7 · 1.15`.
- It is what culling, ray traversal, broad phase and refinement discovery trust. Field evaluation uses each blob's
  reach (`3.5 k`, 05 §4.2) separately.

How bounds are set:
- **When a blob's children don't exist**, the bound is declared by the generator in the split output: a normal margin
  and an in-plane margin beyond the representative, from what it knows about finer content (the ray cut's bound `A_ℓ`
  on finer octaves; a tree generator's maximum crown spread).
  - For kind A ground, the margins are the prism's `dev_lo` and `dev_hi`: the analytic bound `A_d` for untouched
    cells, and min/max restrictions of the real descendants for touched ones (14 §4.7).
  - If a type declares nothing, the default is the representative grown by `0.5 r_max + k ln 8`.
- **When a blob's children exist**, the bound must contain every child's bound. The core checks this after each split
  with the ray cut's exact ellipsoid-in-ellipsoid test: the maximum of the parent's quadratic form over the child
  ellipsoid, via Jacobi eigenvalues and bisection on the secular equation, on the stored shapes.
  - On a violation the core **expands** the parent's bound minimally, and its ancestors' (they're always loaded), and
    counts it against the plugin.
  - The ray cut instead *shrank* children to fit. That changes geometry to protect a culling structure. Expanding is
    always safe and keeps the plugin's geometry.

**The error:** `err` (f32, metres) is how far this blob's representative surface can lie from the finest surface its
descendants would show, within its footprint.
- It is declared by the generator (it knows the finer amplitude), and recomputed for touched blobs from their
  children: `err_p = max_i(err_i + dist(child surface, parent representative surface))`, sampled at a few points per
  child, as the ray cut did per blob.
- The render tier's split request uses it, via the ray cut's formula: `q = 64 + ⌈8 log₂(err / (tolerance ·
  footprint))⌉`, where `q > 64` means too coarse here.
- Raycut found one error per node didn't converge, so `err` is per blob. 12 bakes it per texel.

**The insight carried over from the ray cut:** render and simulate the *representative*; trust the *bound* for "could
anything be here". A ray that misses a coarse blob's representative but crosses its bound may be passing something
only its children would show: a spire, a branch tip, the gap the ray cut's merged parent wrongly closed.
- The render tier marks such leaves "possibly hiding detail" (§4.8). This fixes the `BLOBCUT.md` failure where pixel
  counts couldn't discover what an aggregate would reveal.
- The ray cut's enclosure-as-representative needed "lift compensation" to look right. Separating the two removes the
  need.

### 4.6 Merge (restrict) semantics, and residuals

**Interior blobs keep their representative.** While a blob is split, the core keeps its representative, bound, error
and moments.
- These are what a merge restores, and what the renderer morphs children into. Per-blob parent links are `M7.md`'s
  parent maps, for free.
- The ray cut likewise kept each interior node's own content.

**Stored fields** of interior blobs are `R(children)`, recomputed after every sim step for the fields that changed (09,
10). This is `planet/`'s "inner cells are caches".

**Geometry of interior blobs:**
- *Untouched families:* the representative stays the one generated. So merge(split(B)) = B bit for bit, and repeated
  split/merge cycles don't drift. For kind C with `full` projection it equals `restrict(children)` anyway, up to
  rounding.
- *Touched families:* an edit, or a sim process that changed geometry (lava, erosion, avalanches). When children
  change, the core updates the parent and propagates up one ancestor chain. This is `M7.md`'s "every level is
  re-merged from the edited source", done incrementally. How the parent is updated depends on the kind:

  | kind | the touched parent |
  |---|---|
  | A | its fields are restricted (`z`, `dev_lo`/`dev_hi`, …), then the owner **re-realises** its plate from them |
  | B | its counts are restricted; the owner re-realises its closure representative |
  | C | moment restrict; the representative is re-derived by render kind |

  In every case `err` is recomputed and bounds re-checked.
- Measured cost in `M7.md` for reference: levels 0–2 updated within 0.1–0.2 s, level 7 within 2.5–3.3 s (JS, whole
  nodes). The incremental chain should be far cheaper. That is an estimate; see E2.

**Kind D: placed content lives beside the families, not in them** (R7, 14 §4.8).
- A placed object (a log, a dam, a hut, a settled rock, ejecta) is a record with a minted key. Its blobs form its own
  small pyramid: the object's LOD, merged by §4.4's moment rules (coverage cap for piles), and built once per record
  version.
- It hangs off the region tree in the region containing its centre, at the level of its pyramid's root. If that
  region is coarser than the root, the root is shown as a leaf there, like an atom of a finer level.
- The sim uses the object at the cut's level where it lies (finer pyramid levels near observers). Far away, the
  **renderer** merges it with its surroundings or drops it by footprint. The sim never folds it into a ground or
  canopy parent, so no re-lift double-counts it.
- Whatever kind D changes in *another* plugin's state is derived, never folded in. Example: hydrology's `blockage`
  per cell is computed from the placed solids in the cell, and the ground's `z` is unchanged (15).

**Residuals** (`planet/`'s definition, per kind; 15 owns the format). When a touched family is merged, 15 stores what
cannot be regenerated:

| kind | persisted |
|---|---|
| A | **field residuals** per child key, `xᵢ − L(R(x))ᵢ` (`z`, water, …), in the null space of `R`; plus the owner's **3D edit terms** (tunnels, caves) as a resource. A dig changes generator input, so `std:ground_height` reflects it |
| B | **candidate overrides** (felled, damaged, planted, stumps) plus living-set snapshots |
| C | **removals** (read by the prior, so the projection never redistributes a removed lump's volume over its siblings) plus per-key **shape residuals**: the child's moments and representative minus those of a fresh lift (prior + projection) of the *current* parent. With `full` projection these lie in the null space of geometric restrict, so they survive later changes of the parent, like field residuals |
| D | the **placed record** itself |

- **Halo** families (untouched families whose split reads a touched family through the neighbour stencil) store
  their residuals too. Otherwise they regenerate "leaning into the hole" (`DESIGN.md` §4.8).
- **What the blob store sees vs what is persisted.** D15 holds at the level of the store: an edit is blobs removed and
  blobs added, and that is what the renderer and queries see. The *persisted* form follows the kind (R7). The
  general rule (14 §4.8): discrete edits are prior input; continuous edits are residuals.

**Decay.** On return: children = lift(current parent) + residual × `exp(−elapsed/τ)` per field and type (`planet/`:
τ = 400 years for elevation). Removals, overrides, edit terms and placed records don't decay. They change only
through simulation (vegetation regrowth, erosion), which is 10's business.

**Detail and carves** are dropped when their instance is restricted past `detail_max_level` or past the carve's own
level (05). Their volume pools; their shape doesn't. Grass and pebbles are render-tier only anyway (R19).

### 4.7 The split protocol

**Who splits.** When a region R of level ℓ splits, every owner with splittable level-ℓ blobs in R gets **one batch
call** for all of them.
- Owners are called in the topological order of their declared **split reads** (§4.13). For example, the plugins
  contributing shape terms to `std:ground_height`'s phases, and river conditioning to the ground's
  `lift_conditioning` extension point, come before the ground's own lift. This is `planet/`'s `liftReads` order. A
  cycle is a load-time error.
- Batches for many regions can be merged into one call per owner. The region is an input, not a call boundary.

**Inputs** (copied into the plugin's memory by the 08 glue; nothing else is visible). For each parent:
- key, level, type, instance key, representative (centre f64, quaternion, radii, `k`), moments, bound, `err`;
- the owner's private columns (D11);
- the values of the **split-read fields** it declared (09), at the parent level.

**Context:** the same records for the owner's level-ℓ blobs in R's 26 neighbour regions. They exist by the neighbour
invariant, and for the ground they cover the 3×3 cell stencil (§4.3). With the read fields declared by other plugins
(the phases of `std:ground_height`), contributions to the owner's extension points (`lift_conditioning`, 14 §4.6),
and read-only views of declared resources (a river segment network, a volcano list, edit terms, removal sets).

**Header:** body key, the universe seed, the tick (for sim-tier time-dependent priors, rarely needed), the tier.

**Rules for priors** (determinism, from `planet/`):
- **Pure.** The output is a function of the inputs only. No reading of other state, no clock, no randomness except
  `rng(seed, key, …)` (02).
- **Persisted inputs only.** A prior may read only state that is persisted or regenerated identically: fields with
  persistence, resources, keys, hashes. It must not read volatile fields (a discharge that is recomputed differently
  after an unload). `planet/`: lifting tree counts by capacity read regenerated fields, and the residual brought back
  the wrong trees.
- **Semantic ordinals.** Each child carries an ordinal that names *what* it is (a child-cell index, a candidate-site
  index, a part number), never its emission index. Keys are `H(parent key, ordinal)` (05 §4.8). So the same child
  keeps its key when a state change alters which siblings exist.
- **Shared boundaries from shared inputs.** Anything two neighbouring families must agree on is derived from the
  boundary itself or from the coarser parent, never from one side's state. Examples: a river's crossing point on a
  shared edge, a shared plate between two cells. This is `planet/`'s "crossing points are hashed from the edge
  itself".
- **Agree with the coarse level.** A coarse blob predicts what its children restrict to (§4.4). Lifted detail is in
  equilibrium with its parent: lifted depressions route water through (`DESIGN.md` §2.8, rule 2).
- **Ground ordinals are S2 cell ids plus a sheet index** (R3, 14 §4.7). A ground child's key is
  `H(parent key, (cell id, sheet))`, so the key of a column's surface sheet doesn't change when a cave adds sheets
  below it.

**Outputs:** child specs (05 `BlobSpec` plus, per child):
- a parent index and a semantic ordinal;
- bound margins (normal, in-plane) and `err`;
- a true volume (0 = its ellipsoid volume);
- an instance reference (inherit the parent's, new by ordinal, or an existing key);
- initial private columns.

**Two phases or one, by kind** (R7, 14 §4.7). The core's steps run in this fixed order:

*Kinds A and B (field-derived, counted): two phases.* Their geometry is a function of their fields, so fields come
first.
1. **Phase 1, the owner:** it proposes children's ordinals and **field priors**: `z`, `rough`, counts…, with
   `lift_conditioning` contributions applied (14 §4.6).
2. **The core:** sorts children by (parent key, ordinal) and computes keys. It applies the **field projection** (exact
   restrict ∘ lift, §4.14), then the decayed **field residuals** (15). It also lifts every other plugin's stored
   fields on these blobs (09), in the order of their declared lift reads.
3. **Phase 2, the owner:** it **realises** geometry from the final fields, plus edit terms (sheets) or candidate
   overrides (B), into plates, closures or individuals (14 §4.7, §4.10). There is no geometric projection.

*Kind C (shaped): one phase.*
1. **The owner:** returns child shapes. Its prior reads the persisted **removal set** for these parents and omits
   removed children, so nothing redistributes their volume.
2. **The core:** sorts and computes keys, applies the **moment projection** (§4.4), then the decayed **shape
   residuals**, then lifts the stored fields (09).

*Kind D (placed):* never split by this protocol. Its pyramid levels come from its record (§4.6). **Edit additions are
kind D, and are never projected with a family.** This replaces step 3 of the first draft, which projected additions
together with generated children, and double-counted them on re-lift.

*Then, for every kind:*
- validate bounds (§4.5);
- assign regions (§4.3);
- allocate family blocks (§4.9);
- notify interested plugins (`on_split` hooks, batched; entities use this to instantiate individuals, 16).

**Asynchrony.**
- Every step is pure given its inputs, and so is the plugin's split. So splits run in **plugin worker instances**,
  plus a core worker for projection and lifting (01, 08). The two phases of kind A are two calls into the same pure
  worker instance.
- The **render tier** applies results whenever they arrive.
- The **sim tier** uses **deterministic latency** (idea 2): a split requested at tick T is applied at exactly
  `T + L_split`, with **`L_split` = 3 ticks** (100 ms at 30 Hz; 06 owns it, R1). If the job isn't ready, the engine
  stalls rather than diverge. (10 owns the per-process async latencies; climate and hydrology are `async(30)`.)
- Inputs are snapshotted at T, so later state changes during the latency don't leak in.
- An embark is the one big exception: its whole observer ring is requested at once, for a **pending observer** (§4.8)
  at the destination. It is a sim event that completes when the convergence query (§5) says the ring has landed,
  with sim time held, and the zoom animation covers it (idea 6, 18).

### 4.8 The cut: two tiers

This section evaluates and adopts idea 1, with changes.

**Why two tiers.** The user wants one loaded hierarchy for sim, physics and rendering. The ray cut showed that
view-driven refinement gives the best image per byte: a 180° snap goes from 63% of pixels over 4× too coarse to
0.05% with prediction. But a view-driven cut makes the *simulation* depend on where the camera looks:
- Two players at the same spot looking different ways would simulate different blobs.
- A replay with a different camera would diverge.
- `planet/`'s measured observer effect (3–36% p50 differences for nonlinear processes resolved finer) would follow
  the gaze, not just the position.

So the hierarchy stays one, and the cut gets two tiers:

| | sim tier | render tier |
|---|---|---|
| requesters | the sim floor of activated planets; observer rings (the avatar, pending observers); physics contacts; events; later network peers | GPU feedback (the ray cut's q marks); prediction rays; cameras, camera transitions and prefetch (18); picking |
| deterministic? | yes: a function of state at tick boundaries, applied at fixed latency | no: depends on the view and on timing |
| budget | `B_sim`: a game constant, the same on every machine | `B_render`: per machine (blobs, node slots, bricks) |
| merges | requester expiry + hysteresis, in a deterministic order | eviction, with the ray cut's rules |
| simulated (processes, physics) | yes | no |
| visible to sim queries (07) | yes | **no** |
| persisted, residuals stored | yes (touched only) | never |
| fields | full stored fields | lifted from the nearest sim-tier ancestor for appearance only (`std:wetness`, `std:snow_cover`, `std:burnt`, `std:emission_temperature`, …; R12); refreshed when that ancestor's fields change past a threshold, at a low rate |
| geometry | split by the owner (the full pipeline of §4.7, persisted forms included: residuals, edit terms, overrides, placed records; so edits are visible far away) | the same pipeline, so the same result |

**Render-only blobs are a deterministic extension, not a different world.**
- They come from the same split function, applied to the same sim-tier state.
- When the sim tier later wants a region the render tier already split (the player walks there), the render-only
  children are **promoted** in place if their inputs' state version matches: flip the tier flag, allocate field rows,
  apply field lifts.
- Otherwise they are regenerated. Either way the result equals a fresh sim split, so promotion can't break
  determinism. (Promotion is an optimisation; correctness never relies on it.)
- **When the sim changes a sim-tier blob's geometry** (an edit, lava, erosion), its render-only descendants are
  invalidated and regenerated lazily. Field-only changes just refresh appearance.

**Sim-tier requesters:**

1. **Sim floor.** On every **activated** planet (10 defines activation: an embark or an event), every region of
   level ≥ `ℓ_sim_floor` is loaded.
   - ℓ_sim_floor = ℓ14 on the 500 km reference (depth 6; pending `questions.md` C22). That is ~32.6 k ground plates
     (24,576 at depth 6 plus 8,064 at depths 3–5 and the planet blob), plus ~11 k canopy closures on land and a few
     thousand lake and river water plates. **About 45–55 k blobs.**
   - The ocean is not blobs at this level: an analytic sea-level sphere plus dynamic water (R18, pending C20).
   - Other bodies keep only their root, or what 10 decides for background simulation. Generating a system's top
     levels on entry (14) loads them for drawing, but does not activate them.
   - The floor is `planet/`'s "the top level always exists, so the whole planet is always simulated".
2. **Observer rings.** The sim observers are the avatar, plus **pending observers** during an embark (R9). Both are
   sim state; cameras are not (orbit, free-fly, photo mode: they make render-tier requests only). For each observer o
   and each level ℓ from `ℓ_sim_floor` down to `ℓ_fine(o)`, split every region of level ℓ within `ρ_ℓ = K · E_ℓ` of o.
   - **K = 8 blob sizes of the level, which is one region edge** (`ρ_ℓ = E_ℓ`). This is `planet/`'s K = 8 cells.
     ℓ_fine is the finest level any process band or physics needs near observers: splitting ℓ0 regions gives ℓ−1
     ground near the player.
   - Merge beyond `1.35 ρ_ℓ` after `H_sim` ticks: `planet/`'s 1.35 K hysteresis.
   - A pending observer is added with `eng_observer_pending_add` at the embark destination (§5). It refines like the
     avatar's, and it becomes the avatar's observer when the embark completes (18).
   - Distances are 3D, so deep regions under the player stay coarse.
   - Process bands *clamp* how fine a process runs (10); they don't request refinement. A band-157 m vegetation
     process runs on whatever the rings refined, clamped.
3. **Physics contacts** (§4.11): split regions overlapping the contact zone down to the finest participant's level,
   with a TTL in ticks. Requests are issued predictively from trajectories (11), because they take `L_split` to apply.
4. **Events** (17): a declared region, level and TTL. An eruption near an observer refines its flank so parcels run
   on real slopes.
5. **Network peers** (later): every peer's observer is a requester of kind 2. The union of sim-tier requests is the
   networking interest set (idea 8, 02, 10).

**Sim-tier processing per tick** (all deterministic):
- Collect requests.
- Sort them by (priority, region key).
- Add prerequisite splits (the neighbour invariant) ahead of the requests that need them.
- Start up to `N_split_tick` splits (a game constant).
- If a split would exceed `B_sim`, refuse the lowest-priority requests. Degradation is coarser detail far from
  observers, the same on every machine.
- Merges: regions whose requesters all expired more than `H_sim` ticks ago, deepest first, up to `N_merge_tick`.

**Render-tier processing per frame:** the ray cut's loop, unchanged in spirit.
- **Level cap.** Each body has a render-only level cap. Requests finer than it are dropped. The orbit view sets it to
  `ℓ_view_floor` (ℓ10), so a rotating planet in the orbit view never generates surface detail (18). The default is no
  cap.
- Read back the feedback marks a few frames late (3 slots, never stalling).
- Split leaves with `q > 64`, primary requests first, most-over first, up to 256 started per frame and 32–64 jobs in
  flight.
- Respect the neighbour invariant (prerequisite render-tier splits).
- **Rays mark the leaves they pass through**, capped at `q = 64`, as "needed". A leaf whose *bound* a ray crosses
  without hitting its representative is marked "possibly hiding detail" (§4.5).
- **Eviction is the only render-tier merge**, in the ray cut's order:
  1. unseen (no mark for 2 frames; 90 for prediction requests), oldest first;
  2. seen only by prediction rays (for a primary request);
  3. on screen, by the q the parent would have again (the children's worst q plus the split-time offset); free
     within the tolerance, and allowed under a short budget if it stays below the requester's q − 8;
  4. forced, only after the budget is lowered, down to 95% of it.
- A family is evictable only after a readback has seen its children.
- Eviction makes room for blobs, node slots and bricks: size capacity for the largest budget (`RAYCUT.md` §11).
- Sim-tier blobs are never evicted by the render tier. The render tier only ever removes its own extension.

**What the two tiers cost:**
- The sim doesn't run at fine resolution where the player only *looks*. A far mountain shows render-only detail with
  lifted appearance, while its simulation runs at the coarse levels. This is the design intent of `planet/`
  (processes at their own scale, consistency across levels), and it is what keeps the observer effect bounded by
  positions.
- Physics can't touch render-only detail. A ball thrown at a far cliff is a sim requester (it predicts its contact),
  so the cliff refines in the sim tier before the ball arrives.
- Bookkeeping: tier flags, state versions for promotion, invalidation of render-tier subtrees.

### 4.9 Families, slots and memory

- **Family blocks.** A blob's children occupy contiguous handles (`first_child`, `n_children`), as the ray cut kept
  children in blocks of 8.
  - The slot allocator uses size classes (powers of two up to 64, then exact) with per-class free lists, so batch
    processing over a family is a linear scan.
  - Freed blocks are quarantined for 2 ticks (05 §4.8).
- **One slot pool, with a tier flag per blob.** Handles are not deterministic anyway (05 §4.8). Two pools would only
  complicate promotion. Budgets are tracked per tier.
- **Capacity** (the ray cut's lesson): node slots, region records and blob slots are sized from the maximum budgets at
  start-up. Eviction frees all of them, not just blobs. A full table must trigger eviction, never a silent drop of
  splits.

### 4.10 Outside the region hierarchy

- **Parcels** (05 `parcel` mode): free blobs in a body's frame.
  - No parent, no split. They are always loaded while they exist, with sizes by observer distance (`PLUGINS.md` §2).
    Coalescing and splitting them is the fast layer's own remeshing (10, 11): new keys from `H(creator, tick, seq)`.
  - Their mass is accounted by the fast layer's ledger, not by restrict.
  - They request sim-tier refinement under their contact footprint (§4.11).
- **Model instances: creatures and the avatar's appearance** (R10, 16 §4.2; working assumption pending
  `questions.md` C10). They are not world blobs and not in the cut.
  - A shared, immutable **model asset** (atoms, bone indices, sub-instances, a fixed LOD pyramid built offline by
    §4.4's merge) plus per-individual **bone poses** and variation.
  - The renderer picks a level per instance by projected size (12). The sim uses the instance's coarse **proxy** (one
    or two ellipsoids per bone, posed) for queries and contacts, through 07's tagged hits.
  - The avatar is a dynamic body for physics (11), with a model instance for its appearance.
  - Whether an individual exists at all is a sim-tier decision: populations ↔ individuals at the entity level (16),
    through 16's thin entity tables.
  - A dead creature the world should absorb is **materialised** into world blobs (16), as kind D.
- **Small rigid objects** (a thrown rock, a log being dragged) are bodies whose blobs are world blobs in their own
  frame, `rigid` mode (05). They have a fixed small pyramid (kind D or C) and no region tree.
- **Large detached bodies** (a 1 km asteroid, a calved iceberg) get their own region tree, like any planet.

### 4.11 Forced refinement and coarsening back

**The rule:** blobs interact (contact, deposit, physics exchange) at the level of the **finest participant**.
- When a participant of level ℓ_i meets blobs of a coarser cut, physics (11) issues a sim-tier request: split the
  regions overlapping the contact footprint down to level ℓ_i, with priority "physics" and a TTL of a few ticks
  beyond the predicted contact.
- Example: a level-3 lava parcel (8–16 m) lands on level-8 ground on a far flank. The ground's regions under the
  footprint split from 8 to 3 (five levels, through the neighbour invariant). The lava then runs over real level-3
  slopes.
- Until the split lands (`L_split` ticks), contact uses the current cut. The coarse representative is within `err`
  of the fine surface, and deterministically so.

**Coarsening back.**
- When the TTL expires and no other requester covers the region, it merges after `H_sim` (§4.8).
- Whatever the interaction changed is **touched**, and survives the merge:
  - **Cooled lava becomes ground as kind A:** a lava lobe shape term (a contribution to `std:ground_height`'s phases,
    R17) or a `z` deposit on the leaves (14). Either way it restricts into every ancestor at once: `DESIGN.md`'s
    "aggregates of an edit go to the parent immediately". The deposit's shape persists as the term, or as `z`
    residuals.
  - **Settled objects become kind D placed records** (R7, R11), with 11's policy: `keep_instance` (a crisp rock or
    log, unchanged look) or `join_ground` (mud or snow clumps, which become `z` deposits and blend in).
- A coarse eruption far from every observer creates no parcels at all. It goes implicit, into coarse fields
  (`PLUGINS.md` §2). So it forces no refinement.

### 4.12 Surface and volume: the vertical structure

Working assumption, pending `questions.md` C13: **14's boundary representation** (R6). The ground is plates over an
implicit solid. Its interior is never blobs.

```
            air: no blobs (regions there are empty, cheap)
  ─────────── the outermost sheet (the surface) ───────────────────────────────────
  plates:   one per surface sheet per cell. Half-thickness max(0.7 e_d, one render sample, A_d), with keels
            where a plate is thinner than its level needs, so every level is closed to rays
  sheets:   more sheets in a column only where 3D edit terms (caves, tunnels, shafts, undercuts) cross it:
            floors face out, ceilings face in (14 §4.9)
  solid:    the implicit solid = everything below the outermost sheet, minus the edit terms. Never blobs
  planet:   the planet blob (the root) carries the body's mass and moments for orbits and physics (04)
```

**Why no interior fill.** The first draft of this doc proposed interior fill lumps and a coarse "deep" volume. It is
dropped:
- At depth 3 a plate is already 70–180 km thick, so the shell is closed from outside at every level.
- Fill would roughly double the blob count for nothing a ray ever sees.
- `world/README.md` #4's "coarse levels need volume" is met by plate thickness and keels (`BLOBMAP3.md`), not by
  interior blobs.
- Interior mass (for a detached chunk, 11) comes from integrating the implicit solid, not from blobs.

**Sheets per column.**
- A cell's column is intersected with the edit terms near it. Each crossing of the solid's boundary within the
  column's range becomes a sheet.
- The sheet index is part of the ground's ordinal (R3), nested from the outside in, so the surface sheet keeps its
  key when a cave adds sheets below.
- **Coarse levels drop sheets smaller than their cell.** A tunnel narrower than `e_d` vanishes from that level, which
  is correct: it is inside the solid and invisible from outside. A cavern wider than `e_d` keeps its sheets, so it
  shows from inside.
- Overhangs and arches are kind C rock instances sitting on the ground, or subtractive undercut terms (14 §4.9).

**Refinement inside the solid.**
- Requests are 3D. An observer underground (in a cave or a shaft) splits the regions around it like any other
  observer. The ground plugin then realises the sheets crossing those regions at the finer levels.
- The render tier refines for looking in; the sim tier for walking in.
- A cave far below the player costs nothing until a ring or a ray reaches it. Regions with no terms near them pay
  nothing for terms (14 §4.9).

**Digging** (R7). A dig is generator input, and the blob store sees the re-realised plates as removals and additions
(D15):
- A dig that lowers the surface (a crater, a pit, a trench) is `terrain.dig` writing `z` on the finest leaves as a
  bowl: a kind A field edit, persisted as `z` residuals. Restrict carries the lost volume to every ancestor at once.
- A tunnel or a shaft is an **edit term** appended to the ground plugin's term resource in log order, and persisted
  as that resource.
- Before editing, the dig forces refinement down to its own level (§4.11), so it edits fine leaves.
- **Refinement chains stay short.** With no interior fill, a dig never has to split a kilometre-sized fill lump down
  to its level.
  - A 2 m dig at the player's feet happens where the cut is already at ℓ−1: at most a few dozen region splits.
  - A 20 m shaft adds sheets to a few columns. As the player descends, the 3D rings follow: per region edge of
    descent at ℓ0 (8 m), roughly 9–27 new ℓ0 regions plus their prerequisites, a few thousand blobs. These are spread
    over ticks at `L_split` latency.
  - E7 measures both.

**Detachment and anchors** (R6).
- Anything connected through the ground's sheets to the planet's root plates is anchored.
- Whether a piece of *ground* has been cut loose (a pillar undercut on all sides, an overhang whose base was dug away)
  is decided by the ground plugin, as a service, against its implicit solid minus the edit terms. Blob connectivity
  doesn't decide it.
- 11 turns a detached region into a new body, and gets its mass from the solid.
- Other content (kinds B–D) is anchored if it touches anchored ground or anchored content (07 `ss_components`).

**Water** (R18, pending `questions.md` C20).
- The ocean is an **analytic sea-level sphere** clipped by hydrology's ocean mask (14 §4.11), plus dynamic water.
  There is no all-blob ocean.
- Lakes, rivers and near-camera waves and shores are water plates: kind A, owned by hydrology.
- Floods and surges are fast-layer parcels (10, 11).

### 4.13 Structure lifting and cross-plugin split reads

`planet/` showed that numbers aren't enough. Rivers must be lifted as structure, and the terrain's lift must know the
river bed, or fine terrain captures the river.

**In plugin terms** (R17; this drops the first draft's `std:river_bed` and `std:shape_offset` fields):
- **River conditioning** uses the ground's `lift_conditioning` **extension point** (14 §4.6).
  - Hydrology owns the river network as a resource: segments with entry and exit points and bed profiles, per coarse
    blob.
  - It contributes per-child bounds on the ground's smooth part `u`: bed profiles (non-increasing downstream,
    identical from both sides of an edge), valley-floor lower bounds within the 3×3 stencil, and "no closed
    depressions" for lifted detail. The ground applies them in phase 1 of its split (§4.7).
- **Shape terms** (volcano cones, lava lobes, crater rims; `PLUGINS.md` §1.3) contribute to the phases of
  `std:ground_height` (09). The ground's lift reads the composed field through `nz`, and tracks the terms' part
  (`nzT`) separately. The volcano plugin never touches ground blobs.
- **Where the two meet** (`PLUGINS.md` §1.2: rivers carved over a cone's summit):
  - Conditioning reads the ground *without* the terms, so a river never carves a cone flat.
  - Hydrology's routing reads `std:ground_height` *with* the terms, so it routes around cones.
  - The dependency graph is volcano → (`std:ground_height` phase) → hydrology → (`lift_conditioning`) → ground. It is
    declared in the manifests and checked at load time.
- **Restricted structure** (upward): `planet/`'s outlets, meaning where water leaves a touched coarse cell, are a
  hydrology resource keyed by blob key and updated when touched families restrict. That's 10's and 14's business. The
  hierarchy only guarantees that a restrict hook runs per touched family (`on_restrict`).

### 4.14 Fields across levels

`planet/`'s operators apply per blob tree, per stored field, as declared in the field's schema (09):
- **Restrict:** sum (extensive), volume- or area-weighted mean (intensive), or max. It runs over a blob's children:
  the same owner's family (kind D content is never part of a family, §4.6).
- **Lift:** a named prior plus an exact projection. Priors read persisted state only (§4.7).
- **Push-down:** a delta on an interior blob is shared out to its leaves by the field's push rule (uniform, by amount,
  by room, multiplicative). Non-negative intensive fields push multiplicatively (`DESIGN.md` §2.8, rule 5).
- **Residual:** `x − L(R(x))`, stored for touched families and their halo (15).

For kinds A and B this is phase 1 of the split (§4.7): geometry is realised from the lifted fields afterwards.

Render-only blobs get lifted field values (no projection needed, since they're never restricted back) for appearance
only.

### 4.15 Numbers

**Resident blobs** (estimates for the 500 km reference planet, one observer):

| part | blobs | how |
|---|---:|---|
| sim floor (ℓ ≥ ℓ14) | ~45–55 k | ~32.6 k ground plates (depths 3–6 and the planet blob) + ~11 k canopy closures on land + a few thousand lake and river plates; the ocean is analytic (R18) |
| ground in the observer rings (ℓ13 → ℓ−1) | ~30–35 k | per level ~1.3–1.8 k leaves (`planet/`: 900–1,300 with K = 8 cells), times 15 levels, plus ~⅓ interior blobs. 14 §4.3 estimates 20–25 k surface cells |
| water, snow, other kind A near the observer | ~5–15 k | |
| counted content (trees, boulders: kind B) | ~20–40 k | individuals appear in ℓ5 leaves within the ℓ6 ring (~0.8 km²): ~4–16 k trees at 1 blob each far, ~5 blobs within ~128 m, 50–150 within ~30 m |
| **sim tier total** | **~0.1–0.15 M** | |
| render-only extension | 0.3–1.5 M | the ray cut converged 0.42 M (valley, no prediction) to 1.6 M (16 km panorama); grass and pebbles are render-only (R19) |
| **total** | **≤ ~1.7 M** | the ray cut's default budget was 1 M, and 2 M was tested |

**Memory:** at ~148 B core columns + ~40 B fields (sim tier only), 1.7 M blobs are ~255 MB CPU (05 §4.12). The ray
cut's GPU bricks at 2 M were 441 MB (12 owns).

**Split throughput:**

| situation | needed (estimate) | notes |
|---|---:|---|
| walking (5 m/s), sim tier | ~1 k blobs/s | ~4 regions/s entering the rings, summed over levels |
| flying (100 m/s), sim tier | ~20 k blobs/s | |
| a new view (a 180° snap, a jump), render tier | **~0.5 M blobs/s for ~1 s** | the ray cut's 0.9 s convergence |
| ray cut, measured in JS | 1–2 k splits/s ≈ 0.4–0.8 M blobs/s | 8 workers on a loaded machine, ~5 ms per 400-blob family |
| **target in WASM** | **≥ 1 M blobs/s on 8 workers** | ≤ ~8 µs per child per worker, including projection and lifting; E4 measures it |

**Sim floor size vs cost:** ℓ14 (R5) gives 12.3 km climate cells. ℓ13 would quadruple the floor (98 k depth-7
plates, ~0.15–0.2 M blobs always loaded) for 6 km cells. Q06.5, `questions.md` C22.

### 4.16 Alternatives considered

| alternative | why not |
|---|---|
| **Per-blob cut with no spatial skeleton** (M6) | no natural unit for batching, balance, neighbourhoods or persistence; M6 re-binned everything per frame, and its tree stayed fully resident (memory grows with the world) |
| **Per-level camera windows** (M5, M7) | fixed levels by distance; memory independent of world size, but a world-space ring per level means no view-driven refinement, and the windows don't define a sim structure. M7's splats become one renderer of the render tier (12) |
| **Cube-sphere quadtree as the core's structure** (`planet/`) | surface-only; volume, caves and detached bodies need a second structure. The ground keeps an S2-style cube-sphere as its *surface index* (R4) |
| **Interior fill blobs** (this doc's first draft) | doubles the blob count for nothing a ray sees; plates and keels already close every level; mass comes from the implicit solid (§4.12, R6) |
| **One view-driven cut for everything** (the ray cut, README D7 as written) | the sim would depend on the camera's direction: replays and multiplayer diverge, and the observer effect follows the gaze |
| **Enclosure as the representative** (the ray cut) | needs render-side lift compensation and still leaves steep slopes 1–1.8 cells off (`RAYCUT.md` §10); moments plus a separate bound do both jobs |
| **Bottom-up only** (every level merged from the real blobs: the M5 v3 pyramid) | needs the finest level first, which is impossible on a planet (the 16 km world's far view took 2 minutes from cold); top-down generation plus restrict-on-touch keeps the sandbox rule "every level reflects the real blobs" where it matters, at edited places |
| **`full` projection for every type** | moves discrete things (trees, rocks) when looked at |
| **No projection** (trust the prior) | drift between levels; residuals not in a null space; mass (rock, water, snow) not conserved across levels. Kind A gets this from the field projection instead |
| **Edit additions projected with the generated family** (this doc's first draft) | the parent's moments include the addition, so a re-lift inflates the natural children and the record adds it again: a double count. Additions are kind D (§4.6) |
| **Shrinking children to fit the bound** (the ray cut) | changes geometry to protect a culling structure; expanding the bound is always safe |

### 4.17 Disagreement with README

1. **D7 ("one cut, many requesters, one budget").** Proposed amendment: **one hierarchy, a two-tier cut, two
   budgets.**
   - The sim tier is deterministic, with a fixed `B_sim`. Its merges come from requester expiry.
   - The render tier is a view-driven extension with a per-machine `B_render`. Eviction is its only merge.
   - The reason is §4.8: a single view-driven cut makes the simulation depend on the camera. The user's "same loaded
     hierarchy for sim, physics and rendering" still holds. The render tier is the same hierarchy, refined further.
2. **D8 ("lift = plugin prior + exact core projection").** For *fields*, as written. For *geometry*, it depends on the
   content kind (§4.4, 14 §4.8, pending `questions.md` C21):
   - kind C gets the exact moment projection;
   - kind A gets its consistency from the field projection, then realises geometry from the fields;
   - kind B keeps authoritative positions, with exact counts through ranked candidates;
   - kind D stays out of lifted families.
3. **Glossary:** the 500 km planet's root blob is level 18 by the definition here, not ~19.

### 4.18 Canonical scenarios

1. **Orbit to grass.**
   - From orbit, the top levels (generated on entering the system, 14) plus render-tier splits drawn by the view show
     the planet. The orbit view's level cap (`ℓ_view_floor`, ℓ10) limits render-only refinement. Nothing is
     simulated yet unless the planet is activated.
   - Picking an embark point registers render-tier prefetch along the zoom path (idea 6). At commit, it adds a
     **pending observer** at the destination, which activates the planet (its sim floor) and requests the observer
     ring.
   - Sim time holds until the convergence query reports the ring landed. The zoom animation (~1–2 s) covers the ~15
     levels of splits (~0.1 M blobs at ≥ 1 M/s).
   - Walking to the stream: rings follow at ~1 k blobs/s. The render tier adds detail wherever the player looks.
2. **The storm and the flood.**
   - The storm is a coarse process at the sim floor. Its precipitation tendency pushes down to the leaves under it
     (§4.14).
   - Hydrology runs on the whole sim-tier cut. The player's ℓ = −1 water blobs are leaves of the same blob trees, so
     the flood arrives through push-down and routing, as in `planet/`.
   - The render tier doesn't participate, except to show wet ground through lifted appearance fields.
3. **The beaver dam.**
   - The beaver (a model instance with an entity row) exists because its region reached the entity level near an
     observer (16).
   - It fells a tree through a vegetation service: a kind B "felled" override on the tree's candidate, as in
     `planet/`'s candidates.
   - The logs and the dam are **kind D placed content** owned by the beaver plugin (R7, R11). They have their own
     pyramids and are never part of the ground's families.
   - Hydrology derives a `blockage` per cell from the placed solids. Its routing at those cells changes, and the
     outlets are restricted upward (§4.13). The ground's `z` is unchanged.
   - When the player leaves, the touched families and their halo store residuals; the placed records and resources
     stay. A year later the dam regenerates exactly, and the pond's water volume is in the coarse water blobs'
     fields.
4. **The asteroid.**
   - The event (17) registers a sim requester over the impact zone at the impactor's level, ahead of time.
   - On impact, the crater is 17's **regenerable crater record** (a shape term and `z` change: kind A) plus touched
     fine detail. The blob store sees removals and additions of re-realised plates.
   - Ejecta are parcels, which force refinement where they land (§4.11), and settle as kind D rocks (or `z` deposits
     for fine material).
   - Coarse ground blobs for kilometres around take the crater through restrict of `z`. From orbit it is a dent in
     ℓ10 plates.
5. **Dig, leave, return.**
   - The dig forces refinement to ℓ−1, then `terrain.dig` writes `z` on the leaves (§4.12). The touched family, its
     halo, the `z` residuals and the felled tree's override persist on unload (15).
   - Ten years later: lift(current parent) + decayed residuals, then realisation. The hole is shallower by decay
     (`planet/`: 3.7 → 3.56 m over 15 years). The felled tree stays a stump; the damaged one ranks first to die.
6. **A community plugin (hut).**
   - A hut the player builds is kind D: a placed record with its own pyramid. Huts generated by the plugin can be
     kind B (counted sites with candidates at the entity level) or kind D (placed by a settlement process).
   - Far away the renderer merges the hut with its surroundings or drops it by footprint. The sim never folds it into
     the ground (§4.6).
   - When it burns down (a fire process), its record is removed (kind D) or its candidate gets a "burnt" override
     (kind B).

## 5. Interfaces

**Plugin exports** (the owner side; C ABI and copying per 08, whose §5 list is canonical). The split export
depends on the type's content kind (§4.4, §4.7):

```c
// kinds A and B (field-derived, counted): two phases, both pure
i32 split_propose(const SplitIn* in, SplitProposal* out);  // phase 1: children's ordinals + field priors
                                                            //   (lift_conditioning applied) -> the core projects the
                                                            //   fields and applies residuals
i32 realize(const RealizeIn* in, RealizeOut* out);          // phase 2: geometry from the final fields: plates
                                                            //   (one per sheet), closures, individuals
// kind C (shaped): one phase; the core applies the moment projection and shape residuals afterwards
i32 split(const SplitIn* in, SplitOut* out);
// kind D (placed): no split export; pyramids come from the placed record

//   SplitIn   { header: SplitHeader, parents: []ParentRec, context: []ParentRec,   // context = 26-neighbourhood
//               split_reads: FieldViews, conditioning: ExtPointViews, resources: ResourceViews,
//               removals: KeySet, cols: PrivateColViews }
//   ChildSpec = BlobSpec (05) + { parent_index: u32, ordinal: Ordinal /* u64 + u16 sub: cell id + sheet */,
//                                 vol: f32, bound_margin: [2]f32, err: f32, instance: InstanceRef }

// optional hooks
i32 on_merge(const MergeIn* in);          // read children before they are freed (entities -> resources)
i32 on_split(const SplitNotice* in);      // after a family is created (instantiate individuals)
i32 on_restrict(const RestrictNotice* in);// a touched family restricted (update structure: outlets)
i32 representative(const RepIn* in, RepOut* out); // optional custom representative for a type
i32 bound(u32 n, const u32* handles, Bound* out); // declared bounds; kind A returns radial prisms (14 §5.2)
i32 error(u32 n, const u32* handles, f32* expected, f32* max);
```

**Core API to plugins** (sim contexts; names as in 08 §5):

```c
typedef struct { f64 center[3]; f32 radius; i8 level; u8 priority; u16 ttl_ticks; } RefineReq;
Status ss_refine_request(u32 body, const RefineReq* reqs, u32 n);   // sim tier; effective at T + L_split (3 ticks)

Status ss_cut_level_at(u32 body, const f64* points, u32 n, i8* out_level, u32* out_leaf);
Status ss_blob_parent(const u32* src, u32 n, u32* out);
Status ss_blob_children(const u32* src, u32 n, u32* out_first, u16* out_n);
Status ss_blob_ancestor_at(const u32* src, u32 n, i8 level, u32* out);
Status ss_blob_moments(const u32* src, u32 n, f32* V, f64* c, f32* cov6);   // kinds C and D
Status ss_blob_bound(const u32* src, u32 n, Bound* out);   // ellipsoid, or a radial prism for kind A ground
Status ss_blob_err(const u32* src, u32 n, f32* out);
Status ss_region_of(const u32* src, u32 n, RegionKey* out);
// "touched" is 15's ss_touch(n, handles, reason): the first draft's mark_touched is dropped in its favour
```

**Engine-side API** (the engine worker: cameras, embark, renderer). These have the `ss_` prefix so 08 and 18 can
name them, but they are **not callable from plugin sim contexts**: cameras and the UI are not sim state (R9).

```c
// pending observers (R9): an embark destination refines like the avatar's observer; it is sim state from the tick
// it is added (issued as a command, 10), and becomes the avatar's observer when the embark completes (18)
Status eng_observer_pending_add(u32 body, const f64 pos[3], key128 observer_key, u64 tick);
Status eng_observer_pending_remove(key128 observer_key, u64 tick);

// convergence: has the given tier, around observer X (or a ball), reached `level` everywhere within `radius`?
// Sim tier: all requested splits landed (no pending jobs, nothing refused by B_sim).
// Render tier: no leaf in the ball over the tolerance by more than `slack` (the readiness gate of 18 uses 2x).
typedef struct { u8 converged; f32 fraction; u32 pending_jobs; i8 coarsest_level; } Convergence;
Status eng_cut_converged(u32 body, key128 observer_key /*or 0*/, const f64 center[3], f32 radius, i8 level,
                        u8 tier, f32 slack, Convergence* out);

// render-only level cap per body: render requests finer than `level` are dropped (orbit view: ℓ_view_floor = ℓ10);
// INT8_MIN removes the cap. Never affects the sim tier.
Status eng_render_level_cap(u32 body, i8 level);

// renderer internals (not ss_): feedback words and transition prefetch
void render_feedback(const FeedbackWord* marks, u32 n, u64 frame);
void eng_render_prefetch(u32 body, const PathSample* path, u32 n);
```

**Expected from other docs.**
- 07: neighbour lookups for the context gather.
- 08: copying, worker dispatch for pure splits, and the load-time check of split-read order.
- 09: field schemas (aggregation, prior, push rule, persisted-or-not) and computed fields for split reads.
- 10: the tick boundary for sim-tier requests, `H_sim`, the stall policy, activation of planets, and the bands that
  clamp. (`L_split` = 3 ticks is owned here, R1.)
- 11: contact prediction and requests.
- 12: feedback words, readback, render-node structures over the cut, morph via parent links.
- 14: generators that predict their children's restriction (§4.4), the two-phase kind-A split, sheets from edit
  terms (§4.12), the ground detachment service, S2 cell ids as ordinals.
- 15: per-kind persisted forms (§4.6) keyed by blob and parent keys, `ss_touch`, placed records.
- 16: model instances and entity tables (§4.10).
- 18: embark through pending observers, the readiness gate through `eng_cut_converged`, the orbit view's
  `eng_render_level_cap`.

## 6. Risks and unknowns

- **The affine projection may distort plugin intent** for kind C where a prior is poor, for example by flattening an
  outcrop to fix the covariance. Diagnostics will show it. If common, fall back to `centroid` and accept
  second-moment drift. E1 decides.
- **The content-kind split is new** (pending `questions.md` C21). A plugin that picks the wrong kind gets either
  double counts (placed content inside families) or drifting levels (shaped content without projection). The SDK
  must make the kind explicit per type and test it (19).
- **Steep slopes:** the ray cut underestimated error by 1–1.8 cells on 60–70° faces. Per-blob `err` from a couple of
  samples may do the same here. It needs proper per-texel or per-column measurement in the generator.
- **Prerequisite cascades** from the neighbour invariant. In 3D, a split can require 26 neighbours, each of which may
  require its parent's neighbours. Normally bounded by 2:1 balance, but an observer descending a deep shaft or a cave
  refines a column of regions. E7 measures it.
- **Two tiers double some logic** (promotion, invalidation), and render-tier invalidation after sim geometry changes
  could thrash near an active eruption. It needs rate limits.
- **Deterministic latency stalls.** If plugin workers are slow (a weak machine), the sim tier stalls. `L_split` (3
  ticks) is a game constant that multiplayer peers must agree on, so it can't adapt per machine. The mitigation is
  throughput: WASM splits in workers (E4), and observer rings that request ahead of the observer's motion.
- **Kind B closures** (coarse canopy representatives) can disagree with the individuals they stand for. BLOBMAP3's
  statistical canopy did, and showed clearings that weren't there. Visual pops at refinement are possible; M7's morph
  hides small ones (14's E14.5 checks the closure).
- **Region size `E_ℓ = 8 · 2^ℓ`** is a guess. Too small means more prerequisite splits and region overhead; too big
  means coarser refinement granularity and bigger batches. Tune with E3/E4.
- **Generator versioning:** a plugin update that changes semantic ordinals or priors invalidates residuals keyed to
  them (15).

## 7. First experiments

| # | experiment | success criterion |
|---|---|---|
| E1 | Projection by kind, on a root-to-ℓ−1 descent. **Kind A:** 14's two-phase ground split (field projection, then realisation): record the field projection's correction per level. **Kind C:** rock outcrops split with the `full` projection: record `‖A − I‖`, child displacement and volume ratio | kind A: correction ≤ 7% of the level's amplitude (`planet/`: 3.5–7%) and realised surfaces within 0.35 cells mean of the finest; kind C: median `‖A − I‖` < 2%, max child displacement < 0.1 cell |
| E2 | Split/merge round trip in Node: random split, merge and evict sequences over a 16 km region, with and without edits | untouched: merge(split(B)) bit-identical; with edits: unload to the floor and reload restores terrain to ≤ 1e-9 m (`planet/` got 1e-12) |
| E3 | Two-tier determinism: two runs with identical observer paths and different camera directions and speeds (render tier on, prediction on), 10 game-minutes with hydrology running | identical sim-tier fingerprints (all stored fields, all sim-tier geometry) every tick |
| E4 | Split throughput: `tdFamily` ported to the core language (WASM), projection and field lifting included, 8 workers, a cold 180° snap | ≥ 1 M blobs/s aggregate; the new view under 1% over tolerance within 1 s (`RAYCUT.md`: 0.89 s) |
| E5 | Arrival-order independence: reach the same region from 8 directions, unload, repeat | 0 spurious residuals (`planet/`'s 1,054 → 0 test) |
| E6 | Level seams: ground across a 2:1 region boundary, with render-tier morph (12) | ≤ 1% of pixels differ by > 16/255 from a uniform-level render of the same view |
| E7 | Digging: a 2 m `z` dig at the surface, and a 20 m shaft as edit terms, with the observer descending it | surface dig < 20 ms of split work; the shaft's refinement < 200 ms in total, spread over ticks at `L_split` (estimates to beat); no holes seen from outside at any level |
| E8 | Bound checks: run the exact containment test on every split of E1 | 0 bound expansions from the terrain plugin; cost < 10% of split time |

## 8. Build order

| milestone | what of this doc |
|---|---|
| M2 | region tree, blob tree, family blocks; render tier only; terrain's two-phase kind-A split (`split_propose`/`realize`), statically linked; bounds (radial prisms) and `err`; the ray cut's eviction; the top levels drawn from orbit with the `ℓ_view_floor` cap (E1, E2, E4, E6, E8) |
| M3 | the split protocol through any plugin, split-read ordering, content kinds per type, the kind C moment projection, kind D pyramids |
| M4 | the sim tier: observer rings, pending observers, `eng_cut_converged`, deterministic latency, forced refinement for physics, promotion and invalidation (E3, E7) |
| M5 | stored fields across levels (R, L, P), touched, halo, per-kind persisted forms wired to 15, structure lifting for hydrology through `lift_conditioning` (E5) |
| M6 | event requesters, edit terms and caves, the eruption and impact paths |
| later | network peers as requesters |

## 9. Open questions

- **Q06.1 Adopt the two-tier cut** (deterministic sim tier + view-driven render-only extension) in place of README
  D7's single cut? (merged into `questions.md` C7)
  - *Why it matters:* it decides whether the simulation depends on the camera's direction, and so whether multiplayer
    and replays are possible.
  - *Default:* yes.
- **Q06.2 Levels by largest radius** (`2^ℓ ≤ r_max < 2^(ℓ+1)` m, negative allowed), with merged blobs inheriting
  child + 1?
  - *Default:* yes. The 500 km planet's root is level 18.
- **Q06.3 A 3D octree region tree per body in the body-fixed frame** as the core's structure, with the ground keeping
  an S2-style cube-sphere as its surface index (R4)? (merged into `questions.md` C8)
  - *Why it matters:* the cube-sphere was proven for the surface. The octree is needed for caves, digging and
    detached bodies.
  - *Default:* both, as described in §4.3.
- **Q06.4 Projection by content kind** (A: field projection then realisation; B: counts only; C: full moment
  projection; D: outside families)? (merged into `questions.md` C21)
  - *Why it matters:* exactness and mass conservation for terrain, water and rock, versus never moving a tree because
    you looked at it, and never double-counting placed things.
  - *Default:* yes.
- **Q06.5 The sim floor on activated planets:** `ℓ_sim_floor` = ℓ14 (depth 6, 12.3 km cells, ~45–55 k blobs always
  loaded) or ℓ13 (6 km cells, ~0.15–0.2 M)? And is `B_sim` a fixed game constant (required for multiplayer
  determinism)? (merged into `questions.md` C22)
  - *Default:* ℓ14, and yes.
- **Q06.6 How deep may players dig?** Unlimited in principle (edit terms add sheets; the 3D rings follow the
  observer), or a practical limit (bedrock at some depth)? (merged into `questions.md` C13)
  - *Why it matters:* deep shafts refine columns of regions and grow saves (one edit term each, so small).
  - *Default:* unlimited by design, budget-limited in practice. Revisit after E7.
- **Q06.7 The finest ground level:** ℓ−1 (depth 21, 0.37 m cells; the ray cut found 0.5 m cells already coarse
  near the camera), or the cheaper ℓ0 (depth 20, 0.75 m)? Grass and pebbles are render-only detail atoms below
  either.
  - *Why it matters:* 4× the sim-tier ground blobs in the finest ring.
  - *Default:* ℓ−1.
- **Q06.8 Physics against render-only detail is impossible by design.** Thrown objects request sim refinement ahead
  of contact. Acceptable? (merged into `questions.md` C7)
  - *Default:* yes.
