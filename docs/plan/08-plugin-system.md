# 08. Plugin system

Status: draft · Depends on: [01](01-runtime-and-toolchain.md), [02](02-numerics-time-and-determinism.md),
[05](05-blob-model.md), [06](06-blob-hierarchy-and-cut.md), [07](07-spatial-queries.md),
[09](09-fields-and-composition.md) · Used by: [10](10-simulation-and-scheduling.md), [11](11-physics.md),
[14](14-planet-generation.md), [15](15-persistence.md), [16](16-entities-and-creatures.md),
[17](17-events-and-discovery.md), [19](19-tooling-and-testing.md)

## 0. Short answers to Joel's questions

You asked whether WASM dynamic linking can be combined with the metadata json, whether we need it at all, and whether
there's a better idea.

**Can dynamic linking work with the manifest?** Mechanically, yes. The loader builds every module's imports, so it can
use the manifest to decide what each import binds to. But dynamic linking doesn't do the composition for you:
- The [convention](https://github.com/WebAssembly/tool-conventions/blob/main/DynamicLinking.md) resolves symbols in
  one global namespace (`env.*` functions, `GOT.mem` / `GOT.func` globals), with one definition per symbol (weak
  binding aside).
- Your manifest describes *chains*: several plugins export `ground_field`, and each one imports "the previous
  `ground_field`". A single namespace can't express "the previous definition of the same name". So the loader would
  have to rename symbols per module anyway.
- That per-module wiring is exactly what a host does for isolated instances.

So the manifest is needed whichever memory model we pick. The real choice behind dynamic linking is something else:
**do all plugins share one memory, or does each have its own?**

**Do we need it?** No, not for composition. What dynamic linking buys is zero-copy access to shared memory and cheap
direct calls. What it costs:
- isolation: any plugin can corrupt the core or any other plugin;
- language freedom: every module must share one allocator and libc ABI;
- ABI stability: the convention says "There is no stable ABI yet";
- a custom loader;
- more complicated threads.

For community plugins those costs are high. The copies it saves are cheap. `wasmgpu/` measured `TypedArray.set` at
4 MB in 0.09 ms, so moving 100 k blobs × 3 f32 fields costs about 0.03 ms.

**A better idea, as proposed in the README (D11):**
- Each plugin is an **isolated instance** with its own memory.
- Plugins call the core through a fixed C ABI of **batched arrays**. Every pointer argument refers to the *caller's*
  own memory; the host copies across.
- The core owns all authoritative state (fields and resources). Plugin code is close to pure functions over that
  state.
- The same ABI also compiles with **static linking** into the core for trusted first-party plugins: same source, no
  copies.

Dynamic linking stays a fallback if measurements ever show copies dominating. The ABI rules below keep that door open.

Your manifest idea is right, and it's kept: static declarations that the core validates before running any plugin
code. Section 4.5 formalises it. Your import/export pairs become field-provider entries with phases
([09](09-fields-and-composition.md)) and service imports.

## 1. Purpose and scope

**In scope:**
- what a plugin is: package, manifest and module;
- how plugins are loaded, validated and resolved into a deterministic plugin set;
- the memory and isolation model;
- the ABI: what the core exports to plugins and what plugins export to the core;
- the lifecycle, including hot reload;
- where plugin code runs (engine thread and workers) and how that stays deterministic;
- cross-plugin interaction (services, extension points, events, resources);
- limits, faults and security;
- versioning, the SDK, packaging and distribution;
- testing plugins.

**Out of scope** (owned elsewhere; this doc states only the interface it expects):
- field schemas and composition semantics, including the `@checkpoint` question: [09](09-fields-and-composition.md);
- the split / lift / restrict protocol and the cut: [06](06-blob-hierarchy-and-cut.md);
- query semantics: [07](07-spatial-queries.md);
- tick phases and process scheduling: [10](10-simulation-and-scheduling.md);
- the fast-layer integrator and rheology: [11](11-physics.md);
- material rendering: [12](12-rendering.md);
- save formats: [15](15-persistence.md);
- the RNG and deterministic maths: [02](02-numerics-time-and-determinism.md).

## 2. Requirements

| | requirement | from |
|---|---|---|
| must | Plugins define blob types, fields, behaviours and processes. The core knows none of them by name. | pillar 4 |
| must | Writing a plugin needs only the SDK and the manifest docs, in more than one language. | pillar 4 |
| must | A faulty plugin can't corrupt the core, other plugins or the save. Its faults are attributed to it. | pillar 4 (community) |
| must | Results are deterministic: the same plugin set, seed and inputs give bit-identical state on every machine, whatever the load order, thread timing or browser. | D5, multiplayer later |
| must | Composition is validated at load: missing dependencies, conflicting field schemas, ordering cycles and service cycles are reported before any tick runs. | `planet/` load report |
| must | Plugin calls are batched (numpy style): handles in, arrays out. | Joel's `neighbors(dst, src, size, k, radius)` |
| must | Plugins can split the blobs they own and create blobs of any level. | Joel's design |
| must | Processes can run off the engine thread without breaking determinism. | D3, 10 |
| should | Hot reload of a plugin during development without losing world state. | workflow |
| should | First-party plugins (terrain, climate, hydrology, vegetation) use the same SDK, and can be linked statically for speed. | dogfooding |
| should | A plugin can be tested headless in Node with the real core. | 19, `planet/tools` |
| later | A plugin registry with signed packages. Plugins downloaded by URL in multiplayer, with identical sets enforced. | multiplayer |
| later | Plugin-provided GPU code (custom materials, shape terms on the GPU). | 12 |

## 3. What the prototypes taught

From `blobby-test/planet/PLUGINS.md` and `planet/js/plugin.js` (seven plugins: terrain, climate, snow, hydrology,
vegetation, fire, volcano):

1. **Fields are the seam, and the port was mechanical.**
   - Moving the five processes into plugins gave a bit-identical world (field totals to 10 digits after spin-up,
     refinement and 60 days, `tools/fingerprint.mjs`).
   - Any subset of plugins builds and simulates. A field read but owned by no installed plugin becomes a constant,
     and the load report says so ("hydrology reads 'lava'... constant 0").
2. **Writes are changes, so composition is free.** Several plugins write `z`, `snow` and `flow`. No ordering was
   ever needed, because every write is a `pushDown` delta. The only order that mattered was **lift order**
   (`liftReads`), and the registry sorts it topologically.
3. **Services were needed as soon as plugins touched each other.**
   - Lava kills trees (`vegetation.killTrees`), burns ground (`fire.markBurnt`) and melts snow (`snow.melt`).
   - A raw field write would have been wrong in all three cases. Only the owner understands its structure: per-tree
     masks, the snow closure.
4. **Extension points with summed contributions** (`hydroSource`):
   - snow takes snowfall and gives melt;
   - hydrology no longer mentions snow.

   The contributor also changes its own fields inside the hook. That is fine, because each contributor only touches
   fields it owns.
5. **Stale arrays.**
   - A plugin kept `w.area` from its `create` time. The core reallocated it when the cell pool grew, so the plugin
     read garbage for new cells.
   - The NaN snow went down the rivers and turned a mesher flood fill into 178 ms.
   - **Lesson:** plugins get handles and accessors, never long-lived pointers into core memory.
6. **Core structure vs plugin shapes.**
   - Top-level rivers knew nothing about volcano cones and carved a channel over a summit (600 m flattened).
   - The fix had to touch the core: track the plugins' part of the terrain (`nzT`), and keep river segments off
     cones (`noRivers`).
   - **Lesson:** where core structure meets plugin structure, there must be an explicit extension point, declared
     in a manifest. Fields alone don't cover it.
7. **Plugins kept mutable state in closures.**
   - `climate.js` kept `sim.storms`; `snow.js` kept a `snowlines` map and an event list; `volcano.js` kept the
     volcano list and its lobes.
   - All of it was authoritative, and none of it was persisted or restricted.
   - That works in one tab. It doesn't survive save/load, workers or multiplayer. That is the reason for section 4.3.
8. **Every scheme was stable for any step.** Processes catch up with `maxSteps` larger steps
   (`PL.Sim.advance`). Doc 10 keeps this. For plugins it means `dt` is an argument, never an assumption.

From `blobby-test/wasmgpu/README.md`:
- the wasm → JS boundary costs ~4 ns;
- `TypedArray.set` copies 4 MB in 0.09 ms;
- floats passed as arguments get boxed (59 ns vs 23 ns per call). Pass bulk data through memory;
- `memory.grow` detaches views of non-shared memory. A stale view silently copies nothing. The glue must re-derive
  views after any call that can grow memory.

## 4. Design

### 4.1 The model

```
 plugin package (.skp)        validated at load               runs as
 ┌─────────────────────┐      ┌────────────────────┐      ┌───────────────────────────────┐
 │ manifest.json       │─────▶│ resolver           │─────▶│ engine thread: one instance    │
 │ plugin.wasm         │      │  dependency check  │      │   (sync: split, fields,        │
 │ assets/ (models,    │      │  field schemas     │      │    services, events)           │
 │   type tables)      │      │  (field, phase)    │      │ workers: replica instances     │
 └─────────────────────┘      │    graph (09)      │      │   (async jobs, fixed latency)  │
                              │  service graph     │      └───────────────┬───────────────┘
                              │  plugin-set hash   │                      │ batched calls, copies
                              └────────────────────┘                      ▼
                                                           core: blobs, fields, resources, cut,
                                                           queries, scheduler (all state)
```

- **A plugin is code plus declarations.** The manifest declares everything structural: blob types, fields and their
  composition entries, processes, services, extension points, events, resources and materials. The wasm module
  supplies the functions those declarations name.
- **The core owns all state.** Plugins compute: they split, evaluate fields, run processes and answer services.
  They read state and write changes through the core.
- **Calls are batched.** Every call carries arrays of handles (or points), never one blob at a time.

### 4.2 The memory model: isolated instances

**The options:**

| | A. dynamic linking (shared memory + table) | B. isolated instances (proposal) | C. multi-memory: plugin imports a core "data arena" memory | D. component model |
|---|---|---|---|---|
| isolation | none: one memory, one table | full: each plugin has its own memory | partial: the arena is writable, the core heap is private | full |
| data path | zero-copy pointers | copy in / copy out per call (memcpy speed) | zero-copy for field columns | canonical ABI copies |
| call cost | `call_indirect`, ~ns | wasm→JS→wasm glue, ~0.1–0.3 µs per call (estimate) plus copies | direct | lifted/lowered, heavier |
| languages | only toolchains that build PIC side modules sharing one libc/allocator (C/C++ via Emscripten or wasi-sdk ≥ 21; Rust and Zig fragile; AssemblyScript and TinyGo no) | anything that emits a wasm module: C, C++, Zig, Rust, AssemblyScript, TinyGo | source languages can't address a second memory (no LLVM front-end support; see [multi-memory#45](https://github.com/WebAssembly/multi-memory/issues/45)) | Rust, C, JS, Python, Go via component tooling |
| loader | custom: `dylink.0`, `__memory_base`, `__table_base`, GOT, `__wasm_apply_data_relocs`, ctors | `WebAssembly.instantiate` with one import object per instance | standard | jco transpilation; no browser runs components natively (`wasmgpu/README.md`, 2026) |
| ABI stability | "This ABI is still a work in progress. There is no stable ABI yet." | ours: a C header we version | ours | WIT, standard but young |
| threads | every worker instantiates every module against one shared memory, with the table layout replicated | each worker instantiates the plugins it needs; no shared plugin memory | as B | as B |
| a crash | memory corruption somewhere, later | a trap in plugin X, attributed to X | a trap, or corrupt field data | a trap |

**Proposal: B, with static linking as an optimisation for trusted code.** The reasons, in order:

1. **Community plugins will have bugs.** Under A, one bad pointer in a community plugin shows up as a corrupt blob
   in the renderer an hour later. Under B, the plugin traps inside its own call and the core names it. This matters
   more than the copies.
2. **Language freedom.** The community will write in Rust, AssemblyScript (TypeScript-like and approachable), Zig,
   C, maybe TinyGo. Under A, all of them must agree on one allocator and libc, and the side-module support of most
   of those toolchains is fragile or missing.
3. **It fits stateless plugins and workers** (4.3, 4.9). An isolated instance can be replicated in any worker and
   reloaded at any time, because it holds no authoritative state. Under A, every thread must share the one memory
   in which all plugins and the core live, which drags the core's heap into every worker.
4. **The copies are cheap where it matters.** Section 4.2.1 has the costs.
5. **It degrades gracefully into static linking.** Under the pointer rule (5.1), a plugin compiled into the core
   binary calls the same `ss_*` functions with pointers into the one memory, and the glue disappears. First-party
   plugins can ship that way, with no source changes.

#### 4.2.1 What the copies cost (estimates from measured primitives)

| call | bytes moved | copy time at 4 MB / 0.09 ms | compared with |
|---|---:|---:|---|
| hydrology step on a 25 k-leaf cut: reads 9 f64 columns + CSR adjacency (~6 edges), writes 3 delta columns | ~2.4 MB in, 0.6 MB out | ~0.07 ms | the step itself: 4–11 ms in JS (`planet/DESIGN.md` §5) |
| a computed field over 100 k blobs, 3 stages, f32 | 3 × (0.4 MB in + 0.4 MB out) | ~0.05 ms | the field maths |
| split of 256 families × 400 children, ~48–64-byte child specs | ~4.9 MB out | ~0.11 ms | generation: ~5 ms per 400-blob family in JS (`RAYCUT.md` §9) |
| a single-handle service call | < 100 B | dominated by ~0.2 µs of glue | — |

The overheads that do matter are per call, not per byte, so the API is batched everywhere. That was the design
anyway. A process that makes thousands of tiny service calls per tick should batch them; the SDK makes batching
the default shape.

#### 4.2.2 The pointer rule (what makes B and static linking the same ABI)

> Every pointer passed across the ABI refers to the **caller's** memory and is valid **only for the duration of the
> call**.

- A plugin calling `ss_neighbors(dst, src, n, k, r)` passes `dst` and `src` in its own memory. The host copies `src`
  into core scratch, runs the query, and copies the result into `dst`.
- When the core calls a plugin export, the host first asks the plugin for a frame (`ss_frame_push(bytes)`), copies
  inputs there, calls, copies outputs back, then pops the frame.
- Nobody keeps a pointer after a call returns. That also removes the stale-array hazard (lesson 5) and the stale-view
  hazard after `memory.grow` (the glue re-derives views after every call that may grow either memory).
- Statically linked, the same calls pass pointers into the one shared memory. The host functions become direct calls,
  and the copies disappear.

### 4.3 Plugins hold no authoritative state

An instance's linear memory may hold only:
1. **static data**, built by a deterministic `init` from the manifest's params and the package's assets: type tables,
   behaviour-tree definitions, noise tables, lookup tables;
2. **scratch** that lives for one call (frames);
3. **caches** that are pure functions of core state. The rule: results must be the same whether the cache hits or
   misses. Caches are keyed by a version number the core provides (cut version, resource version), and the core may
   reset an instance at any time.

Everything else lives in the core:

| state | where | example |
|---|---|---|
| per blob, shared | stored fields, written as deltas ([09](09-fields-and-composition.md)) | `std:water`, `std:temperature` |
| per blob, private | stored fields in the plugin's namespace, `write: owned` (one writer may `set`) | a creature blob's wear, a plant blob's age |
| per entity | **16's thin entity tables** (working assumption, R10, pending `questions.md` C10): rows with a key, an owner, an anchor, flags and plugin-declared columns (`ss_ent_*`) | `beaver:beaver` rows with `hunger`, `action`, `target`, `home` |
| per entity, variable size | **entity-scoped resources** (15) keyed by the entity key | a beaver's path plan, memory of food sites |
| plugin-global | resources keyed by name | `climate:storms`, `volcano:list`, `hydro:outlets` |
| geometry | blobs (created and edited through the core) | trees, logs, lava lobes |

**What this buys:**
- **Persistence is the core's job.** Plugins don't write save code; the core saves fields and resources
  ([15](15-persistence.md)). Section 3 lesson 7 goes away.
- **Replication.** The same plugin can run on the engine thread and in four workers, because no instance is the
  "real" one.
- **Hot reload and fault recovery.** Throw away the instance, instantiate a new one, run `init`. Nothing is lost.
- **Networking.** All state that needs to be synchronised is in one place, with stable keys.
- **Transactions.** A call's writes are staged and committed only if it returns success (4.8). A trap leaves no
  half-written state.

**What it costs plugin authors:** state has to be declared and moved through the API. Take a behaviour tree:
- the *tree* is static (built in `init`);
- each creature's *execution state* (current node, timers, a small blackboard) is a fixed-size owned field, say
  32–64 bytes per creature root blob, or a resource keyed by the entity key when it varies in size;
- a process batch reads the states of the creatures it steps, advances them, and writes them back with one
  `ss_field_set` per column.

That's the ECS discipline. The SDK can hide most of it behind generated accessors (4.13).

**Where per-entity state lives.** The working assumption is 16's thin entity tables (R10; Joel decides in
`questions.md` C10). An entity kind declares its columns in the manifest (`entity_kinds`), and the core stores,
anchors, persists and (later) replicates the rows. If C10 rejects entity tables, the fallback is the one sketched
first here: owned fixed-size fields on the entity's root blob (`bytes:N`, N ≤ 64, [09](09-fields-and-composition.md))
plus entity-scoped resources for anything variable-sized.

