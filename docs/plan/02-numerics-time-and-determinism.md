# 02. Numerics, time and determinism

Status: draft · Depends on: README (D4, D5, D6, D-levels), [01](01-runtime-and-toolchain.md) · Used by: every doc that
stores state; especially [04](04-bodies-and-orbits.md), [06](06-blob-hierarchy-and-cut.md),
[09](09-fields-and-composition.md), [10](10-simulation-and-scheduling.md), [11](11-physics.md),
[12](12-rendering.md), [15](15-persistence.md), [17](17-events-and-discovery.md)

## 1. Purpose and scope

This doc sets the numerical ground rules everything else stands on:
- how positions are represented from galaxies down to eyelids;
- which frame each number lives in;
- the units;
- how time is counted;
- what makes the simulation deterministic, and how we check it;
- the checklist that keeps multiplayer possible later.

In scope:
- precision analysis and frame rules;
- the GPU-side coordinate requirement (the passes themselves are in [12](12-rendering.md));
- units;
- ticks, seconds, phases, time warp as a representation (the governor policy is in [10](10-simulation-and-scheduling.md));
- deterministic arithmetic;
- ordering and reductions;
- NaN policy;
- the RNG;
- fingerprints, cross-engine tests and replays;
- the multiplayer-readiness rules.

Out of scope:
- orbital mechanics ([04](04-bodies-and-orbits.md));
- field schemas ([09](09-fields-and-composition.md));
- the test harness ([19](19-tooling-and-testing.md));
- network protocol design (not planned).

## 2. Requirements

| | requirement | traces to |
|---|---|---|
| must | Sub-millimetre position precision on any planet surface, at any distance from the planet's centre, for blobs, contacts and the camera. | creatures' eyelids are ~1 cm; "a spherical planet you can walk around" |
| must | View distance without jitter or depth fighting, from 1 cm to galaxies. | constraint "near-infinite view distance"; pillar 2 |
| must | Orbits and spins exact and periodic forever: no drift, no growing error with game time. | Joel: "entirely deterministic and periodical" |
| must | The same inputs (seed, plugin binaries, commands) give bit-identical sim state on every machine and browser. | D5, multiplayer later, replays, tests |
| must | Where the camera looks doesn't change the simulation. Only observers (sim state) do. | pillar 5, multiplayer, the observer effect |
| must | Randomness is reproducible, order-independent and parallel-safe. | D5; `planet/` hashed every random choice from ids |
| should | Conserved quantities (water, snow, lava, sediment) are conserved exactly, independent of evaluation order. | `planet/` measured mass balances to 4.5e-15; [09](09-fields-and-composition.md) |
| should | A divergence between two runs is located to the tick, system and blob automatically. | the collaborative, measured workflow |
| later | Lockstep or state-sync networking. Nothing here is built for it, but nothing may block it. | constraint "multiplayer later" |

## 3. What the prototypes taught

- **Physical units made processes scale-free** (`planet/DESIGN.md` §2.4).
  - Fire fronts at m/s, river transit at distance / velocity, lava deposits in m³ per metre: the same code ran on
    10 km and 1 m cells.
  - The one process written in "cells per step" (neighbour transfer) ran away.
  - Units are a correctness tool, not documentation.
- **Every random choice was a hash of an id** (`planet/js/util.js`): "a cell's detail never depends on when or why it
  was generated". Candidate tree sites and ranks were hashes of the entity cell id (`DESIGN.md` §2.6).
- **Order dependence creates phantom state.**
  - A lift that read neighbours which might or might not exist yet produced 1,054 spurious residuals; with the
    neighbour invariant, 0 (`DESIGN.md` §4).
  - In the world prototype, GPU cell lists filled by atomics came out in a different order every build. Float sums
    over them differed, flipped 8-bit samples, and made edits non-local, until each list was sorted by pool index
    (`M7.md` §7).
- **Determinism checks paid off.** The plugin port was verified bit-identical by field totals to 10 digits after
  spin-up, refinement and 60 days (`planet/tools/fingerprint.mjs`).
- **NaN spreads through a coupled simulation.** A stale array gave NaN snow. It ran down the rivers, and the NaN
  discharge made the mesher's flood fill cover the whole graph (`PLUGINS.md` §1.2). Non-finite values must be
  stopped at boundaries, not discovered downstream.
- **Large coordinates bite on the GPU.**
  - On the 250 km planet, noise coordinates had to be made relative to the mesh origin, with per-octave lattice
    offsets reduced mod 256 in doubles on the CPU (`DESIGN.md` §6).
  - Blobs 2 m wide at 5 km needed a ray–ellipsoid solve from the closest approach instead of the textbook formula,
    which "cancels every digit", plus a cancellation-free dual quadric for screen bounds (`M7.md` §2).
  - The ray cut pushed ray exit points across node faces, because a nearly parallel ray would not cross in float
    precision (`RAYCUT.md` §3).
- **Fast-forward is an observer effect in time.** Skipped time lost the dynamics of moving parcels. The fix,
  "nobody observes a fast-forward, everything goes implicit", changes results depending on time warp
  (`PLUGINS.md` §2). So warp has to be sim input, not a viewer setting.

## 4. Design

### 4.1 Precision at every scale

Spacing between adjacent floats (ULP) at a given magnitude:

| magnitude | what lives there | f32 ULP | f64 ULP |
|---|---|---:|---:|
| 1 m | a creature, a boulder | 1.2e-7 m | 2.2e-16 m |
| 1 km | the near field | 6.1e-5 m | 1.1e-13 m |
| 1e5 m | 100 km | **7.8e-3 m** | 1.5e-11 m |
| 5e5 m | the reference planet's radius | **3.1e-2 m** | 5.8e-11 m |
| 6.4e6 m | Earth's radius | **0.5 m** | 9.3e-10 m |
| 3.8e8 m | a moon's orbit | 32 m | 6.0e-8 m |
| 1.5e11 m | 1 AU | 16 km | 3.1e-5 m |
| 4.5e12 m | 30 AU | 520 km | 9.8e-4 m |
| 9.5e15 m | 1 light-year | 1e9 m | 2 m |
| 5e20 m | a galaxy's radius | – | 6.6e4 m |
| 1e24 m | between galaxies | – | 1.3e8 m |

Readings:
- **f32 in a planet frame fails.** 3 cm steps on a 500 km planet, 0.5 m on an Earth-sized one, against 1 cm
  creature detail. This is why D4 puts body-frame positions in f64.
- **f64 works because each body has its own frame.**
  - In a planet frame: 1e-9 m.
  - A planet in its star's frame: ~3e-5 m at 1 AU.
  - A star in its galaxy's frame: ~66 km. Stars are ~1e9 m across and ~1e16 m apart, so this is invisible.
  - Galaxies in the root frame: ~1e8 m, against galaxies 1e21 m across.
- **f64 is not enough without the tree.** A single universe-wide frame would put the player at ~1e5 m precision.
  The body tree is what makes the numbers work, not just the n-body cost ([04](04-bodies-and-orbits.md)).
- **WASM f64 scalar maths runs at f32 speed;** SIMD halves the lane count. So positions, velocities and anything
  composed across frames are f64. Shapes, radii, field values and blob-relative offsets are f32.

### 4.2 Frames and the NCA rule

