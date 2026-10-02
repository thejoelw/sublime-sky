# 11. Physics

Status: draft · Depends on: 04 (bodies, frames, rails), 05 (blobs, the field function, dynamics flags), 06 (cut,
requesters, contact level), 07 (field, surface and overlap queries), 09 (material fields), 10 (tick, the fast layer's
place in it, observers) · Used by: 14, 15, 16, 17, 18

## 1. Purpose and scope

**In scope:** how things move and touch.
- The **dynamics modes** a blob can be in.
- **Parcels:** the fast layer's integrator, its rheologies, and its rules.
- **Rigid objects:** contact against blob fields, the solver, sleeping, and **settling** back into the static world.
- The **character controller**.
- **Connectivity and detachment:** islands become bodies.
- **Collisions across the body tree**, in the NCA frame.
- **The interaction level:** forced refinement where things touch.
- **Drag, atmosphere** and the **rails ↔ integrated handoff** (with 04).
- **Determinism**, budgets, and what we deliberately don't build.

**Out of scope, owned elsewhere:**
- Orbit propagation and SOI parent changes: [04](04-bodies-and-orbits.md).
- How field, surface and overlap queries are computed: [07](07-spatial-queries.md).
- Where the fast layer sits in the tick, explicit vs implicit, the governor, warp: [10](10-simulation-and-scheduling.md).
- What an impact *means* (craters, ejecta budgets, the event): [17](17-events-and-discovery.md).
- Creature locomotion and IK: [16](16-entities-and-creatures.md).
- Cosmetic GPU particles: [12](12-rendering.md).

## 2. Requirements

**Must**

| id | requirement | from |
|---|---|---|
| P1 | Deterministic: CPU only, fixed ticks, canonical order. | D5, multiplayer readiness |
| P2 | Everything that flows uses one integrator with a rheology per material: water, snow, debris, lava. Mass is exact through spawn, motion and deposit. | D16, `PLUGINS.md` |
| P3 | Things touch at the right detail. A contact refines the static side to the moving thing's contact level ("interact at the finest of the two"). | Joel's sketch, D7 |
| P4 | Works on a sphere and in 3D: local up is the gravity direction, and contact works under overhangs and in caves. | constraint "spherical planet" |
| P5 | Pieces that lose their support fall. Disconnected islands become bodies. | Joel's sketch |
| P6 | The player walks, climbs slopes, steps up, swims. | 18 |
| P7 | Bodies hand off cleanly between rails and integrated motion. | 04 |

**Should**

| id | requirement |
|---|---|
| P8 | Objects at rest settle back into static blobs: Joel's "merge blobs instead of making them bounce/roll". |
| P9 | Things float and drift in water. |
| P10 | Physics cost is bounded per frame and degrades by slowing the clock (10), not by breaking. |

**Later:** two-way coupling between flows and objects; structural collapse; fictitious forces if orbital play arrives (§4.2); creatures' ragdoll-lite.

## 3. What the prototypes taught

**The fast layer** (`planet/js/mobile.js`, `PLUGINS.md` §2) is the starting point. It ran avalanches, lava, bombs and
floods on the 250 km planet.

**State and LOD**
- Parcels carry position (f64), velocity, volume (f64), radius, flow depth, one auxiliary value (temperature),
  material, and an airborne flag.
- A **resting store** holds deposits. They take no substeps and only add depth that moving parcels feel.
- Parcel size follows the observer's distance: radius 3 m near, 40 m at 10 km, up to 150 m for big lava. A budget of
  3,000 moving parcels coarsens new ones.

**Forces on a grounded parcel**
- Gravity along the surface.
- The SPH flow-depth gradient (hydrostatic spreading), from a 2D kernel `W = 4/(π r̄²) (1 − d²/r̄²)³` over moving and
  resting neighbours.
- XSPH-style velocity smoothing.
- Basal resistance that can stop a parcel but never reverse it.

**Rheology,** applied semi-implicitly: yield and Coulomb terms as a clamp, viscous and turbulent terms implicit. This is
stable for any step.

| material | resistance (deceleration) | measured parameters |
|---|---|---|
| snow | Voellmy: `μ g cos θ + g u² / (ξ h)` | μ 0.2, ξ 1500; 20k and 200k m³ slabs ran 9.5 km at peak 35–39 m/s; constant μ runs every slab to the bottom of a big cone |
| lava | Bingham: `τ_y / (ρ h) + 3 η u / (ρ h²)` | 3 kPa and 5000 Pa·s at 1150 °C, both ×30 per 100 K of cooling; ~2.5 K/h crust cooling for a 3 m flow |
| water | turbulent only (Manning-like) | μ 0.01, ξ 800; outburst floods at up to 12 m/s, lake surges at up to 21 m/s |
| bombs | ballistic with quadratic drag, substeps ≤ 0.05 s | 80–250 m/s at the vent |

**Instabilities found and fixed,** which become engine rules (`PLUGINS.md` §2 table):

| symptom | cause | rule |
|---|---|---|
| parcels frozen in heaps on 22° slopes | the rest test used the speed before friction | test rest **after** resistance |
| 42,000 splits in a week | the erosion patch followed a shrinking radius | entrainment patches are fixed per event, in physical units |
| lava fired off vent piles at 50–90 m/s | SPH depth of ~20 m on a heap, almost no laminar resistance | cap the depth basal resistance sees at 2.5 × the material's typical depth; clamp the pressure gradient (0.6); smooth velocities |
| hot lava frozen in dips | a resting parcel can't spread | hot lava never rests; slow overlapping parcels coalesce; big parcels are wide |
| 15,000 hash probes per parcel | resting-store cells sized by 2 m bombs | neighbour cells ≥ 40 m |
| 1.6 s frames | the clock started before spin-up | fast-layer state is created at a known tick |
| lava stopped dead on 39° | explicit viscous drag with a 10 s step overshot | semi-implicit resistance, stable for any step |
| 21,000 parcels after a 15-year skip | LOD used the camera position from before the skip | nobody observes a fast-forward: skip is implicit (10) |

**Elsewhere in the prototypes**
- **Mass is exact.** A cascade conserved pack + load + moving + debris to 4.5e-15. An eruption conserved
  5,183,628 m³ to within 3e-8 m³.
- **Cost:**
  - 700–850 moving parcels took 1.2–1.9 ms per frame, and 745 lava parcels 2.3 ms (JS);
  - 200 parcels for 220 s of avalanche took 0.8 s of CPU in Node;
  - ~100 parcels seen from 8 km took 0.5 s of CPU per simulated hour.
- **The CPU and GPU surfaces differed by metres in coarse cells**, so parcels floated or sank and the camera needed 3 m
  of clearance (`PLUGINS.md` §7). The fix is one field definition, used exactly on the CPU (07), with the renderer's
  approximation error bounded (12).
