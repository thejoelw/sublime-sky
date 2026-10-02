# 15. Persistence

Status: draft · Depends on: 02 (determinism, key minting, libm), 05 (keys), 06 (hierarchy, cut, restrict/lift),
08 (plugin ABI), 09 (field schemas), 10 (scheduler, catch-up), 14 (content kinds, ground) · Used by: 16, 17, 18, 19,
and later networking

## 1. Purpose and scope

This doc covers what state the game has, which of it must be kept, and how it is kept: across a region unloading,
across closing the tab, across plugin upgrades, and (later) across machines.

**In scope:**
- the source of truth;
- touched tracking and the halo;
- what is stored for each kind of content: field residuals, edit terms, candidate overrides and living sets,
  geometric residuals, placed content, plugin resources, bodies, the live top level, the live cut;
- decay, and the time model on return;
- the generator-version problem;
- the save format, storage backends, autosave and crash safety, export and import;
- the cache (which is not a save); size estimates;
- what multiplayer will need from all this.

**Out of scope** (interfaces stated here):
- the restrict, lift and projection operators themselves (06);
- the key format and how keys are minted (05, 02);
- what processes run while a region is unloaded, and catch-up scheduling (10);
- content kinds and terrain realisation (14);
- networking protocols (later).

## 2. Requirements

| | requirement | traced to |
|---|---|---|
| must | What the player (or a salient event) changed survives unloading and returning, at every level. | scenarios 3, 4, 5; D8 |
| must | Storage grows with edits, not with travel. Walking around the planet stores nothing. | `planet/DESIGN.md` §4 lesson 2 (593 records from 30 days of growth when everything was stored) |
| must | Restored state is consistent with the current coarse state: `R(children) = parent` after any restore, at any later time. | D8; residuals lie in the null space of `R` |
| must | Looking at a region does not change it more than the observer-effect budget allows (unload/return is not a source of drift). | pillar 5; `DESIGN.md` §2.8 |
| must | Save, quit, reload and continue gives exactly the same future as never quitting. | D5; replays and tests |
| must | Keys, never handles, on disk. | D6 |
| must | A crash or closed tab during a save never corrupts the previous save. | |
| must | Old saves keep loading after plugin updates, or say exactly why not. | pillar 4 (community plugins change) |
| should | Saves are small (MBs, not GBs) and autosave costs the engine under a millisecond per save. | |
| should | The far view of a revisited planet doesn't take minutes to build (a cache). | `BLOBMAP3.md` (two-minute cold start) |
| later | The same records serve as the network sync format; an authoritative store on a host. | constraint: multiplayer later |

## 3. What the prototypes taught

| lesson | source |
|---|---|
| Residual = children − a fresh lift of the current parent. It restricts to zero, so it can be stored, decayed and re-applied to any later parent without breaking consistency. Full unload to the top cell and back restores terrain to 1e-12 m. | `planet/DESIGN.md` §2.7, `world.js` `_storeResidual` / `_restoreResidual` |
| Store only under touched cells. Natural fine-scale drift is regenerated. A dug hole + 4 tree edits: 168–172 records. | `DESIGN.md` §2.7, §5 |
| Untouched neighbours of a touched cell must store too (the halo); otherwise they regenerate leaning into the hole (up to 0.5 m). | `DESIGN.md` §4 lesson 8 |
| Priors may only read persisted state. Tree counts lifted by capacity (which reads regenerated discharge) brought back the wrong number of trees. | `DESIGN.md` §2.3, lesson 8 |
| Aggregates of an edit go to the parent immediately and permanently (the removed volume). Only the *shape* is residual. Lake water stays in the coarse cell, and the bathtub lift puts it back in the lowest children. | `DESIGN.md` §2.7 |
| Decay per field: elevation τ = 400 yr, tree count residual τ = 150 yr. After 15 years away, a 3.7 m hole is 3.56 m deep, felled trees are gone, and the damaged tree still stands. | `DESIGN.md` §2.7, §5 |
| Discrete things are overrides on ranked candidates: felled = a stump blocking the site for 30 years; damaged = a worse rank; planted = a better rank. | `DESIGN.md` §2.6, `world.js` `overrides` |
| Structure must be restricted upward to survive unloading: a diversion is remembered through the touched coarse cells' `outlets`. A diversion that rejoins within one top cell is invisible to it, along with its ecological effect. | `DESIGN.md` §2.5, §5 |
| Edits as an ordered log of removals and additions; objects removed whole. Edits only stay local if merges are deterministic and canonically ordered (a 256 m-snapped origin, sorted cell lists). | `world/M7.md` §7 |
| Persist generated and merged nodes, or a cold far view takes two minutes. | `BLOBMAP3.md` §3, §6 |
| Stale arrays held by a plugin read garbage after the core reallocated. Plugins get accessors, not arrays. | `PLUGINS.md` §1.2 |
| Fast-forwarding loses fast-layer dynamics: skipped lava had to resolve itself (`onSkip`), and parcels must not survive as frozen heaps. | `PLUGINS.md` §2, §7 |

## 4. Design

### 4.1 Principles

1. **Leaves own state. Coarse levels are restrictions** (D8). The *live* state of a body is:
   - its always-simulated top level;
   - plus the state of its cut's leaves below that;
   - plus whatever the residual store holds for unloaded touched subtrees.
2. **An unload is lossy by design. A save is exact.**
   - When a region leaves the sim tier, only touched state (plus the halo) is kept, as residuals and records. Natural
     detail is regenerated next time.
   - When the game saves, it writes the live state *exactly*, so a reload continues bit-identically.
   - The two share one record vocabulary.
3. **Store only what can't be regenerated.** If a deterministic function of (seed, keys, persisted state) produces
   it, it is not saved. It may be cached (§4.16).
