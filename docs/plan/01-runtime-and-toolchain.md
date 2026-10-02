# 01. Runtime and toolchain

Status: draft · Depends on: README (D1–D5, D11) · Used by: every doc; especially [02](02-numerics-time-and-determinism.md),
[08](08-plugin-system.md), [10](10-simulation-and-scheduling.md), [12](12-rendering.md),
[19](19-tooling-and-testing.md)

## 1. Purpose and scope

This doc decides what the program *is* at runtime: the language of the core, the WebAssembly features we use and
avoid, which threads exist and what each owns, how memory is laid out and grown, how bytes reach the GPU, how we build
and host it, and which browsers we support.

In scope:
- the core language and its build;
- WASM feature policy;
- thread and worker topology;
- the memory budget, allocators and heap views;
- the CPU → GPU data path (the glue side; passes and structures are in [12](12-rendering.md));
- the job system that carries work to plugin workers (its mechanics; *what* runs where is in [10](10-simulation-and-scheduling.md));
- hosting headers;
- the browser support matrix;
- debugging, profiling and hot reload.

Out of scope:
- the plugin ABI and its isolation model ([08](08-plugin-system.md));
- deterministic arithmetic rules ([02](02-numerics-time-and-determinism.md));
- render passes ([12](12-rendering.md));
- the test harness ([19](19-tooling-and-testing.md)).

## 2. Requirements

| | requirement | traces to |
|---|---|---|
| must | The core is WebAssembly. JS is glue: it calls Web APIs (WebGPU, DOM, storage, workers) and copies bytes. JS never computes values that enter sim state. | constraint "runs in the browser", D5 |
| must | Perf-critical work (cut, queries, physics, fields, packing GPU uploads) is in WASM. | constraint |
| must | The core can run single-threaded. Plugins can run on the engine thread (synchronous calls) and in workers (asynchronous jobs). | Joel's plan; D11 |
| must | Nothing in the runtime prevents deterministic simulation: no wall-clock reads in the sim, fixed job latencies, and identical binaries produce identical bits. | D5, [02](02-numerics-time-and-determinism.md) |
| must | Memory and the GPU data path handle 2–4 M loaded blobs. The prototypes kept 1–3.8 M resident (`RAYCUT.md` §7, `BLOBMAP.md`). | pillar 2, "big world" |
| must | Stale heap views are impossible by construction. `wasmgpu/` found they silently upload nothing. | `wasmgpu/README.md` |
| should | The same build runs in Chrome/Edge, Safari 26+ and Firefox on desktop, and degrades rather than breaks where a feature is missing. | constraint "browser" |
| should | Iteration is fast: incremental core builds in seconds, shaders and plugins hot-reload without losing the world. | the collaborative workflow |
| later | Threads inside the core (a worker pool running core code on shared memory), memory64. Mobile is not planned (questions.md C5). | |

## 3. What the prototypes taught

- **The wasm data path costs nothing extra** (`wasmgpu/README.md`).
  - `writeBuffer`, `writeTexture`, `bufferSubData` and `readPixels` take views of wasm memory, shared or not, at the
    same throughput as JS ArrayBuffers.
  - The boundary itself costs ~4 ns per call. Browser API calls cost 16–100 ns each, whoever calls them.
- **Stale views fail silently.**
  - `memory.grow()` detaches views of non-shared memory. `writeBuffer(buf, 0, staleView)` then uploads nothing,
    with no exception and no GL error.
  - `toResizableBuffer()` would fix it, but every WebGPU and WebGL entry point rejects resizable buffers, by spec.
- **Pass floats through memory, not as arguments.** f32 arguments crossing wasm → JS are boxed: 59 ns per call
  against 23–29 ns with i32 arguments.
- **Hand-written glue beats binding the API directly.** A tiny arrow function was faster than
  `Function.prototype.call.bind(GPUQueue.prototype.writeBuffer)` imported straight into wasm (96–102 vs 128–133 ns)
  because it keeps V8's fast API path.
- **Chrome's `writeBuffer` has two speeds.**
  - Below 4 MiB it copies at ~3.2 GB/s on the calling thread: 1 MB costs 330 µs of that thread's time.
  - At ≥ 4 MiB it switches to a ~46 GB/s path.
  - A mapped staging ring does 1 MB in 39 µs of in-call time.
- **One thread per GPU device.** `GPUDevice`, `GPUBuffer` and `GPUQueue` can't be posted to workers, but a worker can
  create its own device and render to an `OffscreenCanvas`. So whoever owns the device makes every GPU call.
- **Readback is asynchronous.**
  - `mapAsync` needs JSPI to look synchronous from wasm, and JSPI is Chrome-only.
  - The ray cut already lived with async readback: three readback slots kept the CPU a few frames behind and never
    stalled (`RAYCUT.md` §4).
- **Generation belongs in workers, and their memory needs honest accounting.**
  - In the ray cut, workers' tile caches were counted at 1/17 of their real size. The tab reached 4.7 GB and crashed
    (`RAYCUT.md` §11).
  - Decoded JS objects cost ~650 bytes per blob, against 136 as flat typed arrays. That is the case for doing this
    work in WASM over flat arrays.