**Why not let plugins keep state and serialise it on save?** It was considered and rejected:
- it breaks worker replication (which instance is right?);
- it makes saves depend on every plugin author getting serialisation right;
- it hides state from the core's determinism checks, its fingerprints and, later, its network sync.

### 4.4 Ownership and blob types

Joel's rule is that exactly one plugin owns a blob, the one that created it, and that plugin is asked to split it.
This doc adds one refinement, to make the rule enforceable and dispatch cheap:

> **A blob's owner is the plugin that defines its type. A plugin creates blobs only of its own types.** To make a blob
> of another plugin's type, it calls that plugin's service (e.g. `vegetation.make_log`).

- So the owner is a function of the type, and **owner dispatch is type dispatch**. A global type id is
  `(plugin u16, type u16)` (R12), and the plugin half *is* the owner.
- **Ownership never transfers** (R11; there is no `transfer_owner`). A felled log is vegetation's; the beaver may
  carry it as a physics object ([11](11-physics.md) `ss_object_hold`), but vegetation still owns it. When the beaver
  builds it into the dam, the beaver **places its own `dam_log` content** (kind D, `ss_place`, [15](15-persistence.md))
  and asks vegetation, through a service, to remove the felled log.
- Children made by a split are of the splitting plugin's types: a forest canopy (vegetation) splits into trees
  (vegetation), and a terrain blob into ground blobs (terrain).
- Ids are assigned at resolution, deterministically: plugin indices sorted by plugin id, type indices within a
  plugin sorted by type name. The save maps names to ids, so ids can change between versions
  ([15](15-persistence.md)).
- Each type also declares its **content kind** (A field-derived, B counted, C shaped, D placed; R7, [14](14-planet-generation.md)
  §4.8), which selects its split mode (§5.3) and its projection mode ([06](06-blob-hierarchy-and-cut.md)).

This is a refinement of the README's glossary entry ("the plugin that created a blob"). It's identical in the common
case.

### 4.5 The manifest

Joel's sketch:

```json
{
  "imports": { "terrain_generator_plugin.evaluate_field": "ground_field" },
  "exports": { "ground_field": "terrain_generator_plugin.evaluate_field" }
}
```

It binds wasm names to semantic names. The proposal keeps that idea:
- semantic names (fields, services, processes) are declared in the manifest;
- each entry names the wasm export that implements it;
- the wasm *imports* are always the fixed core API (module `ss`).

A plugin reaches "the previous `ground_field`" without binding a dedicated import:
- the core hands each composition stage `dst`, pre-filled with the value so far ([09](09-fields-and-composition.md)
  §4.4), which is exactly Joel's "first call the imported field to initialise `dst`";
- a stage that needs some other field, or this field at an earlier phase, calls `ss_field_compute(field, phase, …)`.

A full manifest (hydrology, abridged where marked):

