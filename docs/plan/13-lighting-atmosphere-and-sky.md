# 13. Lighting, atmosphere and sky

Status: draft · Depends on: 03 universe, 04 bodies, 05 blob model, 09 fields, 12 rendering, 14 planet generation ·
Used by: 12 rendering, 14 planet generation, 16 entities, 17 events, 18 player

## 1. Purpose and scope

This doc is about how light reaches the eye: from suns, moons, skies, fires and lava, through air, cloud and water,
onto blobs, and through to the tonemapper. Pillar 2 lives or dies here. The sunset, the view from a mountain top,
and the storm on the horizon are mostly lighting, atmosphere and clouds.

**In scope:**
- light units and the HDR pipeline;
- light sources derived from bodies: suns, moons, planetshine, eclipses;
- direct lighting and the material response;
- shadows;
- AO and sky visibility;
- the atmosphere: sky, aerial perspective, sunsets, the view from space;
- clouds, water lighting, volumetrics;
- many emissive lights;
- the night sky;
- weather visuals;
- exposure and tonemapping;
- the GI roadmap;
- level consistency of lighting;
- the lighting share of the frame budget.

**Out of scope:**
- the render structure, passes, precision and data path: [12](12-rendering.md). This doc uses its pyramid of render
  nodes, bricks, parts and splats;
- how clouds, fires and lava are *simulated*: [10](10-simulation-and-scheduling.md), [16](16-entities-and-creatures.md)
  and the plugins. This doc states which fields and blobs it reads;
- where stars and atmospheres come from: [03](03-universe-and-identity.md), [14](14-planet-generation.md);
- sound.

## 2. Requirements

| | requirement | traces to |
|---|---|---|
| must | a sunset and a mountain-top view that inspire awe: correct sky colour through the day, aerial perspective over tens of km, the terminator seen from orbit | pillar 2 |
| must | light comes from the universe: the sun's colour, size and brightness from its star body; several suns; moonlight; eclipses where geometry says so | pillars 1, 2; [04](04-bodies-and-orbits.md) |
| must | shadows from any blob onto any blob, at any distance the eye can see (a ridge shadowing a valley at sunset) | constraint 5 |
| must | AO first, and a path to fuller radiosity with several light sources | constraint 5 |
| must | a storm is visible from far away, before it arrives | constraint 7, scenario 2 |
| must | lighting looks the same at every LOD level (no darker or brighter far field) | `M7.md` §1, constraint 1 |
| must | every effect has a stateless path ([12](12-rendering.md) §4.12) | Joel |
| should | caves and dense forests are dark; valleys get bounced light | constraint 5 |
| should | rare light events: eclipses, auroras, volcanic sunsets, lightning, bioluminescence | pillar 1, [17](17-events-and-discovery.md) |
| should | many emissive lights at once (a lava flow, a forest fire) | scenario 4 |
| later | multi-bounce GI, glossy reflections, caustics, underwater | |

## 3. What the prototypes taught

**Sky and tone.** Every prototype used an ad-hoc sky.
- `planet/js/render.js` used a single-shell optical length plus a sun glow, with `tonemap = x/(1+0.35x)`.
- `world/js/wgsl_lib.js` used a two-colour gradient plus `pow` sun lobes, a fixed fog colour, and the ACES fit.
- None of them gives a correct sunset, aerial perspective or view from space. That is the gap to close first.

**Material response** (`blobby-test/README.md`, creatures).
- What mattered was wrap diffuse with a warm penumbra, sheen for fur, back-light translucency for ears and
  membranes, a GGX-ish specular, and noise displacement (`shag`), more than atom count.
- The creature renders also used a shadowless camera-side fill and a ground-bounce term to keep close-ups out of
  black shadow.
- `world/` kept a reduced version (`lighting()` in `wgsl_lib.js`): wrap by translucency, sheen, a sky-ambient
  gradient by normal.

**Shadows.**
- Soft shadows used the classic `k·h/t` estimator with a small maximum step. The "improved" penumbra estimator gave
  false zeros where the field grew faster than a distance field (`ITERATIONS.md` 02, 06).
