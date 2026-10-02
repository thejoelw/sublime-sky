# 12. Rendering

Status: draft · Depends on: 01 runtime, 02 numerics, 04 bodies, 05 blob model, 06 hierarchy and cut, 07 queries,
18 player and camera · Used by: 13 lighting, 16 entities, 17 events, 18 player, 19 tooling

## 1. Purpose and scope

This doc defines how the blobs of the loaded world become pixels, at every distance, from a grass tuft at the
player's feet to a galaxy seen from outside. It synthesises the seven renderer prototypes (`blobby-test/world/` M1–M7)
and the ray cut (`RAYCUT.md`, branch `worktree-vcut`) into one architecture, and fixes the GPU data path from the
WASM core.

**In scope:**
- the world-space render structure and its relation to the shared cut;
- the near-field march, far-field splats and rasterised detail;
- moving things;
- scale layers, from the surface to the galaxy;
- water surfaces;
- transparency and volumes, as a framework;
- materials as the renderer sees them;
- anti-aliasing and temporal policy;
- isotropy and prefiltering rules;
- picking;
- the GPU data path, the frame graph and budgets;
- WebGPU specifics.

**Out of scope, and owned by other docs:**
- lights, shadows, AO, atmosphere, clouds, the night sky, GI and tonemapping: [13](13-lighting-atmosphere-and-sky.md);
- what a blob is and its CPU storage: [05](05-blob-model.md) (this doc states the GPU record it needs);
- how the cut is chosen for simulation: [06](06-blob-hierarchy-and-cut.md);
- CPU ray queries for gameplay and simulation: [07](07-spatial-queries.md);
- camera behaviour and UI: [18](18-player-camera-and-navigation.md);
- the worker and thread layout: [01](01-runtime-and-toolchain.md).

## 2. Requirements

| | requirement | traces to |
|---|---|---|
| must | everything is drawn as blobs (smooth within an instance, a hard contact between instances); no polygons in the look | pillar 3, D10 |
| must | near-infinite view distance on a sphere: from 5 cm to the horizon of a 500 km planet, and to other bodies and stars | constraints 2, 3 |
| must | ray queries from any point, for shadows, AO and later GI. This means a **world-space** structure, not only a camera-space one | constraint 4 |
| must | every LOD level comes from real blobs (restrictions and lifts of the actual state), never from statistical stand-ins | Joel, `BLOBMAP3.md` |
| must | continuous LOD: levels morph, they don't pop | Joel, `BLOBMAP3.md` §2 |
| must | no visible grid, lattice, ring or brick boundary | constraint 1 |
| must | memory and frame time independent of world size (bounded by a budget) | `BLOBMAP.md` |
| must | edits show at every distance within about a second | `BLOBMAP3.md` §4, `M7.md` §7 |
| must | the renderer never changes simulation state, and GPU results never feed it (§4.3, idea 3) | D5 |
| should | 1080p at 60 fps (16.6 ms) on an M1 Max class GPU in typical views. Minimum: an M1 base or a recent integrated GPU at 720p30; no mobile (R16, pending C5) | |
| should | new views (a 180° turn, a jump, an embark) converge within ~1 s, with the wait hidden where possible | `RAYCUT.md` §6 |
| should | many moving things: parcels (avalanches, lava, floods), creatures, falling objects | D16 |
| later | translucent canopies, powder clouds and other density volumes; full GI ([13](13-lighting-atmosphere-and-sky.md)) | |

## 3. What the prototypes taught

| prototype | lesson | consequence here |
|---|---|---|
| M1 exact grid (`world/README.md`) | evaluating blobs per march step runs at ~3–10 G visits/s, a budget of ~50–80 visits per pixel per frame at 1080p60. A meadow needs 850 | never evaluate every blob per step; sample smooth content |
| M3 bricks | baked sparse SDF bricks cost one fetch per step and are flat in blob count (0.37 M → 4.15 M atoms: 14.5 → 16.5 ms). They lose thin things (trunks, pine tips) | bricks for smooth content only |
| M3 | coarse levels need volume: 2–4 m voxels sample through a 2 m shell | coarse ground is closed for rays at every level: plates with keels over [14](14-planet-generation.md)'s implicit solid (R6, pending C13) |
| M3 | exp smooth-min with per-blob `k` is not a distance field: the gradient reaches K/k_min | divide steps by a gradient bound; bisect overshoots; conservative brick classification |
| M3 | dynamic blobs evaluated exactly made the lake 8× slower; re-baking the dynamic region (2.8 ms) restored the static cost | blobs that blend into big static sheets are re-baked, not evaluated |
| M5 (`BLOBMAP.md`) | smooth blobs sampled plus thin blobs (smallest radius < 0.87 sample) exact gives M1's look at a fraction of the cost; thin flags per cell, with steps capped at 0.9 cell | thin lists per brick |
| M5 | primary rays use smooth-only samples plus exact thin blobs; secondary rays use the all-blob samples | two sample channels |
| M6 (`BLOBCUT.md`) | projected-error cuts work; pixel-count feedback cuts churn, livelock and drop geometry; per-pixel overlap is the cost, not resident blobs; split march and shade passes | error-based refinement only |
| M5 v2 (`BLOBMAP2.md`) | grass was most of the frame: rasterising tufts as boxes that trace their own blend costs 0.04–3.3 ms; exact grass shadows via a sun-space grid; the cone pre-pass removes cheap steps (30–60% of steps, 0–6% of time) but is safe; compute march + shade passes give 1.2–1.65× | detail raster path; compute passes |
| M5 v3 (`BLOBMAP3.md`) | instances decide blending; per-block owner sampling keeps the cost at 1.06–1.16×; continuous LOD as a second pass over flagged rays costs 1.2–1.5×; grid merging draws lattices, jittered sites fix it | owner rule; two-pass continuous LOD; isotropic merges (requirement on [06](06-blob-hierarchy-and-cut.md)) |
| M7 (`M7.md`) | far field as ellipsoid splats morphing into parents: overview 40 → 6.4 ms, horizon 73 → 13.6 ms; prefiltering: AO below a level's accuracy, coverage cap, shadow offsets, far water; parent maps tied to node versions; nothing sub-pixel survives | splats beyond a switch distance; prefilter checklist |
| M7 §7 | edits as removals and additions re-derive every level; deterministic, origin-snapped, canonically ordered merges keep edits local (9,815 → 185 changed pixels); GPU cell lists sorted before baking | sort lists by a canonical key before every bake |
| ray cut (`RAYCUT.md`) | an octree whose leaves are the cut, a brick per leaf, feedback words per node, eviction as the only merge. 14–17 ms at 1080p; the budget is memory, not frame time; prediction rays: 63% → 0.05% bad pixels after a 180° snap; pass-through marks; normals from the hit leaf's brick only | this is the backbone (§4.3) |
| ray cut | rendering a strict superset looks wrong (surfaces 0.8–1.5 cells high) | render representatives, not bounds (D9) |
| ray cut | grazing rays crossing many leaves are ~75% of the frame | cone pre-pass; far splats; content bounds |
| `planet/` §6 | octaves with rotated lattices; the GPU draws the same continuous field inside coarse cells; precision on a 250 km sphere via noise coordinates relative to the mesh origin | isotropy rules; integer node coordinates (§4.2) |
| `wasmgpu/` | the wasm data path equals JS's; nothing aliases GPU memory; stale views after `memory.grow` silently upload nothing; floats cross as boxed args, so pass them through memory; Chrome `writeBuffer` below 4 MiB copies at ~3 GB/s on the calling thread; one thread per device | the data path in §4.15 |
| `world/README.md` #7 | same algorithm, WebGL2 ~2× faster than WebGPU on Dawn/Metal: integer-division polyfills, robustness clamps, loop guards | shift/mask index maths; re-measure per browser |

## 4. Design

### 4.1 The frame in one picture

