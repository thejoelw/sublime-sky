# 04. Bodies and orbits

Status: draft · Depends on: [02](02-numerics-time-and-determinism.md) (f64, ticks, libm, RNG),
[03](03-universe-and-identity.md) (generated bodies and their elements) · Used by: [06](06-blob-hierarchy-and-cut.md),
[07](07-spatial-queries.md), [10](10-simulation-and-scheduling.md), [11](11-physics.md), [12](12-rendering.md),
[13](13-lighting-atmosphere-and-sky.md), [15](15-persistence.md), [17](17-events-and-discovery.md),
[18](18-player-camera-and-navigation.md)

## 1. Purpose and scope

The body tree is the skeleton of space. It says where everything is at any tick, which body's gravity each body
feels, and how coordinates convert between frames. It keeps numbers manageable from a galaxy down to a pebble.

**In scope:**
- the body record;
- frames (inertial and body-fixed) and transforms between any two of them;
- motion modes: fixed, rails (Kepler), integrated, resting;
- the Kepler solvers and conversions;
- parent selection by sphere of influence, and reparenting;
- events on rails (SOI crossings, atmosphere entry);
- spin and tidal locking;
- binaries (barycenters);
- the atmosphere density model and drag;
- rails ↔ integrated transitions;
- the body-level broad phase and the NCA rule;
- dynamic bodies and their keys;
- time warp as far as bodies are concerned;
- impact planning.

**Out of scope:**
- what bodies are generated, and their elements (doc 03);
- contact resolution, rigid-body response, the character controller, and when blobs detach into objects (doc 11);
- blob-level collision and queries (doc 07);
- how the world clock relates to real time, and the time governor (doc 02, doc 10);
- drawing orbits and bodies (doc 12, doc 18).

## 2. Requirements

| | requirement | from |
|---|---|---|
| must | a body's state at any tick is exact and periodic when it is on rails, and the same on every machine | user: "strict ellipse, entirely deterministic and periodical"; D5 |
| must | gravity only from the parent; parent re-chosen when it should be, with the state re-expressed exactly | user's body tree; D13 |
| must | drag that falls to exactly zero above a height; in vacuum, motion is a strict ellipse | user |
| must | numbers stay manageable: no comparing 10²⁰ m positions to get a 1 m answer | user's point (1); D4 |
| must | collisions detected and resolved in the frame of the nearest common ancestor | user |
| must | surface blobs stay still relative to the ground while the planet spins and orbits | pillar 5, constraint "spherical planet" |
| should | cheap: thousands of loaded bodies, rails evaluation O(1), events computed instead of polled | |
| should | time warp at any rate for everything on rails | doc 10 |
| later | ships under thrust; comets; non-Kepler motion | |

## 3. What the prototypes taught

The prototypes had one planet and no orbits, so the direct lessons are about precision, time and fast motion:

- **Precision at planet scale needs care everywhere.** On a 250 km sphere, `planet/` needed:
  - noise coordinates relative to the mesh origin;
  - per-octave lattice offsets reduced mod 256 in doubles;
  - camera-relative rendering with reversed-Z.

  (`planet/DESIGN.md` §6, "float precision on a 250 km sphere".) At 500–1000 km and with orbits it only gets worse.
  So f64 frames are not optional.
- **The CPU and GPU surfaces disagreed by metres in coarse cells,** and the camera needed a 3 m clearance
  (`PLUGINS.md` §7). Anything that places a body on the ground (embarking, landing an impactor) must use the same
  surface the renderer draws. That is doc 07's `ss_surface_along`, not a second approximation.
- **Ballistic parcels were sub-stepped at 0.05 s** so they wouldn't shrink everyone's step (`PLUGINS.md` §2). Free
  flight wants its own substeps. Resistance had to be semi-implicit to be stable for any step: lava "stopped dead on
  a 39° slope" with explicit drag.
- **"Nobody observes a fast-forward: everything goes implicit"** (`PLUGINS.md` §2). After a 15-year skip, 21,000
  parcels appeared because the observer was stale. Fast motion and time warp don't mix. Bodies need a rule for what
  happens to integrated motion at high warp (§4.11).
- **A lag that started before spin-up made 1.6 s frames** (`PLUGINS.md` §2). Every clock-driven computation needs a
  cap on catch-up. For rails this is free, because they are evaluated at a tick rather than stepped to it.

## 4. Design

### 4.1 The body record

The record is SoA in core memory, addressed by a `u32` body handle (0 = none). Keys are as in doc 03, or §4.12 for
dynamic bodies.

| field | type | notes |
|---|---|---|
| key | u128 | stable identity (doc 03 §4.5, §4.12 here) |
| parent | body handle | 0 only for the root |
| first_child, next_sibling | body handle | |
| mu | f64 | GM of this body, m³/s². Stored directly; G never appears |
| radius_ref | f64 | mean radius, for display and the atmosphere's base |
| bound_radius | f64 | radius of the D9 *bound* of the body's top blob: nothing of the body sticks out beyond it |
| reach | f64 | bound_radius ∪ all descendants' orbits and reaches (§4.10) |
| soi_ratio | f64 | λ such that SOI = λ · (distance to parent); 0 = this body captures nothing (§4.3) |
| motion | enum | `fixed`, `elements`, `epoch_state`, `integrated`, `resting` (§4.4) |
| orbit | union | elements, or an epoch state, or an integrated state, or a resting pose (§4.4) |
| spin | struct | axis (unit, inertial axes), period in ticks (signed), phase at epoch (§4.8) |
| atmosphere | struct | ρ0, scale height H, top h_top; 0s if airless (§4.9) |
| switch_radius | f64 | where rails ↔ integrated happens around this body (§4.9) |
| flags | bits | generated, kinematic, has_blobs, embarkable, can_capture, … |
| next_event | i64 tick + kind | for rails bodies (§4.6) |