**Frames** (the details of bodies are [04](04-bodies-and-orbits.md)'s):
- **Inertial body frame.**
  - Origin at the body. f64.
  - **Axes parallel to the root's axes**, proposed. Converting between two inertial body frames is then a pure
    translation (plus a velocity offset): no rotation error accumulates down the tree.
- **Body-fixed frame** for spinning bodies.
  - The inertial frame rotated by the body's spin: a fixed axis, plus an angle from a binary-angle phase (§4.5).
  - Surface blobs live here. They don't move as the planet turns, so their stored f64 coordinates never change.
- **Local frames for gameplay** (east/north/up at a point, or a creature's skeleton). These are derived and never
  stored as state.

**The NCA rule.** The vector from body A to body B is computed by walking each up to their nearest common ancestor C:
1. sum each one's offsets in its parents' frames (rotated where a body-fixed frame is involved);
2. subtract the two sums in C's frame;
3. then transform the difference into the destination frame.

Never go through root coordinates. The error is then the ULP at C's scale, which is always small relative to the
distance being measured:

| camera on | looking at | NCA | absolute error | angular error |
|---|---|---|---:|---:|
| a planet | its own surface, 10 km away | the planet | 1e-12 m | 0 |
| a planet | its moon (3.8e8 m) | the planet | 6e-8 m | 2e-16 rad |
| a planet | a sibling planet (~1e11 m) | the star | 3e-5 m | 3e-16 rad |
| a planet | another star (~1e16 m) | the galaxy | 6.6e4 m | 7e-12 rad |

A pixel at 1080p with a 60° field of view is ~1e-3 rad. Every row is far below it.

### 4.3 Coordinates on the GPU

The GPU has only f32. The scheme is [12](12-rendering.md) §4.2's **integer node keys with per-level camera cells**.
This doc adopts it and states what it guarantees.

1. **Draw in the focus body's body-fixed frame.** The f64 frames and the NCA rule (§4.2) bring the sun, moons and
   stars into that frame every frame, on the CPU.
2. **Node positions are integers.**
   - Each render node is one of [06](06-blob-hierarchy-and-cut.md)'s regions (edge 2^(ℓ+3) m).
   - Its key is `(level, ix, iy, iz)`: its corner in units of its size, on a cube around the body centre.
   - On a 500 km planet that needs ~17–21 bits per axis, so i32 holds it.
3. **The camera is split per level** into an integer cell and an f64 fraction. A node's camera-relative origin is
   `(node.ixyz − cam.ixyz[L]) · size_L − cam.frac[L]`. The difference is exact in integers, and small before it
   becomes a float.
4. **Blob records are f32 relative to their home node**, with a relative error of ~6e-8 of the node's size.
5. **No GPU structure stores planet-frame absolute coordinates, and nothing is ever rebased.**
   - The ray cut's octree used absolute f32 over a 16 km cube, ~1 mm at the far edge. On a 500 km planet that would
     be 3 cm.
   - With integer keys, precision is the same everywhere on the planet, with no drift.

Readings:
- The large part of the camera subtraction is exact integer arithmetic, so the shader's camera-relative positions
  carry only the ULP of the remaining distance: 8 mm at 100 km, a relative ~6e-8. That is always sub-pixel, and
  nothing jitters as the camera moves.
- Rays traversing world-space structures (shadows, AO, any GPU ray query) work in the same camera-relative space and
  switch to node-local coordinates on entry.
- Noise and procedural textures take node-local inputs, with per-octave offsets reduced on the CPU (`DESIGN.md` §6).

Depth (reversed-Z with an f32 depth buffer) and the multi-scale layers are [12](12-rendering.md)'s design.

### 4.4 Units

SI everywhere inside the core and the plugins:

| quantity | unit | notes |
|---|---|---|
| length | m | |
| mass | kg | |
| time | s (derived) and ticks (canonical) | §4.5 |
| temperature | K | the UI shows °C |
| angle | rad | unbounded phases are binary angles (§4.5) |
| volume | m³ | water-equivalent fields say so in their name or unit (`m3_we`) |
| velocity, acceleration | m/s, m/s² | |
| density | kg/m³ | |
| pressure, stress | Pa | |
| energy, power | J, W | |
| rates | per second | **never per tick and never per year inside code**; `planet/` declared process `dt` in years, which we don't repeat |

- Every field declares its unit in its schema with a small grammar (`m`, `m3`, `K`, `m/s`, `kg/m3`, `1` for
  dimensionless; [09](09-fields-and-composition.md)).
- Two plugins declaring the same field must declare the same unit, or loading fails. `planet/`'s registry did the
  same for aggregation, lift and unit.
- There is no automatic conversion. Manifests may write human units for parameters (`"6 h"`, `"2 km"`); the loader
  converts them once.

### 4.5 Time

**The canonical clock is a tick count.**
- `tick: i64`. One tick is 1/30 s (provisional, as in the README; Q02.1).
- Tick 0 is the universe epoch. One universe has one clock, so `(body key, position, tick)` is a meaningful address
  to share (Q02.4).
- An i64 lasts 9.7e9 years at 30 Hz.
- A save records the tick it was at. A new game starts at a fixed tick, after the world's spin-up
  ([14](14-planet-generation.md)).

**Seconds are derived.**
- `t = f64(tick − epoch) / 30`. The integer is exact up to 2^53 ticks (9.5 million years); the division adds at most
  half an ULP.
- Durations and rates use `Δt = f64(Δtick) / 30`. Those are small numbers.
- **Never accumulate time in floating point** (`t += dt`).

**Phases that grow forever are binary angles.**
- Orbit mean anomalies, body spin, and anything periodic are stored as a `u64` fraction of a turn:

```
phase(tick) = phase0 +% rate *% (tick − epoch)        // wrapping u64 arithmetic: exact mod one turn
rate        = round(2^64 / period_in_ticks)            // the period is *defined* by this integer
radians     = f64(phase >> 11) * 2^-53 * 2π            // only at use
```

- **Why not f64 seconds.** With f64 seconds, `M = n·t` at t = 1e12 s (~32,000 years) carries ~2e-11 rad of
  rounding: ~3 m of position error at 1 AU, and it *changes from tick to tick* (jitter).
- **What binary angles give.**
  - The phase is exact at any tick: zero error and zero jitter, forever, at the cost of one integer multiply.
  - The period is quantised to 2^-64 turns per tick. It is deterministic, and nobody can see the difference.
  - Kepler's equation is then solved in f64 from an exact mean anomaly, so the position error stays ~a × 1e-16
    (1.5e-5 m at 1 AU).
- **Open trajectories** (hyperbolic flybys, a thrown rock leaving the atmosphere) aren't periodic. They store an f64
  epoch offset from the tick at which they began ([04](04-bodies-and-orbits.md)), so Δt stays small.

**Cadences are integer tick periods.**
- A process with a 3-hour period runs every 324,000 ticks, at a phase offset hashed from its id to spread the load.
- Whether a process is due is pure integer arithmetic. [10](10-simulation-and-scheduling.md) owns the scheduler.

**Real time and game time: fixed timestep plus interpolation.**
- Each frame the engine adds real elapsed time × warp to an accumulator, runs that many whole ticks, and keeps the
  remainder as the render interpolation α.
- Wall-clock time decides *how many* ticks run, never *what a tick does*. So frame rate and machine speed can't
  change results.
- **Time warp is a command.**
  - At high warp, systems that step every tick (physics, the fast layer) can't keep up, and switch to their implicit
    modes ("nobody observes a fast-forward", `PLUGINS.md` §2). That changes results.
  - So warp changes are tick-stamped commands, logged like any input. Peers and replays then agree.
  - The governor policy (when to lower warp automatically) is in [10](10-simulation-and-scheduling.md). Its
    decisions are commands too.

**The day.** Day length comes from a planet's spin and year length from its orbit ([03](03-universe-and-identity.md),
[04](04-bodies-and-orbits.md)). There is no separate "sun clock" running faster than physics (Minecraft-style),
because it would contradict the body tree. In a physically rotating frame, the spin period would have a side-effect:
the centrifugal share of gravity at the equator is (P_crit / P)², where P_crit = 2π√(R/g):

| planet radius (g = 9.8) | P_crit (break-up) | day 1 h | day 2 h | day 4 h | day 24 h |
|---|---:|---:|---:|---:|---:|
| 250 km | 16.7 min | 7.8% | 1.9% | 0.5% | 0.01% |
| 500 km | 23.7 min | 15.6% | 3.9% | 1.0% | 0.03% |
| 6,371 km | 84 min | – (faster than break-up) | 49% | 12% | 0.3% |

The table explains why the forces matter. It is not a constraint we keep: **the working assumption (pending
questions.md C2) is [04](04-bodies-and-orbits.md)'s model.**
- Centrifugal and Coriolis forces are dropped below the switch radius.
- Surface physics uses radial gravity only.
- The conversion between body-fixed and inertial frames at the switch radius is exact
  ([04](04-bodies-and-orbits.md) §4.9).

So day length is a free gameplay choice, independent of planet size (any day longer than ~24 min). The working
default is a **~1 real-hour day**: sunsets come often. For this doc, the day is just a spin rate: a binary-angle
phase like any other.

### 4.6 Deterministic arithmetic

WebAssembly specifies IEEE 754 binary32 and binary64 with round-to-nearest-even:
- every basic operation (`+ − × ÷ sqrt`, `min`/`max`, conversions, `ceil`/`floor`/`trunc`/`nearest`) gives
  identical bits on every engine and CPU;
- subnormals are kept (no flush-to-zero).

What remains nondeterministic, and the rule for each:

| source | rule |
|---|---|
| NaN sign and payload bits | NaN must never reach state (§4.8). Then NaN bits are irrelevant |
| relaxed SIMD (`relaxed_madd`, relaxed min/max, swizzle, lane select) | **compiled out**: the feature is off in `-mcpu` for the core and the SDK templates. The plugin loader rejects modules that use relaxed SIMD opcodes (a validation pass over the code section) |
| transcendental functions | **the core uses a vendored libm**, never JS `Math` (ECMAScript leaves `sin`, `exp`, `pow` to the implementation, so engines differ) and never a toolchain's libm that can change on upgrade |
| fused multiply-add | WASM has no deterministic FMA instruction. Don't use one (`@mulAdd` falls back to a slow software `fma`: deterministic but slow) |
| compiler fast-math | forbidden in sim code: Zig keeps `@setFloatMode(.strict)` (a lint rejects `.optimized` under `core/sim`); C/C++ plugins use `-ffp-contract=off`, never `-ffast-math` |
| integer overflow | wrapping ops (`+%`, `*%`) only where intended (hashes); otherwise overflow is a bug, trapped in ReleaseSafe |
| float → int | saturating conversions only (`nontrapping_fptoint`) |
| the host | the JS glue copies bytes and never computes sim values |

**The libm.** Proposed: **correctly rounded** functions (the CORE-MATH project's C sources, built with `zig cc`) for
`sin cos tan asin acos atan atan2 exp exp2 expm1 log log2 log1p pow cbrt hypot sinh cosh tanh`.
- A correctly rounded result is unique, so a future change of implementation can't change a single bit. That is the
  strongest guarantee available, and its common-path speed is reported close to ordinary libms.
- Verify the licence and the WASM build in M0 (E1).
- Fallback: a vendored, frozen copy of musl's libm (what Zig's `std.math` is ported from).

**Plugins bring their own maths inside their binaries.** That is fine: **determinism holds per binary**. Every peer
runs the same plugin bytes, identified by content hash, so the same instructions give the same bits whatever libm
the author used. A plugin update can change results, which is why plugin hashes are part of a session's identity
(§4.11).

### 4.7 Ordering and reductions

**Rule:** a tick's result is a function of sim state and commands only. It must not depend on any of:
- handle values or allocation order;
- thread scheduling or job completion order;
- hash-map iteration order;
- which camera looked where.

How each kind of computation meets it:
- **Per-blob kernels** (each output from its own inputs) are order-free by nature. Most of the sim is like this.
- **Queries** return results in canonical order: neighbours sorted by (distance, key), ray hits by (t, key). Ties
  are broken by key, never by handle ([07](07-spatial-queries.md)).
- **Combining contributions** (several processes or neighbours adding deltas to one blob) happens only at
  [10](10-simulation-and-scheduling.md)'s stage barrier. Processes read a stage snapshot and write deltas, transfers
  or owned values ([09](09-fields-and-composition.md)'s write kinds). For a conserved field, the dtype is pending
  (Q02.5, E4), one of two:
  - **Canonical-order float sums (f64).** Contributions are summed in the **canonical order defined in
    [10](10-simulation-and-scheduling.md) §4.4** (sorted by stable ids, never by arrival). This is also how every
    non-conserved field combines.
  - **Fixed-point quantities (`q64`).** For conserved extensive quantities (water, snow, lava, sediment, mass), store
    an `i64` with a per-field power-of-two scale. Process outputs are rounded once to the scale (deterministic), and
    then sums are exact integers, so they're **order-free and conserve exactly**.
    - Jobs can merge their outputs in any order.
    - Restriction is an exact integer sum.
    - Lift projection hands out the remainder by the largest-remainder rule, ties broken by key.
    - Range: at a scale of 2^-8 m³ (~4 ml), an i64 holds ±3.6e16 m³. That covers every top-level cell of a 500 km
      planet (its whole ocean is ~4e15 m³), but not Earth's ocean (1.3e18 m³).
    - A field that needs more uses a coarser scale or i128 totals. Zig has i128 natively in software; summing it is
      cheap.
  - The cost of each is measured in E4 before [09](09-fields-and-composition.md) fixes the default.
- **Job outputs** are applied at their due tick, in [10](10-simulation-and-scheduling.md)'s canonical order
  ([01](01-runtime-and-toolchain.md) §4.4).
- **Allocation.** Within one run, handles come from free lists in a deterministic order. But different histories
  (another peer, a reloaded save) number things differently. A debug mode **shuffles free lists** so that any
  dependence on handle numbering shows up as a changed fingerprint (§4.11).

### 4.8 Non-finite values

- **Checked at every boundary.**
  - Every batch a plugin returns (deltas, new blob records, resource writes) is scanned for NaN and ±Inf while it is
    copied back. A SIMD pass over data already being copied costs almost nothing (E5 measures it).
  - Core systems (physics, the fast layer, lift projections) run the same scan on their outputs.
- **The response is deterministic.**
  - The offending values are replaced by zero (or the field's default).
  - An event `nonfinite {plugin, field, key, tick}` is logged.
  - After N incidents within a window, the plugin is suspended.
  - All of this depends only on sim state, so every peer suspends it at the same tick.
- **Debug builds** also assert finiteness on every stored column after each system, to catch the bug at the system
  that caused it.
- `q64` fields saturate on overflow and log an event, just like non-finite floats.

### 4.9 Commands, and separating sim from render

**Sim state** is everything the next tick depends on:
- bodies (rails elements and integrated states);
- **the cut's sim tier** and its blobs' geometry and stored fields;
- resources and parcels;
- **observers**: the avatar, or a pending observer during embark ([10](10-simulation-and-scheduling.md)). Cameras
  are not observers; they only make render-tier requests;
- pending jobs;
- the command queue;
- the clock.

Everything else is derived and may differ between machines: render-only blobs, GPU buffers, interpolated positions,
the camera's view, UI state.

**Commands.**
- Every input that can affect sim state becomes a command `{tick, player, seq, kind, payload}`: movement intent,
  tool use, embark, selection that changes an observer, time warp, plugin swaps (dev builds).
- Commands are applied at the start of their tick, in order of (player, seq).
- A command may carry values that came from the GPU or the wall clock. A dig at a picked point, for example, carries
  that point's exact coordinates. They're *inputs*: once logged, replays and peers use the logged values.

**The cut's two tiers ([06](06-blob-hierarchy-and-cut.md)): adopted here as a determinism requirement.**
- The **sim tier** is a function of sim state alone: observers' positions, process bands, physics contacts, events,
  and (later) peers' observers. All sim processes run on it.
- The **render tier** may refine further.
  - Its **render-only blobs** come from the same deterministic split functions applied to sim-tier leaves.
  - They are drawn and may be dropped at any time.
  - They are never simulated, never persisted, never read by a process.
  - Detail atoms (grass, pebbles) are render-tier only, generated from fields.
- So two players looking in different directions from the same spot produce the same world, and a camera can't
  cause an observer effect. Only an observer's *position* can, and that is sim state, shared.
- A render-only blob that later enters the sim tier is regenerated bit-identically, because splits are pure
  functions of keys and persisted state ([06](06-blob-hierarchy-and-cut.md)).
- What this costs: distant detail seen through the camera is "natural detail" (regenerated), not simulated. That
  matches `planet/`'s design, where the GPU drew the continuous detail field inside coarse cells.

**The renderer interpolates.** Moving things (bodies, parcels, creatures' bones) keep their previous tick's pose.
The renderer draws `lerp(prev, cur, α)` (slerp for orientations). Static blobs need no second copy.

### 4.10 GPU results never feed sim state

GPU floating point is not bit-identical across vendors and drivers: fused operations, approximate transcendentals,
different reduction orders.
- Every query the sim makes runs on the CPU, in the core ([07](07-spatial-queries.md)).
- The GPU may compute:
  - anything visual;
  - cosmetic effects that never come back (spray, splash particles, grass sway);
  - render-tier refinement feedback (the ray cut's words);
  - inputs to commands (picking).
- That last route is the only one from GPU results to sim state, and it goes through the logged command.

### 4.11 Randomness

**Counter-based and addressed:** every random value is a pure function of *what it is for*. There is no stateful
generator in sim state.

```
mix64(z):                         // SplitMix64 finaliser (Stafford variant 13)
  z = (z ^ (z >> 30)) *% 0xbf58476d1ce4e5b9
  z = (z ^ (z >> 27)) *% 0x94d049bb133111eb
  return z ^ (z >> 31)

absorb(h, w) = mix64(h ^ (w *% 0xd6e8feb86659fd93 +% 0x9e3779b97f4a7c15))
stream_seed(seed, stream, words…) = fold(absorb, seed ^ stream, words)     // key.hi, key.lo, tick, …
draw(s, i)   = mix64(s +% (i +% 1) *% 0x9e3779b97f4a7c15)                    // SplitMix64 in counter mode
u01_f64(x)   = f64(x >> 11) * 2^-53          // exact: an integer conversion and a power-of-two scale
u01_f32(x)   = f32(x >> 40) * 2^-24
```

- `stream` is a 64-bit id: the hash of a namespaced name `"<plugin>:<name>"`, e.g. `"volcano:eruption"`.
  - Stream names are declared in the manifest; there is no runtime registration.
  - The SDK computes the hash header-only, and the loader checks for collisions across plugins.
- For plugin code, `seed` is `ss_call_seed()`: the universe seed absorbed with the plugin's id.
- Keys are 128-bit, so two words.
- Draws are **addressed, not sequential**. How many draws one process takes never shifts another's.
- The algorithm is integer-only, so it gives identical bits in every language. The SDK ships it as header-only
  inline code, so plugins don't pay a call and can't get a different answer.

**Why this generator:**

| option | for | against |
|---|---|---|
| **SplitMix64-style mixing** (proposed) | cheapest (two multiplies per draw); counter mode is a known-good use; trivial in any language | a weaker design margin than Philox; must be checked on our access patterns (E3) |
| Philox4x32-10 (Random123) | designed counter-based; BigCrush-clean; 4 outputs per call | ~2–3× the cost; more code to port to every SDK language |
| Squares (Widynski) | counter-based with a key | needs specially generated keys, an awkward fit for keys derived from paths |
| a stateful PCG per stream | well studied | state to persist and synchronise, and order-dependent: rejected |

Distributions are built on `u01` with our libm: normal by Box–Muller, Poisson by inversion for small λ and PTRS
for large, power laws by inverse CDF. The SDK provides the common ones.

**Worked examples.**

1. **A tree candidate's site and rank** (`DESIGN.md` §2.6). An entity cell has key K and 8 candidates i.
   ```
   s_site = stream_seed(seed, "veg:site", K.hi, K.lo);   x_i = u01(draw(s_site, 2i)); y_i = u01(draw(s_site, 2i+1))
   s_rank = stream_seed(seed, "veg:rank", K.hi, K.lo);   rank_i = u01(draw(s_rank, i))
   dither = u01(draw(stream_seed(seed, "veg:dither", K.hi, K.lo), 0));  n_trees = floor(count + dither)
   ```
   No tick appears, so sites and ranks are fixed forever and identical in every refinement: "the same count always
   shows the same trees".
2. **An eruption trigger.** The volcano process is due every 6 h (648,000 ticks). For volcano resource V at due
   tick T:
   ```
   s = stream_seed(seed, "volcano:erupt", V.hi, V.lo, T)
   erupt = pressure >= 1 || u01(draw(s, 0)) < hazard(pressure) * dt
   vent  = (u01(draw(s, 1)), u01(draw(s, 2)));  rate = lerp(20, 100, u01(draw(s, 3)));  …
   ```
   The result is independent of how many volcanoes exist, their order, and which worker ran them.
3. **The asteroid schedule.** Impacts on planet P are drawn per time window W (say one game year, Y ticks):
   ```
   s  = stream_seed(seed, "impact:window", P.hi, P.lo, W)
   N  = poisson(λ_P * Y_seconds, draw(s, 0))
   for j < N:  d_j    = d_min * (1 − u01(draw(s, 1 + 4j)))^(−1/α)     // Pareto sizes: many small, few large
               tick_j = W*Y + floor(u01(draw(s, 2 + 4j)) * Y)
               dir_j  = sphere(draw(s, 3 + 4j), draw(s, 4 + 4j))
   ```
   Any planet's impact history is a pure function of (seed, P, W). It can be computed lazily for windows nobody
   watched, and it's identical for every player. The size exponent α is where the Zipf balance comes in
   ([17](17-events-and-discovery.md)).

### 4.12 Verification

- **Fingerprints.**
  - `eng_fingerprint(scope)` hashes sim state in canonical order with the same mixer: bodies by key, sim-tier blobs by
    key (geometry and stored fields), resources, parcels, observers, the clock.
  - Per-system scopes localise divergence.
  - Readable field totals are printed alongside, as `planet/tools/fingerprint.mjs` did.
  - At 2 M blobs × ~200 bytes, a full hash costs an estimated tens of ms. Tests take it every N ticks, and per tick
    on small worlds.
- **Golden fingerprints** are committed for the scenario suite. A change must be explained in its commit ("expected:
  hydrology's evaporation rate changed"); an unexplained change is a bug ([19](19-tooling-and-testing.md)).
- **Cross-engine runs.** The headless suite runs the WASM build in V8 (Node), JavaScriptCore (Bun, or the `jsc` shell
  via jsvu) and SpiderMonkey (the `sm` shell via jsvu). These are shells, not browsers, so it's cheap enough for CI.
  The browsers get the same check from a test page. Fingerprints must match exactly.
- **Native vs WASM.** Native builds are for fast unit tests only. Fingerprints are defined on the WASM build.
- **Replays.** A replay is: seed, core hash, plugin hashes, the starting snapshot (or tick 0), and the command log.
  Replaying must reproduce every checkpoint fingerprint. Replays are also the bug-report format.
- **Invariance tests**, each comparing fingerprints that must be equal:

  | test | varies | proves |
  |---|---|---|
  | camera paths | two different camera paths, same observer path | the cut's two tiers: the camera has no effect on the sim |
  | handle shuffle | free-list order randomised | no dependence on handle numbering |
  | worker count and delay | 1 vs N workers, random artificial delays | deterministic async |
  | save/load | save at tick T, load, continue vs uninterrupted | persistence is complete ([15](15-persistence.md)) |
  | warp | the same commands at different frame rates | fixed timestep (warp itself is a logged command) |

### 4.13 The multiplayer-readiness checklist

Other docs follow these rules. Each has a check.

| # | rule | why | checked by |
|---|---|---|---|
| 1 | Every sim-affecting input is a tick-stamped command | lockstep and replays need the full input | replay tests |
| 2 | The sim reads no wall clock, no ambient randomness, no GPU results | these differ between machines | the core's import list (only `host_now_ms`, marked metrics-only); plugin import lists validated at load |
| 3 | The renderer never writes sim state; render-only blobs never feed the sim | camera independence | the camera-path invariance test |
| 4 | Keys, not handles, on disk and on the wire | handles differ between histories | the handle-shuffle test; serialisation APIs take keys only |
| 5 | Observers are sim state; the sim tier is a function of observers and state | the observer effect must be shared and bounded | the camera-path test |
| 6 | Core and plugin binaries are identified by content hash; a session records the set | same binary, same bits | load-time check |
| 7 | Canonical iteration and reduction order; job outputs merged canonically | thread timing must not matter | the worker-delay test |
| 8 | Persistence is snapshots plus regeneration: keys, residuals, edit logs, resources | a joining peer regenerates the rest; the save format is the sync format | save/load and regeneration tests ([15](15-persistence.md)) |
| 9 | The sim tier's requesters are the interest set: each peer's observer is a requester | interest management for free | design review in [06](06-blob-hierarchy-and-cut.md) and [10](10-simulation-and-scheduling.md) |
| 10 | Time warp and pause are commands | one shared clock | replay tests |
| 11 | The float rules of §4.6 | bit-identity across engines | cross-engine runs |
| 12 | Asynchronous work lands at fixed tick latencies | worker speed must not matter | the worker-delay test |

### 4.14 Scenario walkthroughs

- **1 Orbit to grass.**
  - The camera path from galaxy to grass crosses frames: galaxy, then star, then planet's inertial frame, then the
    planet's body-fixed frame. Every displayed vector is computed by the NCA rule in f64 and cast once.
  - Integer node keys keep the near field exact at any radius (§4.3).
  - The transition is render state. **Embark is a command:** at its tick it creates the player body, an observer, so
    the sim tier starts refining there.
  - The zoom animation's early cut requests are render-tier requests until then.
- **2 The storm and the flood.**
  - Climate is due every 12 h and hydrology every 3 h, in integer ticks.
  - The storm's track is drawn from `stream_seed(seed, "climate:storm", P, T)`.
  - Water is conserved: as `q64` (pending Q02.5) its sums are exact in any order; as f64 they follow 10's canonical
    order. Either way the result doesn't depend on which job finished first.
  - At high warp the same ticks run, just more per frame. Results match a real-time run unless warp switched the
    fast layer to implicit mode, which is a logged command.
- **4 The asteroid.**
  - The impactor comes from the (planet, window) schedule.
  - On rails its position comes from an exact mean anomaly (a binary angle, or an f64 epoch offset on a hyperbola).
  - The atmosphere-entry tick is computed analytically in f64, deterministically, and rounded up to a tick. That is
    when it switches to integrated motion ([04](04-bodies-and-orbits.md)).
  - The fast layer's parcels use fixed substep counts per tick ([11](11-physics.md)).
- **5 Dig, leave, return.**
  - Ten years is 9.5e9 ticks: well within i64. Spins and orbits are exact after any jump.
  - The dig command carries its picked position, recorded exactly.
  - Catching up the region runs the processes' canonical ticks ([10](10-simulation-and-scheduling.md)).
  - The felled tree's candidate ranks are the same hashes as before.
- **6 A community plugin.** Its libm is inside its binary. Its import list is validated, with no clock and no
  randomness. Its relaxed-SIMD opcodes, if any, are rejected at load. Its outputs pass the NaN scan. Its hash joins
  the session's identity.

### 4.15 Disagreement with the README

No disagreement. Three refinements to D4 and D5 that other docs should adopt:
1. **"Our own libm" applies to the core.** Plugins may use any maths compiled into their binary, because determinism
   holds per binary.
2. **Unbounded phases (orbits, spins) are u64 binary angles,** not f64 seconds.
3. **No GPU structure stores planet-frame absolute f32.** [12](12-rendering.md)'s integer node keys with per-level
   camera cells provide this, with no rebasing (§4.3).

## 5. Interfaces

**Plugin-facing.** The canonical function list is [08](08-plugin-system.md) §5.2; this doc supplies the semantics of
these:

```c
// core imports
u64  ss_now(void);         // the current tick (an i64 internally, never negative)
u64  ss_call_seed(void);   // this plugin's root seed: absorb(universe seed, hash(plugin id)). The same on every call;
                           // what varies is the addressing words (key, tick, counter) the plugin folds in

// SDK, header-only: integer-only, so bit-identical in every language, and no import cost
u64  ss_stream(const char *ns_name, u32 len);                       // hash of "<plugin>:<name>" (declared in the manifest)
u64  ss_stream_seed(u64 seed, u64 stream, const u64 *words, u32 n); // fold(absorb, seed ^ stream, words)
u64  ss_draw(u64 stream_seed, u64 i);                               // SplitMix64 in counter mode
f64  ss_to_u01(u64 x);   f32 ss_to_u01f(u64 x);                      // 08's batched ss_draw_u01 uses these
f64  ss_seconds(i64 dticks);                                         // f64(dticks) / 30; a call's span is ss_seconds(Δtick)
f64  ss_angle_rad(u64 a);                                            // f64(a >> 11) * 2^-53 * 2π
u64  ss_angle_at(u64 phase0, u64 rate, i64 epoch, i64 t);            // phase0 +% rate *% (t − epoch)
```

**Engine-facing** (glue and tools; an `eng_` prefix, deliberately not the plugin ABI's):

```c
typedef struct { int64_t tick; uint32_t player, seq, kind, len; /* payload follows */ } eng_command;
uint32_t eng_command_push(const eng_command *c);       // rejected if c->tick < the next tick to run
uint64_t eng_fingerprint(uint32_t scope_mask);
uint32_t eng_field_totals(double *dst, uint32_t cap);  // the human-readable companion to the hash
```

**Core-internal.** Frames and the NCA routine belong to [04](04-bodies-and-orbits.md):
`rel_pos(body_a, pa, body_b, pb, dst_frame) -> f64[3]`, always computed through the NCA. Plugins see positions in
the body-fixed frame unless they ask otherwise.

## 6. Risks and unknowns

| risk | likelihood | impact | mitigation |
|---|---|---|---|
| CORE-MATH doesn't build cleanly to WASM, or its licence doesn't suit | low | low | vendored, frozen musl libm |
| an engine bug breaks WASM float determinism | low | high | cross-engine runs in CI from M0 |
| a determinism rule is broken quietly by a new system | high | high | the invariance tests (§4.12) run on every change; the handle shuffle and worker-delay modes are cheap |
| sorting for canonical sums costs too much at 2 M blobs | medium | medium | E4; `q64` for conserved quantities; per-blob kernels need no sort |
| `q64` scales are hard to choose per field | medium | low | declare with headroom; saturation is logged; i128 totals |
| a 1/30 s tick is too coarse for contacts and fast parcels | medium | medium | fixed substep counts inside a tick ([11](11-physics.md)); the tick rate itself stays a parameter until M4 |
| time warp beyond what per-tick systems can run | high | medium | implicit modes and the governor ([10](10-simulation-and-scheduling.md)); warp is a command |
| plugin authors find the rules burdensome | medium | medium | the SDK hides them: header-only RNG, unit strings, no clock to misuse; the loader rejects what breaks determinism instead of trusting authors |

## 7. First experiments

| id | experiment | success criterion |
|---|---|---|
| E1 | **Cross-engine bit identity.** A kernel set: 1e6 random Kepler solves (elliptic and hyperbolic), 8-octave gradient noise at 1e6 points, a libm sweep, a SIMD field-evaluation kernel, 1000 steps of a parcel integrator. Run in Node, Bun (JSC) and `sm` shells, and in Chrome, Safari and Firefox. | identical fingerprints everywhere; the libm builds from CORE-MATH (or musl) |
| E2 | **Binary angles vs f64 seconds.** An Earth-like and a moon-like orbit at t = 1e9 … 1e12 s. | the binary angle's position error and its tick-to-tick jitter are ≤ 1e-4 m at 1 AU at every t; the f64-seconds version is reported for comparison |
| E3 | **RNG quality and speed.** PractRand on three access patterns: sequential keys at a fixed tick; a fixed key over sequential ticks; sequential counters. ≥ 2^38 bytes each. | no failures; ≥ 200 M draws/s scalar in WASM on the M1 Max |
| E4 | **Reductions.** 1 M contributions into 100 k targets: sort plus canonical float sum, vs `q64` integer accumulation, vs an unordered float sum (baseline). | timings for all three; the default for [09](09-fields-and-composition.md) |
| E5 | **The NaN scan** on plugin outputs while they're copied back. | ≤ 2% of the copy's time |
| E6 | **The invariance-test harness** (camera path, handle shuffle, worker delay, save/load), first on a toy sim in M0, then on the real cut in M2–M3. | each test fails when its rule is broken on purpose, and passes otherwise |
| E7 | **GPU precision** (shared with [12](12-rendering.md)'s E8). A surface scene at R = 6.4e6 m with integer node keys; the camera moves in 1 mm steps, including at the antipode of the key origin; a planet 1e11 m away is in the same frame. | no visible jitter (projected vertex motion matches the camera's to ≤ 1e-3 px) |

## 8. Build order

- **M0:**
  - the tick clock and fixed-timestep loop;
  - the command queue;
  - the RNG with tests (E3);
  - the libm (E1);
  - `eng_fingerprint` with the Node harness and cross-engine runner (E1);
  - the NaN-scan helpers;
  - the invariance-test harness on a toy sim (E6).
- **M1:** frames, the NCA rule and binary angles, with [04](04-bodies-and-orbits.md) (E2); camera-relative rendering
  of space.
- **M2:**
  - integer node keys on a planet, with [12](12-rendering.md) (E7);
  - the two tiers' rule enforced from the start;
  - handle-shuffle mode.
- **M3:** stream declarations in manifests (collision checks), plugin import validation and relaxed-SIMD rejection,
  NaN scans at plugin boundaries (E5).
- **M4:** interpolation of moving things; player commands; physics substeps.
- **M5:** `q64` or canonical merging for hydrology (E4); replay logs in saves.
- **M6:** event schedules (the asteroid).

## 9. Open questions

- **Q02.1 Tick rate.** (merged into questions.md C15)
  - Why it matters: it is the quantum of commands, physics and the network later. Changing it after content exists
    changes every result.
  - Recommended default: **30 Hz**, with physics and the fast layer substepping inside a tick. (20 Hz is cheaper and
    Minecraft-like; 60 Hz is smoother contacts at double the cost.)
- **Q02.2 Time scale and day length.** (merged into questions.md C2) Does game time run 1:1 with real time at normal
  speed, and how long is a day on the reference planet?
  - Why it matters: a real 24 h day means one sunset per real day. With fictitious forces dropped in surface physics
    ([04](04-bodies-and-orbits.md); §4.5), the day length is a free choice.
  - Working default: 1:1, a **~1 real-hour day**, player-controlled time warp for waiting, and no separate fast sun
    clock. Year length depends on [03](03-universe-and-identity.md)'s choice of physical scale.
- **Q02.3 Is cross-browser bit-identity required from M0, or only from the multiplayer work?**
  - Why it matters: it is cheap to keep if tested from the first commit, and very expensive to retrofit.
  - Recommended default: **from M0** for the core and the SDK. The cross-engine shells make it cheap.
- **Q02.4 One clock per universe?** (merged into questions.md C14) Should every player's universe share the epoch (tick 0), so that
  `(planet id, place, tick)` can be shared ("an eruption here at this time")?
  - Note: coarse-scale history reproduces; fine detail depends on where observers went.
  - Recommended default: **yes**, with new games starting at a fixed tick after spin-up.
- **Q02.5 Fixed-point for conserved quantities?** Store water, snow, lava and sediment as `q64` (exact, order-free
  sums) rather than f64 with canonical-order sums? (Pending; [09](09-fields-and-composition.md) adopts whichever
  E4 supports.)
  - Recommended default: decide after E4; lean toward `q64` for the extensive quantities that hydrology and the fast
    layer move around.
- **Q02.6 Replays.** Should replays (seed + plugin hashes + command log) be a player-facing feature (share a session,
  watch a recording), or only a development tool?
  - Recommended default: a development and bug-report tool from M0; a feature later, since it costs little once the
    rules hold.