4. **Discrete edits are prior input. Continuous edits are residuals.**
   - A removed rock, a felled tree, a dug tunnel, a placed log: the owner's lift *reads* them (they are persisted
     state), so the prior already accounts for them.
   - A hole's depth profile, a lake's water distribution: stored as the difference from a fresh lift.
   - This keeps residuals sparse. It also avoids the projection redistributing a removed object's volume over its
     siblings (§4.5).
5. **Keys, never handles.** Every record is keyed by stable keys (05). Plugins store keys in resources, never
   handles.
6. **The core persists. Plugins don't.** Plugins are (nearly) stateless (D11, 08). Everything authoritative is a
   field, a resource, a placed record or a body, and all of these are core-managed. A plugin never writes to storage
   itself.

**Where this refines the README:**
- **D8** says persistence is residuals and edit logs on touched subtrees only. That stays true for *unloads*. A
  *save* additionally snapshots the live top level and the live cut exactly (§4.8), because residuals can't give
  bit-exact resume.
- **D15** holds at the blob-store level. Ground (kind A) edits persist as generator input (14 §4.8).

### 4.2 State inventory

| state | where it lives | regenerable? | on unload | in a save |
|---|---|---|---|---|
| universe, rails bodies' elements | generated from the seed (03, 04) | yes | — | no (only perturbed elements) |
| dynamic bodies (thrown rocks, ejecta in flight, the player, detached objects) | core body store | no | settle first (11) | full state |
| a body's t0 top level (generation + spin-up) | worker output | yes | — | no: **cache** |
| a body's live top level | core fields | no (evolved) | stays loaded while the body is simulated | **absolute, every stored field** |
| natural fine detail of untouched regions | lifted from parents | yes (statistically) | discarded | no |
| the live sim tier below the top level | core fields + blobs | no | residuals for touched + halo | **absolute snapshot** |
| render-only blobs (the render tier, R1) | renderer-driven blobs | yes | dropped any time | no |
| field residuals of unloaded touched families | residual store | no | written | yes |
| `z` edits, 3D edit terms (kind A) | terrain plugin resource + `z` residuals | no | kept | yes |
| candidate overrides, living sets (kind B) | owner plugin resource, by candidate key | no | kept | yes |
| geometric residuals, removals (kind C) | residual store / owner resource | no | written | yes |
| placed content (kind D) | core placed store | no | kept (record) | yes |
| placed content's merge pyramids | derived | yes | — | no |
| plugin resources (river outlets, a beaver colony, volcano pressure) | core resource store, by scope | no | kept | yes |
| computed fields (biome, flammability, discharge from routing) | derived per tick | yes | — | no |
| fast-layer parcels | core (blobs with dynamics) | no | resolved implicitly before unload (10, 11) | absolute, while loaded |
| scheduler state (next due ticks, process phases) | core | no | — | yes |
| in-flight async jobs (10) | workers | no | — | none: saves happen at a drained tick (§4.14) |
| the discovery journal, player settings | core resource | no | — | yes |
| plugin instance memory (static tables, scratch, caches) | instance | yes (init) | — | never |

### 4.3 Touched tracking and the halo

**Marking.** `touch(handles, reason)` marks each blob *edited*, and marks it and all its ancestors *dirty*. This is
`planet/`'s `touch`, which stops climbing at the first ancestor already dirty. Who touches:
- **Automatically by the core:**
  - creating or removing placed content (kind D);
  - any write to a field whose schema says `persist: on_write`;
  - writes from a `salient` event (17).
- **Owners:**
  - terrain on `dig` / `raise` / `add_edit_term`;
  - vegetation on fell / damage / plant;
  - any plugin when it changes structure the lift reads (hydrology's outlets, a beaver's lodge).
- **Never:** natural process writes (growth, snowfall, rain). Those are regenerated. A field that should persist
  everywhere is a top-level field, not a fine one.

**The halo.**
- When a family (parent P, children C) merges, it stores a record if P is dirty, **or** if any same-depth neighbour
  of P in its 3×3 neighbourhood is dirty. The neighbour's lift reads P's neighbourhood. Without the halo, untouched
  cells next to a hole regenerate leaning into it (`planet/` measured up to 0.5 m).
- When a halo family is restored, it is marked dirty, so it keeps storing while its neighbour does.

**Untouching (garbage collection).** A record is dropped when it has decayed to nothing: for every field
|r| · e^(−Δt/τ) < ε_f (ε for `z` is 1 mm), and it holds no overrides, terms, removals or placed content. Without
this, saves grow forever. An ancestor's dirty bit is cleared when no descendant record remains (recomputed on
merge).

### 4.4 Field residuals

**On merge of family P → C** (all children are leaves at that moment, since merges run bottom-up):

```
if dirty(P) or halo(P):
    S  = lift_prior(P) then project(P)            # a fresh lift of the current parent, no residual
    for each stored field f with persist = residual and linear aggregation (sum, mean):
        r_f[c] = x_f[c] − x_f[S_c]                # in the null space of R: Σ_area r = 0 (mean) or Σ r = 0 (sum)
    record[P.key] = { t: tick, r, dev_lo/dev_hi of each child (absolute, §14 4.7), baseline_hash(S) }
else:
    delete record[P.key]
```

**On split of P → C:**

```
x[C] = lift_prior(P) then project(P)
if record[P.key]:
    Δt = tick − record.t
    x_f[C] += r_f · exp(−Δt / τ_f)                # τ per field, core libm (02), Δt from integer ticks
    for sum fields: clamp negatives to 0 and rescale to keep the parent's total (planet/ does this)
    mark C dirty; bounds from record.dev_* until the family is realised again
```

**Rules:**
- Only linearly aggregated fields carry residuals.
  - A max/min field is stored absolutely in the record when it is persisted (the bound deviations), or is derived
    (the ocean mask from routing).
  - A field with a `finest` depth (tree counts stop at the entity depth) has no residual below it.