- **In 3D** (`PLUGINS.md` §6), contact becomes a signed-distance query against the blobs, with the normal from its
  gradient. The integrator doesn't change, and deposits become blobs.
- **The field is not a distance field** (`world/README.md` #6). Exponential smooth-min with per-blob `k` has gradients
  up to `K/k_min` where different `k` meet. Rays overshot until steps were divided by that bound. Contact depths have
  the same problem.
- **The same index served physics** in `world/`. The waterfall found neighbours through the render's dynamic grid and
  collided with the baked bricks. "The one bug was letting it collide with last frame's water."
- **Animation as rigid bone groups** (`blobs/`, iteration 09): rotating a part's blobs about a pivot is the whole
  skinning, because the smooth union re-blends every joint each frame.

**What changes for the engine:** the prototype's fast layer could break out on a wall-clock budget and lag the world
clock (`msBudget`, `maxLag`). Here every tick completes, and overload slows the clock instead (10 §4.11).

## 4. Design

### 4.1 Dynamics modes

Every blob has a 2-bit dynamics mode ([05](05-blob-model.md) stores it):

| mode | position is stored | moves by | collides as | examples |
|---|---|---|---|---|
| `attached` | in its body's frame (body-fixed for a spinning planet) | its body only | static geometry | terrain, trees, settled rocks, deposits |
| `bone` | in a bone's frame; the bone is a kinematic sub-frame of a body | the owner plugin sets bone transforms | pushes, isn't pushed | world blobs carried on a bone: a held log |
| `rigid` | in an object body's local frame (constant) | the object body integrates | as part of its object | a felled trunk, a detached boulder, ejecta chunks |
| `parcel` | in the planet's frame, with its own velocity | the fast layer | as a flow (depth and pressure), not rigidly | avalanche, lava, flood water, bombs |

These four modes are 05's (R12). Creatures and the avatar's appearance are **not** world blobs in any of these modes:
they are model instances (a shared asset plus bone poses, outside the blob store), seen by physics and queries through
a coarse proxy of a few ellipsoids per bone, with tagged hits ([16](16-entities-and-creatures.md) §4.2; R10, pending
`questions.md` C10). The avatar and walking creatures are dynamic bodies driven by the character controller (§4.5).

**Transitions** between modes happen at barriers (10 §4.2):

| from → to | who | how |
|---|---|---|
| `attached` → `rigid` | connectivity (§4.6) or a service (felling) | an island becomes an object body |
| `rigid` → `attached` | settling (§4.4) | kind D placed records (R7) |
| field → `parcel` → field, terrain or resting | the fast layer's exchange | transfers (10 §4.7) |
| `rigid` ↔ `bone` | hold and release (§5) | e.g. a beaver carrying a log |

### 4.2 Frames and gravity

- **The frame.** Physics on a planet runs in its **body-fixed (rotating) frame** ([04](04-bodies-and-orbits.md)),
  because the terrain lives there. Positions are f64 (D4). Contact maths runs in f32 relative to a local contact
  origin.
- **Gravity: radial only, no fictitious forces** (R8, 04 §4.8, working assumption pending `questions.md` C2). Below a
  body's switch radius (its atmosphere top, or the floor of an airless body, 04 §4.9), surface physics uses
  `g(x) = −GM x / |x|³` and nothing else: no centrifugal, no Coriolis term. Local up is `x / |x|`.
- **The handoff is exact.** At the switch radius, 04 converts between the body-fixed frame and the inertial frame
  exactly (`v_in = v + ω × r`, §4.9). The inconsistency is only *inside* the switch radius, where a long throw follows
  a slightly different path than real physics would give.
- **Why:** day length is then independent of planet size. Any day longer than ~24 min works, and the working default
  is a ~1 real-hour day (R8). Including the fictitious forces would tie the day to the planet's radius, as the
  alternative below shows.

**The alternative: include the fictitious forces.** With centrifugal force included, the effective gravity is
`g_eff(x) = −GM x / |x|³ + ω² (x − (x·ω̂) ω̂)`. The centrifugal term is constant per point, so it would cost nothing to
compute. But a small planet then can't spin fast. A surface orbit takes `T_s = 2π √(R/g)`, and centrifugal force at the
equator is `g (T_s / T_day)²` of gravity:

| planet radius (g = 9.81) | surface orbit period T_s | day for centrifugal ≤ 4% of g (T_day ≥ 5 T_s) |
|---|---:|---:|
| 250 km | 16.7 min | ≥ 1.4 h |
| 500 km | 23.6 min | ≥ 2.0 h |
| 6,371 km (Earth) | 84 min | ≥ 7 h |

- **The default day would be marginal.** On the 500 km reference planet, a 1 h day would give 15% of g at the equator.
  A Minecraft-style 20-minute day would fling the surface into orbit.
- **Coriolis would be small.** With a 2 h day, ω = 8.7e-4 rad/s, and a 30 m/s parcel feels 0.05 m/s² (0.5% of g).
- **When to switch to it.** Choose it only if days of several hours are acceptable, or if orbital play (Q04.5) makes
  the mismatch inside the switch radius matter. Then the switch radius could become a gradual blend (04 §6).

### 4.3 Parcels: the fast-layer integrator

**State per parcel** (SoA, owned by the fast layer; parcels are blobs with mode `parcel`):

| field | type | | field | type |
|---|---|---|---|---|
| position | f64×3 | | velocity | f32×3 |
| volume V | f64 (conservation) | | kernel radius r | f32 |
| flow depth h | f32 | | aux (temperature, water content) | f32 |
| age, slow timer | f32 | | material | u8 |
| airborne | u8 | | island | u32 |
| cached surface (signed distance, gradient) | f32×4 | | | |

**One island step** of length `dt`. Islands are connected groups with their own power-of-two clock (10 §4.7). The
algorithm generalises `mobile.js` to 3D contact:

1. **Neighbours.**
   - A uniform grid over the island with cell = the island's largest kernel radius (≥ 40 m for the resting store).
   - Cell lists are filled in parcel order, and cells are visited in sorted key order, so the iteration order is
     deterministic.
2. **Depth.** `h_i = Σ_j V_j W(|x_i − x_j|, r̄_ij)`, plus `∇h_i` and the neighbours' mean velocity. Moving and resting
   neighbours both add depth; only moving ones add velocity.
3. **The surface under the parcel.**
   - `surface(p, scale = r_i / 2)` against the static field: the attached blobs and settled objects, never parcels
     ([07](07-spatial-queries.md)).
   - It returns the field value `f` and the gradient `∇f`. The normal is `n = ∇f / |∇f|`.
   - A large parcel queries at its own scale, so it uses the cut's representatives at that level and feels 40 m
     slopes, not 1 m bumps. The query does not trigger refinement (§4.8).
4. **Driving acceleration.**
   - Tangential gravity: `g_t = g − (g · n) n`, with `g` the radial gravity of §4.2.
   - The pressure term: `−g cosθ ∇_t h`, with `|∇_t h|` clamped to 0.6.
   - Then XSPH smoothing toward the neighbours' mean: `f = 0.3 (1 − e^{−dt/τ_mix})`.
5. **Stay tangent.** Remove the normal component of velocity. Where curvature turns the flow, keep its speed, because
   curvature redirects a flow and doesn't brake it.
6. **Resistance, semi-implicit,** with `h̃ = clamp(h, 0.05, 2.5 h_typ)`:
   ```
   s1 = max(0, s − c·dt)            c = μ g cosθ (Coulomb) or τ_y / (ρ h̃) (yield)
   s2 = s1 / (1 + a·dt + b·s1·dt)   a = 3η / (ρ h̃²) (viscous), b = g / (ξ h̃) (turbulent)
   ```
   This is stable for any step and never reverses the flow.
7. **Move, then re-seat.**
   - Advance, then project the centre back to half a flow depth outside the surface, along `−∇f`, with Newton steps
     on `f` divided by the field's Lipschitz bound (§4.4).
   - Under an overhang, project along gravity instead, so parcels in caves don't snap to the ceiling.
   - If the ground falls away faster than the parcel can follow, it **launches**: it's moving faster than 15 m/s and
     more than 2 m above the projected surface.
8. **Airborne parcels** are ballistic with quadratic drag `−k |v| v` in ≤ 0.05 s substeps. On landing, the
   tangential velocity is ×0.3 and the material's `land` hook runs.
9. **Bookkeeping.**
   - Split if `V > maxV`.
   - The rest timer counts time at speed `< stop_speed`, measured after resistance.
   - `deposit` runs after `rest_time`.
   - Every 8 steps, coalesce slow overlapping parcels of the same material, conserving V, momentum and aux.

**Materials** are a built-in rheology family plus parameters plus batched hooks. Hooks are called once per material
per island step with a handle array, never once per parcel, because the ABI cost of an isolated plugin instance must
be amortised ([08](08-plugin-system.md)):

| family | resistance | parameters | owner (proposed) | status |
|---|---|---|---|---|
| `turbulent` | Manning-like drag | μ ≈ 0.01, ξ ≈ 800 | hydrology (water) | measured |
| `voellmy` | Coulomb + turbulent | μ ≈ 0.2 (should grow for small volumes), ξ ≈ 1500 | snow | measured |
| `coulomb` | Coulomb + turbulent | μ ≈ 0.5 | debris / rockfall | proposed |
| `bingham(T)` | yield + viscous, functions of `aux` | τ_y, η at a reference T, ×30 per 100 K | volcano (lava) | measured |
| `ballistic` | drag only, airborne | k | volcano (bombs), impacts (ejecta) | measured |
| `buoyant` | drag toward a wind field, buoyancy | later | ash, smoke, spray, clouds | later |

| hook | signature (batched) | used for |
|---|---|---|
| `step` | `(handles, n, dt)` → transfers, events | entrainment (snow), cooling (lava), melt, ignition |
| `deposit` | `(handles, n)` → disposition per parcel | to a field / to terrain as blobs / to the resting store |
| `land` | `(handles, n, impact_speed[])` | bombs becoming rocks, splash events |
| `on_skip` | `(handles, n, span)` | warp and skip resolution (10 §4.11) |

**What deposits become in the blob world.** Lava lobes and debris become **blobs**. The blob store sees attached
blobs added (D15). What is persisted follows the content kind (R7, [14](14-planet-generation.md) §4.8):
- **Cooled lava becomes ground** (kind A). A `deposit` returning "terrain" adds a deposit term to
  `std:ground_height` (09's `shape` phase, R17), so every level re-derives the new rock. That replaces the prototype's
  2D deposit shape field and lifts its resolution limit (`PLUGINS.md` §6).
- **Debris that keeps its own shape** (boulders, logs carried by a flood) becomes kind D placed records, owned by the
  depositing plugin.

**Water handback.** The prototype handed water back to the routing after 4 s under 0.4 m/s, which stopped flood waves
early. Proposed rule: a water parcel hands back only where it is over a channel or lake blob of the routing
(hydrology tells it, through a field), and otherwise keeps flowing or pools as resting water. To be measured (E11.1).

### 4.4 Rigid objects

**What becomes an object:**
- detached islands (§4.6);
- felled trunks (a service in the vegetation plugin calls `object_from_blobs`);
- rocks and items dropped or thrown by entities;
- large ejecta chunks.

Small islands (V < 0.05 m³, provisional) become debris parcels instead. Very large ones (> 500 m³, provisional) become
a rockfall of debris parcels: rigid physics for a collapsing cliff would be both costly and wrong-looking.

**An object is a body** ([04](04-bodies-and-orbits.md)) with its own blob subtree. Its blobs are `rigid` and stored in
its local frame. It holds **one or more instances** (R11). Blobs of the same instance blend; blobs of different
instances meet with a hard min (D10). Blobs of different objects never blend. A felled tree keeps its trunk and crown
instances; a detached chunk of ground gets a fresh instance id, so a crack opens visibly instead of the chunk blending
into the ground it left.

**Mass properties.** For a detached chunk of ground, the mass and inertia are integrated from the ground plugin's
implicit solid minus its edit terms (R6, [14](14-planet-generation.md) §4.7), since the ground's plates are a
boundary representation with no interior. For every other object they come from the ellipsoids:
- `V_i = 4/3 π a b c`;
- `m = Σ ρ_i V_i`, with ρ from the blob type's `std:density` field ([09](09-fields-and-composition.md));
- the inertia of each ellipsoid is `m_i/5 · diag(b²+c², a²+c², a²+b²)`, rotated, plus the parallel-axis term.

Overlaps are double-counted. A blend is a smooth union, so the true volume is a little less. That is good enough for
a game, and it is computed once at creation.

**Contact generation: sampled points against fields.**

- **Sample points.** Each blob at the object's contact level carries a fixed sample set on its ellipsoid surface: 6
  axis tips plus 8 diagonals (14 points), grown outward by the blob's blend reach. A blob larger than 4× the contact
  level's size gets a denser set: 26 points.
- **Object vs static world.** Transform the samples to the planet frame and evaluate the static field in one batch
  call ([07](07-spatial-queries.md) `field_eval`), which returns value and gradient. A sample with `f < ε` is a contact
  with:
  - depth `d = −f / L`, where L is the field's Lipschitz bound near the point (07 returns it). The exp smooth-min is
    not a distance field (`world/README.md` #6), so `−f` alone overestimates or underestimates depth where `k` varies,
    and dividing by L keeps it conservative;
  - normal `n = ∇f / |∇f|`.
- **Object vs object:** A's samples against B's field and B's samples against A's field, both in the NCA frame (§4.7).
- **Reduction.** Per pair, keep ≤ 4 contacts: the deepest, then the farthest from it, and so on. Ties break by sample
  index.
- **Fast objects** (speed × dt > the smallest blob radius) get **conservative advancement**: sphere-trace each sample
  along its motion against the field, with steps divided by L. This finds the time of impact without tunnelling. The
  asteroid needs it, and so does a thrown rock.

**Solver**
- **Integrator:** semi-implicit Euler with 2 fixed substeps per tick (60 Hz). Orientation is a quaternion,
  renormalised each substep. Gyroscopic torque is ignored.
- **Contacts:** sequential impulses (projected Gauss–Seidel) with 8 fixed iterations, contacts in canonical order
  (body keys, then sample index).
  - Warm-starting reuses last tick's impulses, keyed by `(sample index, other body key)`.
  - Friction is Coulomb, with `μ = √(μ_a μ_b)` from `std:friction`.
  - Restitution defaults to 0.1, because blobs are soft-looking and shouldn't bounce like billiard balls.
  - Position error is corrected with split impulses, so corrections add no energy.
- **Buoyancy and drag in water.**
  - Buoyancy: the fraction of an object's samples inside the water instance (`f_water < 0`), times its volume, gives
    `F_b = ρ_w g V_sub` at the submerged samples' centroid.
  - Water drag toward the local flow velocity: `std:flow_velocity` (a proposed addition to 09's list, §5), from hydrology, which is discharge over
    channel cross-section.

  So logs float and drift downstream, as the beaver dam needs.

**Sleeping and settling**

- **Sleep.** An object sleeps when its linear speed is below 0.05 m/s and its angular speed below 0.05 rad/s for 30
  ticks.
- **Wake.** It wakes on:
  - contact from an awake body or a parcel;
  - an edit or detachment inside its bound;
  - being grabbed;
  - wind above a threshold, for light objects.
- **Settle.** An object settles when all of these hold:
  - it has been asleep for `T_settle` (5 s provisional);
  - it is supported by attached blobs or a settled object;
  - it is not held;
  - its type's settle policy allows it (a per-type physics property in the manifest's type entry, §5).
- **Settle policies:**

  | policy | on settle | for |
  |---|---|---|
  | `keep_instance` | blobs become `attached`, keep their own instance (a crisp contact, unchanged look) | rocks, logs, deposits that should stay distinct |
  | `join_ground` | blobs become `attached` and join the ground's instance (they blend in) | mud, snow clumps, soil |
  | `never` | stays an object (sleeping costs nothing per tick) | items an entity may pick up again soon, carcasses |

- **What settling does.** The body is dissolved, and its blobs become a **kind D placed record** (R7,
  [14](14-planet-generation.md) §4.8, [15](15-persistence.md)), owned by the object's type definer (R11). Placed
  records are never part of a lifted family: they are stored as they are and re-inserted when their region loads.
  The region is marked touched. Both settle policies produce placed records; they differ only in which instance the
  blobs join.
- **Settling is the inverse of detachment.** A settled log whose support is dug away is detected by connectivity
  (§4.6) and becomes an object again. An entity can pick a settled thing up: `object_from_placed` turns the placed
  record back into an object body.
- **Settling is not visible:** with `keep_instance`, the before and after images are identical (E11.7).
- **Budget pressure.** With more than 200 awake objects (provisional), the oldest sleepers settle first. If that
  isn't enough, the slowest objects farthest from observers are dropped onto the ground along gravity and settled.
  This is deterministic and invisible far away.
- **Warp and skip:** every awake object is dropped and settled (10 §4.11).

### 4.5 Characters

The player and walking creatures share one controller ([16](16-entities-and-creatures.md) adds legs and IK on top).

- **What a character is** (R10, pending `questions.md` C10): a **dynamic body** that this controller moves. Its
  appearance is a model instance (16 §4.2): a shared asset plus bone poses, outside the blob store. Physics never
  touches the model's atoms. Other bodies and queries see the character through its proxy: the controller ellipsoid
  plus 16's per-bone ellipsoids, posed. Feet don't push the body; root motion comes from this controller.
- **Shape:** one ellipsoid at the character's contact level, e.g. radii 0.3 × 0.3 × 0.9 m for a human, about level
  −1. An ellipsoid uses the same query code as a blob.
- **Intent:** from commands (18): desired horizontal velocity in the local tangent frame, jump, crouch, swim
  up/down.
- **Per substep (collide and slide):**
  1. Apply intent and gravity. Ground friction acts when grounded; air control is 10% when airborne.
  2. **Move and resolve** in up to 4 iterations:
     - evaluate the ellipsoid's 26 samples against the world field (static plus nearby objects);
     - push out along the depth-weighted normal sum, scaled by `d / L`;
     - remove the velocity component into the surface.
  3. **Step-up.** If horizontal motion is blocked and an obstacle is lower than 0.4 m, lift by 0.4 m, retry the move,
     then sweep down.
  4. **Ground probe.** March down along gravity up to 0.3 m. The character is grounded if the probe hits and the
     slope (between the normal and up) is ≤ 50°. On a steeper slope it slides.
  5. **Snap** to the ground when grounded and moving downhill, so it doesn't hop down slopes.
- **Water.**
  - Swimming starts when more than half the samples are inside the water instance.
  - Buoyancy is `ρ_w g V_sub` (V_sub from the inside fraction), with water drag.
  - At the surface the character floats at the waterline.
- **Moving supports.** Standing on an object body inherits its velocity at the contact point.
- **Flows.** Parcels overlapping the character drag it toward their velocity, `F = ½ ρ C_d A |v_rel| v_rel`. An
  avalanche can carry the player. Whether it hurts is gameplay (Q11.3).
- **Responsiveness.** The sim applies intents per tick. The render predicts the local avatar from intents so input
  feels instant ([18](18-player-camera-and-navigation.md)).

### 4.6 Connectivity and detachment

**When it runs:** only after removals. Removals come from edits, craters, felling, lava melting ice, and blobs
destroyed by plugins. It runs in stage 2 (10 §4.2), before dynamics, so new islands fall in the same tick.

**The graph**
- **Nodes:** blobs of the sim tier near the removal.
- **Edges:** geometric contact. The ellipsoids, grown by `min(k_a, k_b) + 1 cm`, overlap (07 `overlap_pairs`). The
  edge counts regardless of instance, because support is physical: a boulder resting on the ground is connected to it.
- **Mixed levels:** blobs of different levels connect by the same test.

**Anchors, and who decides for the ground** (R6, [14](14-planet-generation.md) §4.7, pending `questions.md` C13).
The ground is a boundary representation: plates over an implicit solid, with keels, and no interior volume blobs. A BFS
over ground plates can't tell solid rock from a hollow shell, so the ground plugin decides for its own content:
- **Ground:** anything connected through the ground's sheets to the planet's root plates is anchored. After an edit
  that touches the ground (a dig, a crater, a 3D edit term that undercuts), physics calls the ground provider's
  detachment service with the edited region. The service evaluates the implicit solid minus the edit terms and
  returns the ground regions that came loose, each with its plates. Physics turns each into an island (step 3).
- **Everything else** (trees, placed records, objects' remains: kind B, C and D content) uses the blob BFS below.
  Its anchors are ground blobs the service reports as anchored, which in practice is any ground plate not in a
  detached region.

**The algorithm** (for non-ground content)
1. **Seeds:** surviving non-ground blobs that touched the removed ones, sorted by key.
2. **BFS from each unlabelled seed,** visiting neighbours in key order, with a budget of 4,096 blobs:
   - **an anchor is reached:** everything visited is anchored;
   - **the frontier empties first:** the visited set is an **island**;
   - **the budget runs out:** presume anchored, and queue an async full check ([10](10-simulation-and-scheduling.md)
     §4.6, latency k). Its result can make the region an island k ticks later. That delay is deterministic.
3. **Each island,** by volume:

   | volume | becomes |
   |---|---|
   | small | debris parcels |
   | large | a rockfall: debris parcels at a coarse LOD, plus an event |
   | otherwise | an object body: a child of the planet; blobs → `rigid`, local frame, mass properties (for ground, from the implicit solid), zero velocity in the body-fixed frame. A ground chunk gets a fresh instance; other content keeps its instances (R11). Then it falls |

**Cost.** Edits happen near observers, where the cut is fine. The BFS only walks non-ground content (a tree, a
pile of placed logs) until it touches anchored ground, so it visits few blobs. The ground service's cost is 14's to
estimate. The target is ≤ 0.5 ms for a typical dig in total, with 07's overlap test at ~0.1 µs (estimate). A crater removes thousands of blobs at once, so its seeds are many. Its ejecta, though, are made
by the impact event as parcels, not discovered by connectivity.

**Overhangs** only fall when they are fully disconnected. A thin overhang held by one blob stays up. Structural
strength (a stress field: cantilevers fail beyond a span) is a later plugin (Q11.4).

### 4.7 Collisions across the body tree

**Broad phase.** It is hierarchical, by bounds. Two bodies can interact only if their bounds overlap in their NCA's
frame. The bounds are blob bounds (D9) swept over the tick. On one planet, the participants are:
- the planet's static blobs (through the static structure in 07);
- object bodies and characters (per-body bounds; a sort-and-sweep over them in the planet frame);
- parcel islands (island bounds).

Sibling planets have disjoint annuli (D14), so bodies on different planets never test against each other, unless one
of them is in transit, and then its SOI parent is one of the planets (04).

**Narrow phase** runs in the NCA frame. For everything on a planet, that is the planet's body-fixed frame. Transforms
to it are f64. Contact maths is f32 relative to the pair's midpoint, so precision doesn't depend on distance from the
planet centre.

**Coupling between kinds:**

| pair | now | later |
|---|---|---|
| parcel ↔ static | contact (§4.3) | |
| parcel ↔ object | objects are obstacles: parcels feel `min(f_static, f_object)` | two-way: parcels push objects with `½ ρ C_d A |v_rel| v_rel`, objects displace parcels (Q11.5) |
| object ↔ static, object ↔ object | §4.4 | |
| character ↔ anything | §4.5 | |
| body ↔ planet at orbital speed (asteroid) | conservative advancement finds the impact tick; the impact becomes an event (17), not a rigid contact | |

### 4.8 The interaction level and forced refinement

The README's rule is "blobs interact at the level of the finest of the two". With absolute levels (D-levels), "the
finest" has to mean the moving thing's **collision shape**, not its finest detail. A creature's eyelid (level −6) must
not refine the terrain under it to 1.5 cm.

- **Contact level.** Each body or material declares a contact level: the coarsest level that still represents its
  collision shape (`contact_level` in the type's manifest `physics` entry, §5):

  | thing | contact level (≈ size) |
  |---|---|
  | character (human) | −1 (0.5–1 m) |
  | log, rock | −1 to 1 |
  | parcel of radius r | `⌊log2 r⌋`: 3 m → 1, 40 m → 5 |
  | 50 m asteroid | 5 |

- **The interaction level** of a contact is `max(min(contact_A, contact_B), ℓ_atom_min)`, where `ℓ_atom_min` is the
  static side's finest level (its atoms).
- **The physics requester** ([06](06-blob-hierarchy-and-cut.md)). For each awake object, character and parcel island,
  it requests:
  - **region:** the body's bound, swept over the next `L_split` ticks plus a margin, where `L_split` is the split
    latency (06);
  - **level:** the interaction level;
  - **priority:** speed divided by time to contact.

  Predicted paths are requested ahead of time: ballistic arcs, and rails orbits about to enter the atmosphere.
- **Until refinement arrives,** contacts use the existing leaf's **representative**: its moment-matched shape, D9. The
  bound would hold objects visibly above the ground. The representative's error is bounded by its level (06), so a
  rock may sit a few centimetres off for a few ticks. A debug view shows "coarse contact".
- **Afterwards,** requests expire when bodies sleep or settle and parcels deposit. Eviction coarsens the region later.
  Material that came to rest is persisted by kind (R7): ground changes as `z` residuals and edit terms, settled things as
  placed records, field values as residuals. Coarsening then restricts it into the parents, and nothing is lost.

### 4.9 Drag, atmosphere and the rails handoff

- **One atmosphere model: 04's** (04 §4.9, R8). It is the tapered exponential
  `ρ(h) = ρ₀ (e^{−h/H} − e^{−h_top/H}) / (1 − e^{−h_top/H})`, which reaches zero exactly at `h_top` (≈ 12–15 H). 13
  uses the same model for scattering. If physics and rendering used different atmospheres, a glowing fireball would
  appear where the drag isn't.
- **Drag** on integrated bodies: `F = −½ ρ C_d A |v_rel| v_rel`, applied semi-implicitly as 04 specifies.
  - v_rel is relative to the air. The air is at rest in the body-fixed frame, plus `std:wind` from climate (a point
    field, 09).
  - A is the projected area of the body's bound ellipsoid along v_rel. C_d defaults to 0.8.
  - Parcels use their material's `k`. Light things (leaves, ash) are moved by wind.
- **Integrated → rails.** When all of these hold for one tick:
  - the body is outside the switch radius (`h_top`, or the floor of an airless body, 04 §4.9);
  - there are no contacts;
  - the body is not held;
  - it is an object or character, not a parcel.

  Then convert the body-fixed state to inertial (`v_in = v + ω × r`) and to Kepler elements in the parent's frame
  (04 `elements_from_state`).
- **Parcels above `h_top`** with speed above 0.5 × the local circular speed are promoted to bodies: big impact ejecta
  can leave. Otherwise they stay ballistic and come back.
- **Rails → integrated.** 04 predicts, analytically, the tick at which a rails orbit crosses the switch radius
  inbound. At that tick, convert to body-fixed and integrate.
- **Entry heating.** `P = |F_drag · v_rel|` drives a glow (12) and the "fireball" event (17). Ablation, meaning mass
  loss to debris parcels, comes later.
- **Bodies are only integrated inside the atmosphere** or in contact. Integrated motion in vacuum is out of scope
  until something has thrust.

### 4.10 Determinism

- **CPU only.** Fixed ticks; 2 fixed substeps for objects and characters; power-of-two island steps for parcels
  (10 §4.7). No wall-clock early-outs: every tick completes.
- **Canonical orders:**
  - bodies by key;
  - contacts by (body key A, body key B, sample index);
  - parcels in their array order. Spawns append, and removals swap-with-last in a deterministic sequence, as in
    `mobile.js`;
  - neighbour grids visit cells in sorted key order (no iteration over a hash table's internal order);
  - connectivity seeds and neighbours by blob key.
- **Floats:**
  - f64 for positions and volumes;
  - WASM `sqrt` is correctly rounded, so it is safe;
  - exp, log, pow, sin and cos come from our own libm (02);
  - no relaxed SIMD.
- **No GPU results in sim state** (idea 3, adopted). Vendors differ in float results, and readback adds latency.
  **Cosmetic GPU particles are encouraged:** spray, splash, dust, sparks, the `world/` waterfall. They are seeded from
  sim events (a parcel entering water emits a splash event) and never read back.
- **The test:** a fingerprint of every body, object and parcel state after a scripted scenario, compared across runs
  with different frame pacing (E11.2).

### 4.11 What "fluid-like physics on coarse blobs" means here

Joel asked for physics on higher-level blobs to be "more of a fluid-like simulation than rigid-body". Concretely:

- **Anything coarse that moves en masse is parcels with a continuum rheology, never rigid bodies in contact:**
  - landslides and rockfalls: `coulomb` debris parcels;
  - avalanches, lava, lahars, floods: as measured.

  Far from an observer, parcels are 40–150 m wide and feel the terrain at their own scale (§4.3, step 3). A 23,000 m³
  avalanche 50 km away is ~10 parcels; seen from 2 km it is ~150 (`PLUGINS.md` §2).
- **Slower mass movement is a slow-layer process on fields, in a plugin:**
  - **Glaciers** (the shallow-ice approximation). Ice flux
    `q = −(2A / (n+2)) (ρ g)^n H^{n+2} |∇s|^{n−1} ∇s` with n = 3, solved implicitly in thickness H. This also fixes the
    prototype's snow piling up on summits year after year (`PLUGINS.md` §7).
  - Soil creep and hillslope diffusion, already in the generator's erosion (14).
- **Rigid physics is for a handful of objects near observers,** at contact levels around −1 to 2.
- **Rest is merging:** settled objects join the static set (§4.4). That is Joel's "merge blobs instead of making them
  bounce/roll".

### 4.12 What we don't build

| thing | why not |
|---|---|
| stable stacks of many rigid bodies, joints, ragdolls, vehicles | cost, complexity and a determinism burden for little gameplay; settling covers piles |
| n-body gravity | bodies feel their parent only (04) |
| fracture or FEM; structural stress | detachment by connectivity covers the visible case; a stress plugin can come later |
| cloth, soft bodies | creature squash is a visual effect (16) |
| GPU physics feeding state | not bit-identical across vendors (§4.10) |
| a third-party physics library (Jolt, Rapier) | their shapes are meshes and convexes, ours are smooth-min fields; Jolt's determinism is good, but the contact generator is the part we'd have to write anyway. Revisit if rigid needs grow |

### 4.13 Performance targets

These are estimates, scaled from the JS prototype. They are targets for the engine thread, per tick.

| work | target | basis |
|---|---|---|
| moving parcels (budget 3,000) | ≤ 2.5 ms | 700–850 parcels in 1.2–1.9 ms in JS (`PLUGINS.md`); SoA in WASM expected faster |
| resting parcels (≤ 50k) | ~0 per tick (no substeps) | prototype resting store |
| awake rigid objects (≤ 200) | ≤ 1.5 ms | ~14 samples × blobs at contact level per object; 07's batched field evaluation |
| characters (≤ 50) | ≤ 0.5 ms | 26 samples × ≤ 4 iterations × 2 substeps |
| connectivity | ≤ 0.5 ms per typical edit | §4.6 |
| physics requester | ≤ 0.1 ms | bounds only |

These sum to stage 3's 4 ms target in 10 §4.15 when everything is busy at once. Usually most of it is idle. Over
budget, the clock slows (10 §4.11).

### 4.14 Scenarios

**The asteroid** (scenario 4):

| when | stage | what happens |
|---|---|---|
| T−k | | 04 predicts the atmosphere entry tick. The physics requester asks for the impactor's contact level (5 for a 50 m body) along the predicted ground track, so the ground is refined before impact |
| T_entry | 3 | rails → integrated in the body-fixed frame (§4.9). Drag and heating start, and the glow is visible from far away (13) |
| each tick after | 3 | conservative advancement against the field finds the impact tick without tunnelling (§4.4) |
| T_impact | 3 | the impact event is emitted with energy, point and angle (17) |
| T_impact + 1 | 1–2 | the handler's crater: 17's regenerable crater record plus touched detail (R7); the blob store sees removals and rim additions (D15) |
| | 2 | detachment around the rim: the ground service for ground, the BFS for trees and placed things (§4.6); loose pieces become objects or debris |
| | 3 | ejecta spawn as `ballistic` parcels. Big chunks become objects, or rails bodies if fast enough (§4.9) |
| landing | | parcels `land` and `deposit`: ground-forming material as kind A deposits, boulders as kind D placed records. Objects sleep and settle. Fires and dust are deltas on fields (10) |
| afterwards | | everything is persisted by content kind on touched subtrees (R7, 15). Requests expire, and the region coarsens |

**The beaver dam** (scenario 3), logs as objects. Ownership follows R11: the owner is the type's definer, and
ownership never moves.
- **Felling.** `vegetation.fell(tree)` is vegetation's service. The standing tree becomes a stump, a kind B candidate
  override (14). The trunk and crown become an object body owned by vegetation, keeping their instances. It falls
  with rigid contact and sleeps. Its type settles with `never`, because beavers will want it.
- **Carrying.** The beaver plugin calls `hold(log, beaver_body, jaw_bone, pose)`. The log's blobs go to `bone` mode,
  and the beaver walks (character controller) to the creek. Holding doesn't change the owner.
- **Placing.** At the dam site the beaver places its **own** content: a `dam_log` object of the same shape, owned by
  the beaver plugin. Through a vegetation service, it asks for the felled log to be removed. No ownership transfer is
  needed.
- **The creek.** The `dam_log` is `rigid`. Buoyancy and drag along the water's flow velocity move it downstream until
  it lodges against others. It sleeps, then settles with `keep_instance` as a kind D placed record, so the dam keeps
  its log shapes.
- **The pond.** The dam's blobs are now attached. Hydrology reads them as an obstruction in its routing elevation (14
  and the hydrology plugin), and the pond forms over the next steps (10).

**Dig, leave, return** (scenario 5):
- The dig is a kind A, generator-input edit (R7): the terrain's `dig` service writes a `z` edit, or 3D edit terms if
  it undercuts (14 §5.3). The blob store sees removals plus floor additions.
- Detachment: the ground service reports nothing loose unless the dig undercut something (§4.6).
- On leaving, any awake objects are dropped and put to sleep. Settling ones become placed records. When the region
  unloads, the rest (including `never` objects such as the felled log) are stored as kind D placed records flagged to
  come back as objects (15).
- On return, it is regenerated with the region. Decay of the log over years is a slow process, the vegetation
  plugin's job.

**Orbit to grass** (scenario 1), at embark:
- A pending observer (10 §4.9) stands at the embark point. The avatar's dynamic body is created only once the sim
  tier there reaches the character's contact level. The zoom animation covers the wait (18).
- It is placed 1 m above the surface query's result and snaps down within a tick.

## 5. Interfaces

**Declarations** live in the manifest (R2): fast-layer materials and per-type physics properties. There is no runtime
registration. A material entry, in 08's `materials.fast` shape, with the keys physics needs:

```json
"materials": {
  "fast": [
    { "name": "lava", "rheology": "bingham_t", "density": 2600, "h_typ": 3.0,
      "tau_y_ref": 3000, "eta_ref": 5000, "t_ref": 1150, "t_scale": 100,
      "stop_speed": 0.3, "rest_time": 2, "dt_max": 10, "coalesce_speed": 0.5, "max_volume": 4.0e5,
      "flags": ["never_rest_hot", "stop_in_sea"],
      "step": "lava_step", "deposit": "lava_deposit", "land": null, "on_skip": "lava_on_skip" }
  ]
},
"types": [
  { "name": "dam_log", "kind": "D", "material": "beaver:wood",
    "fields":  { "std:density": 550, "std:friction": 0.6 },
    "physics": { "settle": "keep_instance", "contact_level": -1, "restitution": 0.1 } }
]
```

- Rheology families: `turbulent`, `voellmy`, `coulomb`, `bingham_t` (Bingham with `x = x_ref · 30^((t_ref − T)/t_scale)`),
  `ballistic`, `buoyant`.
- Hooks are exports named in the entry, all batched.
- `physics` on a type is a proposed addition to 08's type entry. Settle policy, contact level and restitution are
  per-type constants, not fields.

**Core imports** (C ABI; names per 08 §5.2, which is canonical). Handles are `u32` (D6); arrays are pointer plus
count; f64 positions are in the named body's frame.

```c
// ---- parcels ----
u32  ss_parcel_spawn(u32 material, u32 body, const f64 pos[3], const f32 vel[3],
                     f64 volume, f32 aux, u32 group);          // pair with ss_field_transfer for the volume (10 §4.4)
void ss_parcel_state(const u32* h, u32 n, ss_parcel_state_t* dst);
typedef enum { SS_DEPOSIT_FIELD, SS_DEPOSIT_GROUND /*kind A*/, SS_DEPOSIT_PLACED /*kind D*/,
               SS_DEPOSIT_REST, SS_DEPOSIT_KEEP } ss_disposition;

// ---- objects (own types only, R11; holding and impulses work on any object) ----
u32  ss_object_from_blobs(const u32* blobs, u32 n);            // -> body; keeps instances (R11), mass properties
u32  ss_object_from_placed(u32 placed_record);                 // pick up a settled thing
void ss_object_hold(u32 object, u32 holder_body, u32 bone, const f32 local_pose[7]);
void ss_object_release(u32 object, const f32 vel[3], const f32 ang_vel[3]);
void ss_apply_impulse(u32 body, const f64 point[3], const f32 impulse[3]);
void ss_object_settle_now(u32 object);
void ss_body_motion(const u32* bodies, u32 n, ss_motion_t* dst);  // pose, velocity, asleep, settled

// ---- characters (a dynamic body; appearance is a model instance, 16) ----
typedef struct { f32 radii[3]; f32 step_height, max_slope_deg, mass, swim_density; } ss_character_desc;
u32  ss_character_create(u32 body, const ss_character_desc* d);
void ss_character_intent(u32 character, const ss_intent_t* intent);   // plugin-driven creatures; the player's come from commands
void ss_character_state(const u32* c, u32 n, ss_character_state_t* dst); // grounded, in water, ground normal, support

// ---- detachment (runs automatically after removals; exposed for services) ----
void ss_detach_check(u32 body, const ss_region* r);            // calls the ground provider's detachment service (14) + the BFS

// ---- contact information for behaviour (16) ----
u32  ss_contacts(u32 body, ss_contact_t* dst, u32 cap);         // other body, point, normal, impulse
```

**Events physics emits** ([17](17-events-and-discovery.md), through `ss_event_*`): `impact` (body, point, speed,
energy), `detach` (island, volume), `settle`, `splash` (a parcel or object entering water), `launch`, `rockfall`.

**Fields physics reads** (09's seed list, R12): `std:density`, `std:friction`, `std:wind` (point domain),
`std:water` (for which blobs are water). One field is a proposed addition to 09's list: `std:flow_velocity` (point,
computed, `f32x3`, m/s, steward hydrology), the water's flow velocity for drag on floating objects (§4.4).

**What physics needs from other docs:**
- **07:**
  - `ss_field_eval` against a body's static set, at a requested scale, **plus a Lipschitz bound per point** (not in
    07's signature yet);
  - `ss_surface_along` and `ss_surface_project`;
  - `ss_overlap` (REACH test) for the connectivity graph, and `ss_components`;
  - per-body local structures for objects.
- **06:**
  - the physics requester (region, level, priority, until);
  - the split latency `L_split`;
  - each level's representative error bound.
- **04:**
  - body creation as a child of the planet;
  - body-fixed ↔ inertial conversion;
  - `elements_from_state`;
  - analytic prediction of switch-radius crossings;
  - the frame model of §4.2 (R8) and the atmosphere density model (§4.9).
- **14:** the ground provider's detachment service (14 §4.7; not yet listed in 14 §5.3), and mass from the implicit
  solid.
- **05:**
  - the 2-bit dynamics mode;
  - instance ids allocated deterministically;
  - per-body blob frames.
- **13:** uses 04's atmosphere model too, so the glow and the drag agree.

## 6. Risks and unknowns

- **The field is not a distance.** Contact depth and normals are noisy where `k` varies a lot. The Lipschitz scaling
  is conservative, but it may make contacts soft or jittery. E11.4 measures it. The fallback is to evaluate contacts
  against each nearby blob's own ellipsoid distance (exact) and use the blend only for the normal.
- **Coarse contacts before refinement** can be visible for `L_split` ticks: a rock hovering, a parcel sinking.
  Prediction-based requests should hide most of it. It needs measuring at the split latency 06 achieves.
- **Point-sampled contact misses thin features** between samples (a thin plate). Sample density scales with blob
  size, and fast objects use conservative advancement. Thin static features (pine needles) are below the contact
  level anyway.
- **Settling mid-pose:** a rock asleep on a slope could settle in a pose that would have rolled. Sleep thresholds must
  be tight enough. E11.7 looks for it.
- **Parcels in true 3D terrain** (caves, overhangs, arches) are untested. The re-seating rule under overhangs is a
  guess.
- **The connectivity budget's "presume anchored"** means an island can float for `k` ticks until the async check
  lands. It's rare, but it can be seen.
- **Cost depends on 07's field evaluation speed,** which is blob evaluations per point: ~10–20 overlapping terrain blobs
  at a point, per `world/` measurements. If it is slow, rigid objects are the first casualty.
- **Dropping fictitious forces** (§4.2, R8) makes long throws inside the switch radius slightly unphysical. That is
  invisible for anything we simulate now, but it would need revisiting if orbital play arrives (Q04.5; Q11.1, C2).

## 7. First experiments

| id | experiment | success criterion |
|---|---|---|
| E11.1 | **Port `mobile.js`** to the core language with 3D field contact on a blob terrain. Rerun `PLUGINS.md`'s slab test (2k / 20k / 200k m³ on a 33° flank) and the outburst flood. | runouts and peak speeds within 15% of the prototype's (~3 km and 9.5 km; 26 / 35 / 39 m/s); mass to 1e-12; cost per parcel step reported |
| E11.2 | **Determinism.** The same avalanche plus 50 falling rocks, run with two different frame pacings. | bit-identical fingerprints |
| E11.3 | **Rigid contact on blobs.** Drop 50 rocks on a blob hillside. | rest penetration < 2 cm at level 0; asleep within 2 s; no jitter at rest; cost per awake object ≤ 10 µs per tick |
| E11.4 | **Field vs distance.** On terrain where `k` ranges over 0.05–2 m, compare contact depth and normal with the exact distance (brute force). | depth error < 20% of `k_min`, normal error < 10°; else adopt the per-blob fallback (§6) |
| E11.5 | **Character controller.** A 1 km path over blob terrain: 0.3 m steps, 45° and 55° slopes, an overhang, a lake. | never stuck; steps up 0.4 m; slides above 50°; swims and climbs out |
| E11.6 | **Connectivity.** 100 random digs under overhangs and arches. | islands match a brute-force flood fill 100%; p95 cost ≤ 0.5 ms |
| E11.7 | **Settling.** 200 dropped rocks settle with `keep_instance`. | 0 px change at the settle moment; placed-record size reported; no settled rock in a pose the brute force says would still roll |
| E11.8 | **Rails handoff.** Throw a body upward at 0.9× the circular speed: integrated → rails → integrated on re-entry. | specific orbital energy continuous to 1e-6 relative across both handoffs |

## 8. Build order

| milestone | physics work |
|---|---|
| M1 | none (bodies on rails only, 04) |
| M4 | the character controller (E11.5); rigid objects v1 (sampled contact, PGS, sleep) (E11.3, E11.4); connectivity and detachment (E11.6); the physics requester and contact levels; determinism test (E11.2, partial) |
| M5 | the fast layer port: parcels, materials, islands, LOD, exchange (E11.1); settling (E11.7); buoyancy and flow drag; wind; deposits as terrain blobs |
| M6 | impacts: rails → integrated, conservative advancement, ejecta (E11.8); rockfalls from big islands; two-way parcel ↔ object coupling if Q11.5 says so |
| later | glaciers (a plugin); structural stress; fictitious forces if orbital play arrives (§4.2); ablation |

## 9. Open questions

**Q11.1 Day length vs small planets.** (merged into `questions.md` C2)
- *Why it matters:* with fictitious forces included, a planet that spins faster than about `5 × T_s` has noticeable
  centrifugal force. At or near `T_s` (~24 min for 500 km), the surface flies off.
- *Options:*
  - (a) long days (≥ 2 h at 500 km), centrifugal included;
  - (b) any day longer than ~24 min, with fictitious forces dropped inside the switch radius and an exact frame
    conversion at it (04's model).
- *Working assumption (R8):* (b) with a ~1 real-hour day. The analysis of (a) stays in §4.2 as the alternative.

**Q11.2 Settling.**
- *Question:* is it OK that dropped rocks, logs and debris freeze into the static world a few seconds after coming to
  rest? It is invisible with `keep_instance`, but they won't roll again unless something disturbs them.
- *Recommended:* yes, with per-type opt-out (`never`).

**Q11.3 What the player can do physically.** (merged into `questions.md` C3)
- *Options:* walk, run, jump, swim, climb? Carry and throw? Push objects? Be hurt by falls, avalanches, lava?
- *Why it matters:* it sets the scope of characters, holding and coupling.
- *Recommended for M4–M5:* walk, jump, swim, carry and throw small objects. Hazards push the player but don't harm
  them, until there is a gameplay reason.

**Q11.4 Overhangs.**
- *Question:* only fully disconnected islands fall, or should thin overhangs and long cantilevers collapse under their
  own weight?
- *Recommended:* islands only for now; a stress plugin later if digging feels too "floaty".

**Q11.5 Two-way coupling of flows and objects.**
- *Question:* should an avalanche or flood pick up and carry logs, rocks, the player?
- *Recommended:* the player yes (it's cheap, §4.5). Objects later (M6), since it needs per-pair momentum exchange.