- Feeding a coarse skip bound into the penumbra printed the voxel grid as shadow lines (`BLOBMAP.md` #9).
- M6: a light-space grid plus a light pre-pass, marching *down* from the topmost blob reach, cut shading by 22–55%,
  at the cost of some penumbra from occluders beside the column (`BLOBCUT.md`).
- M5 v2: grass needs exact shadows. A sun-view depth map was faint at 7 cm texels and cost 6–34 ms. A sun-space 2D
  grid with analytic ray–ellipsoid tests is exact at 0.1–2.5 ms. The penumbra needs the ellipsoid's radius in the
  miss direction (`BLOBMAP2.md` #3).
- Half-resolution secondary rays (2×2 blocks, depth- and normal-weighted) are worth ~10%. The residue is 1–2 px
  steps at grass shadow edges.
- The ray cut: half-resolution sun shadows with a depth-aware upsample cost 1.0–3.5 ms at 1080p.

**AO.**
- 4–5 field samples along the normal (M7: 0.12, 0.48, 1.08, 1.92 m).
- The "nearest blob in the list" term made list-shaped seams (`BLOBCUT.md`, `gNoDmin`).
- AO sampled below a level's accuracy reads sampling error as occlusion. Scaling AO distances with the level adds
  occlusion the near levels never had. The fix is to fade samples below 1/16–1/8 of a cell (`M7.md` §1).

**Level consistency.** Measuring first showed coarse levels 34–44% *darker*, not brighter. There were three causes:
AO below accuracy, merged tree clusters becoming ground-covering plates, and shadow rays starting inside coarse
surfaces. After the fixes, coarse levels were within −15% to +8% (`M7.md` §1). The canopy-darkening bake planned
beforehand turned out to be unnecessary.

**Water.** Far water glints, because each level's blob bumps are a few pixels wide. Flattening the normal and widening
the roughness (0.06 → 0.22) from ~20 m to 200 m at 1080p turns it into a sheen (`M7.md` §1).

**Snow.** The GPU drew snow per pixel from the same law as the simulation (a snowline plus a stable depth from the
slope), so snowlines followed elevation at any distance (`PLUGINS.md` §3).

## 4. Design

### 4.1 Units and the HDR pipeline

**Physical units, pre-exposed.**
- Radiance is in cd/m²-equivalent (luminance) per linear Rec.709 channel. Illuminance is in lux.
- A sun like ours gives ~127 k lux at the top of the atmosphere (1361 W/m² at ~93 lm/W) and ~100 k on the ground at
  noon. A full moon gives ~0.1–0.3 lux. A clear moonless night is ~0.002 lux.
- The sun's disc has ~1.6e9 cd/m², which overflows f16. So everything is rendered **pre-exposed**: multiplied by the
  current exposure before it reaches an f16 target, as in Frostbite.
- Exposure is a scalar known before the frame (§4.12). Carrying it from the last frame is not pixel reuse, so it is
  compatible with the stateless policy.

Why physical:
- One set of numbers then covers noon, dusk, night, space, lava glow and a lightning flash.
- Light sources come from simulation and generation (star luminosity, lava temperature) rather than artist tweaks.
- Eye adaptation can do what a real eye does.

**Working space.** Linear Rec.709 primaries in `rgba16float`. Wide gamut is a later option. Blackbody and spectral
effects (§4.2) are converted to Rec.709 once, in tables.

### 4.2 Light sources from bodies

The core computes the **light list** for the focus body every frame, in f64, from [04](04-bodies-and-orbits.md)'s body
states. The **same function** feeds simulation (insolation for climate, [10](10-simulation-and-scheduling.md)) on the
CPU at tick times, so the sim and the picture agree on where the sun is.

**Suns.** For each star body within range:
- **Direction** from the star to the shading point. For a planet's surface, one direction per frame is exact enough:
  the parallax across a planet is ~1e-6 rad.
- **Colour.** Blackbody chromaticity from the star's effective temperature (Planck's law integrated against the CIE
  1931 colour-matching functions, then XYZ → linear Rec.709), in a 1000–40000 K table. The Sun is ~5772 K.
- **Illuminance** at the focus body: `E = L_v / (4π d²)`, from the star's luminous luminosity. The generator
  ([03](03-universe-and-identity.md)) places habitable planets so that `E` is in a pleasant range. Gameplay brightness
  never depends on a hand-tuned constant.
- **Angular radius** `α = R_star / d`: the Sun is 4.65 mrad. It sets the disc drawn in the sky and the shadow
  penumbra (§4.4). A red giant close by gives soft shadows; a distant white dwarf gives razor-sharp ones.
- **The disc** is drawn with limb darkening and a glare kernel. The atmosphere's transmittance colours it (§4.6).

**Several suns.**
- Up to 4 suns are lights. Each brings its own shadow rays, in brightness order.
- Suns below ~10% of the brightest get quarter-resolution shadows. Below ~1%, none.
- Two coloured suns give two coloured shadows: a rare, beautiful sight that falls out for free.

**Moons and planetshine.**
- A body lit by a sun reflects `E_refl = E_sun · A · (R/d)² · Φ(phase)`, where A is its albedo and Φ the Lambert
  sphere phase function (2/3 at full).
- For our Moon this gives ~0.2 lux, which matches reality, so the formula needs no tuning.
- Each moon (and a big parent planet in a moon's sky) is a directional light with its albedo colour and an angular
  radius. Its shadows are optional (soft, low resolution).
- The dark side of a moon drawn in the sky gets planetshine from the focus body by the same formula. This is the
  ashen glow on a crescent moon.

**Eclipses fall out of geometry.** They are analytic, deterministic and predictable, so [17](17-events-and-discovery.md)
can list them in advance.
- **Solar.** For each occluder body `o` near the sun's direction, the visible fraction of the sun is
  `1 − overlap(α_sun, α_o, θ) / (π α_sun²)`: the overlap area of two discs in angular space (with limb darkening as a
  later refinement). It multiplies the sun's illuminance.
- Near an eclipse it is evaluated **per pixel**, from the pixel's world position, because a nearby moon's parallax
  across the landscape is the umbra's edge. The player can watch the umbra sweep across the valley.
- During totality the sky's sun input drops (§4.6), the exposure adapts, stars appear, and a corona is drawn around
  the occluded disc.
- **Lunar.** When a moon passes into the focus body's shadow, its sun input is occluded by the focus body, and the
  remainder is lit by sunlight refracted through the focus body's atmosphere. That light is the transmittance LUT
  (§4.6) at grazing incidence, which is red. So the moon turns copper without a special case.

### 4.3 Direct lighting and material response

The material model is [12](12-rendering.md) §4.11's table. This section is how it responds to light:
- **Diffuse:** Lambert, mixed toward **wrap** (`(n·l + w)/(1 + w)`) by the material's wrap or translucency, with the
  creatures' warm penumbra tint.
- **Specular:** GGX with Schlick Fresnel; roughness and specular from the material.
- **Sheen / fuzz:** a grazing lobe for fur and velvet, scaled by AO.
- **Translucency:** back light through thin parts (leaves, ears, wing membranes, grass). The material's
  `trans × albedo² × sun × pow(max(dot(view, sun), 0), 3) × shadow` is the prototype's form, kept for its look.
- **Emissive:** added after lighting.
- **Ambient:** sky irradiance (§4.5) times AO times sky visibility, plus a ground-bounce term (§4.13 step 0).

**No fill light** in the game's normal lighting. The creature renders used a camera-side fill to rescue close-ups,
which is an artistic cheat. The real fix is sky light plus bounce. A photo mode may bring it back.

### 4.4 Shadows

Shadow rays are world-space ray queries through [12](12-rendering.md)'s structure: the pyramid, the parts' top level,
and the detail sun grid. They are traced once per 2×2 block at the block's nearest hit, then upsampled by depth and
normal. A pixel no block agrees with traces its own (4–19% of pixels in `BLOBMAP2.md`).

**The cone.**
- A shadow ray toward a sun of angular radius α is a cone of radius `r(t) = t·tan α + r₀`, where `r₀` is at least
  the hit level's footprint, so the ray never starts inside a coarse surface (`M7.md` §1).
- Along the ray, the sample uses the pyramid level whose cell is ~r(t). This is **cone tracing on the ancestor
  bricks** that [12](12-rendering.md) §4.3 keeps resident.
- Long rays get cheap fast. Toward a low sun, at t = 1 km the cone is ~5 m (level ~2); at 10 km it is ~50 m (level
  ~5). So a 30 km shadow ray costs a few dozen coarse steps, not thousands of fine ones (an estimate, to be measured
  in L3).
- The penumbra is the `k·h/t` estimator with `k = 1/tan(α_eff)`, where `α_eff` is the physical angular radius
  clamped to an artistic minimum (Q13.2). It reads the brick's sampled field, never a coarse skip bound.

**Early-out: the sun top map.**
- An orthographic sun-view raster of node content bounds over ±2 km around the camera stores the topmost surface
  per texel (M6's light pre-pass).
- A receiver above it, nearer the sun, is lit, with no ray.
- Otherwise the ray starts there and marches down. M6 lost some penumbra from occluders beside the column (0.4–0.9%
  of pixels); the cone from the real receiver avoids that.
- It is rebuilt only when the sun moves by more than a texel's angle or the bounds change.

**Grass and thin detail.** M5 v2's sun-space grid: 25 cm cells over the detail range, each listing the detail blobs
whose sun-view footprint (+10 cm) covers it. The test is analytic ray–ellipsoid with the penumbra from the radius in
the miss direction. It is exact and costs 0.1–2.5 ms to build.

**Parts.** Rigid parts and the bones of model instances (creatures, the avatar's appearance; R10) are traced in
their local frames through the top level, the same as primary rays ([12](12-rendering.md) §4.7).

**Far shadows.** A ridge shading a valley 20 km away at sunset is the cone-traced pyramid's job. The alternative
from `M7.md` §6, cascaded sun-view splat depth maps, is the fallback if L3 shows cone tracing is too slow at grazing
sun angles.

**Clouds and volumes.**
- A **cloud shadow map** covers ±50 km around the camera in sun view: the optical depth of cloud blobs along the sun
  direction, analytic per blob (§4.7). It is updated every few frames.
- Smoke and dust shadows come from the froxel volume's transmittance toward the sun (§4.9).

**Eclipses** come from the analytic factor (§4.2), not from shadow rays.

### 4.5 AO, sky light and sky visibility

**Small-scale AO.**
- Four field samples along the normal, at 0.12, 0.48, 1.08 and 1.92 m, using the all-blob channel.
- Each sample fades out below the hit level's accuracy (1/16–1/8 of a cell). So a coarse level loses the
  short-range AO it can't resolve rather than inventing it. This is `M7.md`'s `aoScale 2`; scaling distances with
  the level was tried and darkened far levels.
- No nearest-blob term, so there are no list seams.
- It runs at half resolution with the shadows.

**Sky light.**
- Each frame the sky-view LUT (§4.6) is projected to L2 spherical harmonics. That gives sky irradiance for any
  normal: blue from above, warm near the horizon at dusk, dark at night.
- The ground's upward bounce is approximated per frame from the sun's illuminance and an average ground albedo
  around the camera.

**Sky visibility: large-scale occlusion.** Small-scale AO can't make a cave dark or a canyon floor dimmer than a
plateau. So:
- 4–6 cones per half-resolution block over the hemisphere around the normal.
- They are cone-traced on the pyramid's ancestor bricks out to ~100 m–1 km, depending on level.
- The result is the visible fraction of the sky, plus a bent normal.
- This is the first step of the GI roadmap (§4.13). **Caves need it.** Without it, a cave interior gets the full sky
  ambient through a 2 m AO and glows.

### 4.6 Atmosphere

**Hillaire 2020** ("A Scalable and Production Ready Sky and Atmosphere Rendering Technique", CGF 39(4)). It needs no
4D tables, has few edge cases, works from the ground to space, and has reference code. Its LUTs:

| LUT | size (paper's defaults, as commonly implemented) | updated |
|---|---|---|
| transmittance | 256 × 64 | when atmosphere parameters change |
| multiple scattering | 32 × 32 | when parameters change |
| sky-view | 192 × 108 | every frame (camera height, sun) |
| aerial perspective | 32 × 32 × 32 froxels | every frame |

Hillaire reports sub-millisecond total cost on 2020 hardware. The estimate here is 0.3–0.6 ms per frame, measured
in L1.

**The air's density is [04](04-bodies-and-orbits.md)'s model** (R8). 04 owns it, and drag (11) and scattering
(here) both use it, so looks and drag always agree:
`ρ(h) = ρ0 · (e^(−h/H) − e^(−h_top/H)) / (1 − e^(−h_top/H))`, tapered to zero at `h_top`.
- The Rayleigh density profile is `ρ(h)/ρ0`, with 04's `H` and `h_top`.
- The atmosphere's top radius is `R_ref + h_top`, which is also 04's switch radius.
- Mie (aerosol) and ozone are *optical* profiles layered on that air, owned here. They use the same taper, so
  nothing scatters above `h_top`.

**Per-planet optical parameters,** from [14](14-planet-generation.md) (via 03's `PlanetSpec.atmosphere`):
- Rayleigh scattering β_R at ρ0;
- Mie scattering and absorption, Mie scale height and phase `g`;
- the ozone (absorption) layer's coefficients and profile;
- ground albedo.

Earth's values, as used in the reference implementation: β_R = (5.802, 13.558, 33.1)·1e-6 m⁻¹ with H_R = 8 km; Mie
3.996e-6 scattering and 4.40e-6 absorption with H = 1.2 km and g = 0.8; ozone (0.650, 1.881, 0.085)·1e-6 m⁻¹ in a
tent centred at 25 km, 30 km wide. The ozone absorption is what makes the blue hour blue. Here the Rayleigh height
is 04's `H` (Earth-like: 8.5 km), not the reference's 8 km.

**Small planets.**
- Scale height is `RT/(Mg)`, not a function of radius. A 500 km planet with Earth-like surface gravity therefore
  keeps 04's Earth-like `H` = 8.5 km and `h_top` ≈ 100–130 km. Its atmosphere is about a fifth of its radius thick,
  a visible glowing shell from orbit. KSP's Kerbin, 600 km in radius with a 70 km atmosphere, reads fine.
- The horizon is close: ~32 km from 1 km up, 1.4 km from eye height. At Earth's scattering, 30 km of near-ground air
  still has a blue optical depth of ~1, enough for blue distant ridges.
- If tests show the distance haze too weak for the shorter sightlines, β can be raised per planet (×1.5–2). That is
  a generation parameter, not a renderer hack.

**Aerial perspective beyond the volume.** Mountain views on these planets reach 30–100+ km, beyond the
froxel volume's ~32 km.
- Pixels beyond the volume's far slice march the remaining segment per pixel with 4–8 samples against the
  transmittance and multiple-scattering LUTs.
- This is cheap, because those pixels are splats or sky, and it is accurate where the awe comes from (L2).

**From space.**
- Outside the top radius, the sky-view LUT doesn't apply.
- Each pixel that crosses the shell marches it directly (16–32 samples, low resolution, upsampled), using the
  shared transmittance and multiple-scattering LUTs.
- This draws the limb glow, the reddened terminator band, and the planet seen through its own air.
- Other planets with atmospheres, when larger than a few pixels, get the same per-pixel shell march with their
  own parameters.

**Aerosols from the simulation.**
- A planet-wide aerosol optical depth, `std:aerosol` (a coarse field from climate, raised by eruptions, wildfire
  smoke and impact dust), multiplies Mie density.
- The transmittance and multiple-scattering LUTs are rebuilt when it changes by more than a few percent (sub-ms,
  every few seconds at most).
- So a big eruption gives **vivid sunsets for months**, as Krakatoa did, and an impact winter dims the sun. These
  are rare-event visuals that cost nothing extra ([17](17-events-and-discovery.md)).
- *Local* smoke and dust are volume blobs (§4.9), not atmosphere parameters.

### 4.7 Clouds

**Proposal: clouds are blobs.** They are volume blobs (a density, not a surface; R12). The volume flag sits in
spare bits of geometry word 7, and the density is an f16 in the spare half of appearance word 1
([05](05-blob-model.md)). They are owned by a clouds plugin, with detail added by noise in the renderer.

**Where cloud blobs come from** (the simulation side is owned by [10](10-simulation-and-scheduling.md) and the
plugin):
- On activated planets, climate runs at `ℓ_sim_floor` (ℓ14, ~12.3 km cells, R5, R9). It produces per-cell cloud
  fields: cover, base, top, type (stratiform or convective), water or ice.
- A planet that is only seen from afar is not simulated. Its cloud fields come from the climate state
  [14](14-planet-generation.md) generates with its top level on entering the system.
- The clouds plugin lifts them into cloud blobs the way vegetation lifts tree counts into trees: ranked candidate
  sites per cell, so the same fields always give the same clouds.
  - A few large blobs per top cell (levels 12–13, kilometres across): stratus decks and anvils.
  - Finer blobs near observers: cumulus towers, wisps.
- Cloud blobs move with the wind as airborne parcels (`PLUGINS.md` §5 proposed exactly this) and grow and decay with
  their cell's fields.
- They don't persist: they regenerate from fields.
- **A storm is therefore present at a coarse scale everywhere it exists, always.** That is what lets the player see
  it 100 km away (scenario 2). A 12 km tall cumulonimbus on a 500 km planet clears the horizon out to
  √(2·R·h) ≈ 110 km.

**Density.**
- Each blob has a smooth density profile inside its ellipsoid (Gaussian-like in the ellipsoid's normalised radius).
- Density is the sum over blobs, eroded by tiling 3D Perlin–Worley noise.
- The noise's scale follows the blob's level: big blobs get big billows, small blobs fine wisps. So LOD is the same
  hierarchy as everything else, and a far cloud is its coarse blobs with coarse noise.

**Rendering, in three ranges:**
1. **Near and mid** (inside ~30 km):
   - A ray march at quarter resolution through the cloud blobs in each screen tile's list (froxel-style binning of
     cloud bounds, as M6 did for blobs).
   - The light toward each sun comes from a short march (4–6 samples) through a coarser density.
   - Beer–Powder for the dark edges of thick clouds.
   - Multiple scattering by Wrenninge's octave approximation.
   - Ambient from the sky SH, darker at the cloud's base.
   - Aerial perspective is applied between the camera and the cloud sample.
2. **Far** (beyond ~30 km, up to the horizon): a panoramic cloud map around the camera, refreshed a slice per frame
   (time-sliced). Far clouds change slowly, so a few seconds of refresh is invisible.
3. **From orbit and high above:** cloud blobs drawn as **translucent Gaussian splats**. The optical depth through a
   Gaussian ellipsoid has a closed form, so they cost a quad each, composited per tile in depth order. They are
   cheap enough for a whole cloudy hemisphere.

**Temporal policy.** Clouds are where temporal accumulation is allowed now ([12](12-rendering.md) §4.12): a quarter of
the pixels per frame (checkerboard) with reprojection by the cloud blobs' own wind motion. They are soft and slow,
so ghosting doesn't show. The stateless path (every pixel, every frame) remains for reference images.

**Cloud shadows** are the map from §4.4. Analytic per blob, so it is cheap.

**Lightning** is a flash light (§4.10), plus the bolt as a short chain of thin emissive blobs, plus light injected
into the cloud blobs near it, so the cloud glows from inside.

**Alternative considered: classic shell clouds** (a 2D weather map from climate fields plus 3D noise in an altitude
shell, as in Horizon's Nubis).
- For: proven quality.
- Against: they are not blobs. They need a separate representation to fly through (Nubis³ moved to voxel clouds
  for that). They give no natural far-field LOD or orbit view. They are a second system next to the fast layer the
  simulation already has.
- Blob clouds keep pillar 3 and reuse the hierarchy. The risk is the look of wispy edges, which L4 must settle
  (Q13.3).

### 4.8 Water

**Surface lighting.**
- Schlick Fresnel, GGX with roughness from waves.
- Far water is prefiltered: normal toward local up and roughness 0.06 → 0.22 over ~20–200 m at 1080p (`M7.md`).
- Sun glitter: wave normal variance is folded into the roughness, so it sparkles near and sheens far.

**Reflection.**
- The sky comes from the sky-view LUT, which is cheap and always present.
- The world comes from **reflection rays** traced through the pyramid (any-origin ray queries). They run at half
  resolution, only where the water is smooth (roughness < ~0.15) and within ~1–2 km, with the cone widened by the
  roughness.
- Mountains then appear upside down in a still lake at dawn: a classic awe shot, and a direct use of constraint 4.
- Elsewhere, sky reflection times a specular-occlusion term from sky visibility.

**Below the surface.**
- The thickness `t_ground − t_water` ([12](12-rendering.md) §4.9) drives Beer–Lambert absorption per water type:
  - clear sea: blue-green;
  - lake: green;
  - glacial: turquoise;
  - muddy river: brown, from hydrology's `std:sediment` field, so a flooding river turns brown (scenario 2).
- Refraction is approximated by offsetting the thickness lookup along the refracted ray.

**Later:** caustics, a true underwater view (fog by absorption, Snell's window), wakes and foam from fast-layer
water parcels.

### 4.9 Volumetrics near the camera

[12](12-rendering.md) §4.10's froxel volume, lit here:
- **Media:**
  - **height fog** from humidity and temperature fields: valley fog at dawn, seen from a mountain top;
  - **volume blobs**: smoke, ash, dust, powder snow, rain shafts, steam over lava;
  - optionally a thin global haze.
- **Injection.** Per froxel: density, albedo, and emission (fire, glowing ash). The in-scattered light uses each
  sun's shadow (one coarse shadow sample per froxel slice, or the cloud shadow map), sky SH and the nearest
  emissive lights.
- **Integration.** Front to back along each froxel column. Applied per pixel as transmittance plus in-scatter.
  God rays through trees and smoke come free.
- **Range:** ~1–2 km, log-depth slices. Beyond that, aerial perspective (§4.6) and the cloud passes take over.

### 4.10 Many lights: emissive blobs

Light comes from lava, fire, lightning, glowing creatures, the player's torch, meteors and the asteroid.

**Emission from temperature.**
- Lava parcels carry a temperature (`PLUGINS.md` §4: 1150 °C at the vent, cooling through a crust).
- A fire's flames are volume blobs with a temperature.
- Their emission is `blackbody(T) × emissivity × (1 − crust)`, with T from `std:emission_temperature`, using the
  **same table as the stars**. One law covers a sun, a lava channel and a campfire, and cooling lava dims and
  reddens by itself.

**Aggregation by the hierarchy.**
- Emitted power is `std:emitted_power`, a stored field with sum aggregation ([09](09-fields-and-composition.md)), so
  every coarse blob carries the light of its descendants.
- A lava flow 5 km away is a handful of coarse lights; up close it is hundreds of parcels. The same LOD as the
  geometry, with no special light clustering.

**Per frame.**
- The core gathers emitters from the render tier and picks the top ~256 by estimated contribution at the camera.
- They are binned into froxels (16 px tiles × 32 log slices, M6's binning) and shaded from per-froxel lists, also
  injecting into the volume (§4.9).
- The brightest 2–4 get short shadow rays (≤ 20 m); the rest are unshadowed. Emitters beyond the top 256 contribute
  only through their own emissive glow and bloom.
- Far emitters (a lava flow seen from orbit, lightning in a distant storm at night) are drawn as emissive surfaces.
  They light their surroundings only via the coarse lights that made the cut.

**Flashes.** Lightning, the impact and an eruption's first blast are lights with a short envelope (≤ 200 ms) and a
huge intensity. The exposure system (§4.12) gets a "flash" hint, so the flash blinds and recovers rather than being
averaged away.

### 4.11 The night sky

- **Stars are real.**
  - Point sprites from [03](03-universe-and-identity.md)'s star catalogue for the camera's position: positions,
    apparent magnitudes, colours from temperature.
  - The visible set is everything brighter than magnitude ~6.5. On Earth that is ~9,000 stars, mostly within a few
    hundred light-years plus luminous giants farther out.
  - **Requirement on 03:** a query "stars brighter than apparent magnitude m from point P". Bright stars are seen
    from far, so it can't be a fixed-radius query.
  - Constellations are therefore stable for a planet and differ between planets, and every star you see is one you
    can fly to ([18](18-player-camera-and-navigation.md)).
- **The galaxy band.**
  - The integrated light of unresolved stars, with dust lanes, from the galaxy density model.
  - A cube map ([12](12-rendering.md) §4.8) re-baked when the camera's galactic position changes enough, which is
    never during surface play.
  - Other galaxies are faint smudges.
- **Planets and moons:** other planets of the system as bright "wandering stars" with phases; moons with phases and
  planetshine on their dark side (§4.2).
- **Through the atmosphere.**
  - Stars and the band are attenuated by the transmittance LUT, so they fade toward the horizon and vanish at dusk
    in order of brightness, as the exposure adapts.
  - Twinkling is a small scintillation for points near the horizon. It is cosmetic.
- **Later:**
  - aurora (a rare event from stellar activity, [17](17-events-and-discovery.md)): an emissive volume in the upper
    atmosphere, drawn by a shell march;
  - airglow;
  - zodiacal light;
  - meteor showers as brief emissive streaks.

### 4.12 Exposure and tonemapping

**Auto exposure.**
- A log-luminance histogram (compute, 64 bins) of the frame, with the brightest and darkest few percent excluded.
- Adaptation is asymmetric: ~0.5 s toward bright, ~3–5 s toward dark, game-tuned.
- Each context clamps the target EV100 (Q13.1):

  | context | EV100 range (proposal) |
  |---|---|
  | day | 9–16 |
  | twilight | 3–9 |
  | moonlit night | −3 to 3 |
  | moonless night | floor at ~−5, so it stays dark rather than turning grey |
  | caves | down to −6 |
  | space | sun-lit, ~12–15 |

- Pre-exposure needs the value before rendering. It is the previous frame's adapted value, a scalar, not pixel
  reuse. A flash hint (§4.10) lets the exposure jump.

**Map views** ([18](18-player-camera-and-navigation.md): galaxy and system browsers) use a **map exposure** instead:
- stars and planets are brightened by a perceptual curve, so all are visible and comparable;
- the physical exposure would make every star but the nearest invisible next to a lit planet.

**Night vision.** A Purkinje shift at low exposure: a gradual desaturation toward blue, as human scotopic vision. It
is what makes moonlight read as moonlight rather than as dim daylight.

**White balance.** Under an alien sun, the image is partially adapted to the sun's colour (proposal: 70%, von Kries
in LMS). A red-dwarf world looks warm but not monochrome orange (Q13.5).

**Tonemapper.**
- **AgX** is the proposed default, with a "punchy" look option. The prototypes used the ACES fit, which shifts
  saturated bright colours (sunsets, lava, bioluminescence) toward oversaturated, hue-skewed results. AgX handles
  exactly those colours gracefully, and those are this game's money shots.
- Khronos PBR Neutral is the alternative if colours must stay closer to albedo.
- L6 shows Joel side by side (Q13.2).

**Bloom.** A mip-chain convolution of the HDR image, energy-conserving and small, for suns, lava and lightning.

### 4.13 GI roadmap

Constraint 5 asks for "eventually more full radiosity with multiple light sources". Each step below is useful alone,
and each lists what it needs from the world-space structure ([12](12-rendering.md)).

| step | what | needs from the structure | when | cost (estimate) |
|---|---|---|---|---|
| 0 | direct sun and moon light with shadows; sky SH × AO; ground-bounce term; emissive lights unshadowed or short-shadowed | rays from surface points; per-level accuracy for AO fade | M4 | in the base budget |
| 1 | **sky visibility** cones (§4.5): caves dark, canyons dim, overhangs shaded; specular occlusion for water | resident ancestor bricks for cone tracing; **the ground closed for rays at every level** (no see-through shells): plates with keels over [14](14-planet-generation.md)'s implicit solid, caves as 3D edit terms adding sheets per column (R6, pending C13) | M5, before caves exist | 0.5–1 ms, half resolution |
| 2 | **irradiance probes** (DDGI, Majercik et al. 2019): world-space probe cascades in the focus frame around the camera; each probe traces 64–128 rays per update (a quarter of probes per frame); hits shaded from brick albedo, direct light and the previous probe state (multi-bounce); Chebyshev visibility against leaks | **rays from arbitrary origins** (probes float in air and in caves): pyramid traversal from any point; **hit attributes** (normal, albedo from the brick's appearance texels, emissive from the material); cone footprints on far rays; sky as the miss | after M6 | 1–2 ms |
| 3 | world-space radiance cascades (e.g. Freeman & Sannikov's *Split Radiance Cascades*, arXiv 2607.20384, July 2026: sparse probes in a hashmap), or ReSTIR-style GI; glossy GI via reflection rays | the same, plus throughput | later | to be evaluated |

**The temporal question.** Probes are temporal by nature: they accumulate with hysteresis. But they accumulate in
*world space*, not by reprojecting screen pixels. So moving objects can't ghost in the image; the only effect is that
lighting responds over ~0.1–0.5 s. This should be compatible with Joel's concern, and is raised with Q12.2.

**Isotropy risk.** A regular probe grid can print its lattice through light leaks. So probe positions are jittered
per cell and relocated out of geometry, and visibility weighting is used, as DDGI does.

### 4.14 Level consistency of lighting

[12](12-rendering.md) §4.13's checklist, with the lighting items owned here:
- AO samples fade below the level's accuracy. AO is never computed at the level's own scale (`M7.md`'s
  `aoScale 1` was tried and rejected).
- Secondary rays start at least one level footprint off the surface, and from the level's blended surface, not
  from inside a splat (`M7.md` §2).
- Sky visibility is cone-traced from the pyramid at a footprint set by the cone, not by the receiver's render
  level. So it is the same whether the receiver is drawn fine or coarse.
- Probes (step 2) trace at their own footprint, independent of the camera's LOD choices.
- Coverage-capped aggregates are a requirement on [06](06-blob-hierarchy-and-cut.md): merged tree clusters must not
  become ground-covering plates that shadow the ground.
- **The standing test** ([19](19-tooling-and-testing.md)): forced-level renders of forest, meadow, lake shore and
  snow patches at levels 1–5, with full lighting. Mean luminance must be within ±10% of the finest level, and the
  60-pixel block error must not regress. `M7.md` reached −15% to +8%, so the target tightens slightly.

### 4.15 Weather visuals

| weather | visual | reads from |
|---|---|---|
| rain | camera-local streak particles (GPU only, cosmetic, allowed by idea 3), slanted by wind; splashes; rain shafts under clouds as volume blobs | precipitation and wind fields |
| wet ground | the wetness overlay darkens albedo and lowers roughness; puddles where the brick normal is near up and wetness exceeds a threshold | `std:wetness` ([09](09-fields-and-composition.md)) |
| snow falling | particles; reduced visibility via froxel density | precipitation, temperature |
| snow cover | per pixel, by the stability law: snowline plus a stable depth from the pixel's slope (`PLUGINS.md` §3) | `std:snow_cover` |
| fog | froxel height fog; valley fog from inversions at dawn | humidity, temperature |
| dust, sand, ash | volume blobs; aerosol AOD for the sun's colour | plugin blobs; aerosol field |
| wind | rasterised grass sways (vertex offset in the detail raster, from a wind field texture); cloud motion | wind field |
| lightning | flash light, bolt blobs, cloud injection (§4.7, §4.10) | climate events |

Trees swaying would need bones on static world blobs, which means re-baking. That is out of scope: trees stand still
until [16](16-entities-and-creatures.md) makes some of them kinematic.

### 4.16 Budget

These are the lighting items inside [12](12-rendering.md) §4.16's 16.6 ms (1080p, M1 Max class, estimates):

| item | ms | notes |
|---|---:|---|
| atmosphere LUTs (sky-view + aerial perspective per frame; others on change) | 0.3 | L1 |
| sun shadows + AO (half resolution, 1 sun) | 2.0 | part of 12's "secondary" 2.5 |
| sky visibility (from M5) | 0.5 | the rest of "secondary"; L5 |
| lights, aerial perspective, water, materials in shade | part of 12's 2.0 | |
| froxel volumes | 0.5 | |
| clouds (quarter resolution, temporal) + cloud shadow map | 1.2 | L4 |
| exposure histogram, bloom, tonemap | 0.5 | |
| *later:* GI probes | 1–2 | needs budget from upscaling ([12](12-rendering.md) §4.12) or lower resolution |

Each extra sun with full shadows costs ~1.5 ms. A second sun drops to quarter resolution unless the budget allows
more.

### 4.17 Alternatives considered

| topic | chosen | rejected, and why |
|---|---|---|
| sky model | Hillaire 2020 | Bruneton 2008 4D LUTs: more memory and edge cases, slow to update for changing aerosols. Analytic models (Preetham, Hosek–Wilkie): ground only, no space view, no other planets |
| far shadows | cone tracing on ancestor bricks | cascaded shadow maps of splats: kept as the fallback. Shadow maps of blobs at full resolution: faint and dithered for grass, 6–34 ms (`BLOBMAP2.md`) |
| clouds | volume blobs + noise, three ranges | shell clouds (§4.7): proven but a parallel system |
| local volumes | froxel volume (one technique for fog, smoke, dust, god rays) | per-effect ray marching: N techniques, N costs |
| many lights | hierarchy-aggregated emitters + froxel lists | deferred light volumes (too many overlaps); ignoring emitters (lava must light the night) |
| GI | the staged roadmap | screen-space GI (misses off-screen light, the reason for constraint 4); baked lightmaps (the world changes) |
| tonemapper | AgX | ACES fit (hue skews on saturated highlights); plain Reinhard (washed out) |

### 4.18 Canonical scenarios

1. **Orbit to grass.**
   - From space: a black sky, the star field and the galaxy band; the planet lit by its sun with its limb glowing;
     a red band along the terminator (shell march per pixel); clouds as Gaussian splats.
   - On the descent, at the top radius, the sky-view LUT takes over without a seam (L1's test). Clouds switch from
     splats to the marched pass. The exposure slides from space to day.
   - On the ridge: aerial perspective beyond 32 km is marched per pixel, so the far ridges are blue.
2. **The storm and the flood.**
   - At dusk, a sunlit cumulonimbus 100 km away stands over the horizon in the panoramic cloud map, its anvil
     reddened by the transmittance.
   - At night, its lightning flashes light it from inside.
   - Then rain: wet ground, puddles, and a river turning brown as sediment rises.
3. **The beaver dam.** Low sun through the trees (froxel god rays); the pond reflects the dam and the far ridge
   (reflection rays).
4. **The asteroid.**
   - A streak of emissive blobs, then the flash (an exposure jump, bloom).
   - Fires and lava-like ejecta are emitters aggregated by the hierarchy.
   - The dust plume is volume blobs. The aerosol field rises, and the planet's sunsets glow for months.
5. **Dig, leave, return.** Sky-visibility caches on the dug nodes are invalidated by node versions. A deep hole is
   correctly dark at the bottom (step 1).
6. **A community plugin.** The hut's "lantern" material is emissive with a temperature, so it lights the night via
   the emitter list, with no shader code.

**Eclipse** (a rare event, [17](17-events-and-discovery.md)): the per-pixel eclipse factor draws the umbra's edge
crossing the landscape. The sky darkens through the atmosphere's sun input, stars appear as the exposure adapts, and
a corona surrounds the dark disc.

## 5. Interfaces

These are core-side functions (C ABI), called when the frame record is filled ([12](12-rendering.md) §5). Handles
are `u32` per D6.

The `lt_*` names are **internal engine interfaces** between the core and its renderer and lighting modules. They are
not part of the plugin ABI, which is `ss_*` only (R2, [08](08-plugin-system.md) §5.2). Plugins affect lighting only
through fields, materials and blobs.

```c
// ---- lights from bodies (also used by the sim, on the CPU, at tick times) -------------------------
typedef struct {
  u32 kind;            // SUN, MOON (reflected), PLANETSHINE, POINT, SPHERE, FLASH
  f32 dir[3];          // focus frame, unit (directional kinds); or camera-relative position (point kinds)
  f32 color[3];        // linear Rec.709 chromaticity, luminance 1
  f32 illuminance;     // lux at the focus body (directional) or luminous power (point)
  f32 ang_radius;      // radians (directional) or radius in m (sphere)
  f32 eclipse;         // visible fraction at the focus body's centre; per-pixel factor uses occluders below
  u32 shadow_res;      // FULL, HALF, QUARTER, NONE
  u32 body_key[4];     // the emitting body's 128-bit key (R3), if any
} Light;

u32  lt_light_list(u32 focus_body, i64 tick, f32 alpha, Light* out, u32 max);
// occluders near any sun's direction, for the per-pixel eclipse factor: centre (camera-relative), radius
u32  lt_eclipse_occluders(u32 focus_body, i64 tick, f32 alpha, f32* out /* n × 4 */, u32 max);

// ---- atmosphere (parameters generated by [14], aerosol from a climate field) -----------------------
typedef struct {
  f32 r_bottom, r_top;  // R_ref and R_ref + h_top: [04]'s density model and switch radius (R8)
  f32 rayleigh_scatter[3], rayleigh_h;   // β_R at ρ0; H from [04]; the profile is [04]'s tapered ρ(h)/ρ0
  f32 mie_scatter, mie_absorb, mie_h, mie_g;
  f32 ozone_absorb[3], ozone_centre, ozone_width;
  f32 ground_albedo[3];
  f32 aerosol_mult;    // ≥ 1, from std:aerosol
  u32 version;         // bumps when anything changes -> rebuild transmittance + multiple scattering LUTs
} Atmosphere;
bool lt_atmosphere(u32 body, Atmosphere* out);

// ---- emitters -------------------------------------------------------------------------------------
// the top-N emissive blobs of the render tier by estimated contribution at the camera; aggregated by the
// 'std:emitted_power' stored field (sum) so coarse blobs stand for their descendants
typedef struct { f32 pos[3]; f32 radius; f32 color[3]; f32 power; u32 blob; u32 flags; } Emitter;
u32  lt_emitters(const f32 cam_rel_origin[3], Emitter* out, u32 max);

// ---- stars for the night sky (from [03]) ------------------------------------------------------------
// stars brighter than mag_limit seen from the camera; positions as unit directions in the focus frame
u32  lt_visible_stars(f32 mag_limit, f32* dir_mag_temp /* n × 5 */, u32 max);
```

**Fields and blob flags this doc reads.** The names come from [09](09-fields-and-composition.md)'s seed list (R12).
Values reach the GPU in the per-blob modulation word (appearance words 2–3) or as emitter and atmosphere inputs.

| name | kind | aggregation | used for |
|---|---|---|---|
| `std:emission_temperature` | stored, per blob | mean (mass-weighted) | blackbody emission (lava, fire), the glow slot |
| `std:emitted_power` | stored, per blob | sum | emitter aggregation |
| `std:snow_cover`, `std:wetness`, `std:burnt` | per 09 | per 09 | appearance overlays (modulation word) |
| `std:sediment` | per 09 | per 09 | water turbidity |
| `std:aerosol` | per planet (coarse) | per 09 | Mie multiplier |
| cloud fields (`cover`, `base`, `top`, `type`) | stored, `ℓ_sim_floor` | mean | the clouds plugin's lift |
| volume flag | spare bits of geometry word 7 ([05](05-blob-model.md), R12) | — | clouds, smoke, dust, rain shafts |
| volume density | f16 in the spare half of appearance word 1 ([05](05-blob-model.md)) | — | the same |

## 6. Risks and unknowns

| risk | why it matters | mitigation |
|---|---|---|
| blob clouds don't look like clouds | wispy edges and cauliflower tops come from noise erosion details that shell clouds have tuned for a decade | L4 side by side with a shell reference; fall back to shell clouds fed by the same fields |
| cone-traced long shadows too slow at grazing sun | sunsets are exactly when long shadows matter | L3; cascaded splat shadow maps as the fallback |
| aerial perspective beyond 32 km | the per-pixel tail march costs where many far pixels exist (overview views) | 4–8 samples; quarter resolution with depth-aware upsampling |
| physical exposure range | nights too dark to play, or too bright to feel like night | Q13.1 clamps; Purkinje shift; emissive lights matter at night |
| GI probes leak or print a lattice | constraint 1 | jittered, relocated probes with visibility; step 1 alone already darkens caves |
| atmosphere generation can make ugly skies | random coefficients give mud-coloured skies | curated ranges in [14](14-planet-generation.md); Q13.4 |
| many suns multiply shadow cost | binaries are rare but stunning | quality tiers per sun |
| f16 overflow and banding | sun discs, flashes, deep night | pre-exposure; dithering before quantisation |

## 7. First experiments

| id | experiment | success criterion |
|---|---|---|
| L1 | **Hillaire atmosphere in WebGPU** at 500 km planet scale, from 2 m to 2000 km altitude, sun from noon to below the horizon | all LUTs ≤ 0.5 ms per frame at 1080p; no seam crossing the top radius; a blue hour from ozone; terminator band visible from orbit |
| L2 | **aerial perspective beyond 32 km**: per-pixel tail march vs a reference march with 256 samples, at 50 and 100 km | ≤ 2% luminance error; ≤ 0.3 ms at 1080p in a mountain-top view |
| L3 | **long shadows**: a ridge shading a valley at 3° sun elevation, cone tracing on ancestor bricks vs cascaded splat shadow maps vs a full-resolution reference march | matches the reference's shadow line within 2 px at 1 km; ≤ 2 ms total for shadows |
| L4 | **blob clouds**: a stub clouds plugin lifting blobs from a fake cover field; the quarter-resolution march with noise erosion; far Gaussian splats; a storm at 100 km | Joel judges the side-by-side with a shell-cloud reference; ≤ 1.5 ms with temporal accumulation; a readable storm at 100 km at dusk |
| L5 | **sky visibility**: cone tracing on the ray cut's bricks with ancestors, a cave and a canyon | cave interior ≤ 5% of outside luminance with the same AO; ≤ 1 ms at half resolution; passes the forced-level test |
| L6 | **tonemapper**: AgX vs ACES fit vs PBR Neutral on sunset, lava at night, forest noon, snow field and a bioluminescent pool | Joel picks |
| L7 | **forced-level lighting test**: M7's patch method with full lighting, levels 1–5 | mean luminance within ±10% of level 1 in forest, meadow, lake shore and snow |
| L8 | **many lights**: 10 k lava parcels aggregated to the top 256 emitters, froxel lists | ≤ 0.5 ms; no visible popping as emitters enter and leave the top set (fade by rank) |

## 8. Build order

| milestone | lighting work |
|---|---|
| M1 | blackbody table; star points with magnitudes and colours; map exposure; system bodies lit by their sun (analytic spheres); the light list from bodies |
| M2 | Hillaire atmosphere (L1) for the planet from orbit and from space; terminator; sun disc; exposure v1 and the tonemapper (L6); planet colours lit by sun + sky; Gaussian cloud splats from a stub |
| M4 | sun shadows (half resolution, cones on the pyramid), AO with fade, sky SH and ground bounce, aerial perspective with the tail march (L2), grass sun grid, water lighting v1 (sky reflection, absorption), histogram exposure |
| M5 | sky visibility (L5); froxel volumes (fog, smoke); weather overlays (wet, snow); the clouds plugin and cloud pass (L4); cloud shadows; reflection rays on water; far water prefilter; the forced-level lighting test (L7) |
| M6 | emitters and flashes (L8): lava, fire, lightning, the impact; volcanic aerosols; eclipses (solar and lunar); meteor streaks |
| later | GI probes (step 2); aurora; caustics and underwater; radiance cascades (step 3) |

## 9. Open questions

- **Q13.1 — How dark are nights?** Physically, a full moon gives ~0.2 lux and a moonless night ~0.002 lux.
  *Why:* it decides whether night is a mood, a challenge, or a reason to light fires and find bioluminescence.
  *Default:* physical light, with exposure floors so a moonlit night reads clearly (blue-shifted). Moonless nights
  are genuinely dark but starlit, and emissive things (lava, fire, glowing creatures) matter.
- **Q13.2 — Art direction of light** (merged into questions.md C17). Physically based light on blobby shapes (the stylisation comes from shapes and
  materials), or a more stylised, claymation-like grade with softer shadows and fill? Linked: the minimum penumbra
  softness, the tonemapper (AgX / ACES / Neutral), the fill light. *Why:* it sets every lighting default and the
  look of every screenshot. *Default:* physical transport, AgX, mild grading, a minimum sun penumbra slightly softer
  than physical. Decided after L6.
- **Q13.3 — Clouds as blobs?** Volume blobs from a clouds plugin, noise-eroded, drawn three ways (marched near,
  panoramic far, Gaussian splats from orbit), or classic shell clouds driven by the same fields? *Why:* pillar 3
  against a decade of tuned technique. Blob clouds also make clouds part of the simulation (they move as parcels).
  *Default:* blobs, gated by L4.
- **Q13.4 — Alien atmospheres** (merged into questions.md C17). Earth-like skies everywhere, or skies that vary per planet (salmon skies, green
  sunsets, thick hazy worlds) within curated ranges? *Why:* the discovery pillar against the risk of ugly skies.
  *Default:* varied within curated ranges, with Earth-like skies most common (Zipf again: strange skies are rare
  finds). [14](14-planet-generation.md) owns the ranges.
- **Q13.5 — White balance under alien suns** (merged into questions.md C17). Should the image partially adapt to the local sun's colour (a
  red-dwarf world warm but not monochrome)? *Why:* a 3000 K sun without adaptation turns everything orange; full
  adaptation hides that the sun is different. *Default:* 70% adaptation.
- **Q13.6 — GI timing** (merged into questions.md C13). Sky visibility (dark caves, dim canyons) at M5, and probe GI after M6. Does that fit? It
  also means **no caves before sky visibility exists**, because they would glow. *Why:* caves are a classic
  exploration reward, and the order of [14](14-planet-generation.md)'s cave work depends on this. *Default:* as
  stated.