- Residual values are stored as f32. A `z` residual of a few metres keeps ~1e-7 m relative precision. That is
  plenty, since decay already makes them approximate. Exact saves use their own encoding (§4.8).
- **15 defines the persistence keys** `persist`, `persist_tau` and `persist_on` (§5.3), and 09's schema adopts them
  (R13). `persist_tau` = 0 means never persisted below the top level.

Starting values:

| field | τ |
|---|---|
| `z` | 400 yr |
| tree count residual | 150 yr |
| water, snow | 0 (regenerated; their *coarse* totals carry the information) |
| burnt fraction | ~20 yr |
| lava temperature | 0 (cooled lava is `z` and terms) |

These are tuning, not physics.
- `baseline_hash` is a 32-bit hash of the quantised fresh lift S. It detects generator changes (§4.11).

### 4.5 Geometry records: the four content kinds

The kinds are defined in 14 §4.8. Each persists differently, and only kind C involves 06's geometric (moment) projection.

**A. Field-derived (ground, water, snow).**
- Geometry is never stored. It is realised again from fields.
- A `z` edit is a field residual (§4.4).
- A non-heightfield edit (tunnel, undercut) is a **3D edit term** in the terrain plugin's region resource:
  - an ordered list of `{seq, op (add/sub), primitive (capsule / ellipsoid / bowl), params in f64 body-frame
    coordinates, level}`;
  - applied in `seq` order at realisation, at every level (the ray cut's "digs are generator input");
  - terms don't decay by default (rock doesn't heal in centuries); a plugin process can remove or shrink them
    (collapse, sedimentation).
- This is how D15 ("edits are removals and additions") holds for kind A. To the blob store it *is* removals and
  additions. The persisted form is the generator input, because a list of removed plate keys would not survive a
  re-lift from a parent that changed.

**B. Counted (trees, boulders, bushes).**
- **Overrides by candidate key:**
  - `stump{t}` blocks the site for 30 years;
  - `health{h}` is damage, which ranks the individual worse;
  - `planted{t, rank}`.
- **Living-set snapshots** per entity cell: a bitmask over 8 slots, stored when it differs from the default set for
  the cell's count. On restore it is reconciled with the current count (incumbency: rises activate the best free
  slot, falls kill the worst).
- Count residuals are ordinary field residuals.
- Overrides are owner-plugin resources at region scope, so the owner's prior reads them (principle 4).

**C. Shaped (outcrops, arches, coral).** This is where the geometric projection (06) matters.
- **Removals are prior input.** The family's removal set (keys of removed children) is persisted, and the owner's
  prior reads it and omits those children. The core's moment projection then fits the remaining children to the
  parent's moments, which `restrict` already reduced by the removed volume.
  - The alternative fails. If the prior ignored removals, the projection would inflate every sibling to carry the
    missing volume. The residual would then have to undo that for every sibling, so it would not be sparse.
- **Continuous shape changes are geometric residuals:** per child key, Δ(shape params) = actual − (prior +
  projection). Because both sides have the parent's moments, the residual sums to zero in moment space (exactly
  for volume and centroid, to first order for second moments). On restore: prior → projection → add the decayed
  deltas → re-project. So restrict∘lift stays the identity for geometry, as for fields.
- Removing a kind C instance takes it whole (`M7.md` §7: no floating crowns).

**D. Placed (logs, the player's hut, ejecta rocks).**
- **Full records:** minted key (02), owner, blob type, attachment (the ground body, or its own object body), blob
  parameters with f64 positions in the body frame, per-blob stored fields, and owner state as a resource keyed by
  the placed key.
- **No decay by default.** Rot, fire, floods or the owner's catch-up hook remove or change them.
- **The coarse view** comes from the record's merge pyramid, which is regenerated deterministically (the
  origin-snapped, canonically ordered merge of `M7.md` §7). It is never merged into the ground's lifted families.
  That would double-count the record on a re-lift (14 §4.8).
- **Storage location:** the storage region containing the record's centre. Its sort key is the Morton code of the
  centre within that region (§4.12).

### 4.6 Plugin resources

Plugins are stateless (D11, 08), so anything a plugin must remember is a **resource**: typed bytes the core stores,
with `(plugin id, resource id, schema version)` and a **scope**:

| scope | key | examples |
|---|---|---|
| global | — | a plugin's global calendar, a species registry grown at runtime |
| body | body key | volcano list and magma pressures, the plate table, the river network's top-level segments |
| region | cell key (any depth) | hydrology `outlets` of a touched coarse cell, a cave network, overrides, edit terms |
| entity | placed key or candidate key | a beaver colony's state, a creature's memory |

**Rules:**
- Region- and entity-scoped resources are chunked with their region (§4.12).
- A region-scoped resource *is* a touch: its cell and ancestors are dirty while it exists. So outlets keep their
  coarse cells from forgetting a diversion (`DESIGN.md` §2.5).
- Bytes are opaque to the core. A plugin that stores handles in a resource has a bug. The SDK's key types and the
  save/load fingerprint test (E15.2) catch it.
- A plugin may export `migrate_resource(from_schema, bytes) → bytes`. Without one, a schema bump makes old resources
  unreadable, and the load report says so (§4.11).

### 4.7 Bodies

- **Rails bodies** are regenerated from the universe seed. They are stored only when perturbed: an asteroid nudged
  off its orbit stores new elements and an epoch, keyed by its body key.
- **Integrated bodies** store their full state: parent body key, frame, f64 position and velocity, orientation,
  angular velocity, mode flags, and their blobs (as placed records attached to the body) or the generated key their
  blobs come from.
- **Minted bodies** (a detached cliff chunk, ejecta, a thrown rock) get keys minted from (creator key, tick,
  sequence) (02). A body that comes to rest becomes a kind D placed record, by 11's policy (R7). Under `keep_instance` it
  keeps its own instance; under `join_ground` it takes the ground's instance and blends in. Its body record is
  deleted.