That is about 250 bytes per body. The loaded set is the bodies of the systems near observers, plus dynamic bodies:
hundreds to tens of thousands. Stars in the sky are *not* body records until their system is generated (doc 03 §4.11).

### 4.2 Frames and transforms

**Every inertial frame has the same axes.** There is one global orientation (call it the universe's ICRF). A body's
inertial frame is the global axes translated to the body's centre. Changing between inertial frames is therefore only
a translation, and translations add along the tree in f64.

**A spinning body also has a body-fixed frame**, rotated by its spin (§4.8): `x_inertial = R_B(t) · x_fixed`, about
the body's centre. A planet's surface blobs live in its body-fixed frame (doc 05, 06), so they stay still while it
spins and orbits.

**Transforms between two frames at a tick:**
1. Find the NCA of the two bodies.
2. Sum each body's position relative to its parent along both paths up to the NCA. Each relative position is small
   compared with its parent's scale.
3. Subtract the two sums.
4. Apply body-fixed rotations at the ends if requested.

There are no absolute positions anywhere. Intermediate sums are bounded by the NCA's extent. This is the user's
point (1): never compare two huge numbers to get a small one.

**Precision of f64 in each frame:**

| magnitude | where | f64 step |
|---|---|---|
| 5e5–1e7 m | surface in a planet frame | 6e-11 – 2e-9 m |
| 1.5e11 m | a planet in its star's frame | 3e-5 m |
| 3e16 m | a star 1 pc away, galaxy frame | 4 m |
| 5e20 m | the galaxy's extent | 65 km |
| 3e11 s | 10,000 years of world time | 6e-5 s |

Galaxy-frame precision (65 km) only ever positions star points and system nodes. A camera in the galaxy browser is
re-anchored to a star frame before it gets close (doc 18). On the GPU, doc 12 uses integer node keys with per-level
camera cells, with no rebasing (doc 02, 12). This doc delivers the exact f64 frame transforms that feed them.

### 4.3 Parent selection: sphere of influence, not strongest force (D13)

**Why strongest force fails.** The Sun pulls on the Moon harder than Earth does:

```
Earth on Moon:  GM_E / r²   = 3.986e14 / (3.844e8)²   = 2.70e-3 m/s²
Sun on Moon:    GM_S / d²   = 1.327e20 / (1.496e11)²  = 5.93e-3 m/s²   (2.2×)
Sun's tidal:    2 GM_S r / d³                          = 3.05e-5 m/s²   (1.1% of Earth's pull)
```

The Sun accelerates the Earth and the Moon almost equally. What disturbs the Moon's orbit around the Earth is only the
*difference*, the tidal term, which is 1/90 of Earth's pull. "Strongest force" would hand the Moon to the Sun, and its
orbit around a moving Earth would then be a wobbly non-ellipse. The right comparison is perturbation ratios in each
frame. The **Laplace sphere of influence** is defined exactly by that comparison:

```
s = r · (m / M)^(2/5)          r = distance from the parent, m/M = mass ratio
Earth:  s = 1.496e11 · (3.003e-6)^0.4 = 9.24e8 m      Moon at 3.844e8 m: inside
```

The same formula gives KSP's Kerbin SOI of 84,159 km.

**The rule.** A body's parent is the smallest body whose SOI contains it. In practice only two tests ever run:
- **exit:** the distance to the parent exceeds `s_P · (1 + ε)`, so the new parent is the grandparent;
- **entry:** the distance to a child C of the parent drops below `s_C · (1 − ε)`, so the new parent is C. Recurse
  into C's children; it never fires twice at once, by the moon condition in doc 03 §4.8.

Hysteresis is ε = 0.01. A body inside the band keeps its current parent, so it can't flap.

**SOI radii per kind of body:**

| body | SOI | why |
|---|---|---|
| planet or moon | Laplace, `λ = (μ/μ_parent)^(2/5)`, times the current distance | m ≪ M |
| binary component | Eggleton's Roche-lobe radius `0.49 q^⅔ / (0.6 q^⅔ + ln(1 + q^⅓))` × separation, with q = m_self/m_other | Laplace breaks when masses are comparable; Eggleton holds for any q |
| star or system node (static galaxy, doc 03 Q03.3) | half the thinning clearance d_min (doc 03 §4.4) | the galaxy has μ = 0, so Laplace is undefined |
| galaxy, root | infinite | |
| small dynamic bodies (rocks, the player, ejecta) | 0 (`can_capture` off) | their Laplace SOI is smaller than they are: a 1 t rock on the reference planet has s ≈ 1 cm |

**Reparenting** happens at a tick T:

```
r_new = r_old + r_oldparent→newparent(T)      v_new = v_old + v_oldparent→newparent(T)
```

Both terms are evaluated at T in parallel inertial axes, so the result is exact up to one f64 rounding per
component. The body's motion is re-expressed in the new parent: as an epoch state (§4.4) if it stays on rails, or as
an integrated state.

**What never reparents:**
- **Generated bodies** are kinematic. Their parent is fixed by construction (D14 keeps them inside it), and nothing
  can knock them off their rails (§4.10).
- **Integrated bodies inside the switch radius** stay with the body they are touching (§4.9).

So in practice reparenting only happens to dynamic bodies on rails: an asteroid falling in, ejecta escaping, later a
ship.

### 4.4 Motion modes

| mode | state | position at tick t | used for |
|---|---|---|---|
| `fixed` | position in parent frame | constant | galaxies, stars (doc 03 option S), barycenters of static systems |
| `elements` | a, e, i, Ω, ω, M0, epoch, μ_central, **period in ticks** | Kepler from elements (§4.5) | every generated orbiting body |
| `epoch_state` | r0, v0 (parent inertial), tick0, μ_central | universal-variable propagation from (r0, v0) (§4.5); a straight line if μ = 0 | dynamic bodies on rails |
| `integrated` | r, v in the integration frame (§4.9), plus doc 11's rotational state | stepped each tick | anything below the switch radius, in contact, or under force |
| `resting` | position and orientation in the parent's body-fixed frame | constant in that frame | dynamic bodies asleep at rest (doc 11's sleep): transient, until doc 11 settles them into the world or wakes them; also bodies with settle policy `never` |

