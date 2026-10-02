# 03. Universe and identity

Status: draft · Depends on: [02](02-numerics-time-and-determinism.md) (deterministic maths, RNG) · Used by:
[04](04-bodies-and-orbits.md), [13](13-lighting-atmosphere-and-sky.md), [14](14-planet-generation.md),
[15](15-persistence.md), [17](17-events-and-discovery.md), [18](18-player-camera-and-navigation.md)

## 1. Purpose and scope

This doc defines what exists in the universe above the scale of a planet's surface, how each thing is named, and how
any one of them can be produced from its name alone.

**In scope:**
- the hierarchy of generated bodies: root, galaxies, star systems, stars, planets, moons, belts;
- the universe seed and **body keys** (the stable identity of every generated body);
- human-readable ids, procedural names, search, and shareable links;
- the galaxy model (density, stellar populations) and star, planet and moon properties;
- **lazy generation**: which unit is generated from what, and when;
- the construction that makes generated systems non-colliding (D14), with its proof;
- the visible-star list that the night sky needs (doc 13 draws it);
- the `PlanetSpec` handed to planet generation (doc 14).

**Out of scope:**
- motion, frames, parent selection and orbit evaluation (doc 04; this doc produces orbital elements, doc 04 evaluates
  them);
- anything on a planet's surface (doc 14);
- keys of bodies created at run time (ejecta, the player, detached rocks). Doc 04 owns those. This doc only reserves
  a namespace for them;
- blob keys (doc 05 and 06). A blob key starts from its body's key.

## 2. Requirements

| | requirement | from |
|---|---|---|
| must | the same universe seed and generator version give the same universe on every machine, bit for bit | D5, multiplayer later |
| must | any body can be generated from its key without generating the rest of its galaxy | search by id; a galaxy of ~10¹¹ stars |
| must | a short, typeable, checksummed id for every generated body, and a link format carrying body, location and time | user: "searching for their id" |
| must | generated systems never collide, provably (D14) | user: "non-colliding trajectories" |
| must | a starting planet that is habitable and embarkable | user: "appears on a planet" |
| must | enough stars generated around any viewpoint for a full night sky and a galaxy browser | pillar 2 (wonder), user: jump to stars "he sees" |
| should | the sky looks like a galaxy from inside: a band, dust lanes, bright stars that are rare, faint ones that are many | pillar 2 |
| should | a Zipf-like spread of system types: most are ordinary, some spectacular (giant moons, binary suns, rings) | pillar 1 |
| should | names you can say out loud | |
| later | many visitable galaxies; comets; rings; rogue planets; nebulae | |

## 3. What the prototypes taught

The prototypes never generated anything above a single planet, so the lessons here come from other levels and carry
over by analogy:

- **Everything is a pure function of seed and position, with one random stream per object type.** In the streamed
  world (`blobby-test/world/BLOBMAP.md`, "The big world"), a tree is identical at every level that includes it
  because its stream depends only on its cell. Stars per sector work the same way.
- **Statistical stand-ins disagree with the real thing.** M5 v2's far levels placed canopy blobs where a density
  exceeded 0.3. They showed clearings where the real generator puts ~27k trees per km² (`BLOBMAP3.md` §3). For stars
  this means the sky and the galaxy browser must draw the *same* stars the player can jump to. A separate "sky
  texture" that merely looks right is not enough. The diffuse galaxy band (stars too faint to resolve) is the one
  exception, because nobody can click it.
- **Grouping by grid cells shows up as a lattice from far away** (`BLOBMAP3.md`, jittered sites). Star sectors are
  grid cells, so stars must be placed with continuous random positions inside them, never on cell centres or a
  sub-grid.
- **Deterministic generation pays off later.** Making merges deterministic is what let edits stay local (`M7.md`
  §7). Here it is what makes an id meaningful to another player.

## 4. Design

### 4.1 The hierarchy

```
root (the universe; fixed at the origin)
└─ galaxy g                                     a body; GM may be 0 (§4.6)
   └─ system node                                one per surviving star candidate
      ├─ (single star)  the star itself is the system node
      └─ (multiple)     a massless barycenter body whose children are stars A, B (doc 04 §4.7)
         ├─ star A ── planets ── moons
         ├─ star B ── planets ── moons
         └─ circumbinary planets ── moons
      planets, belts (a belt is a pseudo-body that owns an annulus), moons
```

Rules:
- **Moons of moons are not generated.** They are rare in nature, and they would add a key level.
- **A belt is a body without blobs.** It reserves an annulus in its parent's slot list. Its members (asteroids) are
  generated lazily by index, like stars in a sector (§4.3).