```jsonc
{
  "manifest": 1,
  "id": "hydro",                         // namespace for everything below; [a-z][a-z0-9_]{1,31}
  "name": "Hydrology",
  "version": "0.4.2",                    // semver of this plugin
  "abi": 1,                              // major version of the ss ABI (5.1)
  "wasm": "hydro.wasm",
  "sha256": "9f2c…",                     // of the wasm; the package hash covers assets too (4.6)
  "generator_version": 3,                // 15: bump when generated output changes (4.12)
  "rebase_safe": false,                  // 15: true if a bump changed priors only, not keys or structure
  "license": "MIT",
  "requires": { "terrain": "^1.2", "climate": "^0.5" },
  "optional": { "snow": "^0.3", "volcano": "^0.2" },
  "params": { "runoff": 0.6, "burn": 4.0 },             // tunables; the world may override them (in the hash)
  "memory": { "max_mb": 256 },                         // above the 64 MB default (R20): must be declared, and the
                                                      // load report shows it; the core may still cap it
  "workers": true,                                    // replicas allowed (4.9)

  "stewards": [                                       // std stored fields whose specialised lift/push this plugin supplies (09 §4.8)
    { "field": "std:water", "lift": "fill", "push": "amount" }
  ],
  "fields": [                                         // fields this plugin DEFINES (schema owner, 09 §4.1)
    { "name": "flow",  "domain": "blob", "kind": "stored", "dtype": "f64", "unit": "m3",
      "agg": "sum", "lift": "transit", "lift_reads": ["std:ground_height"], "push": "amount",
      "write": "delta", "conserved": true, "persist": "never" },
    { "name": "discharge", "domain": "blob", "kind": "stored", "dtype": "f32", "unit": "m3/s",
      "agg": "max", "lift": "export:lift_discharge", "push": "none", "write": "owned", "structured": true,
      "persist": "never" }
  ],
  "reads": [                                          // fields used but not defined here
    "std:ground_height", "std:precipitation", "std:temperature",
    { "field": "volcano:lava", "default": 0 }         // absent plugin => constant 0, listed in the load report
  ],
  "provides": [                                       // composition stages on fields defined elsewhere (09)
    { "field": "std:wetness", "phase": "base", "types": "own", "export": "wetness_base" },
    { "field": "std:flammability", "phase": "environment", "op": "mul", "export": "flammability_wet",
      "reads": ["std:water", "std:porosity"] },
    { "field": "std:flow_velocity", "phase": "base", "export": "flow_velocity_at" }   // point domain (11 reads it)
  ],
  "lifts":  { "fill": "lift_fill", "transit": "lift_transit" },   // named priors -> wasm exports (09 §4.6)

  "types": [                                          // blob types this plugin owns (4.4); kind per R7
    { "name": "water_sheet", "kind": "A", "material": "water", "flags": ["sheet", "fluid"],
      "fields": { "std:density": 1000 } },            // type-table constants (09 §4.3)
    { "name": "river_water", "kind": "A", "material": "water", "flags": ["sheet", "fluid"],
      "fields": { "std:density": 1000 } }
  ],
  "split": { "propose": "split_propose_water", "realize": "realize_water" },   // kind A: two phases (5.3, 06, 14)

  "processes": [                                      // keys per 10 §4.3
    { "name": "route", "export": "proc_route", "dt": 10800, "phase": "auto",   // phase: tick offset (10 §4.3)
      "band": "finest", "scope": "body",
      "where": "async", "latency_ticks": 30,          // 10's async(30)
      "max_skip_steps": 64, "on_skip": "route_skip",  // only skip schedules stretch steps (10 §4.5)
      "cost_hint": 1.2,
      "reads":  ["std:ground_height", "std:precipitation", "volcano:lava@final"],
      "transfers": [["std:water", "hydro:flow"], ["hydro:flow", "hydro:flow"], ["hydro:flow", "std:water"]],
      "writes": ["hydro:discharge"],
      "calls":  ["ext:hydro:source"] }
  ],
  "services": {
    "exports": [ { "name": "steady", "export": "svc_steady", "args": [], "where": "sync" },
                 { "name": "add_water", "export": "svc_add_water",
                   "args": [["handles", "u32[]"], ["volume", "f64[]"]], "returns": [] } ],
    "imports": [ ]
  },
  "extension_points": {
    "defines": [ { "name": "source", "args": [["handles", "u32[]"], ["precip_volume", "f64[]"], ["h", "f64"]],
                   "returns": [["net_volume", "f64[]"]], "combine": "sum" } ],
    "contributes": [ { "point": "terrain:lift_conditioning", "export": "condition_rivers",
                       "reads": ["std:ground_height@base"] } ]       // river beds, valley floors (14, R17)
  },
  "events": {                                         // format owned by 17
    "hydro.surge":       { "trigger": "threshold", "magnitude": { "unit": "log10_m3" } },
    "hydro.lake_formed": { "trigger": "threshold", "magnitude": { "unit": "log10_m3" } }
  },
  "subscribe": ["core.cut_changed"],                  // delivered to on_events at the next tick (17)
  "resources": [ { "id": "outlets",  "scope": "region", "schema_version": 2 },    // 15's scoped resources
                 { "id": "segments", "scope": "body",   "schema_version": 1 } ],
  "materials": {
    "render": [ { "name": "water", "base": "std:water", "params": { "rough": 0.06 } } ],
    "fast": [ { "name": "water", "rheology": "voellmy", "mu": 0.0, "xi": 800,
                "on_rest": "svc:add_water" } ]
  },
  "assets": [ ]
}
```

**Notes on the shape:**
- **Everything structural is static.** The resolver can check the whole plugin set, and a registry or a tool can
  inspect a plugin, without running any of its code. `init` only fills data. Runtime registration is not offered:
  it would defeat validation.
- **Names are namespaced.** A plugin's own fields, types, processes and resources are `id:name`. `std:` names are
  defined by the core's curated vocabulary ([09](09-fields-and-composition.md) §4.8).
- **`reads` and `writes` are declarations**, not wishes. The core enforces them:
  - a call to `ss_field_add` on an undeclared field fails;
  - the scheduler uses them to build job snapshots and to order work ([10](10-simulation-and-scheduling.md)).
- **`@phase` suffixes** in `reads` (`volcano:lava@final`) are the formal form of Joel's `@checkpoint`s
  ([09](09-fields-and-composition.md)).
- **Processes, watches and fast-layer materials exist only as manifest entries** (R2).
  - Process keys are 10's (§4.3): `dt` (converted to a whole-tick `period`), `phase` (the tick offset; `auto` =
    `hash(id) mod period`), `band`, `scope`, `reads` / `writes` / `transfers` (declared conserved pairs),
    `where` + `latency_ticks`, `max_skip_steps`, `on_skip`, `cost_hint`. Steps are never stretched in normal play;
    `max_skip_steps` applies only to skip schedules (10 §4.5). The prototype's `maxSteps` stretching is gone.
  - **Watches** (10 §4.12): threshold-crossing triggers on stored fields, for load → release → cascade processes.
    The core evaluates them only on handles whose value changed, and calls the export at stage 4 of the next tick:

    ```jsonc
    "watches": [ { "name": "slab_load", "field": "snow:over", "cmp": "cross_up",
                   "threshold": 0.5, "quantum": 0.5, "export": "on_slab_load" } ]
    ```
  - Fast-layer materials (`materials.fast`) carry the rheology family and parameters and the batched hooks
    (`step`, `deposit`, `land`, `on_skip`), with the keys [11](11-physics.md) §5 lists.
- **Type entries** carry three kinds of per-type constants:
  - `fields`: type-table values for computed fields ([09](09-fields-and-composition.md) §4.3);
  - `physics`: per-type physics constants that are not fields, read by [11](11-physics.md) (`restitution`,
    `friction` default, `settle` policy (`keep_instance` / `join_ground`), `contact_level`, `bedrock`):

    ```jsonc
    { "name": "dam_log", "kind": "D", "material": "beaver:wood",
      "fields":  { "std:density": 550, "std:friction": 0.6 },
      "physics": { "settle": "keep_instance", "contact_level": -1, "restitution": 0.1, "bedrock": false } }
    ```
  - `kind`, `material` and flags (05).
- **Roles** (14): a plugin that generates a body's ground declares `"roles": ["ground"]` with an `applies_to`
  predicate over body kinds (14 §5.4). The resolver requires exactly one ground provider per body kind in use.

### 4.6 Resolution: from packages to a plugin set

At load, in this order (all on the engine thread, all before tick 0):

1. **Fetch and hash.** Each package's hash is SHA-256 over the manifest, the wasm and the assets, in a canonical
   order. The world's **plugin lock** lists `(id, version, package hash, params)`. A mismatch is an error unless the
   player chose to upgrade (4.12).