```
 core (WASM, engine worker)                         GPU (one device, engine worker)
 ─────────────────────────                          ──────────────────────────────────────────────────────
 interpolate bodies, camera (f64)                  0 upload: one copy of the upload arena, then GPU scatter
 render tier: apply finished render-only splits    1 bake: queued bricks (budgeted), dynamic re-bakes
 write upload arena: new/changed blob records,     2 parts: per-frame TLAS of moving parts, parcel grids
   node records, list chunks, bake jobs            3 cull: nodes, far blobs (splat records), detail groups
 write command list                                4 cone pre-pass (16 px tiles)
 ──── one JS call decodes the command list ────▶   5 detail raster: grass/tuft boxes → depth + group id
                                                   6 march: near-field primary rays → hit buffer
                                                   7 splat raster: far-field ellipsoid quads, exact depth
                                                   8 secondary (half res): shadows, AO ([13])
                                                   9 shade: materials, lights, aerial perspective, water
                                                  10 volumes: froxel volumetrics, clouds ([13]), particles
                                                  11 post: exposure, bloom, tonemap ([13]); UI composite
 ◀── readback ring (3 slots): feedback words,      feedback marks are written by passes 6–8
     pick results, timings
```

The design in one sentence: **a pyramid of render nodes over the loaded hierarchy, with bricks as a cache; rays
choose their level per sample; the near field is marched, the far field is splatted, and fine detail is
rasterised.** Moving things are separate small structures under a per-frame top level, because different instances
meet with a hard min anyway (D10).

### 4.2 Coordinates and precision

**Render in the focus body's body-fixed frame.** The focus body is the one the camera is attached to or nearest
(normally the planet the player stands on; [18](18-player-camera-and-navigation.md) picks it). The planet's blobs
live in that frame ([04](04-bodies-and-orbits.md)), so they never move in it. The sun, moons and stars are
transformed into it every frame, in f64 on the CPU.

**Integer node keys, not float origins.**
- Render nodes are [06](06-blob-hierarchy-and-cut.md)'s regions (§4.3). So a node's key is the region key
  `(ℓ, x, y, z)`: integer cube coordinates relative to the body's root region, in units of the region edge
  `E_ℓ = 2^(ℓ+3)` m.
- A 500 km planet's root region is ℓ17 (edge 1,049 km). Regions of the finest ground level (ℓ−1, 4 m edge) need
  18 bits per axis. Creature-scale regions (ℓ−5) need about 22. They fit in i32.
- The camera position (f64) is split per level into an integer region coordinate and a fraction. A node's
  camera-relative origin is then
  `rel = vec3f(node.xyz − cam.xyz[ℓ]) · E_ℓ + (−cam.frac[ℓ])`. The difference is exact in integers and small
  before it becomes a float.
- There is no rebasing and no drift, anywhere on the planet. [02](02-numerics-time-and-determinism.md) adopts this
  scheme for all GPU coordinates. The planet prototype hit the same problem and solved it ad hoc (noise coordinates
  relative to the mesh origin, offsets reduced in doubles).

**Blob records are anchor-relative.**
- A blob's GPU position is f32 relative to its anchor: the region it belongs to
  ([06](06-blob-hierarchy-and-cut.md) §4.3; every blob belongs to exactly one region).
- f32 relative error ~6e-8 of a node size is far below anything visible.
- Splat records are produced each frame in camera-relative f32 by the cull pass.

**Depth.**
- One reversed-Z f32 depth buffer with an infinite far plane. Its relative precision (~1e-7) is uniform from 5 cm
  to the stars, so scale layers (§4.8) are about technique, not depth precision.
- The march writes linear `t`. Splats write hardware depth.
- Both convert through one function, so they compose in the depth test.

**Grazing rays to the horizon.**
- From 1 km up on a 500 km planet the horizon is √(2Rh) ≈ 32 km away; from 2 m it is 1.4 km.
- A ray skimming the ground crosses many nodes, which was 75% of the ray cut's frame. Three tools work together:
  - the cone pre-pass gives every ray a safe start;
  - content bounds skip nodes that the ray passes over (§4.4);
  - beyond the switch distance, splats replace marching (§4.5).

### 4.3 The render structure: a pyramid of render nodes, with bricks as a cache

#### Render nodes

- **A render node is one of [06](06-blob-hierarchy-and-cut.md)'s regions** (R4, pending questions.md C8). There is
  one octree of regions per body, in its body-fixed frame, used for splitting, balance and rendering alike.
- A region of level ℓ is a cube of edge `E_ℓ = 2^(ℓ+3)` m: about 8 cells of `2^ℓ` m across. A blob of level ℓ
  (largest radius in `[2^ℓ, 2^(ℓ+1))` m) is one to two cells.
- A region's baked field is split into up to **8 octant bricks**. Each covers 4³ cells with 8³ samples (10³ with
  apron). That gives a **sample spacing of `2^(ℓ−1)` m = `E_ℓ / 16`**: half a cell, or 2–4 samples per blob
  radius.
  - The ray cut used the same ratio: 4³ cells per node, 8³ samples plus a half-texel apron, colour and error at 6³.
  - Only octants that content reaches are allocated: about half near a surface.
- The node table covers **the whole loaded hierarchy**: every region that holds blobs of the cut *or of any
  ancestor level*.
- The loaded hierarchy includes the ancestors of every leaf (D8: inner levels are caches of restrictions). So a
  pyramid of nodes exists from the planet's top level down to the finest leaves, as in the ray cut's octree.
- The ray cut keeps ~0.13 node slots per resident blob. The pyramid adds about a third for a surface-like world.

**A node's content** is every blob of its level whose blend reach overlaps it: its own cells' blobs and
neighbours' overlapping ones. Where levels meet at a 2:1 boundary, the coarser leaf's blobs also reach into the
finer node. They are included (they are the same instance and blend). The renderer needs from
[06](06-blob-hierarchy-and-cut.md):
- representatives for inner levels (D9);
- 2:1 balance, so a node's content mixes at most two adjacent levels;
- a parent handle and a version for every blob.

**An octant brick** holds part of a node's baked content:

| texels | format | what |
|---|---|---|
| 10³ field (8³ + apron) | `rg8snorm` | r: the owner instance's smooth field (v3 owner rule); g: all blobs (secondary rays, skip) |
| 6³ albedo | `rgba8` | albedo |
| 6³ material | `r8uint` | index into the brick's **material palette**: up to 255 of 05's `u16` material ids, stored with the brick's lists. A brick touching more materials keeps the 255 most covering and maps the rest to the nearest by colour (logged) |
| 6³ overlays | `rgba8` (+ a second `rgba8` only where custom slots are used) | the baked per-blob modulation word (appearance words 2–3, R12 / [09](09-fields-and-composition.md)): the standard slots snow, wet, char, heat/glow, and up to four custom slots |
| 6³ error | `r8` | log-encoded: how far this level's surface can be from the finest (`RAYCUT.md` §2) |
| per cell | bits | has-thin, has-non-owner |
| list | `u32` handles | thin blobs (smallest radius < 0.87 sample) and non-owner smooth blobs, sorted by key |

That is about 4 KB per octant brick plus lists and palette. The ray cut measured 441 MB at a 2 M-blob budget with
~2 KB bricks. The estimate for this layout is ~450 MB at 1 M blobs, plus ~30% for ancestor bricks.

**Bricks are a cache over nodes.**
- A node's brick is baked when a ray wants it and evicted when unused, least recently seen first.
- The top few levels are always baked, so every ray has a fallback.
- A ray that wants an unbaked node uses its nearest baked ancestor and marks the node. The bake lands a frame
  later, as in virtual texturing.

Keeping ancestors' bricks resident costs ~30% more brick memory and buys five things:
1. **Per-ray LOD**: a ray stops descending at the first node that is fine enough for its footprint (below), even
   where the cut is finer, for example behind the camera's focus or under a distant ridge;
2. **Instant merges**: evicting a subtree needs no re-bake, because the parent's brick is there;
3. **Continuous LOD in the march**: a sample can blend a node's brick with its parent's (below);
4. **Cone tracing** for soft shadows, AO and GI at coarse levels ([13](13-lighting-atmosphere-and-sky.md) §4.13);
5. **Far shadows and splat shading** use the same structure (`M7.md` built separate M5 structures for that, at ~1.7 GB).

#### Per-ray level selection and continuous LOD

At distance `t`, a ray's footprint is `f = t · pixelAngle · lodBias`. Descending the pyramid, a sample uses the
deepest baked node whose error ε (from its error texels) satisfies `ε ≤ τ · f`, with the tolerance τ ≈ 2–4 px.
- The fractional part of `log2(ε / (τ f))` gives a blend weight `w`. Inside the last half octave, the sample mixes
  the node's field with its parent's: `d = mix(d_parent, d_node, w)`. Colours, normals and secondary terms are mixed
  with the same weight.