- **Comets are deferred.** They break the annulus rule (§4.8). When added, they use the MOID form of the guarantee.
- **Other galaxies** are visible as sky decoration from M1. Whether they are visitable is Q03.1. The key reserves
  8 bits for them either way.

### 4.2 Generation units

Lazy generation needs units small enough to make on demand. Each unit must also be a pure function of its key and its
ancestors' units, never of its siblings:

| unit | inputs | produces | cost (estimate) | when |
|---|---|---|---|---|
| universe | seed, version | galaxy list (positions, shapes, seeds) | µs | at start |
| galaxy model | galaxy spec | analytic density and dust functions; arm parameters | µs | at start, always resident |
| sector cell | galaxy, class, cell coords | star candidates in the cell; survivors after thinning (§4.4) | ~µs per candidate | on demand around the camera, and for any key looked up |
| system | a surviving candidate's key | its stars, planets, belts and moons, with elements and properties | well under 1 ms | when selected, approached, or looked up |
| planet top level | `PlanetSpec` (§4.10) | the coarse planet (doc 14) | ms to s | on approach or embark |
| belt member | belt key, index | one asteroid | µs | when in view |

The system is the unit for planets, rather than each planet on its own. A planet's orbit depends on its inner
neighbours' annuli (§4.8), and generating a whole system costs only a few dozen hash calls.

The one place where a unit reads its siblings is **sector thinning** (§4.4). It reads star *candidates* in
neighbouring cells, never their survival status. So the read stays local and order-free.

### 4.3 Stellar classes and sector cells

Stars are generated in cubic cells of the galaxy frame. Each **class** has its own cell size, chosen so that its
cells are large where its stars are rare and bright. This does two jobs:
- each class's cells hold a manageable number of candidates;
- the stars visible from far away come from coarse cells, so the sky is a *visibility-driven* level of detail. Elite
  Dangerous's galaxy uses the same idea, with mass categories mapped to cell sizes.

| class | stands for | share of stars (main sequence, approx.) | typical M_V | visible to mag 6.5 | cell size |
|---|---|---:|---:|---:|---:|
| 0 | M dwarfs (and brown dwarfs, not drawn) | 76% | ~12 | ~0.8 pc | 10 pc |
| 1 | K dwarfs | 12% | ~7 | ~8 pc | 10 pc |
| 2 | G dwarfs | 7.6% | ~5 | ~20 pc | 10 pc |
| 3 | F | 3% | ~3.3 | ~45 pc | 20 pc |
| 4 | A | 0.6% | ~1.5 | ~100 pc | 40 pc |
| 5 | B | 0.13% | ~−1.5 | ~400 pc | 80 pc |
| 6 | O | 3e-5% | ~−5.5 | ~2.5 kpc | 640 pc |
| 7 | giants (evolved K/M/G) | ~0.5% (set by age) | ~0 | ~200 pc | 80 pc |
| 8 | white dwarfs | ~5% | ~12 | ~0.8 pc | 10 pc |

The shares are the commonly quoted main-sequence fractions for the solar neighbourhood. The visibility distances use
d = 10^((m − M + 5)/5) pc. Both are estimates, to be tuned by eye. All cell sizes are 10 pc × 2^k, which keeps the
key encoding (§4.5) and the visibility query simple.

**Candidates in a cell:**
- `λ = ρ(centre) · share(class, region) · volume`, where ρ is the galaxy density (§4.6). Bigger cells use a 2³-point
  quadrature.
- `count = poisson(λ, rng(seed, galaxy, class, cell, "count"))`, clamped to 65,535. This caps the density of the
  galactic nucleus, which a game can do.
- Candidate `i` (for i < count) gets `rng(seed, galaxy, class, cell, i, stream)` for each property stream: position
  (uniform in the cell, continuous f64), mass within the class range (from the Kroupa IMF restricted to the class),
  age, metallicity, multiplicity.

Everything is computed with the core's own deterministic libm and counter-based RNG (doc 02). A candidate never needs
`i − 1`: its properties come from its own stream.

### 4.4 Thinning: no two systems too close

Poisson placement occasionally puts two candidates very close together. The rule is: **a candidate survives iff no
candidate with a lower key, of any class, lies within `d_min` of it**. `d_min` is the sum of the two systems' reach
radii (§4.8) plus a margin; 0.01 pc is the default.

- **The rule reads only candidates,** not their survival. It is a pure, order-free function of the candidates in the
  cells that overlap a sphere of radius `d_min_max` around the star: at most 8 cells per class grid.
- **It can over-thin slightly.** If A is killed by B, and B by C, then A dies even though B is gone. That doesn't
  matter.