2. **Validate each manifest**:
   - the JSON schema;
   - ABI version supported;
   - names well formed;
   - every named export exists in the module, with the right signature;
   - the module's imports are only from `ss`.

   Then **scan the code section** ([4.11](#411-limits-faults-and-security)): reject relaxed-SIMD opcodes and other
   non-deterministic features ([02](02-numerics-time-and-determinism.md)).
3. **Order the plugins** by id. This canonical order is used everywhere a tie needs breaking:
   - type and field id assignment;
   - the order in which commutative contributions are combined;
   - the order of extension-point contributions.

   It never depends on download order.
4. **Resolve dependencies** (semver ranges, `requires` and `optional`). Missing required → error. Missing optional →
   that plugin's reads of the absent namespace become constants.
5. **Build the field schema.**
   - Merge every definition; conflicts are errors (as in `planet/js/plugin.js`, which checked `agg`, `lift` and
     `unit`).
   - Fields read but defined by nobody become constants at their declared default.
   - Build the **`(field, phase)` graph** and the evaluation plans; detect cycles
     ([09](09-fields-and-composition.md) §4.5).
   - Sort lift order by `lift_reads`.
6. **Build the service and extension-point graph.**
   - Edges come from `services.imports`, `calls` and `provides[].reads`.
   - A cycle between plugins through synchronous services is an error, because it would re-enter an instance
     unboundedly (4.8).
7. **Assign ids** (types, fields, processes, services, events, resources), deterministically.
8. **Compute the plugin-set hash** over the resolved set: the lock entries plus the effective params. This goes in
   every save and, later, in every multiplayer handshake.
9. **Write the load report**:

   ```
   plugins: climate 0.5.1, fire 0.2.0, hydro 0.4.2, snow 0.3.0, terrain 1.2.4, vegetation 0.6.0
   set hash: 4c1f…  (generator_version: terrain 7, hydro 3, vegetation 2)
   fields: 31 (std 14, plugin 17); constants: volcano:lava = 0 (read by hydro, fire)
   std:flammability  base[type tables: vegetation, hut] · environment(mul)[hydro, snow] · clamp[core]
   std:ground_height base[terrain] · shape(add)[volcano] · cover(add)[snow] · override[ordered: —]
   ! hut:foundation and road:grade both override std:ground_height with no order: tie broken by id (hut first)
   ```

### 4.7 Lifecycle

```
fetch → validate → resolve (set) → compile → instantiate (engine thread)
      → ss_init(params)                 static data, type tables; must be deterministic
      → world attach                    core.world_begin event; 'generate' extension points (14)
      → run                             split / fields / processes / services / events, every tick
      → (save: nothing to do)           the core saves fields and resources
      → unload                          drop the instance
```

- **Compile once, instantiate many.** A `WebAssembly.Module` is compiled once and posted to the workers that host
  replicas (4.9). Each replica runs `ss_init`, so `init` must be fast (milliseconds) and deterministic.
- **Hot reload (development).**
  - The dev server pushes a new package.
  - At the next tick boundary the core re-resolves the set. If structure changed, it reports what changed: new or
    removed fields, changed phases.
  - It then instantiates and inits the new module and swaps it in on every thread.
  - World state survives, because it's in the core:
    - new fields start at their lift or default;
    - removed fields are kept dormant until the save is pruned;
    - a changed field schema runs the plugin's migration (4.12).
- **Adding a plugin to an existing world** is a resolution with one more package. Its stored fields lift from
  defaults over the cut, and its generator only affects blobs that split from then on.
  - The doc 15 rule applies: regions generated earlier don't regenerate. Their residuals are relative to the old
    generator. See 4.12.
- **Removing a plugin.**
  - Its blobs can't be split, simulated or composed any more. They stay as inert atoms of a placeholder type.
  - Its fields and resources stay in the save, dormant. The player is warned.

### 4.8 How calls work

**Synchronous calls on the engine thread** are used by:
- split;
- field stages;
- services;
- event handlers;
- short processes.

The call protocol for core → plugin:
1. The host asks the plugin for a frame: `ptr = ss_frame_push(bytes)`. The SDK implements it as a bump allocator
   with frames, growing memory if needed.
2. The host copies inputs into the frame (handles, prefetched columns), then calls the export with pointers into
   that frame.
3. During the call, the plugin may call `ss_*` imports. Each is a host function bound to this instance; it copies in,
   runs core code, and copies out.
4. On return `0`, the host copies outputs out and **stages the call's writes for commit**: deltas, transfers, sets,
   creates, destroys, geometry edits, events. They are committed at 10's next merge barrier (stage 5), in 10's
   canonical order. Then `ss_frame_pop()`.
5. On a trap or a non-zero return, the staged writes are discarded and the frame is reset. The fault is recorded
   against the plugin (4.11).

**Staged writes make calls transactional.** They also make determinism easy. Writes from one call are applied
together, in the call's own order, at 10's merge barrier. A plugin never observes its own uncommitted writes through
reads, and nobody else observes them either. Reads always see the stage-start snapshot (10 §4.2, Jacobi semantics).

**Re-entrancy** happens legitimately, e.g. when plugin A's field stage reads a field one of whose stages A also
provides.
- It is allowed, because frames nest (each call pushes its own).
- The resolver has already rejected cycles in the field and service graphs.
- The host enforces a depth limit (default 8). Exceeding it is a fault, not a hang.

**Creating blobs inside a call.**
- `ss_blob_create` returns handles immediately: they are allocated but not yet visible to queries.
- Keys come from the creator's context: the parent key plus an ordinal, or the plugin's key plus a counter seeded
  from the call ([05](05-blob-model.md)).
- If the call fails, the handles are released.

**Split is pure, and its mode follows the content kind** (R7, §5.3).
- The split exports only read their inputs (parent records, the 26-neighbourhood, fields and resources through
  `ss_*` reads) and return records. The core creates the children, assigns keys, applies residuals and projections,
  and runs field lifts ([06](06-blob-hierarchy-and-cut.md)).
- **Kind A** (field-derived: ground, water sheets, snow) is two-phase. `split_propose` returns the children's
  ordinals and field priors. The core projects the fields and applies residuals. Then `realize` builds geometry
  (plates, sheets, keels) from the final fields ([14](14-planet-generation.md) §4.7). There is no geometric
  projection for kind A.
- **Kind B** (counted) is two-phase like kind A: `split_propose` returns ordinals and the count priors, the core
  projects the counts, and `realize` turns them into individuals from ranked candidates. There is no geometric
  projection.
- **Kind C** (shaped) is one phase: `split` returns child specs (§5.4), and the core applies the full moment
  projection (idea 5).
- **Kind D** (placed) never splits. Placed records have their own object pyramid.
- Purity is what lets one export serve both sim-cut splits and the renderer's display-only splits (06's two-tier cut,
  adopted here), and what lets it run in workers.
- That matters: generation throughput, not the GPU, limited the ray cut's convergence (~1–2 k splits/s, `RAYCUT.md`
  §9).

### 4.9 Workers and deterministic async

**Placement.** Every plugin has one instance on the engine thread. Plugins with `"workers": true` also get a replica
in each worker of the plugin pool (pool size `hardwareConcurrency − 2`, tunable). Replicas are identical: the same
module, the same `init`.

**Async jobs** are used for long processes (`"where": "async"`) and bulk generation (split batches):

```
tick T:    the core snapshots the job's declared inputs (reads at their phases, the band graph in CSR form,
           positions, levels, area/volume, the versions of the resources it reads) into a staging buffer
           and posts the job (id, process, h, rng seed, snapshot) to a worker
worker:    runs the plugin's process export against the snapshot through the job API (below); writes
           outputs (deltas per declared write field, events, create/destroy requests) to an output buffer
tick T+k:  the core applies the outputs, in canonical order with everything else due at T+k
           (k = latency_ticks; 10 §4.6 owns the rule; climate and hydrology are async(30), R1)
           if the job hasn't finished, the engine waits for it. It never proceeds without it.
```

**Why this is deterministic.**
- The inputs are a snapshot taken at a fixed tick, and the outputs are applied at a fixed tick. Neither depends on
  how fast the worker was.
- Waiting when a job is late costs frame time, never correctness. The scheduler's budget ([10](10-simulation-and-scheduling.md))
  should make waits rare, and its telemetry shows when they aren't.

