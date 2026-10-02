# Open questions for Joel

The 19 docs ask 134 questions. Many of them are the same question seen from different systems, so they are grouped
into clusters `C1`–`C23`. This page has three parts:
1. **The 20 that matter most**, grouped by what they block.
2. **The other clusters**, plus a few vision questions of mine that no doc owned.
3. **An index of every question**, by doc.

**How to answer.** Write under a question (`**Joel:** …`), or answer in chat by id ("C2: two clocks", "Q12.2: no TAA
ever"). Every question has a recommended default. "Default" is a perfectly good answer, and silence on a question
means the default stands for now. When you answer, the docs' status lines and the README's decision table get
updated.

## 1. The 20 that matter most

### Platform (blocks M0)

**1. Core language** (Q01.1, [01](01-runtime-and-toolchain.md))
- Zig, C++ or Rust?
- *Default:* **Zig**, with a pinned compiler and a vendored libm, conditional on the M0 spike (DWARF debugging in
  Chrome works, builds are fast). The fallback is C++ with clang and wasi-sdk, no Emscripten.
- Do you write Rust? If so, it deserves a fair look before we commit.

**2. Browsers, devices, minimum hardware** (C5: Q01.2, Q01.3, Q12.3, [01](01-runtime-and-toolchain.md),
[12](12-rendering.md))
- *Default:*
  - Tier 1: desktop Chrome/Edge and Safari 26+ on macOS. Tier 2: Firefox. No mobile.
  - Design point: an M1 Max at 1080p60. Minimum: an M1 base or a recent integrated GPU at 720p30.
- This sets every budget: frame time, brick memory (~0.45–0.6 GB; ~0.7–1.0 GB of GPU memory in all), blob budgets.

**3. Hosting** (Q01.4)
- Can production hosting set COOP/COEP headers? They are required for SharedArrayBuffer and threads.
- *Default:* a host with header control (Cloudflare Pages, Netlify `_headers`). Keep the non-isolated fallback
  working for embeds.

**4. Tick rate** (C15: Q02.1, Q10.6)
- *Default:* **30 Hz**, with 2 fixed physics substeps.
- This is the quantum of commands, physics and later the network. Changing it after content exists changes every
  result.

**5. Bit-identical results across browsers from M0?** (Q02.3, [02](02-numerics-time-and-determinism.md))
- *Default:* **yes**, for the core and the SDK. It is cheap if tested from the first commit, using headless V8, JSC
  and SpiderMonkey shells, and very expensive to retrofit.

### World scale and time (blocks M1)

**6. Planet size and gravity** (C1: Q03.2, Q14.1, Q04.2)
- What radius do you want to walk or fly around?
- *Default:*
  - planets of 250–1,000 km, with a 500 km reference and top-level cells of ~12 km;
  - gravity set per planet (GM = gR², dense planets as in KSP); habitable planets near 9.81 m/s², small moons
    floaty.
- Real interstellar distances cost nothing visible ([03](03-universe-and-identity.md) §4.9). Earth-sized planets
  would need ~100 km top cells and lose large-scale fidelity.

**7. Day, year, time scale, fictitious forces** (C2: Q02.2, Q03.9, Q04.1, Q04.4, Q10.1, Q11.1, Q17.3)

This is one decision seen from seven docs.
- **Fictitious forces.** With no centrifugal or Coriolis force below the switch radius (working assumption W1), any
  day longer than ~24 minutes works, even on a 500 km planet. Including them forces days of at least ~2 h there.
- **Clock options:**
  - (a) one clock at 1:1, a short planet day, and slow processes sped up through their own rate parameters;
  - (b) two clocks: physics at 1:1, the world's processes accelerated. This breaks consistency where they meet.
  - (c) player-controlled warp only.
- *Default:*
  - drop the fictitious forces;
  - (a) + (c): a **~1 real-hour day**, giving a golden sunset of a few minutes;
  - a year of a few tens of days, set by the orbit.
- **Why it matters beyond the sky.** At natural rates, a wandering player witnesses about one eruption per ~4,000
  hours of play (at the ~1 h day). The time scale and the event director (#20) together decide how often anyone sees something rare.

**8. The galaxy** (Q03.1, Q03.3, Q03.4)
- One visitable galaxy (other galaxies as decoration)?
- Static stars? Their motion would be invisible over game time anyway, and static stars make non-collision exact.
- Binary stars from M1? Two suns, two shadows, a double sunset.
- *Default:* yes to all three.

**9. One shared universe and clock** (C14: Q03.6, Q02.4, Q15.4)
- Does everyone get the same universe, with the same epoch, so that "an eruption here at this time" can be shared?
- Do edits stay in private saves?
- *Default:* one canonical seed per universe version; a shared epoch; private local saves with export. A hosted
  shared world is a later option using the same records.

### Architecture (blocks M2–M3)

**10. The two-tier cut** (C7: Q06.1, Q12.6, Q07.4, Q06.8, [06](06-blob-hierarchy-and-cut.md))
- The sim runs on a deterministic sim tier. The renderer may refine further, but render-only blobs are never
  simulated or saved.
- *What it costs:*
  - a distant region shows lifted detail and saved edits, never the live fine state the sim would compute there;
  - physics never touches render-only detail (a thrown object asks for sim refinement ahead of contact).
- *What it buys:* the sim doesn't depend on where any camera looks, which makes replays, tests and multiplayer
  possible.
- *Default:* yes.

**11. Spatial index and keys** (C8: Q06.3, Q14.7, Q05.6)
- Keys are frozen into the save format, so this is hard to change later.
- *Default:* working assumption W5, plus f64 CPU positions:
  - the core's per-body octree of regions for splitting, balance and render nodes;
  - the ground's S2-style cube-sphere as its surface index;
  - keys as 128-bit hashes of (parent key, semantic ordinal).

**12. Underground, caves and digging** (C13: Q14.3, Q06.6, Q13.6)
- *Default:*
  - working assumption W2: the ground is plates over an implicit solid, caves are 3D edit terms adding sheets, and
    there are no interior volume blobs;
  - sheets designed in from M3, caves built after M6 (they glow until sky visibility exists in M5);
  - digging unlimited by design, limited by budget in practice.
- How important is underground exploration to you?

**13. Renderer direction and temporal reuse** (Q12.1, Q12.2, [12](12-rendering.md))
- *Default:* working assumption W3, the hybrid renderer. It replaces M7's per-level camera windows, and M7's look is
  the bar to match.
- *Temporal reuse, by default:*
  - stateless frames are the baseline and the test reference;
  - motion vectors are written from day one;
  - temporal accumulation is used for clouds and volumes only;
  - TAA and upscaling come later, behind a toggle.
- You were wary of temporal reuse in the prototypes. Is that policy acceptable?

**14. Content kinds and projection** (C21: Q06.4, D21)
- Exact geometric projection for continuous media (ground, water, snow; shaped objects); never moving a tree or a
  placed log because someone looked at it.
- *Default:* yes.

**15. Plugin trust, isolation and shaders** (C16: Q08.1, Q08.2; C4: Q05.1, Q08.7, Q12.5)
- *Default:*
  - community plugins are **untrusted by default** (isolated, fuel-metered, validated); first-party and local
    plugins can be marked trusted;
  - **isolated instances** are the default, and dynamic linking is dropped unless measurements demand it
    ([08](08-plugin-system.md) answers your dynamic-linking question at the top);
  - **no plugin GPU code in v1**: materials are data, and a data-defined material graph can come after M6.

**16. Field phases** (Q09.1, Q09.2, [09](09-fields-and-composition.md))
- Phases declared by the field's definer, as the formal version of your `@checkpoint`s
  ([09](09-fields-and-composition.md) compares the two notations side by side).
- Stored fields change only by deltas, transfers, owned sets and services.
- *Default:* yes to both.

**17. Entities and model instances** (C10: Q16.1, Q16.7, [16](16-entities-and-creatures.md))
- Working assumption W4: thin entity tables in the core, and creatures and the avatar drawn as model instances
  (shared assets posed by bones) outside the blob store.
- *Default:* yes. The alternative keeps the core free of the word "entity" and puts creature atoms in the store, at a
  real cost per tick.

**18. Generator versioning** (C9: Q03.8, Q15.1, Q08.6)
- Do old saves keep their old generators?
- *Default:* a save pins and embeds the plugin modules it was made with. Upgrading (rebasing) happens on request, or
  automatically only for plugins that declare it safe. The universe generator is frozen per major version.
- Should old universes stay visitable after an upgrade?

### Game design (shapes M4 onward)

**19. What can the player do?** (C3: Q18.1, Q18.3, Q18.4, Q11.3, Q16.2, Q17.4)
- *Default:*
  - **M4:** look, walk, run, jump, swim, carry and throw small things, and a journal camera;
  - **M5:** dig and plant; building later;
  - **creatures:** watch, follow, feed, name; no hunting;
  - **danger:** no death. Hazards push the player away, or make them faint to a safe spot. Big events may land near
    the player's things but never directly on touched structures.
- Climbing and gliding are strong exploration verbs to consider for M5.

**20. The event director and the observer effect** (C12: Q17.2, Q10.5)
- Working assumption W6: a bounded, deliberate observer effect. The director may move an event's timing within
  plugin-declared windows, and pick its place among 4 candidates. It never changes mass or energy, and acts only on
  notability class ≥ 2.
- *Default:* on, with tolerances for the unintended observer effect, checked per top-level cell:
  - conserved totals per planet within 0.5%;
  - slow-layer fields within 10% (p90 per cell);
  - fast-layer deposits within 35% (p90 per cell).

## 2. The other clusters

| id | question | default | members |
|---|---|---|---|
| C6 | **Time controls for the player.** How fast can warp go, and is skipping allowed? | explicit play up to 8×; warp up to ×600 on the surface (higher in orbit view) with simplified fast physics; skip any length; off by default in multiplayer | Q10.3, Q18.6 |
| C11 | **Observers and the orbit view.** Does browsing in orbit view simulate anything, and what detail does it show? | it simulates nothing (cameras only request render detail); the orbit view's cap is `ℓ_view_floor` = ℓ10 (~1 km blobs) | Q10.4, Q18.7 |
| C17 | **Look and art direction.** Physical light on blobby shapes, or a claymation grade? Alien skies? White balance under alien suns? | physical light with AgX and a mild grade; skies varied within curated ranges, Earth-like most common; 70% adaptation to the sun's colour | Q13.2, Q13.4, Q13.5 |
| C18 | **Species and theme.** Plausible stylised animals or invented creatures? The same species everywhere? | mostly plausible animals, stylised, a few invented; authored species from plugins with procedural variation per planet | Q16.4, Q16.5 |
| C19 | **Physical travel.** Can the player ever fly to orbit or between planets in something? | jump only (camera moves) until after M6; doc 04 keeps the door open | Q04.5, Q18.5 |
| C20 | **The ocean.** Analytic sea-level sphere, or all blobs? | W8: analytic sphere plus dynamic water at the shore and in floods | Q12.4 |
| C22 | **Size of the always-simulated level.** | `ℓ_sim_floor` = ℓ14 (24,576 cells of ~12 km on 500 km); `B_sim` a fixed game constant | Q06.5 |
| C23 | **Time away.** While the player is elsewhere, are activated planets and dormant creatures stepped in the background, or caught up on return? Do named animals age and die meanwhile? | background stepping for the ~8 most recently visited planets and lazy catch-up beyond; dormant creatures follow the same rule (15's `catch_up` and 16's `dormant_step` are the two halves and still need one owner); named animals do age, and the journal says what happened | Q10.2, Q15.6, Q16.6 |

**Vision questions no doc owned:**

| id | question | why it matters | my default |
|---|---|---|---|
| V1 | **Is there a goal or progression**, or is discovery its own reward? | if discoveries unlock anything, witnessing must become deterministic sim state (Q17.7) | the journal with rarity tiers; no unlocks at first |
| V2 | **What shape will multiplayer take**: a few friends in co-op (2–8), or many players in one world? | it picks between lockstep and state sync later, and how big an observer set the sim tier must handle | small co-op, host-authoritative |
| V3 | **Audio.** Should a doc 20 plan sound now? | thunder, rumbles and booms carry much of an event's impact (Q17.8) | yes, a short plan before M6; event records carry sound hints from M5 |
| V4 | **Is "Sublime Sky" the game's name**, or the repo's? | names in ids, links and the SDK (`ss_` prefix) | the working name |

## 3. Every question, by doc

Each line gives the question and, where it has one, its cluster. The full text, with its reasoning and default, is in
the doc's section 9.

<!-- index generated below -->

### [01. Runtime and toolchain](01-runtime-and-toolchain.md)

- **Q01.1** Core language: Zig, C++ or Rust? — top #1
- **Q01.2** Which browsers and devices must work at launch? — C5 (top #2)
- **Q01.3** Minimum hardware? — C5 (top #2)
- **Q01.4** Hosting. — top #3
- **Q01.5** Plugin memory cap.
- **Q01.6** Release builds.

### [02. Numerics, time and determinism](02-numerics-time-and-determinism.md)

- **Q02.1** Tick rate. — C15 (top #4)
- **Q02.2** Time scale and day length. — C2 (top #7)
- **Q02.3** Is cross-browser bit-identity required from M0, or only from the multiplayer work? — top #5
- **Q02.4** One clock per universe? — C14 (top #9)
- **Q02.5** Fixed-point for conserved quantities?
- **Q02.6** Replays.

### [03. Universe and identity](03-universe-and-identity.md)

- **Q03.1** One visitable galaxy, or many? — top #8
- **Q03.2** Planet size and the scale of systems. — C1 (top #6)
- **Q03.3** Should stars move in the galaxy? — top #8
- **Q03.4** Binaries and multiples from the start? — top #8
- **Q03.5** Names.
- **Q03.6** One shared universe? — C14 (top #9)
- **Q03.7** Where does a new player start?
- **Q03.8** Generator versioning. — C9 (top #18)
- **Q03.9** Year length. — C2 (top #7)
- **Q03.10** Is universe generation pluggable?

### [04. Bodies and orbits](04-bodies-and-orbits.md)

- **Q04.1** Fictitious forces at the surface. — C2 (top #7)
- **Q04.2** How is surface gravity set? — C1 (top #6)
- **Q04.3** Simplifications to confirm.
- **Q04.4** Day length, in real minutes or hours. — C2 (top #7)
- **Q04.5** Will the player ever travel physically? — C19
- **Q04.6** Gravity of irregular bodies.
- **Q04.7** Planned impacts.
- **Q04.8** Debris lifetime.

### [05. Blob model](05-blob-model.md)

- **Q05.1** Plugin shaders? — C4 (top #15)
- **Q05.2** A per-blob second colour?
- **Q05.3** Instance-scoped carves for modelling while world edits stay removals: is that within the spirit of D15?
- **Q05.4** GPU records of 32 + 16 bytes instead of `BLOBMAP3.md`'s single 32 bytes?
- **Q05.5** Should different instances ever blend with a small contact radius
- **Q05.6** f64 positions on the CPU (24 B) or f32 relative to region origins (12 B)? — C8 (top #11)

### [06. Blob hierarchy and the cut](06-blob-hierarchy-and-cut.md)

- **Q06.1** Adopt the two-tier cut — C7 (top #10)
- **Q06.2** Levels by largest radius
- **Q06.3** A 3D octree region tree per body in the body-fixed frame — C8 (top #11)
- **Q06.4** Projection by content kind — C21 (top #14)
- **Q06.5** The sim floor on activated planets — C22
- **Q06.6** How deep may players dig? — C13 (top #12)
- **Q06.7** The finest ground level
- **Q06.8** Physics against render-only detail is impossible by design. — C7 (top #10)

### [07. Spatial queries](07-spatial-queries.md)

- **Q07.1** Should plugins get GPU-backed cosmetic queries
- **Q07.2** Neighbour distance by default
- **Q07.3** Deterministic work caps per plugin per tick
- **Q07.4** Sim queries never see render-only detail. — C7 (top #10)
- **Q07.5** How many CPU field rays per tick should gameplay expect to afford

### [08. Plugin system](08-plugin-system.md)

- **Q08.1** What is the trust model for community plugins? — C16 (top #15)
- **Q08.2** Do you accept isolated instances (B) as the default, with static linking for trusted plugins and dynamic linking dropped unless measurements demand it? — C16 (top #15)
- **Q08.3** Which plugin languages should be supported officially at first?
- **Q08.4** Should the first-party gameplay (terrain, climate, hydrology, vegetation, fire, snow, volcano) be written as plugins with the public SDK and linked statically?
- **Q08.5** Is everything structural declared in the manifest, with no runtime registration?
- **Q08.6** Can a save change its plugin set (add, remove, upgrade)? — C9 (top #18)
- **Q08.7** Should plugins ever ship GPU code (WGSL materials, shape terms evaluated on the GPU)? — C4 (top #15)
- **Q08.8** Should a plugin's async job be allowed to pull in other plugins through extension points (hydrology + snow), or must cross-plugin calls in async jobs be forbidden to keep workers lean?

### [09. Fields and composition](09-fields-and-composition.md)

- **Q09.1** Do you approve phases (declared by the field's definer, each with a mode) as the formal version of `@checkpoint`s? — top #16
- **Q09.2** Should stored fields be changed only by deltas, owned sets and services, with no composition stages on stored fields? — top #16
- **Q09.3** Should the `std` vocabulary use SI everywhere (kelvin, m/s water equivalent for precipitation, m3 for volumes), with conversion only in the UI?
- **Q09.4** For contributors in an ordered phase with no `before` / `after`, should the loader warn and tie-break by plugin id, or refuse to load?
- **Q09.5** Who curates `std`, and how open is it?
- **Q09.6** Is a per-blob modulation word of 8 bytes (05's appearance words 2–3, R12) with 5 standard slots (wet, snow, char, glow, sediment) plus up to 3 custom slots enough for field-driven looks?
- **Q09.7** Do you accept the distinction between generator-input fields and live queries?

### [10. Simulation and scheduling](10-simulation-and-scheduling.md)

- **Q10.1** One clock, and how fast does the world move? — C2 (top #7)
- **Q10.2** Activated planets: background stepping or lazy catch-up? — C23
- **Q10.3** Time control for the player. — C6
- **Q10.4** Who is an observer? — C11
- **Q10.5** How much observer effect is acceptable? — C12 (top #20)
- **Q10.6** Tick length. — C15 (top #4)

### [11. Physics](11-physics.md)

- **Q11.1** Day length vs small planets. — C2 (top #7)
- **Q11.2** Settling.
- **Q11.3** What the player can do physically. — C3 (top #19)
- **Q11.4** Overhangs.
- **Q11.5** Two-way coupling of flows and objects.

### [12. Rendering](12-rendering.md)

- **Q12.1** Is the hybrid the renderer direction? — top #13
- **Q12.2** Temporal reuse. — top #13
- **Q12.3** Target hardware — C5 (top #2)
- **Q12.4** The ocean — C20
- **Q12.5** Plugin shaders — C4 (top #15)
- **Q12.6** The two-tier cut — C7 (top #10)

### [13. Lighting, atmosphere and sky](13-lighting-atmosphere-and-sky.md)

- **Q13.1** How dark are nights?
- **Q13.2** Art direction of light — C17
- **Q13.3** Clouds as blobs?
- **Q13.4** Alien atmospheres — C17
- **Q13.5** White balance under alien suns — C17
- **Q13.6** GI timing — C13 (top #12)

### [14. Planet generation](14-planet-generation.md)

- **Q14.1** How big is the reference planet, and its top cell? — C1 (top #6)
- **Q14.2** Should the ground provider be replaceable by community plugins
- **Q14.3** How important are caves and underground exploration? — C13 (top #12)
- **Q14.4** Realism vs drama in landforms.
- **Q14.5** Is a short wait acceptable on a first jump to a never-visited system?
- **Q14.6** Erosion-shaped fine detail.
- **Q14.7** Do the S2-style cube-sphere and its keys suit you — C8 (top #11)

### [15. Persistence](15-persistence.md)

- **Q15.1** Do old saves keep their old generators? — C9 (top #18)
- **Q15.2** Exact saves: is bit-exact resume worth the extra few MB per save
- **Q15.3** Should the player's changes fade?
- **Q15.4** Are worlds private saves, or one shared world? — C14 (top #9)
- **Q15.5** Autosave and slots.
- **Q15.6** Does every activated planet keep its full live top level in the save — C23

### [16. Entities and creatures](16-entities-and-creatures.md)

- **Q16.1** Should the core have thin entity tables (§4.1), or no entity concept at all? — C10 (top #17)
- **Q16.2** What can the player do with creatures? — C3 (top #19)
- **Q16.3** Creature authoring.
- **Q16.4** Which creatures? — C18
- **Q16.5** Same species everywhere, or per-planet ecologies? — C18
- **Q16.6** Do named animals age and die while the player is away? — C23
- **Q16.7** Is the two-residence split OK? — C10 (top #17)
- **Q16.8** How are death and injury shown?

### [17. Events and discovery](17-events-and-discovery.md)

- **Q17.1** The notability ladder.
- **Q17.2** The director. — C12 (top #20)
- **Q17.3** Game time scale. — C2 (top #7)
- **Q17.4** Can big events hurt what the player made? — C3 (top #19)
- **Q17.5** Notifications.
- **Q17.6** Forewarning realism.
- **Q17.7** Does discovery unlock anything?
- **Q17.8** Audio.
- **Q17.9** An almanac and sharing links.

### [18. Player, camera and navigation](18-player-camera-and-navigation.md)

- **Q18.1** What can the player do on the surface? — C3 (top #19)
- **Q18.2** What is the avatar?
- **Q18.3** Can the player be hurt, or die? — C3 (top #19)
- **Q18.4** Movement verbs. — C3 (top #19)
- **Q18.5** Is travel between bodies only a jump (a camera move), or ever physical (a ship)? — C19
- **Q18.6** Time warp for players. — C6
- **Q18.7** The orbit view's LOD floor. — C11
- **Q18.8** Where can the player embark?
- **Q18.9** System view scale.
- **Q18.10** Default view: first or third person?
- **Q18.11** Jumping from the ground.

### [19. Tooling and testing](19-tooling-and-testing.md)

- **Q19.1** Do GPU tests run only on your machine, or on a self-hosted Mac runner in CI?
- **Q19.2** Who else will contribute, and when?
- **Q19.3** Where do images live?
- **Q19.4** Mono-repo for the first-party plugins?
- **Q19.5** Licence for the core and the SDK?
- **Q19.6** Authoring before the editor exists