- **The cost is negligible.** At solar density the chance of a neighbour within 100 AU is ~5e-11 per star, so the
  test almost never fires. It only exists so that the guarantee in §4.8 holds without exceptions.
- **A key of a thinned candidate** resolves to "no such body" (§4.5). Ids are never reissued.

### 4.5 Keys and ids

**The body key** is 128-bit (`u128`), the stable identity of a body (README glossary). Body keys are **their own key
space**:
- generated body keys are *structured*, not hashed, so that an id decodes back into a key (search by id);
- dynamic body keys are hashes in namespace 1 (doc 04 §4.12);
- blob, instance and event keys are a different space: 128-bit hashes `H(parent key, semantic ordinal)` (doc 05),
  whose chains start from a body key. The `ns` bits keep the two spaces apart.

The strawman layout of a generated body key:

```
bit 127..126  ns      0 = generated, 1 = dynamic (doc 04 §4.12), 2–3 reserved
generated:
    125..118  galaxy  8   0 = the home galaxy; 255 = the root itself
    117..114  class   4   0–8 star classes (§4.3); 15 = the galaxy body itself
    113..106  —       8   reserved (0)
    105..58   cell    48  x, y, z: 16 bits each, signed, in the class's cell units
     57..42   index   16  candidate index in the cell
     41..39   comp    3   0 = system node (the star, or the barycenter); 1–7 = component A–G
     38..33   slot    6   0 = the component itself; 1–63 = the k-th planet or belt slot around it
     32..2    sat     31  0 = the slot body itself; j = moon j (planet slot) or member j (belt slot)
      1..0    —       2   reserved (0)
```

- Circumbinary planets are slots of component 0 (the barycenter).
- 16 bits per axis at 10 pc covers ±327 kpc, which is more than enough for a galaxy and its halo.
- The key is the root of every blob key on that body: doc 05's `H(body key, "root")`, `H(body key, "ground")`, ….

**The human-readable id** has two parts:

1. **The system id: a checksummed code for (galaxy, class, cell, index).**
   - Cells are re-indexed *densely*, inside each class's bounding box of the galaxy (a cylinder of radius 20 kpc and
     height ±2 kpc for a Milky-Way-like galaxy). For class 0 that is ~4000 × 4000 × 400 cells, about 6.4e9 or 33 bits.
     Coarser classes need fewer bits.
   - So the system id packs into ~53 bits for class 0, plus 4 for the class, plus 0–8 for the galaxy (omitted when 0).
   - This is written in **Crockford base32**: digits and letters without I, L, O, U, case-insensitive. Hyphens are
     ignored on input, and `O → 0` and `I, L → 1` are folded.
   - A final check symbol, mod 37 per Crockford, detects every single-character error and every adjacent
     transposition.
   - The result is about 12 characters, grouped: `7KQ2-XM9D-3PA4`.
2. **The in-system path.** It is short and human-readable, and kept out of the check symbol so that people can edit
   it:
   - `7KQ2-XM9D-3PA4` is the system (its star, or its barycenter);
   - `…/B` is star B;
   - `…/3` is planet slot 3 of the primary;
   - `…/B3` is slot 3 of star B;
   - `…/3.2` is moon 2 of planet 3;
   - `…/0.5` is circumbinary slot 5.

   Slots are numbered from the inside out, so `/1` is always the innermost.

**Procedural names** are cosmetic aliases:
- A syllable generator is seeded by the key, so a star gets a name like "Velorin" and its planets get "Velorin 3" or
  their own names.
- They are **not unique**: 10¹¹ stars against perhaps 10⁶ pleasant names. A name can't be inverted into a key without
  enumerating the galaxy.
- So name search only covers bodies the save already knows: visited, bookmarked, journaled, or in generated sectors
  near the camera (doc 18). The id is always the authority.
- **Players may rename bodies.** Those names live in the save and journal (doc 15, 17), never in the universe.

**Links** look like this:

```
https://<host>/#v=<universe version>&s=<seed>&b=<id>[&p=<lat>,<lon>,<alt>][&t=<tick>][&m=<plugin set hash>]
```

- `v` and `s` are needed because a generator change alters what an id points to (Q03.8).
- `m` is a hash of the plugin set and versions (doc 08). A receiver with a different set is told that the surface may
  differ.
- `p` is in the body-fixed frame (doc 04), so it doesn't depend on the planet's spin phase. Six decimals of a degree
  is ~9 mm on a 500 km planet.
- `t` is a world tick. Rails motion is a pure function of time (doc 04), so the receiver can reproduce the
  configuration of the sky exactly. Whether the *simulated* state (weather, a running eruption) matches depends on
  determinism and the observer effect (doc 10). Without edits and with a small observer effect, the coarse state at
  tick `t` should match too. That is a nice property worth testing (§7, E6).