- **Workers out-generate the GPU.** Eight workers generated chunks far faster than the GPU could rebuild windows
  (`BLOBMAP.md`). In the ray cut, generation (~1–2 k splits/s in JS) limited convergence, not the GPU.
  - Estimate: WASM generation over flat arrays should be several times faster than that JS. Measure it in M2.
- **Accessors, not arrays** (`planet/PLUGINS.md` §1.2). A plugin kept an array the core later reallocated. It read
  garbage, and NaN snow ran down the rivers. The same hazard exists for heap views and for handles into reallocated
  pools.
- **GPU timing on Apple hardware.**
  - Pass timestamps overlap, so a frame is timed by its whole-frame span.
  - The M1 Max adapter allows only 10 storage buffers per shader stage.
  - Thermal state moves timings 10–30% (prior-session notes; [19](19-tooling-and-testing.md)).

## 4. Design

### 4.1 Core language: Zig, pinned (D1 confirmed)

Most of the core is custom data-oriented code with few dependencies:
- SoA pools of blobs, bodies and parcels;
- batch kernels over u32 handle arrays;
- the cut and its queues;
- spatial structures;
- Kepler maths;
- field storage;
- a scheduler and a job system;
- serialisation.

Physics is custom (blob fields, parcels with rheology; [11](11-physics.md)), so the main C++ library advantage (Jolt)
mostly doesn't apply. The comparison:

| criterion | Zig 0.16 | C++ (clang / wasi-sdk / Emscripten) | Rust |
|---|---|---|---|
| browser WASM without a runtime | `wasm32-freestanding` is first class: the module contains what you wrote and nothing else | clang `--target=wasm32 -nostdlib` works; libc++ needs a libc (wasi-sdk, then stub the WASI imports) or Emscripten (its own JS glue and conventions) | `wasm32-unknown-unknown` is first class; `no_std` possible; std mostly works |
| SIMD128 | `@Vector(4, f32)` lowers to simd128 with `-mcpu=…+simd128`; portable source | `wasm_simd128.h` intrinsics or clang vector extensions | `core::arch::wasm32` intrinsics; portable `std::simd` still nightly as far as I know (verify) |
| allocator control | allocators are explicit parameters everywhere; `ArenaAllocator`, `MemoryPool`, `FixedBufferAllocator` | possible (`std::pmr`, custom), but the standard library allocates implicitly | one global allocator; arenas via crates (`bumpalo`); the allocator API is unstable |
| SoA | `std.MultiArrayList` built in; comptime generates layouts from a struct | by hand or macros | crates (`soa_derive`) |
| C ABI for the SDK | `export fn … callconv(.c)`, `extern struct`; the core translates the SDK header (`addTranslateC` in the build, or `@cImport`) so its exports are checked against it | native | `extern "C"`, `#[no_mangle]`, cbindgen |
| debugging in Chrome | DWARF via the LLVM backend should work with the C/C++ DevTools extension (verify in M0, and that the self-hosted wasm backend isn't used for debug builds) | DWARF, known to work | DWARF, known to work |
| compile speed | fast; incremental compilation improving | slow with templates | slow-ish |
| safety | bounds and overflow checks in Debug / ReleaseSafe; no borrow checker | least safe | safest |
| stability | **pre-1.0: every release breaks APIs** (0.15's I/O rewrite, 0.16's `std.Io`) | stable | stable |
| ecosystem for us | can compile and link C (`zig cc`), so C libraries stay available | Jolt (deterministic, compiles to WASM), meshoptimizer, Dear ImGui | wgpu, glam, rapier (has a determinism mode), bevy_ecs |
| contributors | smallest community | largest | large, strong in WASM and gamedev |

**Recommendation: Zig, with conditions.**
- **Pin the compiler.** `build.zig.zon` declares `minimum_zig_version`. CI and every machine use exactly that
  version. Upgrades are deliberate: one commit, determinism fingerprints compared before and after
  ([19](19-tooling-and-testing.md)).
- **Use little of `std`.** Use its containers, allocators and `MultiArrayList`. Avoid `std.Io`, `std.Thread` and
  `std.fs` (none of them apply in `wasm32-freestanding` anyway).
- **Vendor our own maths for anything sim-critical.** `std.math` is ported from musl but can change between
  releases, and that would silently change results across an upgrade ([02](02-numerics-time-and-determinism.md) §4.6).
- **The SDK header is the source of truth for the ABI.** `sdk/include/sublime.h` is hand-written, and the core
  translates it (the build system's `addTranslateC`, or `@cImport` while it lasts), so a mismatch is a compile error. Recent Zig versions don't emit headers (`-femit-h`) reliably, so
  hand-writing them is the plan either way.
- **Release mode.** Use `ReleaseSafe` during development. Ship `ReleaseFast` only after the M0 benchmark shows the
  gap, and run determinism tests on the shipping build.

**Rust is worth a look.** It is the safest of the three, and its WASM ecosystem is the most mature. What it costs
here: slower builds, a single global allocator, and more friction for the arena-heavy, handle-indexed style the core
wants. If Joel already writes Rust comfortably, it is a reasonable choice. The plugin ABI is the same whatever the
core language is.

**C++** is the fallback if Zig's churn or debugging story fails the M0 spike. Use clang + wasi-sdk with `-nostdlib`
and our own containers, not Emscripten: we want no generated JS glue and no implicit runtime.

### 4.2 WebAssembly feature policy

| feature | policy | why |
|---|---|---|
| SIMD128 | **use** | batch kernels (field evaluation, queries, packing); shipped everywhere |
| relaxed SIMD | **forbidden**, compiled out (`-mcpu` without `relaxed_simd`) | results differ between engines and CPUs (`relaxed_madd` may or may not fuse), which breaks D5 |
| threads + shared memory (SharedArrayBuffer, Atomics) | **use for the core heap and for job arenas**; core *code* stays single-threaded at first | zero-copy job inputs and outputs for plugin workers; keeps a core worker pool open for later; needs COOP/COEP (§4.8) |
| bulk memory (`memory.copy`, `memory.fill`) | use | memcpy and zeroing of field columns |
| multi-value, sign-ext, nontrapping float-to-int, mutable globals | use | baseline features, shipped everywhere |
| tail calls | allowed, not relied on | shipped (Safari since 18.2; verify), but no need |
| exception handling | **avoid in the core** | error unions and return codes; a trap is a bug, caught at the JS boundary |
| memory64 | **not now** | wasm32's 4 GiB covers the budget (§4.5); 64-bit memories lose the guard-page bounds-check tricks (reported ~10% or worse); Safari stable doesn't ship it yet |
| multi-memory | **not now**, maybe for plugins later | would let a plugin import a core data memory beside its own heap ([08](08-plugin-system.md)), but source languages can't address a second memory without intrinsics |
| JSPI | **don't depend on it** | Chrome-only; readback is asynchronous by design (§4.6) |
| GC, stringref, component model | not used | the core manages its own memory; there is no component model in browsers |

### 4.3 Threads and workers (D3)

```
main thread                 engine worker                                plugin workers (N)
──────────                  ─────────────                                ──────────────────
DOM UI                      core.wasm (shared memory, single-threaded)   plugin instances
pointer lock, keys          plugin instances (sync calls)                (async processes,
  │ input ring (SAB)  ───▶  renderer JS + WGSL                           splits, generation)
  │ UI snapshot (SAB) ◀───  WebGPU device + OffscreenCanvas                 ▲ │
  │ UI events (msg)   ◀──▶  save snapshots ──▶ persistence worker (OPFS)    │ │ job arenas (SAB)
                            job system ─────────────────────────────────────┘ │
                                        ◀─────────────────────────────────────┘
```

**Main thread.**
- **Owns:** the DOM UI (menus, search box, journal, settings), pointer lock and focus, and the canvas element. It
  transfers control of the canvas once, with `transferControlToOffscreen()`.
- **Never** computes game state, never touches the GPU, and keeps no copy of the world.
- **Input.**
  - Events go into a lock-free SPSC ring in a small SharedArrayBuffer.
  - The ring has fixed 32-byte records: kind, key or button, mouse deltas as i32 (sub-pixel ×256), and the time
    since page load as f64 milliseconds.
  - Mouse motion under pointer lock is accumulated into one record per frame.
  - The engine drains the ring at the start of a frame and turns records into **commands stamped with the tick they
    apply to** ([02](02-numerics-time-and-determinism.md) §4.9).
  - The wall-clock timestamp is used for input latency metrics only, never for sim values.
- **UI state.**
  - The engine writes a small fixed-layout UI snapshot every frame into another SAB: the selected body, its key and
    name, the camera mode, time and time warp, and counters. The main thread reads it in its own
    `requestAnimationFrame`.
  - Rare, variable-size data (search results, journal entries) goes by `postMessage`.

**Engine worker.**
- **Owns** the core instance, the engine-thread plugin instances, the renderer, and the `GPUDevice`. Every GPU call
  happens here.
- **Drives the frame loop** with `requestAnimationFrame` in the worker where available.
  - **Fallback:** the main thread's `requestAnimationFrame` posts a "frame" message, or pokes an `Atomics` word the
    engine waits on.
- **Saves** go to a dedicated **persistence worker**, which owns all storage ([15](15-persistence.md) §4.13).
  - At a tick boundary the engine serialises dirty records into a staging buffer and hands it over, transferred or
    through a SAB.
  - The persistence worker compresses, hashes and writes through OPFS `createSyncAccessHandle` (dedicated workers
    only), with IndexedDB as the fallback.
  - The engine never waits on storage; its cost per autosave is the serialisation.

**Plugin workers.**
- `N = clamp(hardwareConcurrency − 2, 1, 8)` workers. Each instantiates the plugins that declare asynchronous work
  ([08](08-plugin-system.md)).
- Because plugins hold no authoritative state (D11, and §4.4 below), the same module can be instantiated on the
  engine thread *and* in workers without the copies diverging.
- **Generation runs here.** Terrain splits and other heavy split work are the main job. The prototypes measured that
  generation, not the GPU, limits convergence (ray cut).

**Fallback: the engine on the main thread.**
- The engine code never touches `window` or the DOM. All Web API access goes through one `host` object.
- The same engine module runs on the main thread with a regular canvas if:
  - WebGPU in workers is missing;
  - `OffscreenCanvas` can't be transferred;
  - `requestAnimationFrame` in workers is missing.
- The cost of the fallback is UI jank during long frames, not a broken game.
- Which browsers need it is the first M0 experiment (§7, E2).

**Audio** (later) would be an `AudioWorklet` fed from a SAB ring written by the engine. It is not planned here.

### 4.4 The job system: deterministic async

Work sent to plugin workers must not make sim results depend on how fast a worker was. The rule:

> A job issued at tick T with latency k is applied at the start of tick T + k, never earlier and never later. If its
> result hasn't arrived when tick T + k is about to run, the engine waits for it. A late job causes a frame-time
> stall, never a different world.

**Mechanics:**
- **Inputs.** At issue time, the core copies the job's inputs into a **job arena**: a region of shared memory owned
  by that job until T + k. Inputs are field columns gathered by handle, the handles' keys, and parameters.
  - The arena is a snapshot, so later ticks can mutate the live columns freely.
  - Arenas form a ring indexed by due tick, so freeing one is a pointer reset.
- **Dispatch.** The worker's glue copies the arena slice into the plugin instance's own memory (one `TypedArray.set`),
  calls the plugin, and copies the outputs (deltas, new blobs as records, events) back into the arena's output area.
  Then it sets a completion word with `Atomics.store` + `Atomics.notify`.
- **Application.** At tick T + k, the core checks the completion words of every job due then. Pending ones are waited
  on (`Atomics.wait` is allowed in workers, so the engine worker may block).
  - Completed outputs are applied in the **canonical order defined in [10](10-simulation-and-scheduling.md) §4.4**
    (sorted by stable ids, never by completion time; [02](02-numerics-time-and-determinism.md) §4.7).
- **Choosing k.**
  - Processes declare their latency in the manifest under [10](10-simulation-and-scheduling.md)'s per-process rule
    (`affinity: async(k)`; climate and hydrology are `async(30)`).
  - Sim-tier splits land at `T + L_split`, [06](06-blob-hierarchy-and-cut.md)'s split latency (3 ticks,
    provisionally).
  - The scheduler warns when a job's measured duration approaches k × tick time, and records stall time per process
    ([10](10-simulation-and-scheduling.md)).
- **Render-tier work is exempt.** Render-only blobs (the cut's render tier, [06](06-blob-hierarchy-and-cut.md))
  aren't sim state, so they're applied whenever they arrive. Asynchronous GPU readback for the ray-guided cut
  feedback is likewise exempt.

Without cross-origin isolation (no SAB), the same protocol runs over `postMessage` with transferred ArrayBuffers.
Expect slower round trips (verify in M0, E6); determinism is unaffected.

### 4.5 Memory budget and layout

**Per loaded blob in the core** (estimate; [05](05-blob-model.md) owns the real layout):

| item | bytes |
|---|---:|
| centre, f64 × 3 in the body frame (D4) | 24 |
| orientation (f32 quaternion; 4 bytes if packed smallest-three, as on the GPU) | 16 |
| radii f32 × 3 + blend radius `k` | 16 |
| colour, material, flags | 8 |
| instance, owner, type ids | 8 |
| level, dynamics mode, misc | 4 |
| stable key (128-bit: H(parent key, semantic ordinal), [05](05-blob-model.md)) | 16 |
| tree links (parent, first child, child count) | 12 |
| bound (f32 offset from centre + radius) | 16 |
| handle generation counter | 4 |
| **core record** | **~124 → 128** |
| stored fields: ~16 × f32, plus per-plugin columns | ~64 |
| spatial index share (BVH nodes, neighbour grids) | ~32–64 |
| **total per blob** | **~220–260** |

**Whole-process estimate at 4 M loaded blobs:**

| region | size |
|---|---:|
| blob SoA + fields + index | ~1.0 GB |
| render staging: packed GPU records (32 B geometry + 16 B appearance) for dirty regions and ring buffers | 64–128 MB |
| per-tick and per-frame arenas | 64–128 MB |
| job arenas (ring of k ticks) | 32–128 MB |
| bodies, parcels (≤ 100 k), resources, persistence buffers | ~100 MB |
| **core heap** | **~1.3–1.6 GB** |
| plugin instance memories (separate `WebAssembly.Memory`s; ~10 plugins × ~16–64 MB, replicated in each worker that hosts them) | 0.2–2 GB, **must be capped** |
| GPU memory (not in any wasm heap): M7 used 1.7 GB, the ray cut 0.44 GB of bricks at 2 M blobs | budget in [12](12-rendering.md) |

**Conclusions:**
- **wasm32 is enough.**
  - Reserve the core memory with a declared maximum of 3 GiB. Shared memories need a declared maximum, and that also
    keeps us clear of browser limits near 4 GiB.
  - Revisit memory64 only if a measured budget exceeds that.
- **Plugin memory is the hidden risk.** Every plugin instance gets a **64 MB cap by default**; a manifest may
  declare more ([08](08-plugin-system.md)). The host enforces the cap at instantiation.
  - Since plugins are nearly stateless (D11), most should stay well under the default.
  - Only plugins with asynchronous processes are replicated into workers.
- **The tab total matters more than the heap.** The ray cut's tab sat at 3.4–3.6 GB, mostly generation workers
  holding JS objects. Moving generation into WASM over flat arrays is the fix. Track the tab's total in the
  performance HUD ([19](19-tooling-and-testing.md)).

**Allocators** (no general-purpose `malloc` on hot paths):

| lifetime | allocator | contents |
|---|---|---|
| program | static reservations at init | tables, pools' backing stores |
| pooled objects | SoA slab pools with free lists and generation counters, grown in large chunks | blobs, bodies, parcels, cut nodes |
| one tick | bump arena, reset at the end of the tick | batch scratch, query results, delta buffers, plugin call staging |
| one frame | bump arena, reset per frame | render preparation, upload packing, debug draw |
| job lifetime (T … T + k) | ring of arenas indexed by due tick | job inputs and outputs (§4.4) |
| long-lived variable | a TLSF-style general allocator, off hot paths | plugin resources (byte blobs), persistence buffers, strings |

### 4.6 Heap views and the GPU data path

**Views can't go stale by construction.**
- The glue owns exactly one accessor, `heap()`. It returns `{u8, u32, i32, f32, f64}` typed arrays over
  `memory.buffer`, rebuilt whenever the cached buffer object differs from the current one.
  - **Non-shared memory:** `grow` replaces `buffer` and detaches the old one.
  - **Shared memory:** `memory.buffer` returns a new, longer SAB, and old views keep their old, too-short length.
  - Either way, the identity check catches it.
- Every glue function that touches memory calls `heap()` first. No module holds a view across a call into WASM.
- Growth is rare and coarse anyway: pools grow in 64 MiB steps.
- A debug assertion compares the view's byte length with the core's own `heap_size()` export.

**Uploads** follow `wasmgpu/` and Chrome's `writeBuffer` behaviour:
- **Packing happens in WASM.** The core packs dirty blobs into the GPU record inside a per-frame upload arena,
  grouped by destination buffer. The record is [05](05-blob-model.md)'s: 32 B of geometry, grown from `BLOBMAP3.md`
  §1's atom, plus 16 B of appearance.
- **Small per-frame data goes out in one call.** Uniforms, cut changes and feedback resets are concatenated, so a
  frame issues one `queue.writeBuffer` per destination region rather than one per object.
- **Streaming bulk data goes through a mapped staging ring.**
  - Bake inputs and generated node contents are batched to ≥ 4 MiB where latency allows (Chrome's fast path).
  - Otherwise they go through a ring of mapped staging buffers: `mapAsync(WRITE)` ahead of time, then
    `getMappedRange()`, then one `TypedArray.set` from the heap, then `unmap`, then `copyBufferToBuffer`.
  - The ring was 1 MB in 39 µs of in-call time (`wasmgpu/README.md`). Re-measure on Safari and Firefox (E5).
- **Uploads go through a deliberately small API** that only takes `(memory.buffer, byteOffset, byteLength)` forms.
  The `bufferSubData(view)` style without offsets silently uploads nothing from a stale view.

**Readback** (feedback words, picking, GPU timers) is `mapAsync` into a ring of three readback slots, as in the ray
cut. Results are consumed one to three frames later. No sim state depends on them: GPU results never feed sim state
([02](02-numerics-time-and-determinism.md) §4.10).

**The renderer's JS** (passes, pipelines, bind groups) lives in the engine worker.
- Each frame it calls the core once, `eng_render_prepare(frame_params)`. That call runs culling, cut traversal and
  packing in WASM, and returns offsets to the upload arena and a small **command list** of fixed-size records:
  dispatch sizes, draw counts, buffer ranges.
- The JS decodes the list in one loop. This gets WASM to JS parity on API calls (`wasmgpu/` measured 17–19 ns per
  decoded draw) without per-call boundary crossings.

**Glue style:**
- Hand-written arrow functions, not `call.bind` imports.
- Float parameters go through memory, not as arguments.
- Each import has one job and a fixed signature, so V8 keeps its fast paths.

### 4.7 The JS surface: small and boring

The JS is in three groups. Each is a plain ES module with no framework.

| group | where | what |
|---|---|---|
| `web/ui/` | main thread | DOM, input capture, the canvas transfer, the UI snapshot reader |
| `web/glue/` | engine worker and plugin workers | instantiation, `heap()`, imports (logging, storage, timing for metrics only), the plugin host ([08](08-plugin-system.md)), the job system's worker side |
| `web/render/` | engine worker | WebGPU device, pipelines, passes, WGSL ([12](12-rendering.md)) |

Rule: if a piece of JS loops over blobs, it should probably be in WASM. Exceptions: copying bytes, and decoding the
render command list.

### 4.8 Build and hosting

**Build.**
- `zig build` is the single entry point. It produces:
  - `core.wasm` (ReleaseSafe by default);
  - every first-party plugin's `.wasm` (Zig or C, through `zig cc`);
  - native test binaries;
  - a `build-info.json` with content hashes (used for determinism checks and plugin identity; [02](02-numerics-time-and-determinism.md) §4.11).
- **JS:** native ES modules in development (no bundling). For production, esbuild bundles each entry (main, engine
  worker, plugin worker) and inlines WGSL as strings. The only npm dependencies are esbuild and dev tools.
- **Target flags** (verify exact spellings in M0):
  - compile: `-target wasm32-freestanding -mcpu=generic+simd128+bulk_memory+atomics+multivalue+sign_ext+nontrapping_fptoint`;
  - link: `--import-memory --shared-memory --max-memory=3GiB --export-dynamic` (or explicit exports);
  - no `relaxed_simd`.

**Dev server** (`tools/dev-server.mjs`, no framework):
- static files;
- `Cross-Origin-Opener-Policy: same-origin` and `Cross-Origin-Embedder-Policy: require-corp`;
- `Cross-Origin-Resource-Policy: same-origin` on everything;
- a file watcher that reruns `zig build` and pushes reload events over SSE ([19](19-tooling-and-testing.md)).

**Hosting.** Cross-origin isolation is required for SAB: the input ring, shared memory and job arenas.
- GitHub Pages can't set headers. The `coi-serviceworker` workaround injects them through a service worker, at the
  cost of a reload on the first visit.
- Cloudflare Pages and Netlify set them from a `_headers` file. **Recommended.**
- **COEP mode.** `credentialless` would make embedding community content easier, but Safari doesn't support it.
  Use `require-corp` everywhere.
- **Community plugins hosted elsewhere** are fetched with `fetch()` in CORS mode, so their host must send
  `Access-Control-Allow-Origin`. That is a requirement on plugin distribution ([08](08-plugin-system.md)).
- **The non-isolated fallback** still runs, more slowly and without the SAB ring:
  - input over `postMessage`;
  - jobs over transferred buffers;
  - non-shared core memory.

### 4.9 Browser support

From current sources (October 2026), with **verify** marking what must be tested in M0 (E2):

| feature | Chrome / Edge | Safari | Firefox |
|---|---|---|---|
| WebGPU | 113+ on macOS, Windows, ChromeOS; Android 121+; Linux 144+ (Intel Gen12+), NVIDIA Wayland 147+ | 26 (macOS, iOS, iPadOS, visionOS) | Windows 141+; macOS 145+ (Apple Silicon), 147+ (all); Linux expected 2026; Android behind a flag |
| WebGPU in a dedicated worker + OffscreenCanvas `webgpu` context | yes (verify) | `webgpu` context on OffscreenCanvas listed for 26.x; in workers: verify | verify |
| `requestAnimationFrame` in dedicated workers | yes | sources conflict (16.4+ vs Chromium-only): verify | verify |
| SAB + Atomics with COOP/COEP | yes | yes | yes |
| SIMD128, bulk memory, threads | yes | yes | yes |
| tail calls | yes | 18.2+ (verify) | yes |
| OPFS sync access handles in workers | yes | yes (verify version) | yes |
| COEP `credentialless` | yes | **no** | yes |
| memory64 (not used) | yes | not in stable | yes |
| JSPI (not used) | yes | no | verify |

**Proposed support policy:**
- **Tier 1:** desktop Chrome/Edge and Safari 26+ on macOS.
- **Tier 2:** Firefox (Windows and macOS), wherever WebGPU ships.
- **Not planned:** mobile. iOS memory limits and thermal budgets are a different design point (working assumption;
  questions.md C5).

### 4.10 Debugging, profiling and hot reload

**Debugging.**
- DWARF in the Debug and ReleaseSafe builds, read by Chrome's C/C++ DevTools extension: breakpoints and variables in
  Zig source. Whether this works with Zig's LLVM backend is part of E1.
- Traps surface as JS exceptions at the boundary. The glue catches them, logs the core's last-error buffer and the
  current tick, and stops the engine cleanly.
- A debug build checks handles' generation counters and, at every plugin boundary, that values are finite
  ([02](02-numerics-time-and-determinism.md) §4.8).

**Profiling.**
- CPU: the core exports per-system counters (ns per tick per system and per plugin, measured by a host import
  wrapping `performance.now()`; metrics only). The glue mirrors them as `performance.mark` / `performance.measure` for
  DevTools.
- GPU: timestamp queries per pass, with the whole-frame span as the frame's number (passes overlap on Apple GPUs).
- Both feed the performance HUD ([19](19-tooling-and-testing.md)).

**Hot reload, in order of value:**
1. **WGSL and render JS.** Recompile the pipelines and keep everything else. Seconds.
2. **Plugins.** Because a plugin's authoritative state lives in core fields and resources, reloading is:
   - instantiate the new module and run its deterministic `init`;
   - swap it in at a tick boundary;
   - reconcile its manifest (new fields get defaults, removed fields are dropped with a warning; [08](08-plugin-system.md)).

   The swap is recorded in the replay log as an event, because it changes results.
3. **The core.** Save, instantiate the new core, load. This goes through the persistence path
   ([15](15-persistence.md)), which also tests that path for free. Expect about a second for a small world.

### 4.11 Scenario walkthroughs

- **1 Orbit to grass.**
  - The zoom transition registers render-tier requests for the destination ahead of time ([18](18-player-camera-and-navigation.md)), and
    generation jobs fan out to plugin workers.
  - The upload path has to absorb a burst: the ray cut baked hundreds of nodes per second. The staging ring and
    coalescing (§4.6) are what keep this from stalling the engine thread.
  - Memory: the cut's budget (2–4 M blobs) is the dominant line in §4.5.
- **2 The storm and the flood.**
  - Hydrology steps (several ms each in `planet/`) run as plugin-worker jobs with a latency of several ticks.
  - At high time warp many come due at once. The engine stalls rather than diverging, and the time governor
    ([10](10-simulation-and-scheduling.md)) lowers the warp when stalls accumulate.
- **6 A community plugin.**
  - Loaded from another origin: the host must send CORS headers (§4.8).
  - It is instantiated as an isolated module with its declared memory cap.
  - It can't reach the core heap, any clock, or any randomness; its imports are exactly the SDK's.
  - Its hash enters `build-info` / the session's plugin set.

### 4.12 Disagreement with the README

None of substance. Two refinements:
- D3 is conditional on the M0 worker experiment. The glue must support running the engine on the main thread from
  day one.
- D1 holds only with a pinned compiler and our own maths library.

## 5. Interfaces

The core's exports to the glue. These are engine-internal, so they use an `eng_` prefix that doesn't look like the
plugin ABI (whose `ss_` functions are listed canonically in [08](08-plugin-system.md) §5.2). Signatures are
illustrative:

```c
// lifecycle
uint32_t eng_init(const eng_init_params *p);          // returns an error code; p in core memory
uint32_t eng_heap_size(void);                        // for the view assertion
void     eng_input_push(const eng_input_record *r, uint32_t n);   // drained input, stamped by the core
uint32_t eng_frame(const eng_frame_params *fp);       // advance due ticks, apply due jobs, update the cut
uint32_t eng_render_prepare(const eng_render_params *rp, eng_render_out *out);
                                                    // packs uploads + command list into the frame arena
void     eng_readback(uint32_t slot, const uint8_t *data, uint32_t len);  // GPU feedback, render-only
// jobs (engine side)
uint32_t eng_jobs_ready(eng_job_desc *out, uint32_t cap);   // jobs issued this tick, with arena ranges
void     eng_job_done(uint32_t job_id);                    // the glue marks completion (also via an Atomics word)
// diagnostics
uint32_t eng_last_error(char *buf, uint32_t cap);
void     eng_counters(eng_counters_out *out);
```

The core's imports from the host (all deterministic except the explicitly metrics-only ones):

```c
void     host_log(uint32_t level, const char *msg, uint32_t len);
double   host_now_ms(void);           // METRICS ONLY: the core never feeds this into sim state
void     host_persist_submit(uint32_t seq, uint32_t off, uint32_t len);  // hand a serialised snapshot to the
                                      // persistence worker (15 §5.5); returns immediately
uint32_t host_persist_poll(eng_persist_status *out);   // committed{seq} | failed{seq, reason}; loads arrive as chunk batches
uint32_t host_plugin_call(uint32_t plugin, uint32_t fn, uint32_t arena_off, uint32_t len);
                                      // synchronous call into an isolated plugin instance (08)
```

The glue's own API to the renderer and UI is plain JS and is specified in [12](12-rendering.md) and
[18](18-player-camera-and-navigation.md).

## 6. Risks and unknowns

| risk | likelihood | impact | mitigation |
|---|---|---|---|
| Zig churn costs days per upgrade | high | medium | pin; minimal `std`; upgrade rarely and deliberately |
| DWARF debugging of Zig in Chrome is poor | medium | medium | E1; fall back to logging plus native debugging of the same code (it compiles natively too) |
| WebGPU or `requestAnimationFrame` in workers missing on Safari or Firefox | medium | medium | main-thread fallback designed in from day one |
| shared memory with a 3 GiB maximum fails or reserves too much on some browser or OS | medium | high | E3; fall back to non-shared core memory with SAB job arenas |
| plugin memories replicated into workers blow the tab budget | medium | high | declared caps; replicate only async plugins; HUD tracks the total |
| job latency k too small at time warp, causing stalls | medium | medium | per-process k; the governor; stall metrics |
| staging-ring behaviour differs on Safari/Firefox (the 4 MiB threshold is a Chrome detail) | high | low | measure (E5); keep both paths behind one upload API |
| hosting without COOP/COEP (e.g. an embed somewhere) | low | medium | the non-isolated fallback mode |

## 7. First experiments

| id | experiment | success criterion |
|---|---|---|
| E1 | **Zig spike.** A `wasm32-freestanding` core exporting one C-ABI batch kernel over a `MultiArrayList` SoA, in a SIMD128 and a scalar version; shared memory; built by `zig build`; loaded in Chrome with the DWARF extension. | breakpoints and locals work in Zig source; incremental rebuild < 2 s; SIMD ≥ 2.5× scalar on a field-evaluation kernel; the module carries only what we wrote (inspect it) |
| E2 | **Worker topology** in Chrome, Safari 26 and Firefox: transfer an OffscreenCanvas, create a WebGPU device in the worker, `requestAnimationFrame` there, input through the SAB ring. | a matrix of what works; input-to-engine latency < 1 ms p99; the fallback path chosen automatically |
| E3 | **Shared memory limits.** Instantiate the core with maxima of 1, 2, 3 and 4 GiB, grow to 2 GiB, in each browser. | the largest maximum that instantiates everywhere in tier 1; resident memory before and after growth |
| E4 | **Plugin call overhead** (with [08](08-plugin-system.md)): a batch call into an isolated instance with copy-in/copy-out, vs the same code statically linked. | overhead per call and per MB; break-even batch size |
| E5 | **The upload path on all three browsers:** `writeBuffer` < 4 MiB, ≥ 4 MiB, mapped staging ring (repeat `wasmgpu/`'s `uploadBench`). | a per-browser policy table; 8 MB/frame of streaming costs < 2 ms of engine-thread time |
| E6 | **Job round trips:** issue → worker → done, via `Atomics.wait`/`notify` on SAB vs `postMessage`; jitter under load. | p99 round trip < 1 ms with SAB; the smallest safe k for a 5 ms job |

## 8. Build order

- **M0:**
  - E1–E3 and E6.
  - The repo layout ([19](19-tooling-and-testing.md)), `zig build`, the dev server with headers.
  - The main thread / engine worker split with the fallback.
  - `heap()` with assertions.
  - The input ring.
  - A WebGPU clear driven by `eng_frame` + `eng_render_prepare`.
  - Per-tick and per-frame arenas.
  - Counters into a minimal HUD.
- **M1:** the upload API (small batched writes), command-list decoding, readback slots (picking).
- **M2:** the staging ring for streaming (E5), the job system with plugin workers (generation), memory accounting.
- **M3:** the plugin host on both the engine thread and the workers; per-plugin memory caps; plugin hot reload (E4).
- **M4+:** saves through the persistence worker ([15](15-persistence.md)); core hot reload through save/load.

## 9. Open questions

- **Q01.1 Core language: Zig, C++ or Rust?**
  - Why it matters: everything is written in it, and it is the hardest decision to reverse.
  - Recommended default: **Zig**, pinned, conditional on the E1 spike (debugging works, builds are fast). C++ (clang +
    wasi-sdk, no Emscripten) if the spike fails.
  - Do you write Rust? If so, it deserves a fair look.
- **Q01.2 Which browsers and devices must work at launch?** (merged into questions.md C5)
  - Why it matters: it sets the fallback work (engine on the main thread), the memory caps, and whether mobile
    shapes the renderer.
  - Recommended default: desktop Chrome/Edge and Safari 26+ on macOS as tier 1; Firefox as tier 2; no mobile.
- **Q01.3 Minimum hardware?** (merged into questions.md C5)
  - Why it matters: it fixes the frame budget, GPU memory and blob budgets. Every prototype number is from an M1 Max.
  - Working assumption: the design point is an **M1 Max at 1080p / 60 fps**; the minimum is an **M1 base or a
    recent integrated GPU at 720p / 30 fps**; no mobile.
- **Q01.4 Hosting.** Can production hosting set COOP/COEP headers (Cloudflare Pages / Netlify `_headers`), or must we
  live with GitHub Pages and `coi-serviceworker`?
  - Recommended default: a host with header control. Keep the non-isolated fallback working for embeds.
- **Q01.5 Plugin memory cap.** Is a declared per-plugin cap of 64 MB by default (raisable in the manifest) acceptable
  to the plugin authors you have in mind?
  - Why it matters: memory replicated into workers is the main budget risk.
  - Recommended default: yes; first-party plugins justify anything above 64 MB.
- **Q01.6 Release builds.** Ship `ReleaseSafe` (bounds and overflow checks, slower) or `ReleaseFast`?
  - Recommended default: measure in M0; ship ReleaseSafe unless the gap is over 15%.