- This is v3's continuous LOD with its two-pass trick:
  - the main march runs on the dominant level and stops on a shell enclosing the blend;
  - a second pass finishes only the flagged rays, so the blending code doesn't raise register pressure for every
    ray.
- It is driven by error rather than by distance rings, so level boundaries follow the world, not circles around the
  camera (`BLOBMAP2.md`'s LOD rings).
- Expected cost: v3 measured 1.2–1.5× the discrete march. A quarter-octave band (1.09–1.35×) is the cheaper
  setting.

#### Two tiers: the render tier never touches the simulation

**Adopted** from the planning ideas (idea 1). It refines D7 ("one cut, many requesters"). This is the working
assumption; the decision is pending as questions.md C7.

- **The sim tier** is the deterministic cut of [06](06-blob-hierarchy-and-cut.md). It is driven by the sim
  observers' *positions* (the avatar, or the pending observer during an embark, R9), process bands, physics contacts
  and events. The sim runs on it.
- **The render tier** extends the sim tier for display:
  - **render-only splits** below sim leaves where rays want more detail (a far cliff, a binocular view). The blobs
    they make are **render-only blobs**;
  - per-ray **coarsening** above sim leaves where rays want less (handled by per-ray level selection, so the render
    tier needs no explicit "unsplit").
- A render-only split runs the owner plugin's deterministic `split` and the core's projection: the same lift the
  sim would run, *including* the persisted edits of touched subtrees in the form their content kind stores them
  (R7: a `z` residual plus 3D edit terms for ground, candidate overrides, removals plus residuals, placed records;
  [15](15-persistence.md)). So a distant crater shows. It runs no processes. Render-only blobs are never simulated,
  never persisted, and can be dropped at any time. Grass and pebbles are render-only by nature (R19).
- **The renderer is a requester of the render tier only.** Cameras (orbit, free-fly, photo mode) make only
  render-tier requests (R9). View direction, zoom and FOV never change the sim tier.

Why:
- **Determinism across players.** Two players at the same place, looking different ways, simulate the same world
  ([02](02-numerics-time-and-determinism.md)).
- **A bounded observer effect.** The planet view ([18](18-player-camera-and-navigation.md)) and binoculars change
  nothing in the world.
- **No pop on arrival.** When the observer later walks to a render-only region, the sim tier splits it with the same
  lift and starts from the same state the render tier showed. Lifted detail is in equilibrium with its parent
  (`planet/DESIGN.md` §2.8), so what was drawn is what is simulated.

**What a render-only region cannot show** is fine-scale *live* state that only exists where the sim is refined:
- the exact parcels of an avalanche 20 km away. The fast layer still runs, at the parcel size its observer distance
  gives (`PLUGINS.md` §2), and those parcels are drawn;
- the exact current of a distant river's 1 m water blobs. They are lifted from the coarse state.

This is the right trade: at that distance, the lifted detail is what the eye can resolve anyway.

#### Feedback, prediction and prefetch

Taken from the ray cut, with its four eviction fixes.
- **Feedback words.** Each ray kind writes per node `(frame << 8) | q` with `atomicMax`, where
  `q = 64 + ceil(8 log2(ε / (τ f)))`.
  - There are separate words for primary rays (with their shadow rays) and for prediction rays.
  - Shadow rays mark their occluder against twice the receiver's tolerance.
  - Rays mark the leaves they pass through below their parent's content top, capped at q = 64 ("needed", never
    "split").
- **Readback.** Three slots, so the CPU runs a few frames behind and never stalls.
- **Split candidates.** Leaves seen this frame with q > 64, primary first, worst first. Up to 256 are started per
  frame, with 32–64 generation jobs in flight in the plugin workers ([08](08-plugin-system.md)).
- **Eviction is the only merge, with the ray cut's order.**
  1. Unseen for 2 frames, oldest first.
  2. Seen only by prediction rays.
  3. On screen, by the q the parent would have again.
  4. Forced only after the budget is lowered.

  Groups are evictable only once a readback has seen their children.
- **Prediction rays** (togglable):
  - 16 k rays per frame: half from the camera extrapolated 0.5 s ahead, half looking around at 3× the tolerance;
  - cost 0.6–1.9 ms;
  - about 1.7× the resident blobs;
  - after a 180° turn, 0.05% of pixels are over 4× instead of 63%.
- **Transition prefetch** (idea 6, **adopted**):
  - When a camera transition starts (a jump, an embark, a zoom), [18](18-player-camera-and-navigation.md) hands the
    renderer the transition's camera path.
  - Prediction rays are then cast from points along the path, **destination first**.
  - The transition's duration (2–4 s) covers the ~1 s the ray cut needed to converge a new view, and its motion blur
    and atmosphere fade hide the coarse-first frames.
  - Render-only splits don't touch the sim tier, so prefetching a planet you might not embark on costs only
    generation time.

**Budgets.**
- The render tier has a blob budget (default 1 M, `RAYCUT.md` §7) and a brick budget sized from it: the ray cut's
  fix was 0.2 node slots and 1/9 bricks per blob, plus 30% for ancestors.
- Eviction makes room for blobs, slots and bricks, for the split and every job in flight (`RAYCUT.md` §11).
- Frame time barely depends on the budget (valley: 14.1–15.0 ms from 250 k to 1 M). **The budget is a memory and
  generation decision.**

### 4.4 Near field: the march

The near field runs from the camera to the switch distance `D_s`, where splats take over (§4.5). It is one compute
thread per pixel, from the cone pre-pass's start to the earlier of the detail raster's depth and `D_s`:

1. **Top level.** Read the tile's list of parts (§4.7) whose bounds the ray meets. The static world (the focus
   body's pyramid) is always first.
2. **Locate.** Walk the pyramid to the node containing the sample (up and down from the last node). When leaving a
   node, push the exit point across the face along its axis: a ray nearly parallel to a face would not cross it in
   float precision (`RAYCUT.md` §3).
3. **Skip.**
   - Empty nodes are skipped whole.
   - A node's **content bound** generalises the ray cut's `yTop` to a sphere: the maximum distance from the body
     centre of its content. If the ray segment through the node stays above it (the segment's minimum radius, from
     the line's closest approach to the centre clamped to the segment), skip.
   - A coarse skip distance (the all-blob channel minus a margin) handles the rest.
4. **Trace a brick.**
   - Sphere-trace the owner field (trilinear, `rg8snorm`) with over-relaxation (ω = 1.5, falling back to plain steps
     on overshoot; ray cut: 58 → 46 steps in the meadow).
   - Steps are divided by the field's gradient bound (`world/README.md` #6) and capped at 0.9 cell where the cell's
     thin bit is set.
5. **Thin and non-owner blobs.**
   - In flagged cells, evaluate the cell's thin list and non-owner list exactly.
   - The owner's thin blobs smooth-min with the sampled owner field. Other instances meet it with a hard min (v3's
     `Acc2` two-lane accumulator).
6. **Hit.** Bisect to the tolerance used by the detail raster. A mismatch (0.3 vs 0.6 px) printed outlines in
   `BLOBMAP2.md`. Write `t`, node, level, instance, blend weight and flags to the hit buffer.
7. **Moving parts.** Each part in the tile's list is traced in its local frame (§4.7). The nearest hit wins.