### 4.6 The galaxy model

The galaxy body is analytic. Its density functions are evaluated, never stored:

- **Thin disk:** ρ_d = ρ0 · exp(−R/h_R) · exp(−|z|/h_z), with h_R ≈ 2.6–3.5 kpc and h_z ≈ 0.3 kpc. A thick disk has
  ~10% of the mass at h_z ≈ 1 kpc.
- **Bulge:** ρ_b ∝ exp(−r/r_b), with r_b ≈ 0.5–1 kpc. It is old: its stars are drawn older, so it has more giants.
- **Arms:** the disk is multiplied by
  `1 + A · cos(m (θ − ln(R/R0)/tan p) − φ0)^n`, a logarithmic spiral with m = 2–4 arms and pitch p ≈ 10–15°. The arms
  bias young classes (O, B, A) much more than old ones, which is why arms look blue.
- **Dust:** a thinner, arm-following density used only by the sky renderer for extinction and dark lanes (doc 13).
- **Halo:** sparse old stars, truncated at the cell grid's bounding box.

Each galaxy's parameters (arm count, pitch, bulge fraction, size) come from its seed. The home galaxy is set to look
like the Milky Way, so that the sky from a typical system has a recognisable band.

**Do stars move?** There are three options:

| option | how stars move | non-collision | cost | verdict |
|---|---|---|---|---|
| **S, static** | fixed in the galaxy frame; the galaxy's GM is 0 for its children | exact, by thinning (§4.4) | none | **recommended** |
| K, Kepler | circular orbits around a point-mass galaxy | probabilistic (below) | rails per star | uniform with everything else, but unrealistic speeds near the centre (v ∝ r^−½) |
| F, flat curve | circular, ω = v0/R with v0 ≈ 220 km/s | probabilistic | a non-Kepler rail | realistic, but needs its own motion type |

Why S:
- **The motion would be invisible.** Over a century of game time, a star near the Sun's galactic radius moves
  ~7e14 m relative to the galaxy centre. Relative to its neighbours (~20–30 km/s) it moves ~6e13 m. Seen from a
  neighbour 1 pc away, that is a shift of ~0.1°, after a hundred years.
- **Moving stars can't be made provably non-colliding** without pinning them to a razor-thin disk.
  - Stars at distinct radii on circular orbits never *coincide*. But with differential rotation, any two stars whose
    radii differ by less than the sum of their reach radii (~10¹³ m) will eventually pass that close.
  - At solar density the rate of passes within 100 AU is Γ = n·π·b²·v ≈ 0.1 pc⁻³ · π · (4.9e-4 pc)² · 3e-5 pc/yr ≈
    2e-12 per star per year. That is negligible for any one system, but it is not zero.
- **Under S, "stars feel only the galaxy" becomes "stars feel nothing".** This is the degenerate Kepler case, and it
  stays consistent with the user's "every body orbits its parent".
- **Switching later is cheap.** If Joel wants stellar motion (Q03.3), options K and F are each a small change to the
  galaxy's children (doc 04).

### 4.7 Stars

From a candidate's mass M (in solar masses) and age:

- **Luminosity** (main sequence, piecewise): L = 0.23 M^2.3 (M < 0.43); M^4 (0.43–2); 1.4 M^3.5 (2–55).
- **Radius:** R ≈ M^0.8 (M < 1); M^0.57 (M ≥ 1).
- **Temperature** from Stefan–Boltzmann: T = 5772 K · (L / R²)^¼.
- **Main-sequence lifetime** t_MS ≈ 10 Gyr · M^−2.5. Older stars become giants (class 7), or white dwarfs once well
  past it.
- **Colour** from T, as a blackbody converted to sRGB. Doc 13 owns the conversion; this doc stores T.
- **Multiplicity:** the chance of a companion rises with mass (~30% for M dwarfs, ~50% for G, ~70% for O/B). The mass
  ratio q is roughly uniform on [0.1, 1]. Separation is log-normal, peaking near 50 AU (§4.9 for scaling). Only
  binaries are generated at first; triples are Q03.4.
- **The class used for the cell grid is the primary's,** so a binary is as visible as its brightest star.

These relations are textbook approximations. They need to look right, not be right. Doc 13 decides how stars look.

### 4.8 Systems: the non-colliding construction (D14)

**What has to be guaranteed.** Bodies feel only their parent (doc 04). So every orbit is an exact, periodic Kepler
ellipse, and the system is "stable" forever by fiat. There is no dynamical-stability problem to solve. The only
requirement is geometric: two siblings, together with everything inside their spheres of influence (moons, blobs),
must never occupy the same space.