### 4.8 The live top level and the exact cut snapshot

**The live top level.**
- For every activated body (10, R9), the save holds every top cell's stored fields, *absolutely*, in cell-id
  order, columnar, as f32 (f64 for `z`).
- Reference planet (14): 24,576 cells × ~24 fields ≈ 2.4 MB raw (estimate).
- Bodies never simulated store nothing; their t0 comes from the cache or regeneration.

**The exact cut snapshot.**
- For bodies with an observer at save time, the save holds the sim tier *as it is*: for every refined family, the
  children's stored fields, absolutely, in spatial sort-key order (§4.12).
- Inner nodes are recomputed by `restrict` on load. That is bit-identical, since the same deterministic code runs on
  the same leaf values.
- Load goes down the saved structure, using the saved values instead of priors, then realises geometry (a
  deterministic function of persisted state).
- Why absolute and not residuals: x = L(R(x)) + (x − L(R(x))) is not bit-exact in floating point. Exact resume
  needs the values themselves.
- Estimate: ~25 k surface cells + ~25 k other blobs × ~20 fields × 4 B ≈ 4 MB raw per observer.

The residual store is saved alongside, unchanged. So the snapshot covers what is loaded, and the store covers what is
not.

### 4.9 Render-tier splits read records, never write them

The two tiers (R1) are adopted. Render-tier splits are the deterministic lift of the sim tier's leaves, so they
need the **same stored records**:
- residuals decayed to now;
- edit terms;
- overrides (or the stumps would reappear at a distance);
- placed content (a dam visible from 1 km).

They are **read-only**: never touched, never stored, never saved. Requirements:
- **06 / 12:** render-tier splits call the same lift + restore path with writes disabled.
- **Here:** the residual store of every loaded body stays in memory. It is small (§4.17), and render-tier splits
  happen all over the view.

### 4.10 Returning after time away

When a touched region re-enters the sim tier after Δt:

1. **Coarse state is current.** The top level was simulated meanwhile, or caught up when the body was frozen (10 owns
   the policy). The flood, the drought and the fire that passed are already in the parents.
2. **Field residuals decay:** children = lift(parent now) + r · e^(−Δt/τ). A 3.7 m hole after 15 years at τ = 400 yr
   is 3.56 m (measured in `planet/`).
3. **Overrides expire by time.** A stump older than 30 years frees its site. The living set reconciles with the
   current count, so the damaged tree is first to die if the count fell.
4. **Placed content and region/entity resources get a catch-up call:** `catch_up(scope, Δt)` on the owning plugin. It
   is deterministic and parameterised only by Δt and persisted state (10 schedules it). Logs rot, a beaver colony
   that had no top-level process repairs or abandons its dam, a cave partly collapses.
5. **The fast layer has nothing to catch up.** Parcels were resolved before the unload: lava laid as lobes, snow
   settled (`PLUGINS.md` `onSkip`).

Order on split: restore fields → apply overrides and removal sets (prior input; already read by the prior) →
`catch_up` for resources in scope → realise geometry. Both 10 and 06 have to honour this order.

### 4.11 The generator-version problem

Residuals are relative to a regenerated baseline, and overrides and removals refer to generated keys. If a plugin's
generator changes, stored records can silently mean something else:
- a hole lands on a different slope;
- an override names a tree at a different site;
- a removal names a key that no longer exists.

| option | how | good | bad |
|---|---|---|---|
| pin plugin versions per save | the save records each plugin's exact module; loading uses it | always correct; simple | a bug fix in a generator never reaches old saves unless upgraded on purpose |
| absolute snapshots of touched subtrees | store values, not residuals | survives priors changing | breaks consistency with the new parents (needs re-projection); the surroundings still change; much bigger |
| migration hooks | the plugin converts old records | precise | hard for plugin authors; untestable in general |
| determinism hash per region | store a hash of the baseline; detect a mismatch | catches undeclared changes | detects, doesn't fix |

**Recommendation: pin by default, detect always, upgrade only on request.**

1. **Generator versions.**
   - Each plugin manifest (08) declares an integer `generator_version`. Code changes that don't change generated
     output keep it. Changes to `gen_top`, priors, `realize`, candidate placement or key schemes bump it.
   - The save pins `(plugin id, generator_version, module hash)` for every plugin that owns generated content.
2. **Embedded modules.**
   - Saves keep the bytes of every pinned module, content-addressed and shared between saves in `/modules/<hash>`;
     exports embed them.
   - A save therefore always loads, even when the plugin's site is gone. WASM modules are small (est. 0.1–1 MB).
3. **Loading.**
   - If the installed plugin has the same `generator_version`, it is used, so non-generation fixes reach old saves.
   - If not, the pinned module is used for that save.
4. **Detection.**
   - Every residual record carries `baseline_hash`. On restore, the core recomputes it and compares.
   - A mismatch means an undeclared generator change. The core logs it, and the load report names the plugin.
   - CI fingerprint tests (19) are meant to catch these before release.
5. **Upgrade = rebase, on request.** The player (or the server admin) chooses to move a save to a newer generator:
   - field residuals are re-applied to the new baseline. They are still in the null space, so consistency holds,
     and a hole stays a hole on the new slope;
   - overrides, removals and geometric residuals whose keys no longer exist are dropped and reported;
   - placed content and resources are kept, with `migrate_resource` where provided.

   A plugin may declare `rebase_safe: true` (keys and structure unchanged, only priors tuned). The upgrade is then
   offered automatically.
6. **The core's own generation code** (index maths, projection, libm, RNG, merge order) is frozen by specification
   and golden tests (02, 19). A core change that alters generation is a save-format version bump with a migration
   written by us.

### 4.12 Save layout

