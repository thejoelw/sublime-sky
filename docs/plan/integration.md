# Integration record

The docs cite two kinds of shorthand: **planning ideas** ("idea 1" … "idea 8") and **resolutions** ("R1" … "R20").
This page defines both, and says which README decision (`D…`) or working assumption (`W…`) each became. The README's
§6 is the current statement. This page is the trail of how it got there.

How the docs were written (2026-10-01):
1. The README was written first, as the anchor.
2. The 19 docs were written in parallel from it, together with eight planning ideas that weren't yet in the README.
3. A full cross-doc audit followed. It found 19 conflicts.
4. The conflicts were settled as the resolutions below, and every doc was edited to follow them.

## Planning ideas

| idea | the idea | what became of it |
|---|---|---|
| 1 | **Two-tier cut.** The sim runs on a deterministic tier driven only by observers, bands, contacts and events. The renderer may refine further for display, and those blobs are never simulated or saved. | adopted: D7 (amended) |
| 2 | **Deterministic async.** Worker jobs are applied a fixed number of ticks after they are issued, and a late job stalls the engine. | adopted: D18 |
| 3 | **GPU results never feed sim state.** | adopted: D19 |
| 4 | **Plugins are (nearly) stateless.** Authoritative state lives in core fields, resources and entity tables. | adopted: D11 |
| 5 | **Geometric lift projection.** The core projects the owner's proposed children so that the restrict of the children reproduces the parent. | adopted for kind C content only: D21 |
| 6 | **The zoom animation hides refinement latency.** Camera transitions request the destination's detail ahead of time. | adopted in 06, 12 and 18 |
| 7 | **Self-organised criticality.** Loading, threshold release and cascades give power-law event sizes on their own. | adopted in 10 (watches) and 17 |
| 8 | **Interest management.** The cut's requesters are the future networking interest set. | adopted in 02, 06 and 10 |

## Resolutions

| R | resolution | now |
|---|---|---|
| R1 | **Tiers and latencies.** The tiers are the "sim tier" and the "render tier", and the blobs only the render tier has are "render-only blobs". Split latency `L_split` = 3 ticks (06). 10 owns the per-process async latency rule; climate and hydrology are `async(30)`. | D7, D18 |
| R2 | **ABI names.** The plugin-facing ABI uses the `ss_` prefix only, and [08](08-plugin-system.md) §5.2 is the canonical list (the owning docs give the semantics). Processes and fast-layer materials are manifest declarations. Engine-facing exports are `eng_*`. Internal renderer and lighting modules keep their own names. | 08 §5.2 |
| R3 | **Keys.** 128-bit H(parent key, semantic ordinal). The ground's ordinals are (S2 cell id, sheet). Saves are chunked by spatial region and sorted by a spatial key carried with each record, never by key order. Body keys are a separate key space. | D6 |
| R4 | **Index layers.** The core keeps one octree of regions per body (edge 2^(ℓ+3) m) for splitting, balance and render nodes. The ground uses an S2-style cube-sphere as its surface index. | W5 |
| R5 | **Levels.** Set by largest radius; the 500 km root is ℓ18; ground plates ℓ = 20 − d. `ℓ_sim_floor` = ℓ14, `ℓ_view_floor` = ℓ10, finest ground ℓ−1 by default. The observer ring is K = 8 blob sizes. 06 owns the table. | D-levels |
| R6 | **Ground.** A boundary representation: plates over an implicit solid, with keels. Caves and tunnels are 3D edit terms that add sheets. There are no interior blobs, and the ground plugin decides what is detached. | W2 |
| R7 | **Content kinds A–D and edits.** The kind decides the split protocol, the projection and the persisted form. At the store level, an edit is removals plus additions. | D15, D21 |
| R8 | **Fictitious forces.** None below the switch radius, so day length is free (working default ~1 h). The atmosphere density model is 04's. | W1 |
| R9 | **Observers.** Avatars and pending observers drive the sim; cameras only drive render requests. "Activated planet" is defined in 10. | D20 |
| R10 | **Entities.** Thin entity tables in the core, and model instances for creatures and the avatar's appearance. | W4 |
| R11 | **Objects.** Blobs of different objects never blend, and an object holds one or more instances. The owner is the type's definer; there is no `transfer_owner`. | D10, D11 |
| R12 | **GPU record and types.** 32 B of geometry plus 16 B of appearance; volume and path bits in word 7; the modulation word is appearance words 2–3. The `std:` appearance names come from 09. Four dynamics modes. Type = (plugin u16, type u16). 04's body mode `attached` became `resting`. | [05](05-blob-model.md) §4.11 |
| R13 | **Field writes.** Delta, transfer or owned, plus a `conserved` flag; the persistence keys come from 15. 10 owns the canonical order. | D17 |
| R14 | **Storage** happens in a dedicated persistence worker. | D23 |
| R15 | **GPU coordinates** are integer node keys with per-level camera cells; there is no rebasing. | D4 |
| R16 | **Hardware.** An M1 Max at 1080p60 is the design point; the minimum is 720p30 on an M1 base or integrated GPU. | W7 |
| R17 | **Shape terms and rivers.** Shape terms are phases of `std:ground_height`; rivers use `lift_conditioning`. | [09](09-fields-and-composition.md), [14](14-planet-generation.md) |
| R18 | **Ocean.** An analytic sea-level sphere plus dynamic water. | W8 |
| R19 | **Grass and pebbles** are render-tier detail generated from fields. | [05](05-blob-model.md), [16](16-entities-and-creatures.md) |
| R20 | **Plugin memory** has a 64 MB default cap; more is declared in the manifest. | [01](01-runtime-and-toolchain.md) |