**Proof.** Take two siblings X and Y of parent P.
1. X's distance from P always lies in `[q_X, Q_X]`, its periapsis and apoapsis. This is a property of the Kepler
   ellipse.
2. Every point p of X's SOI satisfies `|p − r_X| ≤ s_X`, where s_X is the largest SOI radius X ever has. So by the
   triangle inequality, `| |p| − |r_X| | ≤ s_X`, and p lies in the shell `[q_X − s_X, Q_X + s_X]`.
3. If X's and Y's shells (the **annuli**) are disjoint, their SOI regions are disjoint at every instant, whatever
   the inclinations, nodes and phases.
4. Every child of X stays inside X's SOI (the moon condition below), and X's blobs lie inside its SOI
   (`R_bound < s_X`, asserted). So by recursion nothing under X ever meets anything under Y. ∎

**The SOI radius.** s = r · (m/M)^(2/5) at distance r (Laplace; doc 04 §4.3). It is largest at apoapsis:
`s_max = Q · λ` with `λ = (m/M)^(2/5)`. It is smallest at periapsis: `s_min = q · λ`.

**The moon condition.** A child C of X must stay inside X's SOI at its smallest:
`Q_C + s_C,max ≤ f · s_X,min` with f = 0.5. Real prograde moons beyond ~½ the Hill radius are unstable. Our rails
don't care, but SOI-based parenting would hand such a moon to the star at the wrong moment (doc 04 §4.3).

**The binary exception.** Components A and B of a binary are on opposite sides of their barycenter. Their annuli
around it can overlap, but they cannot meet. For them the check is the relative orbit: `a_rel (1 − e_rel) > reach_A +
reach_B`.

**Construction of a system** (the system unit):

```
1. stars from the key (§4.7); for a binary, the relative orbit (a_rel, e_rel), the barycenter, and the
   components' elements (doc 04 §4.7)
2. for each attractor (each star, and the barycenter for circumbinary slots):
     a_in  = max(inner limit: k·R_star, the binary's stability zone)
     a_out = the outer limit (a star's disk; for S-type planets ~⅓ of the binary periapsis)
     next  = a_in
     for slot k = 1, 2, …:
        draw the body type from the zone (§4.9); draw μ, R, e (Rayleigh, σ ≈ 0.05), i (σ ≈ 2°), Ω, ω, M0
        choose a ≥ next so that:
            inner edge of annulus  = a(1−e) − λ·a(1+e) ≥ previous outer edge · (1 + margin)
            Hill spacing Δ ≥ Δ_min against the previous slot (optional, for looks; §4.9)
        if a > a_out: stop
        reserve the annulus [a(1−e)(1−λ'), a(1+e)(1+λ')]   (λ' from the SOI at that end)
        next = its outer edge
     belts reserve [a_belt_in, a_belt_out] the same way
3. moons for each planet: the same loop around the planet, between the Roche limit (~2.44 R_p (ρ_p/ρ_m)^⅓)
   and f · s_planet,min
4. assert the invariants: annuli disjoint, moon condition, R_bound < s, binary periapsis rule
```

The loop is deterministic, because every draw comes from the slot's own stream. Slot k's position depends on slots
1..k−1, which is why the system is the unit. A later variant could fix the slot radii to a jittered log grid wide
enough to make the annuli disjoint for any allowed e. Each planet would then be independent, at the cost of some
realism. Not needed now.

**Belts.** Members have overlapping annuli inside the belt's annulus, so they can in principle meet. Doc 04's rule is
that two bodies on rails never collide ("ghosts"). Members are tiny and only exist near observers, so this is safe.

**Comets (later).** Their orbits cross other annuli. The exact, time-independent condition is the **MOID** (minimum
orbit intersection distance) between two ellipses. Requiring `MOID > s_comet + s_planet` with margins guarantees no
meeting whatever the phases, and the annulus rule is just a cheap sufficient case of it. Comets therefore need a MOID
routine (e.g. Gronchi's algebraic method) before they can be generated.

### 4.9 Planets, moons and the question of scale

**Types by zone.** Zones are measured relative to the star's habitable zone (d_HZ ≈ √(L/L☉), in the system's
distance unit) and its snow line (~2.7 √(L/L☉)):

| zone | likely types |
|---|---|
| very close (T_eq > ~1000 K) | lava world, scorched rock |
| inner | rocky barren, desert, cloud-wrapped (Venus-like) |
| habitable zone | rocky temperate, ocean world, desert |
| beyond the HZ, inside the snow line | cold rocky, Mars-like |
| beyond the snow line | gas giant, ice giant, icy world |