```
OPFS root
├── saves/<save-id>/
│   ├── manifest.a, manifest.b     two slots, alternately written: {seq, format version, core version,
│   │                              universe seed, tick, plugin pins, chunk list (name → hash, size), checksum}
│   └── chunks/<hash>              immutable, content-addressed, compressed
├── modules/<hash>.wasm            pinned plugin modules, shared by saves
└── cache/…                        deletable (§4.16)
```

**Chunk kinds:**

| chunk | content | granularity |
|---|---|---|
| `world` | tick, time scale, scheduler state, observers (players), settings, journal, global resources | one |
| `bodies` | perturbed rails elements, integrated and minted bodies | one (or per star system if large) |
| `top/<body>` | the live top level, columnar | per activated body |
| `cut/<body>` | the exact cut snapshot (§4.8) | per body with an observer |
| `region/<body>/<region key>` | field residual records, edit terms, overrides and living sets, kind C residuals and removals, placed records, region- and entity-scoped resources | per **storage region**: one of 06's regions at a fixed storage level, `ℓ_store` = ℓ13 (65.5 km edge; est. ~1,000 intersect the reference planet's surface). Records of families larger than a storage region go to `region/<body>/root` |
| `res/<plugin>/<body>` | body-scoped resources | per plugin per body |

**Records inside a chunk** are grouped by type and stored columnar. They are sorted by a **spatial sort key carried
with each record** (R3), never by key order:
- the sort key is the storage-region key, then an owner-supplied spatial code, then the 128-bit key to break ties;
- the spatial code is the S2-style cell id plus sheet for ground records (14 §4.7), and the Morton code of the centre
  within the region for placed content and other positioned records;
- keys are hashes (05), so key order has no locality and keys don't compress, while sort keys are Hilbert/Morton
  ordered and delta-encode into a few bytes each;
- every record still stores its 16-byte key. For ground families the key could be recomputed from the cell-id path
  (the H chain from the body root). That is an optimisation to measure, not a format rule. Each chunk is compressed with `deflate-raw` through
`CompressionStream`, in the persistence worker.

**Why content-addressed chunks plus two manifests.**
- A save writes new chunk files under their hash, which never overwrites anything. Then it writes the *older*
  manifest slot with `seq + 1` and a checksum, and flushes.
- Loading reads both slots, verifies their checksums, and takes the newest valid one.
- A crash at any point leaves at least one complete save.
- Unchanged regions keep their chunk hash, so an autosave writes only dirty chunks.
- After a commit, chunks referenced by neither manifest are deleted.

### 4.13 Storage backends