**The job API is restricted.** A worker has no core, so a job can't run live queries.
- What it can call (the "job API", a subset of `ss`, the same function names):
  - reads of the snapshot (`ss_field_read` on prefetched fields, `ss_graph_rows` over the snapshot's band graph,
    `ss_resource_get`, `ss_ent_read`);
  - delta, transfer and owned writes on declared fields (`ss_field_add`, `ss_field_transfer`, `ss_field_set`,
    `ss_field_push`);
  - event records (`ss_event_*`, 17);
  - create and destroy requests, applied at T+k;
  - pure helpers (the RNG, the SDK's maths).
- Anything else fails at validation if the process is declared async: the resolver checks a process's `calls` and
  `reads` against what a snapshot can hold.
- Processes that need live queries (creature perception with rays) run synchronously, or in two stages: a sync
  stage gathers candidates with queries, and an async stage thinks.

**Splits in workers.**
- Split jobs carry the parent records, the neighbour records (the neighbour invariant, [06](06-blob-hierarchy-and-cut.md)),
  and snapshots of the resources the split reads (e.g. `volcano:list` for cone shapes).
- If the split evaluates a composed point field such as `std:ground_height`, every plugin that contributes to it
  must have a replica in that worker. The resolver computes that closure and loads those replicas.
- Render-tier splits (render-only blobs, R1) have no latency requirement. Sim-tier splits use the same rule with
  06's `L_split` = 3 ticks.

**Shared memory.** Staging buffers are `SharedArrayBuffer`s when the page is cross-origin isolated (COOP/COEP,
[01](01-runtime-and-toolchain.md)). Otherwise they are transferred `ArrayBuffer`s, with one more copy. Plugin
memories themselves are never shared.

### 4.10 How plugins interact

There are five mechanisms. Each has a clear use, and the manifest declares all of them.

| mechanism | use it when | order semantics | prototype precedent |
|---|---|---|---|
| **fields** (stored: deltas; computed: stages) | many plugins contribute to or read a quantity with simple semantics | deltas commute; stages follow phases ([09](09-fields-and-composition.md)) | `z`, `snow`, `flow` written by several plugins |
| **services** | an operation on structure only its owner understands | a direct call, synchronous or job-local | `vegetation.killTrees`, `snow.melt`, `fire.markBurnt` |
| **extension points** | the definer asks "everyone, contribute to this" | contributions combined by a declared combinator (sum, min, max, concat-sorted) in canonical plugin order | `hydroSource`, `generate` |
| **events** | something happened that others may react to later | records delivered to `on_events` at the next tick, in canonical order (tick, type, key; 17) | snow avalanche events, lava `touched` marks |
| **resources** | plugin-global, body, region or entity-scoped state (15) | single writer (the owner); others read through services | storms, volcano list, river segments, outlets |

**Services: why not just let plugins write each other's fields?**
- Because lesson 3 says that's wrong whenever the field has structure.
- The rule: a plugin may write deltas and transfers to any *shared* field it declares in `writes` / `transfers`.
  For *owned* fields, and for anything structured, it calls the owner's service.

**Service ABI.**
- A service is declared with typed array arguments (`u32[]`, `f32[]`, `f64[]`, `u8[]` and scalars).
- The SDK generates a typed wrapper from the manifest. It packs the argument descriptors and calls
  `ss_call(service_id, argv, argc, retv, retc)`. The host copies each array into the callee's frame, calls the
  callee's export, and copies the results back.
- Statically linked, this is a direct call.

**Extension points** are how core structure and plugin structure meet (lesson 6).
- The terrain plugin (the ground provider, 14) defines the two that replace the prototype's core hacks (R17):
  - `terrain:lift_conditioning`: per-child bounds on the smooth part during the lift. Hydrology contributes river beds
    and valley floors, reading `std:ground_height@base` (the ground without shape terms), which generalises `nzT` and
    `noRivers`;
  - shape terms are not an extension point any more: they are contributions to the `shape` phase of
    `std:ground_height` ([09](09-fields-and-composition.md) §4.12b).
- The core defines `core:generate` (run once per body at world attach) and emits the `core.cut_changed` event (a
  steady refresh after refinement).
- Plugins define their own, such as `hydro:source`.

**Events** ([17](17-events-and-discovery.md) owns the record and the API).
- Event types, with their payload schema, rate priors and notability, are declared in the manifest; subscriptions
  too (`"subscribe": [...]`).
- Records are begun, updated and ended through `ss_event_begin/update/end`, staged with the call's writes, and
  delivered to subscribers' `on_events` at the next tick in canonical order.
- The same log feeds the director, the discovery journal and persistence.

### 4.11 Limits, faults and security

**What an untrusted plugin can and can't do in the browser.**
- A wasm instance can only call what it imports. Here that's only `ss`: no DOM, no network, no storage, no clock, no
  ambient randomness.
- So a plugin can't exfiltrate data or touch the page. The risks that remain are:
  - denial of service: CPU or memory;
  - corrupt *content*: bad field values (NaN), absurd blob counts;
  - non-determinism, if a feature leaks it.

**The defences:**

| risk | defence |
|---|---|
| a trap (out-of-bounds access, `unreachable`, divide by zero) | caught by the host; the call's staged writes are discarded; the fault is counted; after N faults (default 3 in 10 s of game time) the plugin is **suspended**, with its blobs inert and the player told. Traps are deterministic, so every peer suspends at the same tick. |
| an infinite loop or very long call | no preemption exists. **Deterministic fuel metering:** at load the host rewrites untrusted modules to decrement a fuel global at function entries and loop back-edges, and to trap at zero. The budget per call is declared in the manifest (with a core maximum). It is deterministic (counts, not time), so every peer traps identically. Cost estimate: ~5–15% slower (to measure, 7). First-party and trusted plugins run unmetered. As a last resort, a main-thread watchdog restarts the engine worker from the last autosave without the plugin. |
| memory exhaustion | each instance's memory is created with a maximum: 64 MB by default (R20), or the manifest's `max_mb` if declared (shown in the load report, capped by the core); `grow` beyond it fails and the plugin traps |
| absurd outputs | the core validates on commit: finite values on fields marked `finite`, blob count quotas per call and per tick, levels within range, geometry sane (radii > 0, k ≥ 0). A violation is a fault. |
| non-determinism | imports only from `ss`; relaxed SIMD rejected by a code-section scan at load; no NaN-payload-dependent behaviour (the core canonicalises NaNs on commit, [02](02-numerics-time-and-determinism.md)); RNG only through the core's counter-based streams |
| a wall-clock budget | **never** used to change results: wall time differs per machine. Slow plugins show in telemetry and the player can disable them. That decision is an input command, so it's deterministic too. |

**Multiplayer, later.** All peers must run the identical plugin set (the same set hash). An untrusted plugin can't
cheat, because a peer running different code diverges and is detected. Cheating is a host-authority problem
([02](02-numerics-time-and-determinism.md)), not a plugin one.

### 4.12 Versioning and compatibility

- **ABI version** (`"abi": 1`).
  - Additive changes (new `ss_*` functions) keep the major version, and a plugin's import list reveals what it uses.
  - A breaking change bumps the major version, and the core supports N and N−1 during a transition.
- **Plugin semver.**
  - Patch: behaviour fixes, no schema change.
  - Minor: new fields, types, services.
  - Major: breaking schema or semantics.
- **Schema migrations.** If a plugin changes a stored field's dtype, unit or meaning, or a resource schema, it
  exports `migrate_field` / `migrate_resource`. Old data goes in, new data comes out, in batches. The core runs
  migrations at load when the save's lock has an older version.
- **The generator version (the hard one).**
  - Persistence stores residuals and edit logs *relative to what the generator produces* ([15](15-persistence.md)).
    If a split prior or lift prior changes, an old residual applies to a different baseline. That is how the prototype
    learned that priors may only read persisted state.
  - So each plugin declares `"generator_version": n` (and `rebase_safe`), as [15](15-persistence.md) §5.4 specifies.
    The save pins `(plugin id, generator_version, module hash)` for every plugin that owns generated content.
  - Doc 15 owns the policy (pinning old modules, rebasing touched families, reporting dropped overrides); Joel's
    choice is pending in `questions.md` C9. This doc only requires that generator changes are declared, never
    silent, and that the resolver can load a pinned older module beside a newer one.
- **The `std` vocabulary** is versioned with the core. Fields are added, never repurposed. A deprecated field keeps
  working until the next ABI major version.

### 4.13 The SDK and languages

- **One IDL is the source of truth for the ABI** (a JSON description of every `ss_*` import and every export
  signature kind). Four things are generated from it:
  - `ss.h`, the C header (the normative ABI);
  - Zig and Rust bindings: thin wrappers with slices instead of `(ptr, len)`, and typed handles;
  - AssemblyScript declarations;
  - **the JS host glue itself**: per-import copy-in/copy-out functions.

  Generating the glue and the bindings from one file keeps them from drifting.
- **Codegen from the manifest:**
  - typed service wrappers;
  - field and type id constants (resolved at `init` from names);
  - accessors for owned fields (`beaver.hunger.read(handles)`);
  - resource (de)serialisers from declared resource schemas.
- **The SDK runtime per language:**
  - a frame allocator;
  - a deterministic libm (no imports);
  - **static libraries shared with the core** (R4, 14): the S2-style cube-sphere cell index maths (`ss_cell_*`:
    ids, parents, children, neighbours, areas, points), compiled into the plugin so cell maths costs no call and gives
    bit-identical results in the core and in every plugin;
  - the RNG (the counter-based hash from [02](02-numerics-time-and-determinism.md), implemented inline, so it costs
    no call);
  - helpers for batch loops.
- **Supported languages:**
  - first: C, Zig, Rust (all via LLVM; `wasm32-freestanding` / `wasm32-unknown-unknown`, with no WASI imports);
  - soon: AssemblyScript (the most approachable for the community; its GC'd heap is fine in an isolated instance,
    which is another argument for B);
  - possible: TinyGo, C++.
- **Templates:**
  - a "blob type with a property" plugin (the hut);
  - a process plugin;
  - a creature plugin.

### 4.14 Packaging and distribution

- **A package (`.skp`)** is a zip:
  - `manifest.json`;
  - the `.wasm`;
  - `assets/`: blob models such as creature atom lists and structure templates, type tables, material tables, all in
    documented formats ([05](05-blob-model.md), [16](16-entities-and-creatures.md));
  - `README.md`, a licence, an icon.
- Plugins read assets at `init` through `ss_asset_read(name, dst, cap)`. Assets are covered by the package hash.
- **Loading is by URL now:** a world lists package URLs and hashes; CORS is required.
- **A registry later:** signed packages, versions, compatibility metadata, the load report shown before install.
- **First-party plugins** ship as packages too. For speed, the build can also link them statically into the core
  (same source, same manifest). The core then marks them `trusted`, so no copies and no fuel metering.

### 4.15 Testing plugins

- `ss-test`, in Node, runs the real core and the plugin set headless (19):
  - **fingerprints:** field totals and a state hash after N ticks; `planet/tools/fingerprint.mjs` made the port
    provably bit-identical;
  - **the determinism check:** the same run with plugin load order permuted, workers on and off, and different
    `latency_ticks` replicas, gives the same hash;
  - **consistency:** restrict∘lift = identity to tolerance on every stored field after splits ([09](09-fields-and-composition.md));
  - **fuzzing:** random handles, random splits and merges, malformed inputs to services. The plugin must not trap.
- A plugin's own unit tests run in its language against a mock `ss` (generated from the IDL).

### 4.16 Example: climate and hydrology as WASM plugins

**Climate**, ported from `planet/js/plugins/climate.js`.

The manifest essentials:

```jsonc
{ "id": "climate", "version": "0.5.0", "abi": 1, "generator_version": 1, "requires": { "terrain": "^1" },
  "stewards": [                                     // std stored fields climate supplies priors for (09 §4.8)
    { "field": "std:temperature", "lift": "lapse", "push": "mean" },        // K, mean
    { "field": "std:precipitation", "lift": "smooth", "push": "scale" },    // m/s water equivalent, mean, nonneg
    { "field": "std:aerosol", "lift": "smooth", "push": "mean" }            // optical depth (13), finest ℓ_sim_floor
  ],
  "lifts": { "lapse": "lift_lapse" },
  "reads": ["std:ground_height"],
  "resources": [ { "id": "storms", "scope": "body", "schema_version": 1 } ],
  "processes": [ { "name": "step", "export": "proc_climate", "dt": 43200, "band": "ℓ_sim_floor", "scope": "body",
                   "where": "async", "latency_ticks": 30, "phase": "auto", "max_skip_steps": 64,
                   "reads": ["std:ground_height", "std:temperature", "std:precipitation", "res:climate:storms"],
                   "writes": ["std:temperature", "std:precipitation", "std:aerosol", "res:climate:storms"] } ],
  "services": { "exports": [ { "name": "storm", "export": "svc_storm",
                 "args": [["dir", "f64[3]"], ["radius", "f64"], ["life", "f64"], ["amp", "f64"], ["fixed", "u8"]] } ] } }
```

The exports:
- `ss_init`;
- `lift_lapse(field, parents, n, child_off, children, m, dst)`: the prior. The core projects to the parent's mean
  afterwards (`PL.projectMean` moves into the core);
- `proc_climate(ctx)`. It reads the top band's graph, positions and elevation from the snapshot. It recomputes
  `up[]`/`lat[]`/`wind[]` as a **cache** keyed by the top band's cut version; the top level never changes, so this is
  built once per replica. It runs the moisture iteration, advances the storms resource, and writes temperature and
  precipitation as **push-down deltas** (`ss_field_push`) on top-band handles;
- `svc_storm`: appends a record to `climate:storms` through `ss_resource_put`.

What changed from the prototype:
- `sim.storms` → a persisted resource;
- `climateStatic` → a cache;
- `PL.rng` → 02's counter-based stream (`ss_stream("climate:step")`, drawn by tick);
- `maxSteps` step stretching → 10's canonical schedule, with stretched steps only in skips.

**Hydrology**, ported from `planet/js/plugins/hydrology.js`. The manifest is the one in 4.5.

The exports:
- **`proc_route`** (async, `async(30)`). It reads the whole cut's graph (band `finest`, through `ss_graph_rows` on
  the snapshot) and calls the `hydro:source` extension point.

  An extension point inside an async job means the contributors (snow) must run in the same job:
  - The resolver sees `calls: ["ext:hydro:source"]` and requires replicas of every contributor in the worker.
  - The snapshot includes the contributors' declared reads.
  - The contributors' writes (snow changing `std:snow`) join the job's output deltas.

  This is the one place where an async job spans plugins. It's declared, so it's checked.
- `lift_fill`, `lift_transit`, `lift_discharge`: priors.
- `split_propose_water` / `realize_water`: the two-phase kind-A split of water sheets (5.3).
- `condition_rivers`: hydrology's contribution to `terrain:lift_conditioning` (river beds, valley floors; 14, R17).
- `on_events`: on `core.cut_changed`, schedules a steady refresh, which replaces the prototype's `afterCut` hook
  calling `hydrology(sim, DAY, true)`.