Moons follow the same table, using their planet's zone. Habitable moons of gas giants in the HZ are kept on purpose:
a giant filling 10–20° of the sky is exactly the "awe" pillar 2 asks for.

**The equilibrium temperature** is T_eq = T_star · √(R_star / 2a) · (1 − A)^¼. It depends on a *ratio*. Doc 14 uses
it, with greenhouse and atmosphere terms, for the planet's climate.

**Embarkable** means a solid or liquid surface: rocky, ocean (islands), icy, lava (dangerous). Gas giants and stars
are not embarkable. **Habitable** means embarkable, T_eq in range, an atmosphere, and liquid water allowed.

**Scale: the absolute size above the planets is (almost) invisible.** If you scale every distance and star radius
above the planet scale by s, and luminosities by s², then:
- every angular size (the sun, the moons, the other planets) is unchanged;
- every apparent brightness and T_eq is unchanged;
- the sky from every viewpoint is identical.

With jump-only travel (doc 18), the absolute scale has no other visible effect. What the player experiences is a
handful of *independent* numbers. They are independent because each body's gravity matters only to its own children,
so each pair can be tuned on its own:

| number | set by | example |
|---|---|---|
| planet radius R | doc 14, Q03.2 | 500 km (reference planet); prototypes used 250 km |
| surface gravity g | μ_planet = g R² (doc 04 Q04.2) | 9.81 m/s² → μ = 2.45e12 m³/s² |
| day length | spin (doc 04) | Q04.4 |
| year length | μ_star = 4π² a³ / T² | free: choose T, derive μ_star |
| sun's angular size | R_star / a | ~0.5° as on Earth |
| insolation, T_eq | T_star and R_star / a | Earth-like in the HZ |
| moon angular size | R_moon / a_moon, within the moon condition | up to several degrees |

So the recommendation (Q03.2):
- **Real interstellar distances and real star radii.** The real formulas, tables and night-sky counts then apply
  unchanged.
- **Small planets** (250–1000 km radius).
- **Tuned μ per body** for surface gravity and year length.

Compressing systems KSP-style (Kerbin is 600 km and orbits at 1.36e10 m) only changes numbers nobody sees.

There is one real coupling: the SOI radius, which limits how far out moons can be.

```
s = a · (μ_p / μ_star)^(2/5)    with μ_star = 4π² a³ / T_year²
  ⇒ s ≈ (μ_p T_year² / 4π²)^(2/5) · a^(−1/5)
```

Room for moons depends mostly on the year length and only weakly on a. For the reference planet (μ_p = 2.45e12):

| year (world time) | a | SOI | a moon at ½ SOI orbits in |
|---|---|---:|---:|
| 365 d | 1 AU | 1.2e8 m | ~22 d |
| 30 d | 1 AU | 1.6e7 m | ~1.1 d |
| 2.5 d | 1 AU | 2.3e6 m | ~1.3 h |
| 2.5 d | 0.01 AU | 5.6e6 m | ~5 h |

(Estimates; the last column is the Kepler period at 0.5·SOI around μ_p.) This is physics, not a limitation of the
design: the longest possible month is a fixed, small fraction of the year (here 2–6%). So the choice of year length
(Q03.9, tied to the world clock in doc 02 and doc 10) also decides how far out moons can be. A short year leaves
room only for close, fast moons, which look large in the sky.

**The Zipf spread of systems.** Draws are tilted so that most systems are ordinary while a long tail is spectacular:
binary suns with two shadows, giant close moons, ringed giants, habitable moons, double planets (a barycenter pair,
doc 04 §4.7). A rarity score per system (doc 17) lets the browser highlight the tail.

### 4.10 The planet spec handed to doc 14

```
PlanetSpec {
  key: u128, seed: u64                    // seed = rng(universe seed, key, "planet")
  kind: rocky | ocean | icy | lava | gas_giant | ice_giant
  radius_m: f64, mu: f64                  // μ = g R²
  spin: { axis, period_s, phase0 }        // doc 04
  atmosphere: { surface_density, scale_height_m, top_m, composition_tint }   // doc 04 drag, doc 13 scattering
  t_eq_K: f64, insolation: f64
  ocean_fraction, tectonics, volcanism, ice_caps: f32   // style knobs for doc 14
  host_star: { T_K, angular_radius }, siblings_visible: [...]   // for doc 13
}
```

Doc 14 may add knobs. This doc guarantees only that every field is a pure function of the key.

### 4.11 What the night sky and the browser need

Doc 13 draws the sky and doc 18 lets the player click it. Both need:

- **`sky_stars(position, mag_limit)`**: for each class, every surviving star whose apparent magnitude from `position`
  is brighter than `mag_limit`.
  - It returns direction, apparent magnitude, T, and the key, so that a click can jump to it.
  - It visits the cells of each class's grid within that class's visibility distance (§4.3), about (2·d_c / cell)³
    cells per class.
  - At mag 6.5 this is of order 10⁴ stars; at mag 8, a few times more. It is computed in a worker when the camera
    moves to a new system or travels far, and cached per system. Stars are static (§4.6), and in-system parallax is
    negligible.
- **`galaxy_density(p)` and `dust_density(p)`** for the diffuse band. The sky renderer integrates them along view
  rays into a cubemap per system (doc 13). This is the unresolved light of the ~10¹¹ stars nobody can click, so it
  should be consistent with the class shares above.
- **The galaxy seen from outside** (the galaxy browser zoomed out): the same density and dust functions, drawn as a
  volume (doc 12, 13), plus the resolved bright stars near the camera.

### 4.12 Canonical scenarios

**1. Orbit to grass** (the first half; doc 18 has the rest):
- The player types `7KQ2-XM9D-3PA4/3`. Input is normalised and the check symbol validated.
- The id is decoded to (galaxy 0, class 2, cell, index 17, slot 3).
- The candidates in the 8 nearest cells of every class are generated. Candidate 17 survives thinning.
- The system unit is generated (~1 ms). Slot 3 is a rocky temperate planet, so its `PlanetSpec` goes to doc 14.
- Doc 18 starts the transition. Doc 04 evaluates the planet's position at the current tick.
- Nothing else in the galaxy was generated except `sky_stars` around the destination, for its night sky.

**4. The asteroid:**
- Generated belt members are keys in the belt's sat field, and they are ghosts on rails (doc 04).
- The event's impactor is a **dynamic** body. Its key comes from doc 04's dynamic namespace (creation tick, creator
  key, sequence), not from this doc. It does not have to satisfy D14: the event plans its collision on purpose (doc
  17, doc 04 §4.11).
- If the event wants a "real" belt member to be the impactor, it converts that member into a dynamic body with a new
  key, and records the link in the journal.

**5. Dig, leave, return:** ids and links make "return" exact. A bookmark stores the id and `p`, and a later jump
resolves to the same body and the same body-fixed point.

## 5. Interfaces

The core exports these (C ABI; doc 08 for the plugin-facing subset). Keys cross the ABI as two `u64` halves.

```c
typedef struct { uint64_t hi, lo; } key128;

// ids
int   id_format(char* dst, uint32_t cap, key128 key);                   // canonical text, with grouping
int   id_parse(key128* dst, const char* text, uint32_t len,             // 0 ok, <0: syntax / checksum / unknown
                char* suggestion, uint32_t cap);                         // single-character fix that passes the checksum
int   key_exists(key128 key);                                            // 1 if it decodes and survives thinning
int   name_of(char* dst, uint32_t cap, key128 key);                      // the procedural name

// generation (pure; results cached by the core)
int   system_generate(key128 system_key);                                // creates body records for the system (doc 04)
int   planet_spec(PlanetSpec* dst, key128 planet_key);
uint32_t sky_stars(SkyStar* dst, uint32_t cap, uint32_t galaxy,         // nearest-first; returns the count written
                   const double pos[3], float mag_limit);
float galaxy_density(uint32_t galaxy, const double pos[3]);             // stars per pc³
float dust_density(uint32_t galaxy, const double pos[3]);

typedef struct { float dir[3]; float app_mag; float temp_K; uint32_t pad; key128 key; } SkyStar;
```

Plugins see only `name_of`, `planet_spec` and `id_format`. Generation of the universe is not pluggable at first
(Q03.10).

## 6. Risks and unknowns

- **The id length.** About 12 characters plus a path is fine to paste, but tedious to type. Procedural names cover
  the common case of bodies already known.
- **Generator versioning is the biggest long-term risk.** Any change to a density constant, the IMF, or the slot loop
  changes what ids point to.
  - Mitigation: a universe version in every key-bearing artefact (links, saves). Freeze the generator per major
    version. Keep old versions runnable, or migrate saves (doc 15).
  - The same applies to plugins that generate surfaces (the `m` hash in links).
- **Taste.** Whether the sky looks like a galaxy is a matter of tuning the class shares, the arms and the dust. E4
  checks it early.
- **Poisson counts in the nucleus.** Clamping the density there could show as a visible "plateau" in the galaxy
  browser. A finer grid for the bulge, or a separate bulge class, would fix it if it shows.
- **Whether jump-only travel stays** (Q18.5). If ships come later, interstellar distances matter again. Under option
  S, a ship between stars moves in a straight line.

## 7. First experiments