- **OPFS with synchronous access handles** is the primary backend. `createSyncAccessHandle()` works only in
  dedicated workers. It is available in Chrome/Edge 108+, Firefox 111+ and Safari 17+
  ([MDN](https://developer.mozilla.org/en-US/docs/Web/API/FileSystemFileHandle/createSyncAccessHandle)). Reads and
  writes are synchronous and fast, and `flush()` makes them durable.
- **A dedicated persistence worker** owns all storage. The engine worker (D3) only serialises records into a
  staging buffer at a tick boundary and hands it over (transfer, or a SharedArrayBuffer when the threads setup of 01
  allows it). Compression, hashing, writing and committing happen off the engine thread. The engine's cost per
  autosave is the serialisation of dirty regions: est. well under 1 ms for typical edits, a few ms with a full cut
  snapshot. E15.3 measures it.
- **IndexedDB** is the fallback, behind the same chunk/manifest interface (one object store of chunks keyed by
  hash, one for manifests), for browsers or modes where OPFS sync handles fail.
- **Durability:**
  - request `navigator.storage.persist()` at the first save, and show the result in settings;
  - watch `navigator.storage.estimate()`;
  - remind the player to export when storage isn't persistent.
- **Risk:** Safari's policy of deleting script-writable storage for sites without recent user interaction. Whether
  it applies to OPFS in our hosting setup is to be verified in M0.

### 4.14 Autosave, save points and crash safety

**Save points are tick boundaries with no async jobs in flight.** Async work has fixed per-process latencies (10,
R1): `L_split` = 3 ticks for splits, `async(30)` for climate and hydrology. Saving therefore means: stop issuing new
jobs, run until in-flight ones land (≤ 30 ticks, about 1 s at the provisional 30 Hz tick), then snapshot. The world
keeps running meanwhile, so this is invisible to the player. It also means no job state is ever saved. A
requirement on 10: the scheduler can drain.

**Autosave:**
- every 5 minutes of real time;
- on `visibilitychange` → hidden;
- on milestones (embark, jump, before a time warp).

On page hide, the snapshot is handed over at once. If the tab dies mid-write, the previous manifest is still valid.

**Rotation and integrity:**
- Three rotating autosave slots, plus manual saves. A manual save is the same manifest mechanism with its own save
  id. Chunks are shared by hash.
- On load, every chunk's hash is verified. A corrupt chunk makes that manifest invalid, and the loader falls back to
  the other slot, then to older autosaves.

### 4.15 Export and import

- **Export:** one file, the manifest followed by its chunks and embedded modules, in a simple length-prefixed
  container with a header and version. The persistence worker builds it as a stream. The main thread downloads it
  (Blob + anchor).
- **Import** reverses this. It verifies hashes, refuses unknown format versions with a clear message, and offers to
  rebase if the pinned plugins differ from the installed ones (§4.11).
- An exported save is self-contained: universe seed, plugin modules and all records.

### 4.16 The cache

The cache holds regenerable outputs that are expensive to rebuild:
- a body's t0 top level (generation + spin-up, est. 2–4 s per planet in WASM, 14 §4.4);
- coarse realised families used for orbit views (render tier, depths 3–10, up to `ℓ_view_floor`);
- (later) anything else measured as slow to regenerate.

**Keys** are `(universe seed, body key, generator pins hash, kind, node key)`. A plugin upgrade changes the pins
hash, which invalidates the right entries and nothing else.

**Rules:**
- It lives under `cache/`, with an LRU index and a budget (default 1 GB, or 10% of quota, whichever is smaller). It
  is shared between saves of the same universe.
- **It can always be deleted.** Nothing is ever read from the cache that couldn't be regenerated bit-identically.
  CI checks this by comparing cached and regenerated outputs.
- It is not part of exports.

### 4.17 Size estimates

These are estimates, scaled from `planet/`'s measurements. E15.5 measures them.

| item | raw | compressed (est.) |
|---|---:|---:|
| one field residual record (a quadtree family, ~10 persisted fields, f32, bounds, header) | ~210 B | ~120 B |
| a dig + 4 tree edits (`planet/`: 168–172 records over 14 levels; ~1.5× for 21 depths) | ~50 KB | ~30 KB |
| an override / a living-set mask | 24 B / 16 B | |
| a placed blob (f64 position, packed shape, flags, a few fields) | ~64 B | |
| a beaver dam (~200 logs × ~5 blobs) + colony resource | ~65 KB | ~40 KB |
| the live top level of one planet (24,576 cells × 24 fields) | 2.4 MB | ~1.5 MB |
| the exact cut snapshot, one observer | ~4 MB | ~2.5 MB |
| **a typical save after a few hours** (one planet, ~100 edits, some events) | ~10 MB | **~5–8 MB** |
| a heavy builder (thousands of placed objects) | 50–200 MB | dominated by placed content |

The residual store for a loaded body stays in memory (§4.9). It is about the size of its region chunks, so MBs.

### 4.18 Multiplayer implications (not built now)

- **One authoritative store** on the host or server. Clients hold caches and a mirror of the regions they observe.
- **Joining** sends the `world` chunk, the plugin pins (and modules), body states, and the live top level of the
  joining player's body. The client regenerates everything else deterministically.
- **Region sync uses the save's records.** When a peer's observer brings a region into the cut (each peer's
  observer is a requester: interest management, 06), the host sends that region's chunk: residual records, terms, overrides,
  resources, placed content. If the region is in the host's cut, it also sends the cut snapshot.
- **Region hashes** (records + baseline hashes) let clients verify that they regenerated the same world.
- Nothing in the format refers to handles, local paths or clocks, so the same bytes work as a network message.

### 4.19 Canonical scenarios

**Scenario 5: dig, leave, return** (reference planet; depth 21 = 0.37 m leaves, the default finest level, R5).

| when | what happens | what is stored |
|---|---|---|
| t₀ | The player digs. `terrain.dig` lowers `z` on the ~200 depth-21 leaves under a 3 m-radius bowl. Restrict removes the volume from every ancestor at once (the ancestors' `z` drops by the volume over their area), so even the 12 km top cell knows. The leaves are touched and their ancestors dirty. | nothing yet (the region is loaded) |
| t₀ + 1 min | The player fells a tree (vegetation service → override `stump{t₀}`, the count at the entity cell drops by 1) and damages another (`health{0.4}`). | overrides in the vegetation plugin's region resource |
| t₀ + 2 min | It rains. Hydrology's water field fills the hole. The water belongs to the coarse cell's total; its fine distribution is a residual of the water field (τ = 0, so it is regenerated next time by the bathtub lift). | nothing |
| autosave | Region chunks for the dirty storage regions, plus the exact cut snapshot and the live top level. | ~5 MB total |
| t₀ + 5 min | The player flies to another continent. The old region leaves the sim tier and merges bottom-up. Every dirty family and halo family stores a field residual record: est. ~250 records. Others store nothing. The overrides stay as they were. The fast layer had nothing moving. | ~30 KB of records in one or two region chunks |
| 10 years (time warp) | The top level simulates throughout (10). The vegetation process at its band changes the count in the old top cell. Erosion, if any, changes the parent's `z`. Nothing touches the records. | unchanged |
| return, t₀ + 10 yr | The region refines. Each split with a record gets lift(parent now) + r · e^(−10/τ). The hole is 0.975 × its depth, so 3.7 m → 3.61 m. The halo families restore the hole's surroundings. The stump is 10 years old (< 30), so its site stays blocked. If the count fell, the damaged tree is first to die; otherwise it stands, damaged. Rain fills the hole again through the bathtub lift (the coarse water total is current). Records whose residual decayed below ε are dropped. | records refreshed when the region unloads again |
| save + reload any time | The exact snapshot gives bit-identical continuation (E15.2). | |

**Scenario 3: the beaver dam persisting.**

| step | mechanism | persisted as |
|---|---|---|
| Beavers fell creek-side trees | vegetation service `fell(candidate)` | overrides `stump{t}` (region resource of vegetation) |
| They drag logs into the creek | beaver plugin creates placed content: logs with keys minted from (colony key, tick, seq) | placed records (kind D), in the region of each log |
| The dam blocks the creek | hydrology's computed `blockage` per cell (the top of placed solids in the cell) raises the effective ground for routing; the ground's `z` is unchanged | nothing extra: `blockage` is derived from placed records, so the prior rule holds |
| A pond forms | hydrology's water field; lake volume held in the touched coarse cells | water residual τ = 0; the coarse total is in the top level |
| The creek downstream shrinks | hydrology restricts routing upward for touched coarse cells | `outlets` as a region resource of hydrology (keeps the cells dirty) |
| The colony lives here | beaver plugin entity resource: members, lodge key, dam integrity, food | entity-scoped resource (16 owns its meaning) |
| The player leaves for a year | the region merges; families under placed content and outlets are dirty, so they store residuals (mostly empty for `z`); placed records and resources stay | ~40 KB |
| Away | top-level hydrology uses the outlets: the pond's volume stays in its coarse cell, discharge downstream stays reduced, riparian trees downstream respond at their band (`DESIGN.md` §5's diversion result) | |
| Return | split → `z` lift + residual → overrides read by the vegetation prior (stumps) → `catch_up(colony, 1 yr)`: the beaver plugin deterministically updates the colony and the dam's integrity, and maybe removes some logs (a partial breach) → logs realised from records → `blockage` recomputed → the water lift puts the pond's volume behind the dam | |

The dam looks the same from 1 km in render-tier splits, because they read the same placed records and overrides
(§4.9).

**Scenario 4: the asteroid** (briefly).
- **The crater is 17's regenerable `impact.crater` record** (R7): centre, diameter, depth, rim height, age and seed.
  - It is a body-scoped resource of the impact plugin, kept forever as part of the planet's class ≥ 3 history (17).
  - The plugin draws it as a `shape` contribution to `std:ground_height` (14 §4.9) at every level. So the record
    alone regenerates the crater, and it does not touch the region.
- **The explicit run's details on top are touched detail:**
  - slumps and the exact ejecta blanket as `z` residuals (τ = 400 yr);
  - settled ejecta as kind D placed records: rocks under `keep_instance`, fine ejecta under `join_ground` (11, R7).
    Folding fine deposits into `z` would need an explicit ground-plugin service; it is not planned.
- **Decay, settled:**
  - the crater record never decays by a residual τ. Any softening with age is the impact plugin's deterministic
    function of the record's `age` field, the same whether the crater was watched or not;
  - only the touched detail decays, with `z`'s τ. A crater nobody revisits costs one record of tens of bytes, plus
    a few KB of detail that fades over centuries.
- The burnt fraction is a field residual with τ ~20 years. Dust in the atmosphere is a top-level climate field,
  saved with the top level.
- The impactor's body record is deleted when its blobs become ejecta and crater lining.

## 5. Interfaces

### 5.1 Core API used by plugins

```c
typedef struct { uint64_t hi, lo; } key128;     // 05 fixes the width and layout

// touched tracking
void   ss_touch(uint32_t n, const uint32_t* handles, uint32_t reason);   // edited + dirty ancestors

// deterministic key minting for created content (02, 05; the final name is in 08 §5.2)
void   ss_mint_keys(uint32_t n, const uint32_t* creators, uint32_t stream, key128* out);

// resources: scoped opaque bytes, persisted by the core (R2: 15 owns the semantics, 08 §5.2 the list)
// each resource id declares its scope (global | body | region | entity) in the manifest (§5.4), so calls pass only
// the scope key: zero for global, a body key, a region key or an entity key
int32_t ss_resource_get(uint32_t res, const key128* scope_key, uint8_t* dst, uint32_t cap);    // length, or -needed
void    ss_resource_put(uint32_t res, const key128* scope_key, const uint8_t* src, uint32_t len); // owner only; region
                                                                                          // and entity scopes touch
void    ss_resource_del(uint32_t res, const key128* scope_key);
int32_t ss_resource_list(uint32_t res, key128* dst_keys, uint32_t cap, const key128* after);  // deterministic order

// placed content (kind D; 08 may fold this into ss_blob_create with kind D)
void   ss_place(uint32_t n, const placed_desc* in, uint32_t* out_handles);   // mints keys, touches, records
void   ss_unplace(uint32_t n, const uint32_t* handles);

// kind C removals (read by the owner's prior; persisted by the core per family)
void   ss_remove_generated(uint32_t n, const uint32_t* handles);
```

### 5.2 Plugin exports for persistence

```c
// bring a region's or entity's resources and placed content up to now after Δt unloaded (deterministic)
void    catch_up(uint32_t scope, const key128* scope_key, int64_t dt_ticks);
// optional: convert a resource from an older schema version
int32_t migrate_resource(uint32_t res_id, uint32_t from_schema, const uint8_t* src, uint32_t len,
                         uint8_t* dst, uint32_t cap);
```

### 5.3 Persistence keys (defined here; 09's field schema adopts them, R13)

```
persist:      residual | absolute | never          // residual needs linear aggregation
persist_tau:  seconds of game time (0 = regenerate below the top level)
persist_on:   write | touch                         // write: any write touches (salient fields)
```

### 5.4 Manifest additions (08)

```
generator_version: integer      // bump when generated output changes
rebase_safe:       bool         // keys and structure unchanged; priors only
resources:         [{ id, scope, schema_version }]
```

### 5.5 Engine ↔ persistence worker

```
engine → persist:  snapshot{ seq, tick, dirty chunk payloads (uncompressed, transferred) }
persist → engine:  committed{ seq } | failed{ seq, reason }
engine → persist:  load{ save id } ;  persist → engine: chunks (decompressed, by kind)
engine → persist:  cache_get / cache_put { key, bytes }
```

### 5.6 What 15 needs from other docs

- **02:**
  - frozen libm for `exp`;
  - deterministic key minting;
  - frozen generation code paths, with golden tests.
- **05:** a key layout carrying body, owner, spatial id, sheet and minted ids.
- **06:**
  - merges run bottom-up and call the store;
  - splits restore records in the §4.10 order;
  - render-tier splits run the same restore path read-only;
  - the neighbour (halo) test is available.
- **08:**
  - `generator_version`, `rebase_safe`, resource declarations;
  - stateless instances;
  - module hashes.
- **09:** adopts `persist`, `persist_tau` and `persist_on` from §5.3 (R13), and states which aggregations are
  linear.
- **10:**
  - a drainable scheduler (save points);
  - `catch_up` scheduling;
  - the body background-simulation policy;
  - fast-layer resolution before unload.
- **14:** content kinds per blob type; terrain edit terms and `dev_lo`/`dev_hi`; `baseline_hash` inputs.

## 6. Risks and unknowns

1. **Generator changes without a version bump.**
   - Detection (baseline hashes) only fires when a touched region is restored, possibly months later.
   - *Mitigation:* CI fingerprint tests per plugin on a fixed set of keys (19), which the plugin SDK template runs.
2. **Halo cost on big edits.** A large terraforming edit touches thousands of families, and its halo more.
   Records are ~120 B compressed each, so 10⁴ families is ~1 MB. Acceptable, but measure.
3. **Placed content dominates heavy saves.** A builder player can create millions of blobs. Options, if this becomes
   real:
   - placed content made of *generated* parts (a "hut" record = a blueprint key + transform, regenerated);
   - merging old, stable builds into the ground (`z` + terms).
4. **Max/min fields and non-linear aggregations** can't carry residuals. Every such field must be derived, stored
   absolutely, or kept at the top level only. A plugin author who declares `persist: residual` on a max field gets a
   load-time error, which is a good thing but must be documented.
5. **Catch-up hooks that depend on Δt non-additively.** Two short absences then differ from one long one. That is
   acceptable for single player, but it is an observer effect in time. Hooks should be written as closed-form
   functions of Δt where possible (10).
6. **Browser storage eviction.** Safari's policy for script-writable storage, and quota pressure anywhere.
   *Mitigations:* `persist()`, export reminders, a server store later.
7. **Exact-resume brittleness.** Any state the snapshot misses (a scheduler phase, a pending event's countdown, an
   RNG stream counter kept in a plugin's memory against D11) breaks bit-exact resume. E15.2 must run in CI with
   every plugin installed.
8. **Records keyed by generated keys** (overrides, removals) depend on the key scheme never changing, which is part
   of `generator_version`. A plugin that changes candidate placement must bump it, even if "it looks the same".

## 7. First experiments

| id | experiment | success criterion |
|---|---|---|
| E15.1 | Port `planet/`'s unload/return test (`tools/test.mjs`) to the new core: dig + 4 tree edits, unload to the top level, return | terrain restored to ≤ 1e-12 m with decay off; the same living trees; record count within 2× of 168–172 per edit (scaled to depths) |
| E15.2 | Exact resume: run 1 simulated hour with every plugin, save, reload, run another hour; compare against an uninterrupted run | bit-identical fingerprints of every field, body and resource at the end |
| E15.3 | OPFS in a persistence worker: write 200 region chunks (10–200 KB) + manifests; kill the tab at random points 100 times | the newest complete save always loads; engine-side serialisation < 1 ms for 50 dirty regions; worker commit < 50 ms |
| E15.4 | Generator-change detection: change one prior constant without bumping `generator_version`, then load an old save | every restored touched family reports a baseline-hash mismatch; rebase keeps holes and placed content and reports dropped overrides |
| E15.5 | Save sizes: a scripted "heavy" session (1,000 edits over 10 km, a dam, an eruption, a crater) | total < 20 MB compressed; autosave writes only dirty chunks |
| E15.6 | Render-tier splits read records: view a dug, edited region from 1–3 km after the sim tier has merged it | the crater, stumps and dam are visible at the coarse levels; no write to the store during viewing |

## 8. Build order

| milestone | work |
|---|---|
| M0 | storage probe in a persistence worker (OPFS sync handles, IndexedDB fallback); manifest A/B + content-addressed chunks with a stub payload; `persist()` behaviour per browser |
| M1 | `world` and `bodies` chunks (tick, camera/observer, perturbed and minted bodies); export/import container v0 |
| M2 | the cache: t0 top levels and orbit-view families keyed by generator pins |
| M3 | resources (scopes, schema versions); `generator_version` and module pinning; the exact top-level and cut snapshots for one plugin; E15.2 started |
| M5 | **persistence v1:** touched tracking, the halo, field residuals with decay, overrides and living sets, placed content, region chunks, autosave, E15.1–E15.5 |
| M6 | event records (craters, deposits, ejecta); `catch_up` hooks for event-made content |
| later | edit terms for caves; rebase tooling; the multiplayer snapshot and region sync |

## 9. Open questions

- **Q15.1 Do old saves keep their old generators?** (merged into questions.md C9) By default a save keeps the exact plugin modules it was made
  with. A newer generator applies only when you choose to upgrade (rebase), which can drop overrides that no longer
  match.
  - *Why it matters:* it is the difference between "saves never break" and "bug fixes reach every world".
  - *Default:* pin + embed + detect, with upgrade on request; automatic only for plugins that declare `rebase_safe`.
- **Q15.2 Exact saves: is bit-exact resume worth the extra few MB per save** (the cut snapshot) and the discipline it
  imposes (no hidden state anywhere)?
  - *Why it matters:* it is what makes replays, determinism tests and later multiplayer joins trustworthy.
  - *Default:* yes.
- **Q15.3 Should the player's changes fade?** In `planet/` a hole fills with τ = 400 years and residual tree counts
  with 150 years. At 1:1 game time that never shows. With time warp it does.
  - *Why it matters:* it decides whether a long-lived world slowly heals or keeps every scar, which in turn decides
    save growth.
  - *Default:* fields decay (natural healing); placed content and craters don't, unless a process acts on them
    (rot, fire, flood, sedimentation).
- **Q15.4 Are worlds private saves, or one shared world?** (merged into questions.md C14) The universe is the same for everyone (deterministic). Do
  edits stay in the player's save, or should there eventually be a shared persistent world per server?
  - *Why it matters:* a shared world needs an authoritative server store and moderation.
  - *Default:* private local saves with export; a hosted shared world as a later option using the same records.
- **Q15.5 Autosave and slots.**
  - *Default:* autosave every 5 minutes, when the tab is hidden, and at embark/jump; 3 rotating autosaves plus
    manual saves; no cloud.
  - Is there anything you want differently, for example a single "the world just persists" save with no slots, like
    Minecraft?
- **Q15.6 Does every activated planet keep its full live top level in the save** (~1.5 MB compressed each, est.)?
  That only matters if visiting many planets is common.
  - *Why it matters:* save size scales with planets visited, and it couples to 10's background-simulation policy.
  - *Default:* yes. An activated planet's top level is saved, and a planet that was only viewed from orbit
    is not.