**Shading data at the hit:**
- the normal comes from the hit node's brick only (its apron covers the half texel the samples reach);
- within ~10 m, exact blob normals and colours are used (M4's crisp detail);
- beyond, the brick's appearance texels.

Reading the node under each finite-difference sample instead drew outlines along every level boundary
(`RAYCUT.md` §8).

**Cost guide** (measured, 1080p, M1 Max):
- the ray cut's primary march: 6.5–17.5 ms (20–62 steps per pixel, terrain only);
- M5 v2's march in the 512 m world: 8.6–14.4 ms (with trees, grass rasterised);
- the hybrid's target is ≤ 6 ms in typical views (§4.16). Where it lands is the main open number (E3).

### 4.5 Far field: splats

Beyond `D_s`, M7's far field:
- **Selection.** Per node, by the node's distance. The cull compute pass takes the blobs of the level chosen at
  their centre: blob of level L is drawn where its children are not and its parent is not, using the same distance
  for both decisions, so every point is drawn at exactly one level.
- **The morph.** Over the last half octave of its range, a blob slides into its parent:
  - its centre, its shape matrix `R·diag(r²)·Rᵀ` and its colour are interpolated by the blend weight;
  - at the switch it *is* its parent's ellipsoid, so the parent replaces it with no pop;
  - the parent comes from the blob's parent handle, checked against a version token. Tying maps to object identity
    went stale in `M7.md` §4, so versions it is.
- **Raster.** One quad per splat. The fragment intersects the pixel ray with the ellipsoid:
  - use the closest-approach form `t = t_ca ∓ √((1 − |t_ca·d − c|²_Q) / d·Qd)`. The textbook `B² − AC` cancels
    every digit for 2 m blobs at 5 km;
  - screen bounds come from the dual quadric with the cancelling terms expanded (`M7.md` §2);
  - writes go to hardware depth.
- **Growth.** Sheets (ground, water) are grown by 0.6 k and objects by 0.7 k, so a hard union closes the gaps that
  a blend would close.
- **Occlusion tiles.** After the march, each 16 px tile keeps its farthest hit. A splat beyond every tile it covers
  is dropped. In the forest all 562 k splats were dropped; the tile pass costs ~0.1 ms.
- **Shading.**
  - Colour comes from the splat. The normal comes from the splat for objects, and from the level's field for sheets,
    because a flattened ellipsoid's own dome shades as scales.
  - The shading point moves out along the normal onto the level's blended surface before shadows and AO. Otherwise
    the rays start inside the ground and beaches go black (`M7.md` §2).
  - These lookups go to the pyramid's bricks (§4.3), so no separate far-field structures are needed.

**The switch distance `D_s`.**
- M7 used the start of level 2 (~117 m at 1080p).
- Splatting from there is cheapest but least faithful (aerial view: 23% of pixels ≠ M1 vs 14% for M5). Splatting
  from level 3 (~234 m) is between.
- With error-driven levels, `D_s` becomes a tunable distance with a default of ~150 m at 1080p, scaled by
  resolution. E2 decides it.
- Splats need nothing but blob records, parent handles and the pyramid's bricks for shading, so a march-only setting
  costs only time.

**Why keep splats** when the pyramid can march to any distance?
- The marched far field costs grazing steps: the ray cut's peak view had 62 steps per pixel. M7's horizon view spent
  1.1 ms marching and 3.3 ms rastering against M5's 15.9 ms of march.
- Continuous LOD is nearly free in the splatted range: 5.4 vs 6.4 ms, discrete vs continuous.

### 4.6 Fine detail: rasterised groups

Grass, tufts, flowers, pebbles and, by the same rule, any small instance that blends with nothing outside itself.
This is M5 v2's path:
- **Cull.** One thread per detail blob of the near levels. Consecutive blobs whose blend reach touches form a group
  (a tuft's blades; a flower's stem, disc and centre).
- **Raster.** One box per group, far faces only, so the box stays visible with the camera inside it. The fragment
  intersects its members' ellipsoids grown by `k ln N` (plus the hit tolerance) and sphere-traces the group's exact
  exp smooth-min between first entry and last exit. It writes the group id and exact depth.
- **Shadows.** A sun-space 2D grid (25 cm cells) of detail blobs, with an analytic ray–ellipsoid test and penumbra.
  It is exact, and costs 0.1–2.5 ms to build ([13](13-lighting-atmosphere-and-sky.md)).
- **Fading.** Groups shrink toward their base over the fade range instead of thinning by hash (v3).
- **Draw distance.** v2 drew grass only where level 0 was used (~58 m at 1080p). Here the range is a setting:
  raster grass is cheap enough to draw farther, thinned by shrinking.

**The render path is chosen per instance at load time,** from its blob count, smallest radius and extent
(`BLOBMAP2.md`, "rocks as separate objects"):
- **raster** if it blends with nothing else, has ≤ ~16 blobs per group, and is thin;
- **march** (brick samples) if it is thick;
- **thin lists** otherwise.

Plugins can hint the path through the blob type ([05](05-blob-model.md)), but they don't choose it. The resolved
hint travels in spare bits of geometry word 7 (R12).

**Pines are the open problem.**
- Thin object blobs cost 200–255 evaluations per pixel in the big world's forest and ridge views (`BLOBMAP2.md`).
- Candidates:
  - group a pine's tiers as separate raster groups (losing some blend between tiers);
  - reduced-resolution exact evaluation of thin lists with depth-aware upsampling;
  - a "branch" material whose thin discs bake into the owner field at one level finer than the node.
- E4 measures them.

### 4.7 Moving things: a per-frame top level over small parts

Different instances meet with a hard min (D10), so the world's field is the minimum over *instances*. A ray can
trace each moving thing separately and keep the nearest hit, the way hardware ray tracing uses a top-level structure
(TLAS) over bottom-level ones (BLAS).

| part kind | what | structure | rebuilt |
|---|---|---|---|
| static world | the focus body's pyramid (§4.3) | node table + bricks | incrementally, by bakes |
| rigid part | a detached object (a log, a boulder, the asteroid). An object holds one or more instances (R11), which meet inside it by the owner rule as in the static world. Or one bone of a model instance (a creature, the avatar's appearance) | its blobs baked once into a few bricks in the part's local frame, plus thin lists | when its shape changes (rarely) |
| parcel set | fast-layer parcels that are their own instances (lava lobes, bombs, debris) | a camera-near grid of exact blobs, rebuilt per frame | every frame (M3: 17.9 k blobs in 0.3 ms) |
| parcels blending with a static sheet | water parcels joining a lake, snow onto a slope | re-baked into the static nodes they touch, with a backup of the static brick restored each frame (M3) | every frame, for the touched nodes (M3: 2.8 ms for 1–2 k bricks) |

**The top level.**
- Per frame, the core writes each part's transform (camera-relative f32, from f64 interpolation) and bound.
- The cull pass bins part bounds into the 16 px screen tiles for primary rays, and into a coarse world-space grid
  around the camera for secondary rays.
- Expected counts: a few hundred parts in view (creatures × bones + objects).

**Creatures** (from `blobby-test/README.md`, "animation is nearly free"):
- Creatures, and the avatar's appearance, are **model instances**: a shared asset plus bone poses, kept outside the
  blob store (R10, [16](16-entities-and-creatures.md), pending C10).
- Each bone of a shared asset is baked **once per asset** into a small brick set in bone space. Every creature of
  that asset is a list of bone transforms over the same bricks: instancing. A bone is re-baked only when the asset
  changes. This is the Claybook / Dreams family the creature prototype pointed at.
- A creature's body is one instance, so its bones blend: within a creature, the per-bone fields combine by exp
  smooth-min with the instance's `k`, re-blending every joint every frame.
- Hits on model instances are tagged (asset, instance, bone), so picking and the CPU's proxy queries
  ([07](07-spatial-queries.md)) report the same thing.
- A creature further away than ~2 levels' worth of its size is drawn from merged blobs (LOD) as splats: "the bear
  shows ~100 well-placed atoms go a long way".
- Animation itself (bone transforms) comes from the owning plugin ([16](16-entities-and-creatures.md)). The renderer
  only reads transforms.

**Motion vectors** come for free: every part's previous transform is known, and parcels have velocities. They are
written to the hit buffer from the start, for §4.12.

### 4.8 Scale layers: from the surface to the galaxy

There is one depth buffer and one HDR target. Layers are drawn back to front, each with the technique its scale
needs.

| layer | contents | technique | frame cost (estimate) |
|---|---|---|---|
| galaxy | unresolved stars, dust, other galaxies | a cube map baked from the galaxy density model ([03](03-universe-and-identity.md)) when the camera moves far (light-years), depth = 0 | ~0 per frame; ~10 ms per re-bake, time-sliced |
| stars | resolved stars near the camera (catalogue sectors from 03) | instanced point sprites: apparent magnitude → size and brightness, blackbody colour, a glare kernel for the brightest | < 0.3 ms for ~100 k |
| system bodies | suns, planets, moons other than the focus | a few px: an analytic sphere + atmosphere ([13](13-lighting-atmosphere-and-sky.md)); larger: the same pyramid + splats with an LOD floor from its apparent size | ~0.1–2 ms |
| focus body | the planet under the camera | §4.3–4.7, with atmosphere | most of the frame |
| volumes, particles | clouds, froxel volumes, spray | §4.10 and 13 | 1–3 ms |

**The planet orbit view** ([18](18-player-camera-and-navigation.md)) is the focus-body pipeline with an **LOD
floor**: the render tier refines no finer than **`ℓ_view_floor` = ℓ10** (R5; S2 depth 10 for the ground).
- The value matches the closest approach the view allows. At 1080p and 60° FOV, a pixel is ~1 mrad, so at 1000 km
  a pixel is ~1 km, which is level 10.
- A 500 km planet filling the screen is ~0.8 M pixels. At τ = 3 px that is ~0.1–0.25 M splats, within budget.
- Nothing in the sim tier changes: the orbit camera only makes render-tier requests (R9). The planet need not be
  activated.
- A planet seen for the first time is drawn by render-only splits from the coarse levels that
  [14](14-planet-generation.md) generates when the camera enters its system. That generation is separate from
  activation; `ℓ_sim_floor` simulation applies only to activated planets (R9). It takes ~0.5 s on 8 workers
  (estimate from the ray cut's ~5 ms per 400-blob family in JS; WASM should be faster).
- Generated render nodes may be cached across sessions ([15](15-persistence.md), cache not save). This was
  `BLOBMAP3.md`'s remedy for its two-minute cold far view.

**Moving between layers** is a change of focus body, not a change of renderer. When the camera leaves a planet's
sphere of influence ([04](04-bodies-and-orbits.md)), the focus becomes the star's frame, and the planet becomes a
"system body" drawn with its pyramid and an LOD floor. Precision holds because every body's region keys are relative
to its own root region.

### 4.9 Water and the ocean

**Lakes, rivers, waterfalls, floods** are water-instance blobs ([05](05-blob-model.md)), owned by hydrology
([14](14-planet-generation.md), [10](10-simulation-and-scheduling.md)). They render like any sheet. Two exceptions:
- water parcels that blend into a static water sheet are re-baked (§4.7);
- far water is prefiltered (`M7.md` §1): from ~20 m to 200 m at 1080p the normal flattens toward the local up and
  the roughness widens (0.06 → 0.22), so the sun reflects as a sheen instead of glinting blob by blob.

**The ocean (proposal): an analytic sea-level sphere, plus blobs where the sea is dynamic.** Sea level is a known
sphere, and on a 500 km planet most of it is flat to any eye.
- **Hit.** Per pixel, a ray–sphere intersection gives `t_w`. If `t_w < t_ground` and the ocean mask says sea at that
  point, draw water with thickness `t_ground − t_w` for absorption.
- **The mask** is hydrology's ocean-connectivity field (`planet/DESIGN.md` §6: painting `z < 0` as sea made a square
  inland lake). It is baked at each level as a 6³ sea bitmask per octant brick (27 bytes), so it costs one bit read.
- **Waves** are normal detail (a sum of rotated wave trains), prefiltered by footprint (variance in the roughness).
- **The shoreline.** Coverage is softened at the hit, never over the pixel footprint at the water plane: that made a
  horizontal band over grass 8 m above the water (`RAYCUT.md` §8).
- **Dynamic sea.** Surf, tsunamis from an impact and storm swell are fast-layer water parcels on top of the sphere,
  and they blend with each other.

Why not an all-blob ocean? It would cover roughly half the planet with water blobs at every level. That is millions
of sheet blobs drawn to show a flat surface, and their bumps glint (M7's far-water problem) or must be prefiltered
away. The analytic sphere plus dynamic water is the working assumption (R18). Joel decides in questions.md C20
(Q12.4), because it touches the blobby pillar.

### 4.10 Transparency and volumes

| thing | proposal |
|---|---|
| water surfaces | opaque hit + absorption by thickness + reflection; refraction by offsetting the thickness lookup ([13](13-lighting-atmosphere-and-sky.md)) |
| smoke, dust, ash, powder snow, rain shafts, valley fog | **volume blobs**: blobs with a density instead of a surface (the volume flag sits in spare bits of geometry word 7 and the density is an f16 in the spare half of appearance word 1; R12, [05](05-blob-model.md)), injected into a camera froxel grid (e.g. 160 × 90 × 64, log depth to ~1–2 km), integrated once, applied to every pixel. One technique for all near-field participating media (Hillaire's unified volumetrics) |
| clouds | volume blobs at cloud scale, drawn by their own reduced-resolution pass ([13](13-lighting-atmosphere-and-sky.md) §4.7) |
| translucent canopies (far) | later: aggregate blobs carry a density, composited per tile in depth order. This also gives coverage anti-aliasing for far foliage (`M7.md` §6) |
| glass, ice, membranes | thin-surface translucency in shading (the creatures' `trans` and `wrap`), not true refraction |

### 4.11 Materials and appearance

The renderer owns a **fixed, rich material model**. Plugins register material *entries*
([05](05-blob-model.md)); they never supply shader code. That is the working assumption, pending questions.md C4
(Q12.5).
- **Per material** (a table of a few thousand entries, uploaded when plugins register):
  - albedo modulation;
  - secondary colour and its pattern (`col2`, `patch`, `tscale`: the giraffe's patches);
  - roughness and specular;
  - sheen/fuzz (fur, velvet);
  - wrap (soft, SSS-like);
  - translucency (leaves, ears, membranes);
  - `shag` (noise displacement for furry silhouettes);
  - emissive (lava, fire, bioluminescence);
  - the far-field prefilter class (sheet / aggregate / object).

  These are the creature prototype's parameters (`blobby-test/README.md`, "The atom"), which were found to matter
  more than atom count.
- **Per blob:** [05](05-blob-model.md)'s GPU record (R12). It is 32 B of geometry, read by the march, plus 16 B of
  appearance, read at shading only:
  - rgb8 colour + paint coverage (appearance word 0);
  - a `u16` material id (word 1; 65 k materials);
  - the per-blob modulation word (words 2–3).
  
  Bricks don't store the 16-bit id per texel. They store an 8-bit index into a per-brick palette (§4.3).
- **Appearance overlays** are fields that plugins set and the shader interprets: `std:wetness`, `std:snow_cover`,
  `std:burnt`, and a glow derived from `std:emission_temperature` (R12, [09](09-fields-and-composition.md)'s seed
  list). They reach the GPU as the modulation word.
  - Snow is drawn per pixel by its stability law (snowline, plus a stable depth from the pixel's slope), as
    `planet/` did, so snowlines follow the terrain at any distance.
  - The overlays are baked into the brick's overlay texels.
  - [09](09-fields-and-composition.md) owns how the fields compose; [13](13-lighting-atmosphere-and-sky.md) owns the
    lighting.

### 4.12 Image quality: anti-aliasing and temporal reuse

**Facts.**
- Nothing sub-pixel survives: a splat smaller than a pixel covers no pixel centre, and neither does a marched
  surface (`M7.md` §5).
- Thin geometry (grass, pine discs, far fences) shimmers in motion without anti-aliasing.
- Joel prefers stateless frames: no reuse of last frame's depth, because of moving objects (prior-session notes).

**Position.**
1. **Baseline: stateless frames, always available.** Every effect has a stateless path, and that path is the
   reference image for tests. AA in the baseline comes from:
   - an edge-aware post filter on the final image;
   - prefiltering (thin things fade by shrinking or merge into coverage-capped aggregates);
   - analytic coverage at splat edges (an alpha from the ellipse edge distance, resolved in the per-tile compositing
     of translucent aggregates, later).
2. **The hit buffer carries motion vectors and instance ids from day one.** It costs little and keeps the door open.
3. **Temporal accumulation is allowed for clouds and volumes now.** They are soft, slow and have no hard edges, so
   ghosting is invisible, and a stateless volumetric cloud pass at the quality of the classic techniques costs
   several times more ([13](13-lighting-atmosphere-and-sky.md)).
4. **TAA plus temporal upscaling (render at 60–70%, reconstruct) is the planned performance lever for the near
   field**, behind a toggle, once the frame budget needs it. Moving objects are what TAA handles badly, but here
   every moving thing has an exact motion vector and an instance id for disocclusion, which most engines lack.
   Q12.2 asks Joel to accept this.
5. **Dynamic resolution** comes first: it is stateless.

### 4.13 Isotropy and prefiltering: a checklist every pass must pass

Isotropy (constraint 1):
- Merges group aggregates by jittered sites, not grid cells (`BLOBMAP3.md` §3). A requirement on
  [06](06-blob-hierarchy-and-cut.md) for every restrict of geometry.
- Noise octaves have random rotations and offsets, so their lattices align with nothing (`planet/DESIGN.md` §6).
  A requirement on [14](14-planet-generation.md).
- Normals read only the hit node's brick, through its apron (`RAYCUT.md` §8).
- AO never uses the "nearest blob in the list" term (M6's `gNoDmin`: list-shaped seams).
- Level boundaries follow the error, not distance spheres (§4.3). Continuous LOD inside bands.
- Half-resolution passes upsample with depth and normal weights. The accepted residue is 1–2 px steps at grass
  shadow edges (`BLOBMAP2.md`).
- No coarse skip field feeds the soft-shadow penumbra. That printed the voxel grid as shadow lines (`BLOBMAP.md` #9).

Prefiltering (`M7.md` §1; [13](13-lighting-atmosphere-and-sky.md) §4.14 owns the lighting side):
- AO samples fade out below a level's accuracy (1/16–1/8 of a cell);
- aggregates are coverage-capped when merged: a requirement on [06](06-blob-hierarchy-and-cut.md);
- secondary rays start at least one level footprint off the surface;
- far water is flattened and widened;
- **a standing test**: forced-level luminance per patch must stay within ±10% of the finest level
  ([19](19-tooling-and-testing.md)). M7 reached −15% to +8% after its fixes.

### 4.14 Picking

Picking reads the rendered frame (`M7.md` §7).
- When [18](18-player-camera-and-navigation.md) requests a pick at pixel p, the shade pass writes that pixel's
  depth, normal, instance, blob handle and body key to a pick slot, read back with the feedback ring 1–3 frames
  later.
- The result serves the UI: a cursor, a highlight, "what is this".

**A pick never becomes simulation state** (idea 3; GPU floats aren't bit-identical across vendors).
- An action on the world ("dig here", "embark here") becomes a command carrying the *ray* (origin and direction in
  the body frame, f64) plus the tick.
- The sim recomputes the hit on the CPU with [07](07-spatial-queries.md)'s ray query.
- If the GPU and CPU disagree slightly (the CPU uses sim-tier blobs, the GPU may show render-only detail), the CPU
  answer wins. On a plain dig they agree within a few cm (estimate).

Picking far bodies (stars as points) is screen-space with a pixel tolerance, handled by
[18](18-player-camera-and-navigation.md) against the star list, not by the GPU pick.

### 4.15 The GPU data path

From `wasmgpu/README.md`. The renderer is JS + WGSL in the engine worker (D3) and shares the core's heap.

- **Views.** All access to the heap goes through `(memory.buffer, ptr, size)` forms or views re-derived after any
  call that can grow memory. A stale view after `memory.grow` makes `writeBuffer` upload nothing, silently. The
  glue gets one accessor, `heap()`, that re-derives on a grow counter, and a debug assert on zero-length views.
  ([01](01-runtime-and-toolchain.md) owns the glue.)
- **One upload arena per frame.**
  - The core writes every per-frame change contiguously into a heap arena: new or changed blob records, node
    records, list chunks, bake jobs, part transforms, uniforms.
  - JS uploads it with **one** `writeBuffer` into a GPU staging buffer. A first compute pass scatters it into the
    pools, from a scatter table at the arena's head.
  - Chrome copies `writeBuffer` < 4 MiB at ~3 GB/s on the calling thread and switches to a ~46 GB/s path at ≥ 4 MiB.
    So the arena is padded to 4 MiB when it is near that size, or a `mapAsync` staging ring is used (1 MB = 39 µs
    in-call). E7 picks one.
  - A typical frame changes ~1 k blobs and ~100 nodes: ~0.1–0.5 MB (estimate). A large bake burst is a few MB.
- **One command list per frame.**
  - The core writes passes, dispatch sizes, draw counts and buffer offsets as a compact list in the heap.
  - One JS function decodes it with tiny hand-written glue. That reaches JS parity: 17–19 ns per command, against
    34 ns through bound imports.
  - Floats never cross as call arguments (59 ns boxed vs 23 for i32); they live in the heap.
  - Most draws and dispatches are indirect, sized by GPU culling, so the list is short (~50–100 commands).
- **Readback.** A ring of 3 `mapAsync` slots carries feedback words, pick results and timings, so the CPU is 2–3
  frames behind and never waits. Rendering never needs JSPI's blocking readback.
- **Who converts precision.** The core writes GPU records with f32 anchor-relative positions (§4.2), using SIMD in
  WASM. JS never touches f64 positions.
- **One device, one thread.** `GPUDevice` can't be posted to workers. Plugin workers that generate render-only splits
  send blob records to the engine worker, which uploads them.

### 4.16 Frame graph and budget

The target is **1080p, 16.6 ms, M1 Max class, in typical views** (a meadow, a forest edge, a ridge, a coast, an
overview). These are estimates, to be replaced by E1–E5:

| pass | budget (ms) | basis |
|---|---:|---|
| upload + scatter + bakes | 1.0 | ray cut bakes queued per frame; M5 v2 window bakes 1.3–1.8 ms per move |
| parts, parcel grids, culls | 0.8 | M3 dynamic grid 0.3 ms; M7 cull 0.3–0.5 ms; detail cull 0.3 ms |
| cone pre-pass | 0.1 | M5 v2 < 0.1 ms |
| detail raster | 1.5 | M5 v2 0.04–3.3 ms |
| march (incl. continuous LOD) | 5.0 | ray cut primary 6.5–17.5; M5 v2 8.6–14.4. **The main risk** |
| splat raster | 1.5 | M7 1.1–3.3 ms |
| secondary (half res: sun shadows + AO) | 2.5 | ray cut 1.0–3.5; M5 v2 2.4–4.6 |
| shade (materials, lights, aerial perspective, water) | 2.0 | ray cut 0.9–3.0 |
| volumes + clouds | 1.7 | [13](13-lighting-atmosphere-and-sky.md) |
| post (exposure, bloom, tonemap) | 0.5 | |
| **total** | **16.6** | |

**Levers when over budget, in order:**
1. dynamic resolution (stateless);
2. τ (the pixel tolerance);
3. the switch distance `D_s`;
4. half-resolution thin-list evaluation;
5. TAA upscaling (§4.12, if accepted).

720p costs ~0.45× the pixel work. The CPU side of rendering (core arena writing plus the JS decode) should stay
under 2 ms per frame.

**Pass structure rules** (register pressure decided WGSL performance every time it was measured):
- march, secondary and shade are separate compute passes (1.2–1.65×, identical images);
- continuous-LOD blending is a second pass over flagged rays, selected by an `override` specialisation constant
  (dormant blending code cost 35–48%);
- accumulators use `vec4` lanes, never dynamic indexing into private arrays (it spilled);
- timings are measured as the whole-frame span. Pass timestamps overlap on Apple GPUs.

### 4.17 WebGPU specifics

- **WebGPU only (D2).** Compute is required for culls, bakes, parcel grids, froxels and clouds. The WebGL2 port
  (2× faster on Dawn/Metal for one fragment shader) can't do the dynamic parts. Re-measure the Dawn overhead on
  Safari (its own WebGPU) and on Firefox (wgpu) before tuning for one browser.
- **Dawn → Metal overheads** (`world/README.md` #7):
  - integer division and modulo by powers of two are written as shifts and masks (+12–15%);
  - loops are structured so the injected loop guards stay cheap;
  - robustness clamps can't be disabled in production, so hot paths read textures (clamped by hardware) rather
    than computed storage indices where possible.
- **Limits.**
  - Design for the *default* `maxStorageBuffersPerShaderStage` = 8, not the M1 adapter's 10 that the prototypes
    hit twice. Big pools are sub-allocated inside few buffers.
  - Brick atlases are 3D textures: hardware trilinear filtering and clamping.
  - `maxTextureDimension3D` (2048 by default) bounds an atlas at 2048 × 2048 × 256 texels of 10³ bricks: ~1 M
    bricks in r8, more than the budget.
- **No 64-bit atomics.** Splats use the hardware raster (M7's two-pass compute raster gave identical images and was
  no faster).
- **Known bug.** `pack2x16snorm` in a fragment shader writing an `rgba32uint` target zeroed two channels (Chrome
  153–154, Dawn/Metal; repro in `world/tools/packtest.html`). Pack by hand in fragment shaders.
- **Optional features** (subgroups, timestamp queries, `shader-f16`) are used when present, with fallbacks, never
  required.

### 4.18 Alternatives considered

| | A: M7 (per-level windows + far splats) | B: ray cut (sparse octree of bricks over a view-driven cut) | **C: hybrid (proposed)** |
|---|---|---|---|
| world-space rays from anywhere | yes, within each level's window | yes, anywhere resident | yes, anywhere resident; ancestor bricks for coarse rays |
| relation to the shared cut | none: windows are distance spheres around the camera | it *is* the cut | a pyramid over the sim tier, extended by render-only splits (§4.3) |
| thin things (trunks, pine tips) | exact thin lists (M5) | lost below a sample (no thin path; terrain only) | thin lists per brick |
| far field | splats, 6–14 ms in far views | marched; grazing rays dominate | splats |
| memory | 1.95 GB; grows with levels × window size | 465 MB at 2 M blobs | ~0.7–1.0 GB (estimate): bricks for 1 M blobs + ancestors + splat and part buffers |
| moving camera | window rebuilds 5–8 ms per move | no windows; splits arrive in ~1 s, prediction helps | as B |
| LOD transitions | continuous (bands + morph) | discrete (splits pop) | continuous: per-ray blend with resident parents + morph |
| view-dependent detail (binoculars, a far cliff) | no | yes | yes, render tier only |
| sphere | windows are fine but not shaped for it | octree in body frame | octree in body frame, integer keys |

A is the best measured image today. But its structure doesn't follow the cut, its memory is the highest, and its
windows rebuild on every move. B follows the cut and is cheap in memory, but drops thin things and pays for
grazing rays. C keeps B's structure and adds A's three things that worked: thin lists, splats and morphing.

Also rejected:
- **A camera-space only structure (M6 froxels).** It can't answer non-camera rays.
- **Pixel-count feedback cuts.** They churn, livelock and drop geometry (`BLOBCUT.md`).
- **Mesh extraction** (marching cubes or surface nets over the field). It is polygons in the look, re-meshing costs
  CPU on every edit, it carries no thin-blob fidelity, and it breaks pillar 3.

### 4.19 Canonical scenarios

1. **Orbit to grass.**
   - In the orbit view the planet is the focus body, capped at `ℓ_view_floor` = ℓ10, drawn as splats over a coarse
     pyramid, atmosphere on.
   - On "embark", [18](18-player-camera-and-navigation.md) starts a 3–4 s descent and passes its path. Prediction
     rays along the path, destination first, request render-only splits down to the ridge.
   - The sim tier refines around the pending observer that the embark creates at the landing site (R9), not around
     the descending camera. It is position-driven, and it reproduces what was drawn.
   - Precision: integer node keys from 1000 km to 5 cm.
   - The near field appears when the camera passes `D_s` (~150 m) above the ground.
   - Pass: no frame over 33 ms during the descent, and under 1% of pixels over 4× the tolerance 1 s after landing.
2. **The storm and the flood.**
   - The storm is cloud volume blobs at coarse levels, visible from 100 km ([13](13-lighting-atmosphere-and-sky.md)).
   - The river's water blobs widen as hydrology changes them. Each change re-bakes the touched nodes: tens to
     hundreds of bricks per hydrology step, spread over frames by the bake budget.
3. **The beaver dam.**
   - Beavers are model instances (R10): instanced bone parts over one shared asset's bricks.
   - Logs are rigid parts while carried. Once placed, they are kind-D placed content (R7, R11): the beaver plugin's
     own `dam_log` blobs, homed in the planet's regions like any static blobs, so their regions re-bake.
   - The pond appears as water blobs. Version tokens keep parent maps valid through the re-bakes.
4. **The asteroid.**
   - On approach it is a rigid part with an emissive material and a parcel trail. The flash is an emissive light
     plus an exposure response ([13](13-lighting-atmosphere-and-sky.md)).
   - The crater is persisted as [17](17-events-and-discovery.md)'s regenerable crater record plus touched detail
     (R7). The blob store shows it as removals and additions (D15), and that is all the renderer sees. It re-bakes at
     levels 0–2 within 0.1–0.2 s and at coarse levels within ~1–3 s, as `M7.md` measured.
   - Ejecta are parcels. The dust plume is volume blobs; fires are emissive lights plus smoke volumes.
5. **Dig, leave, return.**
   - Leaving evicts render nodes. Returning re-lifts them with the ground's stored form of the dig (R7): a `z`
     residual plus 3D edit terms, which also change `std:ground_height`. So the hole is drawn again from the first
     coarse frame. Its coarse levels show it from afar, because the dig is generator input at every level.
   - Render caches are keyed by `(key, version)`, so nothing stale is drawn.
6. **A community plugin.** The hut registers material entries (wood, thatch) and a blob type with a render-path hint.
   It needs no shader code, and it is drawn by the same passes as everything else.

## 5. Interfaces

These are the renderer's needs from the core, as a C ABI on the core module, called from the renderer's JS in the
engine worker. Handles are `u32` per D6. Pointers are heap offsets.

The `rc_*` functions are **internal engine interfaces** between the core and its own renderer module. They are not
part of the plugin ABI, which is `ss_*` only (R2, [08](08-plugin-system.md) §5.2). Plugins never see them.

```c
// ---- per frame, core -> renderer -----------------------------------------------------------------
typedef struct {
  u32 arena_ptr, arena_len;        // upload arena (scatter table at its head)
  u32 cmd_ptr, cmd_len;            // command list
  u32 frame;                        // frame counter (feedback words carry it)
  f32 alpha;                        // interpolation between the last two ticks
  u32 focus_body;                   // body handle; region keys are relative to its root region
  i32 cam_cell[NLEVELS][3];         // camera's integer region coordinate per level, units of E_ℓ (§4.2)
  f32 cam_frac[NLEVELS][3];         // camera fraction per level, metres
  f32 view[16], proj[16];           // camera-relative, reversed-Z infinite
  u32 lights_ptr, n_lights;         // from [13]: suns, moons, planetshine, top emissive lights
  u32 parts_ptr, n_parts;           // TLAS entries: transform, bound, part kind, prev transform
} RenderFrame;

void rc_begin_frame(RenderFrame* out);          // fills the arena and the command list
void rc_end_frame(void);                        // arena space can be reused

// ---- feedback, renderer -> core (from the readback ring, a few frames late) ----------------------
// words: per node (frame<<8 | q), primary and prediction separately; nodes: their render-node ids
void rc_feedback(const u32* nodes, const u32* words_primary, const u32* words_predict, u32 n, u32 frame);
// the render tier turns these into render-only split requests ([06]) and eviction

// ---- prefetch ----------------------------------------------------------------------------------
// a camera path for prediction rays, destination first ([18]); cleared by rc_prefetch(0, 0)
void rc_prefetch(const f64* poses /* n × (pos3, quat4) in the focus frame */, u32 n);

// ---- picking ------------------------------------------------------------------------------------
u32  rc_pick_request(u32 px, u32 py);           // returns a ticket
// later, from the ring: depth, normal, instance, blob handle, body key. UI only; never sim input.
bool rc_pick_result(u32 ticket, PickResult* out);

// ---- settings -----------------------------------------------------------------------------------
void rc_set_quality(const RenderQuality* q);    // tolerance, switch distance, budgets, resolution, toggles
```

**What the renderer needs from other docs:**

| from | what |
|---|---|
| [05](05-blob-model.md) | the GPU record (R12): 32 B geometry (f32×3 anchor-relative position; radii + k as 4 × f16; smallest-three quaternion; instance handle; word 7: role, kind, level, atom, plus the **volume flag and render-path hint in its spare bits**) + 16 B appearance (rgb8 + paint; `u16` material + a spare half used as volume density; the modulation word in words 2–3) |
| [05](05-blob-model.md) | side arrays per blob: parent handle, version, key, region (anchor) |
| [06](06-blob-hierarchy-and-cut.md) | regions as render nodes (edge `2^(ℓ+3)` m, keys `(ℓ, x, y, z)`); the loaded hierarchy with ancestors; representatives (coverage-capped for aggregates, true volumes carried); 2:1 balance; a render-tier API to request render-only splits and to evict render-only blobs; dirty lists per frame (blobs added, removed, changed, with versions) |
| [06](06-blob-hierarchy-and-cut.md), [14](14-planet-generation.md) | the ground is closed for rays at every level: plates with keels over the implicit solid (R6, pending C13), so coarse bricks and shadow/GI rays never see through shells |
| [16](16-entities-and-creatures.md) | model instances: shared assets (bones and their blobs) and per-instance bone poses at tick t and t+1 |
| [04](04-bodies-and-orbits.md) | body transforms at tick t and t+1 (f64), spin, the focus-body choice inputs |
| [03](03-universe-and-identity.md) | star catalogue sectors near the camera (position, magnitude, temperature) and the galaxy density model |
| [07](07-spatial-queries.md) | the CPU ray query for commands made from picks |
| [08](08-plugin-system.md) | plugin `split` runs in workers for render-only splits, deterministic and stateless (idea 4) |
| [13](13-lighting-atmosphere-and-sky.md) | the light list, shadow and AO passes, the atmosphere and clouds |
| [18](18-player-camera-and-navigation.md) | the camera (f64), transition paths for prefetch, pick requests, view mode (`ℓ_view_floor` in the orbit view) |

## 6. Risks and unknowns

| risk | why it matters | mitigation |
|---|---|---|
| **near-field cost in forests and grass** | M7 spent 30–45 ms in meadow, forest and ridge, mostly the near march + grass + shadows; pines take 200–255 evaluations per pixel | detail raster (done in v2); per-instance render path; E4 on pines; reduced-resolution thin lists; TAA upscaling as a last lever |
| render-only split throughput | the ray cut converged at ~1–2 k splits/s in JS; a fresh 16 km panorama took ~20 s | plugins in WASM (estimate 3–10× faster); 8 workers; coarse first; persistent node cache; transition prefetch |
| mixed-level content within a node | per-blob cuts can put more than two levels near a boundary, bloating lists | require 2:1 balance from 06; measure list lengths (E1) |
| continuous LOD in the march costs 1.2–1.5× | it could push the march over budget | quarter-octave bands; blend only where fields differ by more than a tolerance (v3 next steps) |
| brick memory with ancestors | +30% | bricks are a cache: evict ancestors not used by rays or cones |
| WebGPU differences across browsers | the Dawn/Metal penalties were measured on one machine only; Safari and Firefox are untested | E6 runs the core shaders in all three early |
| splat fidelity just past `D_s` | rock piles look lumpier than the blend (`M7.md` §3) | E2 sets `D_s`; translucent aggregates later |
| sea-level sphere vs blobby look | an analytic ocean could look un-blobby at the shore | dynamic shore water parcels; Q12.4 |
| GPU vs CPU disagreement on picks | a dig could land a few cm from where the cursor showed | the CPU result wins; show the CPU hit as the cursor once known |

## 7. First experiments

All are on the M1 Max, in headless Chrome with the real GPU ([19](19-tooling-and-testing.md)), and compared within
one session against a reference.

| id | experiment | success criterion |
|---|---|---|
| E1 | **pyramid + per-ray LOD on the ray cut.** Port the ray cut to keep ancestor bricks resident; select the level per sample by error; blend with the parent over a half octave | no pops in a fly-through GIF; frame time ≤ 1.3× the ray cut's discrete march; ancestors ≤ 35% extra brick memory |
| E2 | **splats over the pyramid.** Add M7's cull + morph + raster beyond `D_s`, shading from the pyramid's bricks | horizon and overview views ≤ M7's 13.6 / 6.4 ms at 1080p; pixels differing from a march-only reference ≤ 10% at `D_s` = 150 m |
| E3 | **thin lists in bricks.** Add M5's thin lists and owner rule to E1's bricks; trees in the world | lake/forest views within 1.2× M5 v3's time with M5-level fidelity (≤ 7% of pixels ≠ M1) |
| E4 | **pines.** Compare tier groups rasterised, half-resolution thin lists, and baked-one-level-finer branches | the forest view's near field ≤ 8 ms at 1080p without visible loss at 10 m |
| E5 | **render-only splits from WASM.** Port `tdgen.js`'s family split to a WASM module in workers | ≥ 5× the JS throughput; a 180° snap under 1% over 4× in ≤ 0.6 s |
| E6 | **cross-browser.** The march and splat shaders in Chrome, Safari 26 and Firefox on the same Mac | all three render identically within 1% of pixels; record the time ratios |
| E7 | **upload path.** Arena via one `writeBuffer` (padded to 4 MiB) vs a mapped staging ring, at 0.2, 1 and 8 MB per frame | the chosen path spends < 0.3 ms of engine-worker time at 1 MB |
| E8 | **sphere precision.** Integer node keys on a 500 km planet: camera at 5 cm, 2 m and 10 km altitude, at the antipode of the key origin | no jitter or cracks in a 1 s capture; depth-test ordering correct at 5 cm and at 50 km |

## 8. Build order

| milestone | rendering work |
|---|---|
| M0 | engine worker owns the device and OffscreenCanvas; heap-view glue with the grow counter; command-list decoder; one compute pass + one present from WASM-written commands; the readback ring |
| M1 | star points layer, galaxy cube map (simple), system bodies as analytic spheres; reversed-Z; pick by screen tolerance for stars ([18](18-player-camera-and-navigation.md)) |
| M2 | render nodes with integer keys; pyramid + bricks for coarse levels (E1); far-field splats (E2); the planet orbit view with an LOD floor; atmosphere v1 ([13](13-lighting-atmosphere-and-sky.md)); render-only splits from a terrain plugin (E5); transition prefetch |
| M3 | the material table registered by plugins; appearance overlays (snow) |
| M4 | the near-field march with thin lists and the owner rule (E3); detail raster (grass); continuous LOD; moving parts (TLAS) for the player and loose objects; picking; half-resolution secondary passes ([13](13-lighting-atmosphere-and-sky.md)); dynamic resolution |
| M5 | parcel sets and the dynamic re-bake for water; the analytic ocean; froxel volumes; the prefiltering standing test |
| M6 | event visuals: emissive parts, plumes, flashes; cloud pass ([13](13-lighting-atmosphere-and-sky.md)) |
| later | creatures as bone parts; pines (E4); translucent aggregates; TAA upscaling if accepted |

## 9. Open questions

- **Q12.1 — Is the hybrid the renderer direction?** That means a pyramid of render nodes over the shared cut with
  bricks as a cache, thin lists, far splats, rasterised detail and moving parts under a per-frame top level. It
  replaces M7's per-level camera windows. *Why it matters:* it sets M2's and M4's work. *Default:* yes. M7's look is
  the bar to match, E1–E3 are the gates, and M7 stays as the reference renderer in `blobby-test`.
- **Q12.2 — Temporal reuse.** Stateless frames as the baseline and the test reference; motion vectors and instance
  ids written from day one; temporal accumulation allowed now for clouds and volumes only; TAA plus upscaling for
  the near field later, behind a toggle, when the budget needs it. Is that acceptable? *Why:* it is the biggest
  performance lever left (~2×) and the standard way to anti-alias thin geometry. *Default:* yes as stated.
- **Q12.3 — Target hardware** (merged into questions.md C5). 1080p60 on an M1 Max class GPU is the design point. What is the minimum: integrated
  laptop GPUs, 720p30? Mobile at all? *Why:* it sets budgets, the default τ, memory caps (bricks ~0.45–0.6 GB), and
  whether the stateless path must hit the minimum unaided. *Default:* M1 Max 1080p60 typical; M1 base / recent
  integrated 720p30 minimum; no mobile.
- **Q12.4 — The ocean** (merged into questions.md C20). An analytic sea-level sphere with blob water only where it is dynamic (surf, floods,
  tsunamis), or an all-blob ocean? *Why:* the blobby pillar against millions of sheet blobs drawn to show a flat
  plane. *Default:* analytic sphere plus dynamic water parcels at the shore.
- **Q12.5 — Plugin shaders** (merged into questions.md C4). Can plugins ship shader code (custom materials, effects), or only material-table
  entries and appearance fields? *Why:* WGSL from plugins means validation, compile stalls, security and a much
  bigger compatibility surface. *Default:* no shader code. A fixed rich material model plus appearance overlays,
  revisited after M6 with a node-based material graph if needed.
- **Q12.6 — The two-tier cut** (merged into questions.md C7). The renderer refines only a render tier: render-only splits, never simulated, never
  persisted. So a distant region shows lifted detail and persisted edits, but never the live fine state the sim
  would compute there. Agreed? *Why:* it makes the sim independent of view direction and zoom, which matters for
  determinism and for multiplayer, at that cost. *Default:* yes.