| # | experiment | success criterion |
|---|---|---|
| E1 | Generate `sky_stars` for a Sun-like position in a Milky-Way-like galaxy in the core (WASM, Node) | ≤ 50 ms per sky at mag 8; counts per class within 2× of the table's estimates; no lattice in a direction histogram |
| E2 | Id round trip: encode / decode 10⁶ random surviving keys; mutate one character or swap two | 100% round trip; 100% of single errors and adjacent swaps detected |
| E3 | Generate 10⁶ systems; check every §4.8 invariant; plot planets per system, types per zone, moon counts | 0 invariant failures; distributions that look plausible to Joel |
| E4 | Render an equirectangular sky (star points plus the integrated band with dust) from three positions: solar-like, near the bulge, high above the disk | Joel recognises a galaxy; the band and the rifts read as such |
| E5 | Thinning: count thinned candidates over a 1 kpc cube near the centre and near the Sun | thinning is rare (< 1e-6 of candidates) outside the nucleus; deterministic across runs |
| E6 | Two runs of the same seed to tick t: one with the observer at the destination, one elsewhere; compare the coarse fields of the planet in the link | rails identical; coarse fields equal within doc 10's observer-effect budget |

E1–E3 and E5 are pure core code and can run in M0's Node harness.

## 8. Build order

- **M1 the sky:**
  - the key layout, id format and parse, the check symbol;
  - the galaxy model, class grids, candidates and thinning;
  - `sky_stars`;
  - the system unit with planets and moons, binaries included if Q03.4 says so;
  - the invariant asserts, procedural names, the galaxy-browser star list.
  - E1–E5.
- **M2 a planet from orbit:** `planet_spec` complete for rocky and ocean planets, consumed by doc 14.
- **M5:** links with `m` and `t`; the universe version recorded in saves (doc 15).
- **Later:** comets (with MOID), rings, other visitable galaxies, rogue planets, nebulae.

## 9. Open questions

**Q03.1 One visitable galaxy, or many?**
- One galaxy already holds ~10¹¹ systems. Many galaxies mostly add id length and a top-level browser.
- *Recommended:* one visitable galaxy; others drawn in the sky as decoration; 8 key bits reserved.

**Q03.2 Planet size and the scale of systems.** (merged into questions.md C1)
- §4.9 shows that the absolute scale above the planets is invisible. What matters is planet radius, gravity, day,
  year, and the sizes of sun and moons in the sky.
- *Recommended:* real interstellar distances and star radii; planets of 250–1000 km radius; μ tuned per body.
- Joel: what planet radius do you want to be able to walk or fly around?

**Q03.3 Should stars move in the galaxy?**
- Static stars make the non-collision guarantee exact. The motion would be invisible over game time anyway (~0.1° per
  century).
- *Recommended:* static (option S).

**Q03.4 Binaries and multiples from the start?**
- About half of stars have companions, and two suns (two shadows, a double sunset) are a strong "awe" moment. The
  cost is the barycenter body and the S-type and P-type slot rules.
- *Recommended:* binaries yes, in M1; triples later.

**Q03.5 Names.**
- Procedural names are cosmetic, non-unique and searchable only among known bodies. Players can rename bodies in
  their save.
- *Recommended:* yes to both. Should player-given names ever be shared, for example through links?

**Q03.6 One shared universe?** (merged into questions.md C14)
- Every player with the same universe version sees the same universe, and ids are shareable.
- *Recommended:* yes. One canonical seed per universe version, with custom seeds as an option for testing.

**Q03.7 Where does a new player start?**
- *Recommended:* walk a seed-derived sequence of candidate systems near a fixed region of the home galaxy (a
  "frontier" of pleasant G and K stars). Pick the first habitable planet whose rarity score is above a floor: an
  interesting sky, not just habitable.
- Should every new player start at the same place, or on a different planet?

**Q03.8 Generator versioning.** (merged into questions.md C9)
- A change to the generator changes what ids mean.
- *Recommended:* freeze per major universe version, with the version in every link and save. Do you want old
  universes to stay visitable after an upgrade?

**Q03.9 Year length.** (merged into questions.md C2)
- Seasons, and how far out moons can be, depend on it (§4.9). It is tied to the world clock (doc 02, doc 10).
- *Recommended:* decide together with the day length (Q04.4). With C2's working default of a ~1 real-hour day, a year
  of ~20–40 days (20–40 real hours) gives seasons within a few sessions.

**Q03.10 Is universe generation pluggable?**
- Plugins could add body types (rings, comets, megastructures) or change star and planet distributions. That
  complicates ids and versioning.
- *Recommended:* not at first. Plugins act on and below a planet's surface. Revisit after M6.