- `svc_add_water`: the fast layer's `on_rest` target, used when water parcels hand their volume back.

Routing state that the prototype kept as `w.outlets` and `w.rseg`:
- it becomes resources (`hydro:outlets`, `hydro:segments`);
- the "touched" flags it read (`w.dirty`, `w.edited`) are core blob flags, readable through `ss_blob_get_meta`
  ([05](05-blob-model.md), [15](15-persistence.md)); outlets are a region-scoped resource, which is itself a touch
  (15).

### 4.17 The canonical scenarios

- **2. The storm and the flood.**
  - Climate's async process advances the storms resource and pushes precipitation deltas down from the top band.
  - Hydrology's async process routes the whole cut and calls `hydro:source`, where snow contributes.
  - All jobs apply at fixed ticks.
  - Nothing in this path depends on where the camera is; only the cut's shape does.
- **3. The beaver dam.**
  - The beaver plugin owns `beaver` types and an entity kind with columns (`action`, `hunger`, `target`, `home`; 16's
    tables, R10); a per-beaver plan is an entity-scoped resource.
  - Its sync process uses `ss_neighbors` and `ss_raycast` to find trees and the creek. It calls
    `vegetation.fell(...)`, which turns a tree into a vegetation-owned log object, and carries it
    (`ss_object_hold`, 11).
  - At the dam site it places its own `dam_log` content (kind D, `ss_place`) and calls vegetation's service to remove
    the carried log (R11: no ownership transfer).
  - Placed content is persisted as placed records and touches its region. Hydrology's next route reads the blockage
    and the creek backs up.
  - Hydrology emits a `hydro.lake_formed` event record, which the journal picks up.
  - Nothing is plugin memory: on save, beavers, logs, the pond and the outlets are all core state.
- **4. The asteroid.**
  - An event plugin (or the core's impact system, [17](17-events-and-discovery.md)) owns the impactor's types.
  - At impact, it requests refinement to the impactor's level (`ss_refine_request`, effective at T + `L_split`). The
    crater is a kind-A edit through the terrain plugin's service (a `z` edit plus 17's regenerable crater record, R7),
    so `std:ground_height` and every level re-derive it. Ejecta are parcels with a debris fast-layer material declared
    by the terrain plugin.
  - Fire subscribes to the impact event record, and ignites.
  - Every write is staged per call and every call is transactional, so a fault in the fire plugin can't leave a
    half-made crater.
- **6. A community plugin (the hut).**
  - The manifest declares `hut` types and **type tables** for `std:flammability`, `std:density` and
    `std:hardness` ([09](09-fields-and-composition.md) §4.3). No code runs for those.
  - A hut's root splits into its parts from a template asset (kind C, one-phase `split`). A hut the player builds is
    a kind D placed record (`ss_place`).
  - It runs isolated and metered.
  - The load report shows its stages in each field's plan.
  - Fire burns huts with no change to the fire plugin.

### 4.18 Alternatives considered

| alternative | why not (now) |
|---|---|
| dynamic linking for every plugin | no isolation, a fragile toolchain story outside C/C++, an unstable ABI, threads drag the shared heap everywhere (4.2). Kept as a fallback for trusted bundles if measurements demand it. |
| plugins as JS modules (as in `planet/`) | Joel wants plugins in WASM; JS plugins can't be made deterministic across engines (`Math.sin` differs), and they'd share the realm with the engine |
| runtime registration (`ss_register_field` during `init`) | the set can't be validated before code runs, tools can't inspect packages, the result depends on what `init` did. The manifest covers every structural need. |
| plugins keep their own state and implement save/load | breaks worker replicas, hot reload, determinism checks and network sync (4.3) |
| one instance per plugin, in a worker per plugin (actor model) | every sync call (split, field stage, service) becomes a cross-thread round trip, and determinism needs a global ordering of messages anyway. Replicas plus fixed-latency jobs give the parallelism without the round trips. |
| WASI interfaces | WASI is about OS-like capabilities (files, clocks, random) that plugins must *not* have. The ABI is game-specific. |
| the component model | the right long-term standard for typed cross-language calls, but no browser runs it natively yet (jco transpiles). Revisit when browsers ship it; the IDL (4.13) could then emit WIT. |

## 5. Interfaces

### 5.1 ABI conventions

- Target `wasm32`. All pointers and sizes are `u32` in the caller's memory, valid only during the call (4.2.2).
- Handles are `u32`, with `0` meaning none ([05](05-blob-model.md), D6). Keys are 128-bit (`key128`, two `u64`
  halves; R3) and appear only where persistence or identity needs them.
- Floats always go through memory, never as call arguments, except a few scalars such as `dt`. Boxed f32 arguments
  cost 59 ns per call (`wasmgpu/`).
- Positions in a body frame are `f64[3]`. Positions relative to a parent blob (kind B/C child specs) are `f32[3]`.
- Return value `i32` (or a `Status` where the owning doc defines one: `OK`, `PARTIAL`, `STALE`, `NOT_OWNER`,
  `INVALID`, as in 05 and 07): `0` is ok, `> 0` is a count where documented, `< 0` is an error code. An error
  discards the call's staged writes.
- Batches are struct-of-arrays: `(dst, handles, n)`. A row past the end of a result is zero-filled (Joel's
  `neighbors` convention). An optional per-row status array reports row-level problems (07 §5).
- Every id (field, type, service, process, event, resource, material) is assigned at resolution. Plugins look them
  up by name at `init` (`ss_id("std:flammability")`) and keep them in static data. A global type id is
  `(plugin u16, type u16)` (R12).
- **Prefix rule (R2).** Every plugin-facing import is `ss_*`, in module `ss`. Engine-internal functions (renderer,
  lighting, persistence worker) have no prefix and are not importable.