**`resting` is a transient body state, not where things end up.** When doc 11 *settles* a resting object (policy
`keep_instance` or `join_ground`), the body is dissolved. Its blobs become **kind D placed records**, persisted by
doc 15 (README content kinds; doc 14's taxonomy), and the body record is deleted. Only bodies whose type's settle
policy is `never` stay `resting` for long. (The blob dynamics mode `attached`, doc 05, is a different thing: it is
what settled blobs and terrain have.)

`μ_central` is stored in the orbit, not read from the parent. This is what lets binary components use their
effective μ (§4.7).

**Integer-tick periods for generated bodies.** Doc 03 chooses each generated orbit's period `P` as a whole number of
ticks, then derives `a = (μ P² / 4π²)^⅓`. The mean anomaly is then computed from integers:

```
M(t) = M0 + 2π · ((t − t0) mod P) / P        // (t − t0) mod P exact in i64; one f64 multiply
```

Generated motion is therefore exactly periodic in ticks: after a million orbits the state is bit-identical to the
first. The phase error doesn't grow with time at all. The same trick applies to spin (§4.8).

### 4.5 Kepler solvers and conversions

**Elliptic, from elements** (`elements` mode, e ≤ 0.95 for generated bodies):
1. Reduce M to (−π, π].
2. Starter: `E0 = M + 0.85 · e · sign(sin M)` (Danby).
3. Run a fixed count of Halley iterations (4) on `f(E) = E − e sin E − M`. For e ≤ 0.95 that reaches f64 precision
   from Danby's starter. E1 checks this over a grid of (e, M). A fixed count keeps the cost and the code path
   identical everywhere.
4. Position in the perifocal frame: `(a(cos E − e), a√(1−e²) sin E, 0)`. Velocity from `Ė = n / (1 − e cos E)`.
5. Rotate by the precomputed matrix `R(Ω, i, ω)`, computed once per body.

**Any conic, from a state** (`epoch_state` mode, for dynamic bodies): universal variables.
- α = 2/r0 − v0²/μ.
- Solve the universal Kepler equation for χ with the **Laguerre–Conway** iteration, which is robust for elliptic,
  parabolic and hyperbolic cases. Use the Stumpff functions C(z), S(z) with z = αχ², with series near z = 0.
- Then `r = f r0 + g v0` and `v = ḟ r0 + ġ v0`.
- For elliptic orbits, first reduce `Δt` modulo the period, so χ stays small over many revolutions.
- If μ = 0 (between stars, doc 03 option S): `r = r0 + v0 Δt`.

Why two representations:
- Generated orbits are designed as elements (doc 03 draws e, i, …), so they never need a conversion.
- Dynamic bodies arrive as states (from integration or reparenting). Converting a state to classical elements is
  singular at e → 0 (ω undefined) and i → 0 (Ω undefined). Universal variables never need elements.
- When the UI wants to draw a dynamic body's orbit (doc 18), it uses vector forms: the eccentricity vector and the
  angular momentum. These stay well-defined, or it simply samples `f, g`.

**Pitfalls handled:**
- Large M: reduced via integer ticks (§4.4).
- Near-parabolic: universal variables, never elements.
- Hyperbolic `cosh` overflow far out: SOI exit always happens long before.
- Every transcendental (sin, cos, sinh, cosh, atan2, pow) comes from the core's libm (D5), never JS `Math`.

### 4.6 Events on rails

A body on rails never steps. Instead it carries its next event's tick, computed once, and a min-heap of events
drives the tick loop (§4.11). The events:

| event | test | how it is found |
|---|---|---|
| SOI exit | r = s_P(1+ε) | closed form: `r = p / (1 + e cos ν)` gives ν*, then time from the anomaly; the next of ±ν* |
| switch-radius crossing (atmosphere top, or the floor of an airless body) | r = r_switch(1−ε) inbound | same closed form |
| SOI entry into child C | `‖r(t) − r_C(t)‖ = s_C(t)(1−ε)` | conservative advancement: step `Δt = (d − s_C) / v_closing,max`, where v_closing,max bounds the relative speed on both conics; when d − s_C < tolerance, bisect to the tick. Prune by annulus first: only children whose annulus meets the body's [q, Q] |
| scheduled | a plugin or an event asked for a callback at tick T | given |

**Determinism:** every step uses fixed algorithms in f64 with the core's libm, so the event tick is identical
everywhere. An event fires on the first tick ≥ its computed time. The state is evaluated on the old conic at that
tick and converted (§4.3), and then the next event is computed.

**Horizon:** an entry search runs at most to the body's next exit or switch event, or N = 4 periods ahead, and is
re-planned when it expires. The cost is a few dozen evaluations per event, for the few dynamic bodies on rails.

### 4.7 Binaries and barycenters

A binary system node (doc 03 §4.1) is a **barycenter body**: fixed in the galaxy, with μ = G(m_A + m_B) for its
circumbinary children. Components A and B are its children in `elements` mode. Each moves on its own Kepler ellipse
about the barycenter, with the same period, ω differing by π, and semi-major axes `a_A = a·m_B/M` and `a_B = a·m_A/M`.

An ellipse with semi-major axis a_A and period T needs `μ_A,eff = 4π² a_A³ / T² = G m_B³ / M²`. Likewise
`μ_B,eff = G m_A³ / M²`. With `μ_central` stored per orbit (§4.4), the ordinary Kepler code reproduces the two-body
motion exactly. Double planets (a Pluto–Charon pair) are the same construct under a star.

Circumbinary planets feel μ_total at the barycenter. That approximation ignores the real binary's perturbations,
which is fine.

### 4.8 Spin and body-fixed frames

**Spin:**
- an axis (fixed in inertial axes; no precession);
- a period in **integer ticks**, negative for retrograde;
- a phase at the epoch.

`R_B(t) = R_axis · R_z(2π ((t − t0) mod P_spin) / P_spin + φ0)`. Here R_axis takes the body's z to the spin axis, and
a fixed reference fixes the prime meridian. As with orbits, the phase never drifts.

**Tidal locking** (doc 03 decides which moons): `P_spin = P_orbit`, both in ticks, with the phase chosen so the prime
meridian faces the parent at periapsis. Eccentric orbits then librate for free, as real moons do.

**Who lives in the body-fixed frame:**
- the body's blobs: terrain, water, and every blob in dynamics mode `attached` (doc 05), including settled kind D
  placed records;
- integrated dynamic bodies below the switch radius (§4.9);
- resting bodies;
- the player's avatar.

Plugins see positions in the body-fixed frame unless they ask otherwise. Latitude and longitude are body-fixed.

**Fictitious forces (Q04.1).** In a rotating frame, Newton needs a centrifugal term `−ω×(ω×r)` and a Coriolis term
`−2ω×v`. **Working assumption, shared by all docs (pending `questions.md` C2): drop both inside the switch radius.**
Surface physics treats the body-fixed frame as inertial, with radial gravity of magnitude μ/r². Converting to and
from inertial frames at the switch radius is exact (§4.9). The working default day is ~1 real hour (C2).

Why:
- **Day length is decoupled from planet size.** The equatorial centrifugal fraction is `(2π/P)² R / g`:

  | day length P | R = 500 km, g = 9.81 |
  |---|---:|
  | 1 h | 15% of g |
  | 2 h | 3.9% |
  | 6 h | 0.4% |
  | 24 h | 0.03% |
  | breakup | P = 2π√(R/g) = 23.6 min |

  A game-paced day on a small planet would otherwise make things slide toward the equator and fall sideways. With
  the terms dropped, any day longer than breakup works (Q04.4).
- **Surface physics stays simple and intuitive.** A dropped stone falls straight down, and water doesn't drift.
- **Weather still swirls.** Cyclones rotate because the climate plugin uses the spin rate and latitude as a parameter
  of its model (doc 10, 14), not because rigid bodies feel Coriolis.

What it costs:
- The trajectory inside the switch radius differs from the true one. With the ~1 h working-default day, a rock thrown
  at 20 m/s lands ~0.3 m from where true physics would put it after 3 s. A minute-long 6 km ballistic flight would be
  off by ~600 m, and nothing in the game flies like that.
- At the switch, the conversion `v_inertial = R v_fixed + ω × r` is exact, so nothing jumps.
- Above it, the object's path relative to the ground starts to curve, because the planet turns under it. That only
  matters if things routinely go to orbit from the surface (Q04.5).

### 4.9 The atmosphere, drag and the switch radius

**This doc owns the shared atmosphere density model.** Doc 11 uses it for drag on objects, parcels and the avatar.
Doc 13 uses it for scattering, so looks and drag always agree. Nobody else defines ρ(h).

**Density** (per body, from doc 03's `PlanetSpec.atmosphere`):

```
ρ(h) = ρ0 · (e^(−h/H) − e^(−h_top/H)) / (1 − e^(−h_top/H))     0 ≤ h ≤ h_top
ρ(h) = 0                                                        h > h_top
```

The taper makes ρ reach zero exactly at h_top, with no step. With h_top ≈ 12–15 H, the taper changes nothing
visible. Earth-like H = 8.5 km gives h_top ≈ 100–130 km. On a 500 km planet that is a fifth of the radius: a thick
glowing limb from orbit (doc 13).

**Drag** in the integration frame (the air is at rest in the body-fixed frame, plus wind if doc 10 supplies a coarse
wind field):

```
a_drag = −ρ |v_rel| v_rel / (2β)      β = m / (C_d A)      v_rel = v − v_wind
```

- It is applied semi-implicitly per substep, `v ← v / (1 + ρ|v_rel| h / 2β)`. This is stable for any step, which is
  the fast layer's lesson.
- A, C_d and m come from the body's blobs (doc 11). As a check, a 70 kg player with C_d·A = 0.7 m² has a terminal
  velocity of 40 m/s at sea level.

**The switch radius** of a body is where motion around it changes between rails and integrated:
- with an atmosphere: `R_ref + h_top`;
- airless: `bound_radius + margin` (margin ≈ max(1 km, 1% R)). The D9 bound guarantees no terrain reaches it.

**Rails → integrated** at tick T when one of these happens:
- an `elements` or `epoch_state` body crosses its parent's switch radius inbound (a §4.6 event);
- a force, an impulse or a thrust is applied;
- an integrated body touches it, unless it is kinematic (§4.10).

The state at T comes from the conic, converted to the body-fixed frame:
`r_f = R(T)ᵀ r_in`, `v_f = R(T)ᵀ (v_in − ω × r_in)`.

**Integrated → rails** at tick T when all of these hold:
- r > r_switch·(1+ε);
- no contact;
- no applied force this tick.

Then `v_in = R(T) v_f + ω × r_in`, and the motion becomes an `epoch_state` with (r_in, v_in, T). Both directions are
exact changes of representation, with no jump.

**The integrator** for free flight (no contacts; contacts are doc 11's solver, which reads gravity and drag from
here):
- velocity Verlet (kick–drift–kick) for gravity, with drag as the semi-implicit factor above;
- substeps h = tick / N, N = clamp(ceil(|v|·tick / L), 1, 32), where L is a length scale (5 m near the ground). This
  generalises the fast layer's 0.05 s ballistic substep.

Why Verlet: gravity depends on position only, so it is symplectic. That gives second-order accuracy and no secular
energy drift over the minutes a body spends integrated. It is also deterministic and cheap. Higher-order methods buy
nothing at these step sizes. Semi-implicit Euler (doc 11's likely choice for contacts) is acceptable too: what matters
is that one integrator owns a body per tick.

### 4.10 Collisions across the tree

- **Detection and response happen in the NCA frame** (user). Because all inertial axes are parallel (§4.2), "one
  world space" holds conceptually. Only the origin differs, which keeps the numbers small.
- **reach(B)** = max(bound_radius(B), switch_radius(B), max over orbiting children C of (Q_C + reach(C))). It is a
  sphere around B guaranteed to hold B, its blobs and all its descendants at every tick. For generated bodies it is
  fixed. Integrated children never leave the switch radius, so they don't change it.
- **Ghosts:** two bodies both on rails never collide. Generated siblings can't meet anyway (D14, doc 03 §4.8). For
  dynamic rails bodies (debris in orbit) we accept missing the rare pair. A rails body that would hit a planet or moon
  does so through the switch-radius event, so it stops being on rails first.
- **Kinematic generated bodies:** stars, planets and moons are never pushed. In a contact they are static colliders
  moving on rails. A pebble can't knock a moon off its orbit, and a 10 km asteroid can't either. That is a deliberate
  simplification (Q04.3).
- **Broad phase, only for integrated bodies:**

  ```
  for each integrated body D (parent P):
      test D against P's blobs (doc 07/11, in P's body-fixed frame)
      for each other child C of P: if spheres (D.pos, bound(D)) and (C.pos, reach(C)) overlap:
          test D against C's blobs; recurse into C's children
  ```

  P's siblings never need testing: D is inside P's SOI, and D14 keeps P's siblings' reach spheres outside it. The
  narrow phase is blob against blob (doc 07, 11), in the NCA frame. For D against its own parent's terrain, that is
  the parent's body-fixed frame.

### 4.11 Time and warp

- **Rails are evaluated, never stepped.** `state_at(T)` costs the same at T = now + 1 tick and at T = now + 10 years.
  Time warp is exact and free for everything on rails, at any rate.
- **The event heap:** each tick, pop the rails events due at that tick, in (tick, key) order for determinism, and
  apply them. A warp that advances many ticks per frame processes every event at its exact tick. Rails never miss an
  event however fast time runs.
- **Integrated bodies at warp:** doc 10 owns the policy (the time governor). From this doc's side there are three
  options:
  1. keep stepping every tick, if the budget allows: low warp;
  2. go to `resting`, frozen in the body-fixed frame. This is right for bodies at rest, and they wake on contact or
     force;
  3. be forced to rest: place the body where a cheap ballistic estimate says it lands, and make it `resting` there
     (doc 11 may then settle it into a kind D placed record). This is for moving, unobserved bodies at high warp
     ("nobody observes a fast-forward").

  The governor caps warp while integrated bodies move near an observer, as the prototype's governor did.
- **Spin, orbits and the sky** follow the same clock at every warp. Days and years pass exactly.

### 4.12 Dynamic bodies

Dynamic bodies are created at run time: the player's avatar, detached objects (doc 11), an event's impactor (doc 17),
ejecta that escape, later ships.

- **Key:** a 128-bit key like every key (doc 05), in the body key space: namespace 1 in the top two bits, and the
  remaining 126 bits from doc 02's hash `H(creator key, creation tick, sequence)`, where the sequence
  is the creator's own counter within that tick. The creator is a plugin, an event or the core, each with its own
  key. Same inputs, same key, on every machine. A random 126-bit hash collides with probability around 2⁻⁶³ over a
  save's lifetime. That is safe, and a collision would be detected on insert.
- **Record:** as in §4.1, plus a creation record (creator, tick, parent at creation) for debugging and the journal.
- **Lifetime:** a dynamic body ends one of three ways:
  - settled: doc 11 settles a `resting` object (`keep_instance` or `join_ground`), its blobs become kind D placed
    records (doc 15), and the body record is deleted. This is the normal end of a dropped rock or log;
  - destroyed: its blobs pass to the fast layer or to another body (an impactor at contact), and the record is
    deleted;
  - kept: a `resting` body with settle policy `never`, or a body on rails, persists as a record. A rails record is
    cheap. A garbage-collection policy for unimportant, unreferenced debris after N game days is Q04.8.
- **Persistence:** dynamic body records, and the blobs they own, go into the save keyed by their keys (doc 15).
  Generated bodies persist nothing, because they regenerate from their keys.

### 4.13 Impact planning (for doc 17's asteroid)

An event (doc 17) asks for an impact:

```
ImpactRequest { target body P, lat, lon, tick T_i, impactor size/mass, entry speed, entry angle, lead time }
```

This doc plans it deterministically:
1. **The impact point x_i** comes from doc 07's `ss_surface_along` (radially down through (lat, lon), in P's
   body-fixed frame). It is the same surface the renderer draws.
2. **Shoot the atmospheric segment forward.**
   - Guess an entry state at r_switch, in the body-fixed frame at tick T_e, aiming along the requested angle and speed.
   - Integrate forward with the §4.9 integrator to the ground, and measure the miss.
   - Correct the guess with a fixed number (4) of secant steps on the 2D miss.
   - Large impactors barely slow in the air, so it converges in 2–3 steps. The result is deterministic, because the
     iteration count is fixed.
3. **Back-propagate on rails.**
   - Convert the entry state to P's inertial frame. Propagate it *backward* with universal variables to
     T_c = T_i − lead time.
   - If the backward conic leaves P's SOI first, convert the state at that crossing into the star's frame and keep
     propagating there.
   - Kepler propagation is time-reversible, so forward propagation from the result retraces the path within f64
     rounding: millimetres at the entry point.
4. **Create the impactor** at T_c as a dynamic body in `epoch_state` mode.
   - From then on nothing is special. Its SOI entry fires at the planned tick, then the switch event, then
     integration through the atmosphere. Its glow and its trail come from the dynamic pressure ½ρv², which this doc
     exposes to doc 13.
   - It strikes within the shooting tolerance of x_i, at T_i. The planet's rotation during the fall is already
     accounted for, because x_i is body-fixed at T_i.

With a lead time of hours, the impactor is visible as a slowly moving light before it arrives (doc 17's
"discovery"). This is the one place where something collides with a generated body on purpose. D14 covers generated
bodies; events are planned exceptions.

### 4.14 Canonical scenarios

**1. Orbit to grass.**
- The galaxy-browser camera sits in the galaxy frame, with steps of 65 km at worst.
- On approach to the star, doc 18 re-anchors the camera to the star's frame (3e-5 m at 1 AU), then to the planet's
  inertial frame, then to its body-fixed frame for the descent.
- Every hop is a translation along the tree, plus the spin rotation at the end. At the surface, positions are
  sub-nanometre in f64. Doc 12 turns them into integer node keys and per-level camera cells, with no rebasing.
- The planet's position comes from integer-tick Kepler, so after any warp it is exact.

**2. The storm and the flood.** The player waits with the time controls.
- Rails and spin follow exactly at any warp.
- The player's avatar is at rest, so it goes `resting` while the warp is high, and wakes when time returns to 1×.
- Days pass exactly: spin is integer-tick, so sunsets fall at the same ticks on every machine.

**3. The beaver dam.**
- The beaver plugin asks vegetation, through a service, to remove the felled log, and places its own `dam_log`
  content (doc 16). A log it drops is a dynamic body (key from the beaver plugin's sequence), integrated in the
  planet's body-fixed frame.
- It comes to rest (`resting`), and doc 11 settles it with `keep_instance`: the body is dissolved into a kind D
  placed record.
- The placed record persists when the player leaves (doc 15). The body record is gone; nothing of this doc is left
  to save.

**4. The asteroid.** §4.13. The order of events, all predictable ticks:
1. creation (dynamic, `epoch_state`, star frame);
2. SOI entry (parent becomes the planet);
3. the switch event (integrated, body-fixed);
4. contact (doc 11 takes over: crater, parcels; the body's blobs pass to the planet or to the fast layer, and the
   record is deleted).

Determinism: same request, same impact, on every machine.

**5. Dig, leave, return.**
- The player leaves on rails (doc 18's jump is a camera move, not a body).
- Ten years of world time pass. The planet's position, spin phase and moon phases at the return tick are exact
  (integer ticks).
- Settled things left behind are placed records in the body-fixed frame, exactly where they were. Any `resting`
  bodies kept as records are too.

## 5. Interfaces

The core exports these (C ABI). Handles are `u32`, 0 = none; keys are `key128` (128-bit, two `u64` halves); ticks are
`i64`. Batch functions take arrays (D6). Names without a prefix are engine-internal. The plugin-facing subset has the
`ss_` prefix and belongs in 08 §5.2's canonical list.

```c
typedef uint32_t body_h;
enum frame_kind { FRAME_INERTIAL = 0, FRAME_FIXED = 1 };
enum motion     { MOTION_FIXED, MOTION_ELEMENTS, MOTION_EPOCH_STATE, MOTION_INTEGRATED, MOTION_RESTING };

// lookup and tree
body_h   body_lookup(key128 key);                       // 0 if not loaded
key128   body_key(body_h b);
body_h   body_parent(body_h b);
body_h   body_nca(body_h a, body_h b);
uint32_t body_children(body_h* dst, uint32_t cap, body_h b);

// state, batched: positions/velocities of src[i] relative to `frame` at `tick` (f64, 3 per body)
void ss_body_states(double* dst_pos, double* dst_vel, const body_h* src, uint32_t n,
                 body_h frame, uint32_t frame_kind, int64_t tick);

// frame transforms
void frame_xform(double dst_q[4], double dst_t[3],                      // x_to = q · x_from + t
                 body_h from, uint32_t from_kind, body_h to, uint32_t to_kind, int64_t tick);
void ss_frame_points(double* dst, const double* src, uint32_t n,
                  body_h from, uint32_t from_kind, body_h to, uint32_t to_kind, int64_t tick);
void frame_dirs(double* dst, const double* src, uint32_t n, /* rotation only */ ...);

// orbits, for drawing and UI (doc 18)
typedef struct { double h[3], e_vec[3], a, e, q, Q, period_s; int64_t next_event_tick; uint32_t next_event_kind; } OrbitInfo;
void body_orbit(OrbitInfo* dst, body_h b, int64_t tick);

// atmosphere (positions in the body-fixed frame of `planet`)
void ss_atmo_density(float* dst, const double* pos_fixed, uint32_t n, body_h planet);   // §4.9, the shared model
void dynamic_pressure(float* dst, const body_h* src, uint32_t n, int64_t tick);   // ½ρv², for effects

// dynamic bodies (doc 11 adds the rigid-body and contact side)
typedef struct { body_h parent; uint32_t frame_kind; uint32_t motion; double pos[3], vel[3], quat[4];
                 double mu; double bound_radius; uint32_t flags; } BodyDesc;
body_h ss_body_create(const BodyDesc* desc, key128 creator, uint32_t seq);   // key = H(creator, tick, seq), §4.12
void   body_destroy(body_h b);
void   body_set_motion(body_h b, uint32_t motion);    // e.g. resting ↔ integrated (doc 11 wakes and sleeps)

// planning (doc 17)
typedef struct { key128 target; double lat, lon; int64_t impact_tick; double mass, radius, entry_speed,
                 entry_angle; int64_t lead_ticks; } ImpactRequest;
int plan_impact(BodyDesc* dst_impactor, int64_t* dst_create_tick, const ImpactRequest* req);
```

**Plugins** (doc 08) get read access, through the `ss_` subset:
- `ss_body_states` (moon phases, where the sun is);
- `ss_frame_points` (latitude and longitude to positions);
- `ss_atmo_density`;
- `ss_body_create` for objects they spawn, with their own key as creator.

Plugins never write rails.

**Expected from other docs:**
- doc 02: the tick length and the libm;
- doc 03: elements with integer-tick periods, μ, spin, atmosphere, and D14 invariants;
- doc 07: `ss_surface_along` (radial surface hits) and the blob-level narrow phase; doc 06: `blob_bound` (the D9 bound
  that sets bound_radius and the airless switch radius);
- doc 11: the contact solver and rigid state for integrated bodies, and when objects detach or merge;
- doc 10: the warp policy.

## 6. Risks and unknowns

- **Patched conics are an approximation.** A trajectory near an SOI boundary is less accurate than n-body. Players
  will rarely see it: only dynamic bodies reparent, and planned impacts are planned *with* the patched model.
  E3 measures how wrong it is.
- **Dropping fictitious forces** (§4.8) is a deliberate inconsistency between surface physics and orbits. If ships
  and orbital play arrive (Q04.5), revisit it: the switch radius could become a gradual blend.
- **Kinematic generated bodies** mean that even a planet-sized impact can't move a moon. That fits the "rails" world,
  but a future "moon destroyed" event would need a special path.
- **Integer-tick periods** constrain doc 03. The period must be chosen first, and a derived from it. Harmless, but it
  must be respected.
- **Determinism depends on the libm.** One `Math.sin` call in JS glue on a sim path would break replays. Doc 02's
  lint and test harness must cover body code.
- **The airless switch radius** relies on doc 06's bound being truly conservative. A loose bound costs only a few
  seconds of unneeded integration. A wrong one makes impacts pass through terrain.

## 7. First experiments

| # | experiment | success criterion |
|---|---|---|
| E1 | Kepler (`elements`, 4 Halley steps from Danby's starter) over e ∈ [0, 0.95], M on a 10⁴ grid; universal variables (Laguerre–Conway) for e ∈ [0, 3] and Δt up to 10⁴ periods | position error ≤ 1e-12 relative; bit-identical results in Node, Chrome and Firefox WASM (fingerprint of 10⁶ evaluations) |
| E2 | Precision chain: a camera on the reference planet's surface; transform points between galaxy, star, planet inertial and body-fixed frames, and back | round-trip error ≤ 1e-6 m at the surface; camera-relative error ≤ 1e-4 m for anything within 100 km |
| E3 | Patched conics vs an accurate 3-body integration for a dynamic body flying by a moon | report the miss distance at periapsis; acceptable if < 1% of the periapsis altitude for planned events (they use the patched model anyway) |
| E4 | Transitions: drop a body from orbit through the atmosphere and back out (grazing pass); count switches and check state continuity at each | no flapping (hysteresis); position and velocity continuous to f64 rounding; energy loss only from drag |
| E5 | Plan 1000 random impacts on the reference planet | impact within 1 m of the target in ≥ 99%; identical plans and impact ticks across engines |
| E6 | Integer-tick periodicity: evaluate a generated orbit at t and t + 10⁶·P | bit-identical states |

All of these run headless in Node (M0's harness, doc 19).

## 8. Build order

- **M1 the sky:**
  - the body record, `fixed` and `elements` motion with integer-tick periods;
  - frames, transforms and the NCA;
  - spin and tidal locking;
  - barycenters (if Q03.4 is yes);
  - `ss_body_states`, `frame_xform`, `body_orbit` (for drawing orbits);
  - E1, E2, E6.
- **M2 a planet from orbit:**
  - body-fixed frames used by planet blobs;
  - the atmosphere density model, shared with doc 13;
  - the switch radius from doc 06's bound.
- **M4 embark:**
  - `integrated` and `resting` motion; the player's avatar as a dynamic body;
  - drag; the Verlet free-flight integrator;
  - the broad phase across the tree; dynamic body keys;
  - rails ↔ integrated transitions;
  - E4.
- **M5:** persistence of dynamic bodies (doc 15); warp policy hooks (doc 10).
- **M6 events:** `epoch_state` motion, SOI events, the event heap, `plan_impact`; E3, E5.

## 9. Open questions

**Q04.1 Fictitious forces at the surface.** (merged into questions.md C2)
- Dropping centrifugal and Coriolis inside the switch radius makes day length independent of planet size. Surface
  physics stays intuitive, and weather still swirls through the climate model.
- *Recommended, and the working assumption in every doc:* drop them. Is that acceptable?

**Q04.2 How is surface gravity set?** (merged into questions.md C1)
- *Recommended:* μ = g R² per planet: KSP-style dense planets, no tuned G. Habitable planets get g near 9.81. Small
  moons get low g, which means big jumps and slow falls: fun and varied.
- Is a range of gravities across planets wanted?

**Q04.3 Simplifications to confirm.**
- Gravity comes only from the parent (no n-body).
- Two bodies both on rails never collide.
- Generated bodies are kinematic: nothing pushes a planet or moon off its rails.
- *Recommended:* yes to all three.

**Q04.4 Day length, in real minutes or hours.** (merged into questions.md C2)
- With Q04.1, any day longer than ~24 minutes works physically, even on a 500 km planet.
- One world clock drives spin, orbits and physics. Whether slow processes run faster than that clock is doc 10's
  question.
- *Working default:* a day of ~1 real hour (C2). A sunset's golden light then lasts a few minutes, longer at high
  latitudes.
- Do you want a short Minecraft-style cycle (~20 min) instead? That would be faster than breakup on a 500 km planet.
  It only works with Q04.1, and it sets the speed of the sun across the sky.

**Q04.5 Will the player ever travel physically?** (merged into questions.md C19)
- That means flying to orbit or between planets in something.
- If yes: thrust, orbital UI, and a smoother treatment of the switch radius.
- *Recommended:* not before M6; design jumps as camera moves (doc 18).

**Q04.6 Gravity of irregular bodies.**
- *Recommended:* always a point mass, even for lumpy small moons. Walking on a potato-shaped moon would feel slightly
  wrong near the ends. That's acceptable.

**Q04.7 Planned impacts.**
- May events spawn bodies on trajectories planned to hit generated bodies (§4.13)?
- *Recommended:* yes. It is the main source of impacts, since D14 rules out accidental ones.

**Q04.8 Debris lifetime.**
- Dynamic bodies left on rails, or kept `resting`, accumulate in a save.
- *Recommended:* keep anything the player touched, or that is journaled. Garbage-collect other unobserved debris
  after 30 game days by merging it into its parent's blobs, or deleting it if it's in orbit.