- **Declared, not registered (R2).** Types, materials (render and fast-layer), processes, entity kinds, events,
  resources and models are manifest entries. There is no `*_register` import. Where an owning doc sketches one
  (05's `type_register` / `material_register`, 11's `ss_material_register`, 16's `model_register`), it becomes a
  manifest entry with the same fields, and plugins look up the id with `ss_id`. 02's `ss_stream` (RNG stream names)
  is the one init-time call that names something, and it allocates nothing persistent.

### 5.2 Core imports (module `ss`): the canonical list

This is the single list of what a plugin can import. **The owning doc defines each function's semantics and exact
argument layout**; the signatures below are abridged, and the owning doc wins where they differ. Async jobs
(4.9) may call only the functions marked *job*.

```c
// ---- context, time, RNG ............................................................ owner: 02, 08
ss_tick  ss_now(void);                                   // job · current tick (i64)
uint32_t ss_tick_hz(void);                               // job · 30
double   ss_seconds(ss_tick dt);                         // job
uint64_t ss_stream(const char* name, uint32_t len);      // init · "<plugin>:<name>"
uint64_t ss_stream_seed(uint64_t seed, uint64_t stream, const uint64_t* words, uint32_t n); // job · 02's fold
uint64_t ss_draw(uint64_t stream_seed, uint64_t i);                            // job · also inline in the SDK
uint64_t ss_call_seed(void);                             // job · this plugin's root seed; draws vary by key, tick, counter (02)
void     ss_draw_u01(double* dst, uint64_t stream, const ss_key* keys, const ss_tick* ticks, uint32_t n); // job
u32      ss_id(const char* name, u32 len);               // init · namespaced name -> id; 0 if unknown   (08)
u32      ss_version(u32 what, u32 id);                   // job · cut / resource / field versions, cache keys (08)

// ---- bodies and frames (04 §5's four plugin-facing functions) ........................... owner: 04
void   ss_body_states(double* dst_pos, double* dst_vel, const body_h* src, uint32_t n, /* frame, tick */ ...);
void   ss_frame_points(double* dst, const double* src, uint32_t n, /* from, to, tick */ ...);
void   ss_atmo_density(float* dst, const double* pos_fixed, uint32_t n, body_h planet);
body_h ss_body_create(const BodyDesc* desc, key128 creator, uint32_t seq);   // spawned objects; key = H(creator, tick, seq)

// ---- blobs ........................................................................... owner: 05
Status ss_blob_create(u32 body, key128 creator_key, const BlobSpec* specs, u32 n, u32* out_handles); // own types (4.4)
Status ss_blob_destroy(const u32* handles, u32 n);                       // own types; world edits use 15's calls
Status ss_blob_get_geometry(const u32* src, u32 n, f64* center, f32* quat, f32* radii_k);   // job (snapshot)
Status ss_blob_get_meta(const u32* src, u32 n, BlobMeta* out);           // job · level, type, instance, mode, body, flags
Status ss_blob_get_keys(const u32* src, u32 n, key128* out);             // job
Status ss_blob_find(const key128* keys, u32 n, u32* out_handles);        // 0 where not loaded
Status ss_blob_set_geometry(const u32* src, u32 n, const f64* center, const f32* quat, const f32* radii_k); // owner
u32    ss_instance_new(key128 key);

// ---- hierarchy, cut, refinement ......................................................... owner: 06
Status ss_refine_request(u32 body, const RefineReq* reqs, u32 n);         // sim tier; effective at T + L_split
Status ss_cut_level_at(u32 body, const f64* points, u32 n, i8* out_level, u32* out_leaf);
Status ss_blob_parent(const u32* src, u32 n, u32* out);                   // job
Status ss_blob_children(const u32* src, u32 n, u32* out_first, u16* out_n);
Status ss_blob_ancestor_at(const u32* src, u32 n, i8 level, u32* out);    // job
Status ss_blob_moments(const u32* src, u32 n, f32* V, f64* c, f32* cov6);
Status ss_blob_bound(const u32* src, u32 n, Bound* out);
Status ss_blob_err(const u32* src, u32 n, f32* out);
Status ss_region_of(const u32* src, u32 n, RegionKey* out);

// ---- spatial queries (all batched; deterministic order; work-capped) ......................... owner: 07
int ss_neighbors   (u32 frame, u32 n, const u32* src, u32 k, f32 radius, u8 dist_mode, const QueryFilter* f,
                    u32* dst, f32* dist, u8* row_status);                 // Joel's signature, plus a filter
int ss_neighbors_at(u32 frame, u32 n, const f64* points, u32 k, f32 radius, const QueryFilter* f,
                    u32* dst, f32* dist, u8* row_status);
int ss_graph_rows  (u32 frame, u32 n, const u32* src, i8 band, u8 edge_filter,
                    u32* offsets, u32* cols, f32* weights, u32 cap, u8* row_status);   // job (snapshot) · CSR
int ss_raycast     (u32 frame, u32 n, const SsRay* rays, u8 mode, u32 max_hits, const QueryFilter* f, SsHit* hits);
int ss_field_eval  (u32 frame, u32 n, const f64* points, const QueryFilter* f,
                    f32* value, f32* grad, u32* instance, u32* heaviest);   // the blob field (SDF), not 09's fields
int ss_surface_project(u32 frame, u32 n, const f64* points, f32 max_dist, const QueryFilter* f,
                    f64* out_points, f32* normal, f32* signed_dist, u32* handle, u8* row_status);
int ss_surface_along(u32 frame, u32 n, const f64* origins, const f32* dirs, f64 r_max, f64 r_min,
                    const QueryFilter* f, f64* height, f32* normal, u32* handle, i8* level, u8* row_status);
int ss_overlap     (u32 frame, u32 n, const SsShape* shapes, u8 test, const QueryFilter* f,
                    u32* offsets, u32* items, u32 cap, u8* row_status);  // sphere, ellipsoid, box, capsule
int ss_overlap_pairs(u32 frame, u32 n, const u32* handles, const QueryFilter* f,
                    u32* pairs, u32 cap, u8* status);                   // touching pairs among a set (07, for 11)
int ss_sweep       (u32 frame, u32 n, const SsShape* shapes, const f32* dirs, const f64* t_max,
                    const QueryFilter* f, SsHit* hits);                   // sphere and capsule sweeps (07)
int ss_visibility  (u32 frame, u32 n_obs, const f64* observers, u32 n, const f64* points,
                    const f32* heights, f32* out_score);                  // horizon + coarse line of sight (07, for 17)
int ss_components  (u32 frame, u32 n, const u32* seeds, u32 instance, u32 work_cap,
                    u32* label, u8* anchored, u32* n_labels);
int ss_query_budget(u64* remaining_work_units);
int ss_set_work_cap(u64 units);

// ---- process domain and fast-layer sources ............................................. owner: 10, 11
u32 ss_domain(u32* dst, u32 cap);                     // job · the band-clamped cut, canonical (key) order
i32 ss_source_add(const ss_source_desc* s);           // vents: rate, material, until, on_skip (runtime state)

// ---- fields (composition, stored writes) ............................................... owner: 09, 10
i32 ss_field_compute   (u32 field, u32 phase, void* dst, const u32* handles, u32 n);   // computed, as of phase
i32 ss_field_compute_at(u32 field, u32 phase, void* dst, const f64* points, u32 n, u32 body, i8 band);
i32 ss_field_read      (u32 field, void* dst, const u32* handles, u32 n);              // job · stored, snapshot
i32 ss_field_add       (u32 field, const void* delta, const u32* handles, u32 n);      // job · write kind delta
i32 ss_field_transfer  (u32 from_field, const u32* from, u32 to_field, const u32* to,
                        const f64* amount, u32 n);   // job · declared pair; scaled per source at the barrier (R13, 10)
i32 ss_field_set       (u32 field, const void* src, const u32* handles, u32 n);        // job · write kind owned
i32 ss_field_push      (u32 field, const void* delta, const u32* handles, u32 n);      // job · coarse delta -> leaves
i32 ss_virtual_children(const u32* parents, u32 n, u32 samples, VirtualChild* out, u32 cap);  // closures (09 §4.9)
u32 ss_phase_id        (u32 field, const char* name, u32 len);                         // init · phase name -> id

// ---- persistence: touch, keys, resources, placed content ..................................... owner: 15
void    ss_touch(u32 n, const u32* handles, u32 reason);                  // job · edited + dirty ancestors
void    ss_mint_keys(u32 n, const u32* creators, u32 stream, key128* out);
int32_t ss_resource_get(u32 scope, const key128* scope_key, u32 res_id, u8* dst, u32 cap);  // job (snapshot)
void    ss_resource_put(u32 scope, const key128* scope_key, u32 res_id, const u8* src, u32 len); // job · owner
void    ss_resource_del(u32 scope, const key128* scope_key, u32 res_id);                         // job · owner
int32_t ss_resource_list(u32 scope, const key128* scope_key, u32 res_id, key128* dst, u32 cap,
                         const key128* after);                            // proposed addition to 15: key order
void    ss_place(u32 n, const PlacedDesc* in, u32* out_handles);         // kind D: mints keys, touches, records
void    ss_unplace(u32 n, const u32* handles);
void    ss_remove_generated(u32 n, const u32* handles);                   // kind C removals (prior input)

// ---- entities and models ................................................................. owner: 16
Status ss_ent_create(u16 kind, u32 n, const key128* keys, u32* out_handles);
Status ss_ent_destroy(const u32* handles, u32 n);
Status ss_ent_read (u16 kind, u16 col, const u32* handles, u32 n, u8* dst);   // job
Status ss_ent_write(u16 kind, u16 col, const u32* handles, u32 n, const u8* src);  // job · owner
Status ss_ent_set_flags(const u32* handles, u32 n, u32 set, u32 clear);
u32    ss_ent_in_cells(u16 kind, const u32* cells, u32 n, u32* out, u32 cap);
Status ss_ent_by_key(const key128* keys, u32 n, u32* out_handles);
Status ss_body_create_model(u16 model, u32 n, const Xform64* roots, u32* out_bodies);  // models from the manifest

// ---- physics: parcels, objects, characters ........................................... owner: 11
u32  ss_parcel_spawn(u32 material, u32 body, const f64 pos[3], const f32 vel[3], f64 volume, f32 aux, u32 group);
void ss_parcel_state(const u32* h, u32 n, ss_parcel_state_t* dst);
u32  ss_object_from_blobs(const u32* blobs, u32 n);
u32  ss_object_from_placed(u32 placed_record);        // pick up a settled thing
void ss_object_hold(u32 object, u32 holder_body, u32 bone, const f32 local_pose[7]);
void ss_object_release(u32 object, const f32 vel[3], const f32 ang_vel[3]);
void ss_object_settle_now(u32 object);
void ss_apply_impulse(u32 body, const f64 point[3], const f32 impulse[3]);
void ss_body_motion(const u32* bodies, u32 n, ss_motion_t* dst);
u32  ss_character_create(u32 body, const ss_character_desc* d);
void ss_character_intent(u32 character, const ss_intent_t* intent);
void ss_character_state(const u32* c, u32 n, ss_character_state_t* dst);
void ss_detach_check(u32 body, const ss_region* r);
u32  ss_contacts(u32 body, ss_contact_t* dst, u32 cap);

// ---- events and the director ............................................................ owner: 17
key128 ss_event_begin(u16 type, u32 body, const f64 pos[3], f32 magnitude, f32 visibility_m,
                      const u8* payload, u32 len);                        // job (applied at T+k)
void   ss_event_update(key128 key, f32 magnitude, f32 visibility_m, u8 phase);    // job
void   ss_event_end(key128 key, const u8* outcome, u32 len);                       // job
u32    ss_event_log(const key128* body, ss_tick since, u8 min_class, EventRecord* out, u32 cap); // job · the log read (17)
u64    ss_director_offer(u16 type, ss_tick ready, ss_tick deadline, ss_tick nominal,
                         const f64* cand_pos, u32 n_cand, f32 magnitude_est);
void   ss_director_withdraw(u64 offer);
void   ss_director_register_rate(u16 type, i8 band, u32 window_ticks);   // init

// ---- plugin interaction ...................................................................... owner: 08
i32  ss_call(u32 service, const ArgDesc* argv, u32 argc, const ArgDesc* retv, u32 retc);
i32  ss_ext_call(u32 point, const ArgDesc* argv, u32 argc, const ArgDesc* retv, u32 retc);  // definer only;
                                                                         // job if the contributors are replicated
i32  ss_asset_read(const char* name, u32 len, void* dst, u32 cap);      // init

// ---- diagnostics (no effect on state) ...................................................... owner: 08, 19
void ss_log(u32 level, const char* msg, u32 len);                        // job
void ss_debug_draw(u32 kind, const void* prims, u32 n);                  // dev builds
```

Notes:
- 06's first-draft `mark_touched` is 15's `ss_touch`. The refinement request is 06's `ss_refine_request`.
  `ss_event_emit` is 17's one-shot form of `ss_event_begin/update/end`. 07's `ss_field_eval` is the *blob field* (signed distance) at points;
  09's composed quantities are `ss_field_compute`. The names differ on purpose.
- `ss_sweep` (sphere and capsule casts) and `ss_visibility` (the visibility query that 17 needs) are 07's. 07 owns
  their semantics and final argument lists.
- Terrain services (`surface_at`, `dig`, `raise`, `add_edit_term`, 14) and other plugins' services are not `ss_*`
  imports. They are reached through `ss_call` with ids from the manifest's `services.imports`.

### 5.3 Plugin exports (names come from the manifest; signatures are fixed per kind)

```c
// ---- required (08)
u32  ss_abi(void);                                                     // the ABI major version
i32  ss_init(const u8* params_json, u32 len);
u32  ss_frame_push(u32 bytes);  void ss_frame_pop(void);               // provided by the SDK

// ---- splitting (06 owns the protocol; 14 the kind-A side). Pure. The mode follows the type's content kind (R7).
// kind A (field-derived: ground, water sheets, snow): two phases
i32  split_propose(const SplitIn* in, SplitProposal* out);   // phase 1: children's keys/ordinals + field priors
                                                              //   -> the core projects the fields, applies residuals
i32  realize(const RealizeIn* in, RealizeOut* out);           // phase 2: geometry (one or more sheets per child)
                                                              //   from the final fields; no geometric projection
// kind C (shaped): one phase, child specs; the core applies the full moment projection (kind B uses the two phases above)
i32  split(const SplitIn* in, SplitOut* out);                 // SplitOut.children: []ChildSpec (06)
// kind D (placed) never splits: placed content has its own object pyramid (06, 14)
i32  on_split(const SplitNotice* in);  i32 on_merge(const MergeIn* in);  i32 on_restrict(const RestrictNotice* in);
i32  representative(const RepIn* in, RepOut* out);           // optional custom representative (06)
i32  bound(u32 n, const u32* handles, Bound* out);  i32 error(u32 n, const u32* handles, f32* expected, f32* max);  // 14

// ---- fields (09)
i32  lift_prior(u32 field, const u32* parents, u32 n, const u32* child_off, const u32* children, void* dst);
i32  push_weights(u32 field, const u32* handles, u32 n, const void* delta, f64* dst_w);
i32  base_stage(u32 field, void* dst, const u32* handles, u32 n);
i32  op_stage(u32 field, u32 phase, void* operand, const void* in_value, const u32* handles, u32 n);
i32  ordered_stage(u32 field, u32 phase, void* dst, const u32* handles, u32 n);
//   point-domain variants take (const f64* points, u32 n, u32 body, i8 band) in place of handles
i32  support(u32 field, u32 phase, f64* dst_spheres, u32 cap);

// ---- simulation (10), events (17), persistence (15), entities (16), physics (11)
i32  process(const ss_step_ctx* ctx);                // 10: body, tick, h, n_domain, skipping, event; sync or job
i32  on_watch(const ss_step_ctx* ctx, const u32* handles, u32 n);   // 10: a watch's crossing handles, at T+1
i32  on_events(const EventRecord* records, u32 n);   // 17: next tick, canonical order
i32  director_hazard(u16 type, const key128* regions, u32 n, i64 window, f64* out_mu);
i32  director_sample(u16 type, const key128* regions, u32 n, u32 k, f64* out_pos);
i32  director_fire(u16 type, const u64* offers_or_slots, const u16* chosen, u32 n);
i32  catch_up(u32 scope, const key128* scope_key, i64 dt_ticks);              // 15
i32  migrate_field(u32 field, u32 from_version, const void* src, void* dst, u32 n);
i32  migrate_resource(u32 res_id, u32 from_schema, const u8* src, u32 len, u8* dst, u32 cap);   // 15
i32  lift_individuals(const u32* cells, u32 n);  i32 restrict_individuals(const u32* cells, u32 n);   // 16
i32  display_individuals(const u32* cells, u32 n, DisplayInd* out, u32 cap);                         // 16 (render tier)
i32  pose(const u32* bodies, u32 n, f32 alpha, XformF32* out_bones);                                 // 16
i32  describe(const u32* handles, u32 n, u32* out_name_ids);                                         // 16
i32  material_update(u32 material, const u32* parcels, u32 n, f64 h);       // 11: fast-layer aux state

// ---- interaction (08)
i32  service(const ArgDesc* argv, u32 argc, const ArgDesc* retv, u32 retc);
i32  contribute(u32 point, const ArgDesc* argv, u32 argc, const ArgDesc* retv, u32 retc);
```

### 5.4 Records (05, 06 and 14 own the final layouts)

```c
// kinds B and C: one child spec per child (06's ChildSpec = 05's BlobSpec + these)
typedef struct {
  BlobSpec spec;            // 05: level, type (own), centre (f32, relative to the parent), quat, radii, k, appearance
  u32  parent_index;        // index into the split batch
  u32  ordinal;             // semantic ordinal -> key = H(parent key, ordinal) (R3); must be stable
  f32  vol;                 // true volume (0 = the ellipsoid's)
  f32  bound_margin[2];     // normal, in-plane (D9)
  f32  err;
  InstanceRef instance;     // inherit | new from ordinal | existing key
} ChildSpec;

// kind A, phase 1: per parent, the children's ordinals and a block of prior values per owned stored field
typedef struct { u32 parent_index; u32 n_children; u32 first; } ProposalFamily;
// SplitProposal { families: []ProposalFamily, ordinals: []u32, priors: [field][child] }
// kind A, phase 2: RealizeIn { children (handles, final field values, terms nearby) }
//                  RealizeOut { per child: u32 n_blobs; then BlobSpec[] (sheets, keels) }

typedef struct { u32 field; u32 phase; } FieldAt;     // a read at a phase
typedef struct { u32 ptr; u32 bytes; u32 dtype; u32 count; } ArgDesc;
typedef struct { u32 body; i64 tick; f64 h; u32 n_domain; u8 skipping; u32 event; } ss_step_ctx;   // 10 §5
```

## 6. Risks and unknowns

- **Copy and glue overhead in practice.** The estimates in 4.2.1 are built from measured primitives, not measured end
  to end. Chatty plugins (many small service calls) could be slow. Mitigations: batching in the SDK, static linking
  for first-party plugins, and experiment 7.1.
- **Fuel metering cost and tooling.** It needs a wasm rewriter at load: a small custom pass, or a library compiled to
  wasm. Its overhead is unmeasured. Without it, the only answer to an infinite loop is the watchdog restart.
- **The stateless discipline may feel heavy** for creature AI. If plugin authors fight it, there'll be pressure to
  allow "just a little state". The SDK's generated accessors are the answer; the rule shouldn't bend, or replicas and
  saves break.
- **Async processes that call extension points** pull other plugins into the job (4.16, hydrology + snow). If many
  processes do this, workers carry replicas of most plugins (memory × replicas). Measure; allow `workers: false`
  plugins to force their callers to run sync.
- **Generator-version drift** (4.12) is a real long-term hazard for persistent worlds and needs doc 15's policy
  early.
- **Browser features.** Posting a `WebAssembly.Module` to workers, and SAB staging (COOP/COEP hosting), are both
  assumed; doc 01 verifies them per browser.
- **Re-entrancy depth and frame growth.** A plugin whose frames grow memory inside a nested call must keep earlier
  frames valid (wasm memory growth doesn't move data, so this holds) while the host re-derives its views.

## 7. First experiments

| # | experiment | success criterion |
|---|---|---|
| 7.1 | **Glue cost.** A Zig plugin with a field stage over 10 k / 100 k / 1 M handles (f32 in/out), called through generated JS glue, compared with the same code linked statically into the core | isolated ≤ 1.3× static at 100 k; per-call overhead ≤ 0.5 µs |
| 7.2 | **Service chattiness.** 10 k single-handle service calls vs 1 batched call | documents the per-call cost; the SDK defaults follow from it |
| 7.3 | **A deterministic async job.** Climate on a 9 k-cell top band in a worker with `latency_ticks` 1, 4, 16 and a randomly delayed worker | identical state hash across delays and across worker on/off for the same `k` |
| 7.4 | **Fuel metering.** Rewrite a plugin module (loop back-edges + function entries) and run 7.1 metered | overhead ≤ 15%; an infinite loop traps on the same instruction count every run |
| 7.5 | **Resolution and the load report.** Manifests for the seven `planet/` plugins plus the hut; resolve; permute package order | an identical set hash and report under any order; a deliberate cycle gives a one-line error naming the path |
| 7.6 | **Transactions.** A plugin that traps after half its writes | no trace of the writes; fault attributed; suspension after N |
| 7.7 | **Languages.** The hut plugin in Zig, Rust (`no_std`) and AssemblyScript against the same generated bindings | all three load, pass `ss-test`, and give identical fingerprints |

## 8. Build order

| milestone | plugin-system work |
|---|---|
| M0 | the IDL skeleton and generated `ss.h` with 5 functions; a host that instantiates one isolated module and calls it with a frame; experiment 7.1 |
| M1 | none needed (the sky is core-only); 7.4 may start in the background |
| M2 | terrain's two-phase kind-A split (`split_propose` / `realize`) as the first real exports, statically linked (trusted) to start; the purity rule; split jobs in workers at `L_split` (generation throughput) |
| **M3** | **plugins v1:** manifest schema and validation, resolution (dependencies, schema merge, the `(field, phase)` graph with 09, id assignment, set hash, load report), isolated instances with generated glue, transactional calls, services and extension points, resources, terrain as a package, climate ported (async job); experiments 7.3, 7.5, 7.6 |
| M4 | queries exposed (`ss_neighbors`, `ss_raycast`, `ss_surface_along`); events; fuel metering for untrusted plugins (7.4); hot reload in the dev server |
| M5 | hydrology, vegetation, snow ported; async jobs spanning extension points; migrations; generator versions recorded in saves (with 15) |
| M6 | event-heavy plugins (volcano, impact); fast-layer materials from manifests (with 11); the AssemblyScript SDK; experiment 7.7 |
| later | registry, signed packages, a multiplayer set handshake, plugin GPU code (if ever) |

## 9. Open questions

- **Q08.1 What is the trust model for community plugins?** (merged into `questions.md` C16)
  - Why it matters: it decides whether fuel metering, memory caps and validation are needed from the start, or only
    before a registry exists.
  - Default: **untrusted by default** (isolated, metered, validated). First-party and locally developed plugins can be
    marked trusted.
- **Q08.2 Do you accept isolated instances (B) as the default, with static linking for trusted plugins and dynamic
  linking dropped unless measurements demand it?** (merged into `questions.md` C16)
  - Why it matters: it's the core's biggest structural choice for plugins.
  - Default: **yes**. Revisit after experiment 7.1.
- **Q08.3 Which plugin languages should be supported officially at first?**
  - Why it matters: SDK effort.
  - Default: **C header + Zig + Rust** at M3, **AssemblyScript** by M6.
- **Q08.4 Should the first-party gameplay (terrain, climate, hydrology, vegetation, fire, snow, volcano) be written
  as plugins with the public SDK and linked statically?**
  - Why it matters: dogfooding keeps the ABI honest, but costs some speed of development early on.
  - Default: **yes**, plugins from M3. Terrain may start statically linked in M2 before the manifest exists.
- **Q08.5 Is everything structural declared in the manifest, with no runtime registration?**
  - Why it matters: validation and tooling depend on it, but it's less flexible (e.g. a plugin generating types from
    data at `init`).
  - Default: **manifest only**. Large type lists can live in an asset file that the manifest references; it's still
    static.
- **Q08.6 Can a save change its plugin set (add, remove, upgrade)?** (merged into `questions.md` C9)
  - Why it matters: persistence policy (15) and player expectations.
  - Default:
    - adding is allowed;
    - removing leaves dormant data and inert blobs;
    - upgrading a plugin whose `generator_version` changed follows doc 15's policy (pinning or rebase), and is refused if
      there is none.
- **Q08.7 Should plugins ever ship GPU code (WGSL materials, shape terms evaluated on the GPU)?** (merged into
  `questions.md` C4)
  - Why it matters: `planet/` needed WGSL shape terms to draw detail in coarse cells, "a real cost of this design".
    Plugin shaders also raise validation and security questions.
  - Default: **not in v1**. Plugins provide data (materials, blobs); the core's shaders are fixed. Doc 12 decides
    whether coarse detail needs plugin GPU code.
- **Q08.8 Should a plugin's async job be allowed to pull in other plugins through extension points (hydrology +
  snow), or must cross-plugin calls in async jobs be forbidden to keep workers lean?**
  - Default: **allowed, declared and checked** (4.16). Revisit if replica memory grows.
