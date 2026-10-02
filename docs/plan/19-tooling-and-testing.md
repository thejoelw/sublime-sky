# 19. Tooling and testing

Status: draft · Depends on: [01](01-runtime-and-toolchain.md), [02](02-numerics-time-and-determinism.md) · Used by:
every doc (each one's "first experiments" and success criteria run on this tooling)

## 1. Purpose and scope

This doc plans how we work day to day:
- where code lives;
- how it builds and reloads;
- how we know it's correct (determinism, consistency, persistence, images);
- how we know it's fast;
- how we look inside it while it runs;
- how content (blob models) gets made;
- how plugin authors start;
- how we write things down.

It also spells out the collaborative workflow Joel asked for: slower, deliberate, small steps.

In scope:
- repo layout, build commands, the dev server;
- the test pyramid and the harnesses;
- image comparison and benchmark methodology;
- CI;
- debug views, the inspector, the plugin inspector, the event log, the determinism diff, the performance HUD;
- the blob model format and its compiler;
- SDK templates;
- documentation conventions;
- the workflow.

Out of scope: the runtime itself ([01](01-runtime-and-toolchain.md)) and the determinism rules being tested
([02](02-numerics-time-and-determinism.md)).

## 2. Requirements

| | requirement | traces to |
|---|---|---|
| must | One command builds everything; one command runs the dev server with COOP/COEP; edits reload in seconds. | [01](01-runtime-and-toolchain.md) §4.8; the workflow |
| must | Determinism, consistency (`R(L(x)) = x`), persistence round trips and the invariance tests run headless, without a GPU, in CI. | D5, D8, [02](02-numerics-time-and-determinism.md) §4.12 |
| must | GPU rendering is tested against golden images with the prototypes' metrics, on the real GPU. | pillar 2; the prototypes' practice |
| must | Benchmarks follow a method that survives thermal drift and background load. | lessons from `world/` and the ray cut |
| must | Every canonical scenario in the README is an automated test. | README §7 |
| should | Any blob on screen can be inspected at every level, with its fields and history. | `planet/`'s inspector was the most useful debugging tool |
| should | A divergence between two runs is located automatically, down to tick, system, field and key. | [02](02-numerics-time-and-determinism.md) |
| should | A plugin author goes from template to a tested plugin in an afternoon. | pillar 4 |
| should | Every iteration's images are kept (Joel's preference) without bloating the repo. | prior-session notes |
| later | An in-browser blob editor; GPU tests in CI on a self-hosted runner. | |

## 3. What the prototypes taught

- **Headless Node runs of the sim were the backbone of `planet/`.**
  - `tools/test.mjs`: consistency and persistence.
  - `observer.mjs`: the observer effect over 20 years.
  - `divert.mjs`: river diversion.
  - `snowobserver.mjs`: explicit vs implicit avalanches; 10 days took ~35 min.
  - `scenarios.mjs`: a 14-scenario smoke test.
  - `fingerprint.mjs`: field totals to 10 digits, which proved the plugin port bit-identical.
  - All of them ran without a GPU because the sim core loaded in plain Node (`tools/load.mjs`).
- **GPU tests need a real GPU and an unsandboxed Chrome.**
  - `world/tools/cdp.mjs` is a ~100-line DevTools-protocol driver. It launches Chrome with
    `--enable-unsafe-webgpu --use-angle=metal`, opens a page, evaluates, takes screenshots.
  - `tools/multi.mjs` ran shot lists from JSON (`"expr"` steps, returned `snaps`) in one browser session, so the
    world was built once.
  - In agent sessions, GPU work needs the Bash sandbox off (prior-session notes).
- **Image metrics that worked:**
  - the share of pixels whose colour differs by more than 16/255 in any channel;
  - block errors over 32- or 60-pixel blocks.

  Both were used to compare M1–M7 and the LOD levels (`BLOBCUT.md`, `M7.md`).
- **Benchmarks were noisy, and the method mattered.**
  - Thermal state moved GPU timings 10–30%.
  - Other programs (a game client; load 13–23 during the ray cut) loaded the GPU.
  - Pass timestamps overlap on the Apple GPU, so a frame must be timed by its whole span.
  - The fixes: interleave variants in one session, repeat, take the minimum over frames, include a reference shot
    (M3) in every run, and reset every setting per variant ("a leftover debug view silently invalidated one run's
    images").
- **Debug views found the bugs**, again and again:
  - march steps and blob evaluations per pixel (`BLOBMAP.md`);
  - LOD level, owner sampling, albedo, material (`BLOBMAP3.md`);
  - splatted pixels and "how far the shading point moved" (`M7.md`);
  - error against tolerance and node boundaries (`RAYCUT.md`);
  - the planet's colour modes (elevation, temperature, discharge, trees, persistence) and its click inspector, where
    each row is the exact aggregate of the one above (`DESIGN.md` §3).
- **Journey images explain decisions.** Every prototype doc points at `renders/*/journey/` with numbered
  before/after images. They made the write-ups trustworthy, and Joel wants them kept.
- **Determinism problems show up as images.** GPU cell lists built with atomics came out in a different order on
  every build, so re-baked blocks differed and edits weren't local (`M7.md` §7).
  - Golden-image tests are only meaningful if rendering is deterministic for a fixed input on a fixed machine.
    Sort, don't race.
- **Authoring in code worked for creatures.** `blobs/builder.py` (`add`, `sub`, `paint` with `cell`, `tube` along
  Catmull-Rom splines, mirrored parts, bones as parts with pivots and `rotate`) produced five recognisable animals.
  It already exports JSON (`to_json`).

## 4. Design

### 4.1 Repository layout (proposed)

```
sublime-sky/
├─ build.zig, build.zig.zon       the single build entry point (pinned Zig version)
├─ CLAUDE.md                      for AI sessions: where the docs are, how to build/test, conventions
├─ core/                          the engine core (Zig) → core.wasm
│  ├─ src/
│  │  ├─ abi/                     exports to glue and plugins; translate-c of sdk/include/sublime.h
│  │  ├─ base/                    time, rng, libm, math, arenas, pools, hashing (02)
│  │  ├─ space/                   frames, bodies, orbits, universe ids (03, 04)
│  │  ├─ blobs/                   blob store, hierarchy, cut (05, 06)
│  │  ├─ query/                   spatial queries (07)
│  │  ├─ plugins/                 host side: manifests, fields, composition, services (08, 09)
│  │  ├─ sim/                     scheduler, slow/fast layers, physics (10, 11)
│  │  ├─ render_prep/             culling, packing, command lists (12)
│  │  ├─ persist/                 residuals, edit logs, save format (15)
│  │  └─ debug/                   fingerprints, counters, inspector queries
│  └─ tests/                      `zig test` (native) and wasm-in-node suites
├─ sdk/
│  ├─ include/sublime.h           the canonical C ABI (01 §4.1)
│  ├─ zig/, c/                    bindings; header-only RNG and unit helpers (02 §4.11)
│  ├─ rust/                       later
│  └─ templates/{zig,c}/          a minimal plugin: manifest, source, build script, test
├─ plugins/                       first-party plugins: <name>/{manifest.json, src/, assets/, tests/}
├─ web/
│  ├─ index.html
│  ├─ ui/                         main thread: DOM, input capture, UI snapshot reader
│  ├─ glue/                       engine, plugin and persistence workers: instantiation, heap(), plugin host, jobs
│  └─ render/                     WebGPU, passes, WGSL (12, 13)
├─ assets/                        blob model sources (*.blob.json) and their exporters
├─ tools/
│  ├─ dev-server.mjs              static files + COOP/COEP/CORP + watch + reload events
│  ├─ cdp.mjs, shots.mjs          headless Chrome driver; shot lists → images + timings
│  ├─ bench.mjs                   interleaved benchmark runner
│  ├─ fingerprint.mjs, detdiff.mjs   determinism checks and divergence bisection
│  ├─ engines.mjs                 runs the headless suites under Node, Bun (JSC), SpiderMonkey
│  ├─ plugin-test.mjs             loads core + one plugin headless, runs contract checks
│  └─ blobc/                      blob model compiler
├─ tests/
│  ├─ headless/                   consistency, persistence, invariance, determinism, plugin contracts
│  ├─ scenarios/                  the README's six canonical scenarios as scripts with assertions
│  ├─ gpu/                        shot lists, goldens, per-shot thresholds
│  └─ bench/                      benchmark definitions; results/<timestamp>.json
└─ docs/
   ├─ plan/                       these design docs
   ├─ decisions/                  ADRs: NNNN-title.md
   ├─ notes/                      milestone write-ups and log.md
   └─ journey/                    iteration images (storage: Q19.3)
```

**One mono-repo** holds the core, the SDK, the web glue and the first-party plugins (Q19.4). A change to the ABI,
the core and the plugins that use it lands in one commit. Community plugins live in their own repos, made from
`sdk/templates`.

### 4.2 Build and dev loop

| command | does |
|---|---|
| `zig build` | `core.wasm` (ReleaseSafe), every plugin, `build-info.json` (content hashes, Zig version) |
| `zig build test` | native unit tests, then the headless WASM suites under Node |
| `zig build dev` | builds, starts `tools/dev-server.mjs`, watches the sources |
| `zig build test-engines` | the determinism subset under Node, Bun and the SpiderMonkey shell |
| `zig build test-gpu` | GPU shot lists against goldens (needs the real GPU; §4.5) |
| `zig build bench -- <spec>` | an interleaved benchmark run (§4.6) |

**The dev server.**
- Sends `Cross-Origin-Opener-Policy: same-origin`, `Cross-Origin-Embedder-Policy: require-corp` and
  `Cross-Origin-Resource-Policy: same-origin`.
- Pushes reload events over SSE, with three kinds of reload:
  - **Core change:** rebuild `core.wasm`, then save, reload and load. That is the core hot reload of
    [01](01-runtime-and-toolchain.md) §4.10; it falls back to a page reload.
  - **WGSL or render JS change:** recompile the pipelines only.
  - **Plugin change:** rebuild that plugin and swap it at a tick boundary.
- Target: from saving a file to seeing the change, < 3 s for Zig and < 1 s for WGSL (E1).

**URL parameters and a console API**, as in the prototypes:
- URL: `?seed=…&body=<id>&view=<name>&debug=<view>&opts={…}`.
- Console: the engine runs in a worker, so the main thread exposes a proxy, `dbg.call(name, ...args)`, which returns
  a promise. It forwards to the engine's debug API (`setDebugView`, `setOpt`, `pick`, `inspect`, `fingerprint`,
  `step`, `warp`).
- CDP shots use the same proxy, so a shot list can do anything the console can.

### 4.3 The test pyramid

| layer | runs where | what | time budget |
|---|---|---|---|
| 1. unit | native `zig test` | pure functions: RNG, libm, Kepler, binary angles, pools, arenas, key minting, moment maths, parsers | < 10 s, on save |
| 2. headless WASM | Node | everything that doesn't need a GPU: the suites below | PR suite < 2 min; long suites nightly |
| 3. cross-engine | Bun (JSC), SpiderMonkey shell | the determinism subset of layer 2 | < 3 min |
| 4. GPU | headless Chrome over CDP, real GPU | golden images, debug-view images, transition sequences | minutes, on demand and before merging renderer work |
| 5. benchmarks | the same harness | timings with the §4.6 method | on demand; never pass/fail in CI |
| 6. looking | a human | beauty, motion, feel: Joel's review at each milestone, with journey images | each milestone |

**Layer 2 suites:**
- **Determinism:** golden fingerprints per scenario at fixed checkpoints ([02](02-numerics-time-and-determinism.md) §4.12).
- **Invariance:** camera path, handle shuffle, worker count and delay, save/load, frame rate. Each is a pair of runs
  whose fingerprints must be equal.
- **Consistency:**
  - `R(L(x)) = x` for every field, and for geometry (moments) after the lift projection;
  - checked after lifts, long runs and edits;
  - thresholds from `planet/`: worst relative error ≤ 1e-14 for f64, exact for `q64`.
- **Persistence:** unload to `ℓ_sim_floor` (the always-simulated level of an activated planet,
  [06](06-blob-hierarchy-and-cut.md)) and return; the state must be restored. `planet/` restored terrain to
  1e-12 m with the same living trees. Then the decay cases (`planet/`: 3.7 → 3.56 m after 15 years at τ = 400 years).
- **Plugin contracts**, for every plugin:
  - the manifest validates;
  - the imports are in the allowed set;
  - no relaxed-SIMD opcodes;
  - outputs are finite;
  - two instances give identical outputs;
  - memory stays under the cap.
  - Malicious fixtures (a clock import, NaN output, memory over the cap) must be rejected or contained.
- **Scenarios:** the six canonical scenarios (§4.9).
- **Fuzzing:** random command streams and random plugin output perturbations, for N ticks. No traps, no non-finite
  state, fingerprints equal across two runs.
- **Observer-effect experiments** (nightly). These report metrics against thresholds rather than pass/fail: median,
  p90 and max relative difference per field between observed and unobserved runs, as `planet/tools/observer.mjs`
  measured. A regression above the recorded baseline fails the nightly.

**Golden fingerprints and images change only on purpose.** `--update` rewrites them, and the commit message says why.
An unexplained change is a bug.

### 4.4 Determinism tools

- **`tools/fingerprint.mjs`:** runs a scenario and prints the fingerprint per checkpoint, plus readable field totals.
- **`tools/engines.mjs`:** runs the same WASM build under each available engine. Bun gives JavaScriptCore; `jsvu`
  installs the `sm` and `jsc` shells. It diffs the fingerprints.
- **`tools/detdiff.mjs`:** given two configurations (two engines, two commits, two worker counts, or a replay log run
  twice), it:
  1. compares checkpoint fingerprints;
  2. bisects to the first divergent tick, re-running with per-tick fingerprints;
  3. narrows by system scope;
  4. dumps the first differing `(field, key, value_a, value_b)` and the blob's lineage.

  This is the tool that makes D5 livable.
- **Replays:** seed, binary hashes, the starting snapshot and the command log. Every bug report should have one
  attached ([02](02-numerics-time-and-determinism.md) §4.12).

### 4.5 GPU tests

**The harness.** Port `world/tools/cdp.mjs` and `multi.mjs` into `tools/cdp.mjs` and `tools/shots.mjs`.
- A shot list is JSON: page, URL parameters, steps (`expr` to await, `wait` for the cut to converge, `snap`), size.
- One browser session per list, so the world is built once.
- Output: PNGs, per-frame GPU spans, and a JSON summary.

**Comparison** against goldens, with per-shot thresholds:
- the share of pixels differing by more than 16/255 in any channel;
- a 32-px block error (mean absolute difference over blocks);
- a heat-map image of the differences, written beside each failing shot.

**Goldens** are small (960×540; a few 1080p) to keep the repo light. They're regenerated only on purpose, with a
before/after contact sheet in the commit's notes.

**Deterministic rendering for a fixed input on a fixed machine is a requirement** for golden tests.
- Any GPU list built by atomics is sorted before its order can affect a value, as `M7.md` had to do.
- Test shots run with time frozen and the cut converged (wait until no splits are pending).
- E2 checks that repeating a shot gives a 0-pixel difference.

**Browsers.**
- Chrome through CDP is the automated path.
- Safari: `safaridriver` (WebDriver) can load pages and take screenshots, so a reduced shot list runs there (E5).
- Firefox: WebDriver BiDi, when WebGPU ships on the test machine's OS.

**Where it runs.** On the development Mac (in agent sessions, with the sandbox off) and, later, on a self-hosted Mac
runner (Q19.1). Hosted CI runners have no suitable GPU.

### 4.6 Benchmark method

Rules, all learned in `blobby-test/`:
1. **Interleave.** Variants alternate within one browser session, in rounds: A B C, A B C, ….
2. **Take the minimum over frames** (`gpuMin`) as the headline; report the median and p95 too.
3. **Time whole-frame spans.** Pass timestamps overlap on Apple GPUs, so per-pass numbers are indicative only.
4. **Include a reference shot** (a fixed scene and method) in every run, and normalise against it when comparing
   across days.
5. **Reset every setting in every variant.** No state leaks between variants.
6. **Record the environment:** GPU, browser version, OS, power source, machine load average at start and end.
   Refuse to report if the load is above a threshold, or mark the run.
7. **Warm up**, and wait for the cut to converge before timing.
8. **Store results** as `tests/bench/results/<timestamp>.json`, as `wasmgpu/results/` did. Tables in notes are
   generated from those files (`world/tools/tables.py` was the precedent), never typed by hand.

**Performance budgets** (e.g. "the walking view ≤ 16 ms GPU at 1080p on the M1 Max") are tracked as benchmark
outputs with warnings, not CI failures. Thermal noise makes hard thresholds flaky.

### 4.7 Debug tooling

**Debug views.** A registry in the renderer: numbered (the prototypes' habit) and named, switchable from the UI, a
URL parameter or the console. The first set:

| view | shows | precedent |
|---|---|---|
| level | the level ℓ of the blob that owns each pixel, coloured on [06](06-blob-hierarchy-and-cut.md)'s level table, with `ℓ_sim_floor` and `ℓ_view_floor` marked | M5, M6, the ray cut |
| sim tier vs render tier | sim-tier blobs vs render-only blobs (the cut's two tiers, [06](06-blob-hierarchy-and-cut.md); [02](02-numerics-time-and-determinism.md) §4.9) | new |
| node boundaries | [06](06-blob-hierarchy-and-cut.md)'s regions (the render nodes) by level | the ray cut |
| error / tolerance | how far each pixel's blob is from its required accuracy | the ray cut |
| march steps, blob evaluations per pixel | renderer cost | M5, M6 |
| instance / owner / type | blend groups and ownership | M5 v3 |
| splat vs marched | which path drew each pixel | M7 |
| albedo, normal, material, shadow term, AO term | shading inputs | everything |
| field heat map | any stored or computed field by name, with a colour ramp and range | `planet/`'s colour modes |
| touched / persisted | subtrees that will store residuals or edits | `planet/`'s persistence mode |
| requesters | which requester keeps each region refined (observer, band, physics, event, render) | new |

**The inspector**, modelled on `planet/`'s. Click a point: the pick goes through the render, and the result becomes a
`(body, key, position)`. A panel shows:
- **the lineage:** the blob at every level from the top to the leaf. Per row: level, key, handle, owner plugin, type,
  instance, representative shape, bound, and every field's value. Each row is the restriction of the one above it,
  so inconsistencies are visible at a glance.
- **a time series** of chosen fields for the inspected blob, sampled every N ticks into a ring buffer. This is where
  "the river rose three days after the storm" gets seen.
- **why it's loaded:** the requesters and their priorities.

**The plugin inspector.**
- the loaded plugins with hashes, memory used against cap, and time per tick;
- fields owned and read;
- services and extension points;
- incidents (non-finite outputs, stalls);
- the **(field, phase) dependency graph** of [09](09-fields-and-composition.md), drawn as a graph, so composition
  order is something you can see instead of infer.

**The event log viewer.** Events by kind, plugin, body and tick, with filters. Click to move the camera there; scrub
in time. It doubles as the source of the discovery journal ([17](17-events-and-discovery.md)).

**The performance HUD.** It lives in the main-thread UI, fed from the UI snapshot:
- frame: CPU engine time and GPU span;
- ticks per frame, warp, job stalls and job latency per process;
- per-system and per-plugin ms per tick;
- cut size by level and by tier (sim / render), budget, pending splits;
- upload bytes per frame, readback latency;
- memory:
  - core heap;
  - plugin memories;
  - an estimate of GPU memory;
  - the tab's total, via `performance.measureUserAgentSpecificMemory()`, available under cross-origin isolation.
    The ray cut crashed a tab at 4.7 GB without anyone noticing.

**Logging.** A ring buffer in core memory, with levels and per-plugin channels, rate-limited. The glue drains it into
the console and the HUD.

**Time controls.** Pause, single-tick step and warp, all as commands, so they're recorded. Later: rewind, by snapshot
plus replay.

### 4.8 Content authoring

**The blob model format** (`*.blob.json`, source form). It keeps `blobs/builder.py`'s concepts and is
language-neutral:

```json
{
  "format": "blobmodel/1",
  "name": "bear",
  "materials": {
    "fur":  { "col": "#6b4a2e", "col2": "#4a321e", "rough": 0.9, "fuzz": 0.6, "shag": 0.02 },
    "nose": { "col": "#1a1410", "rough": 0.3, "spec": 0.4 }
  },
  "parts": [
    { "name": "body", "parent": null, "pivot": [0, 0.9, 0],
      "atoms": [ { "kind": "add", "pos": [0, 0.95, 0], "r": [0.55, 0.45, 0.8], "rot": [0, 0, 0, 1],
                   "k": 0.12, "mat": "fur" } ] },
    { "name": "leg_fl", "parent": "body", "pivot": [0.25, 0.7, 0.45], "mirror": "x",
      "tubes": [ { "pts": [[0.25, 0.7, 0.45], [0.27, 0.3, 0.5], [0.27, 0.05, 0.55]],
                   "radii": [0.16, 0.12, 0.11], "mat": "fur", "k": 0.06 } ] }
  ]
}
```

- **Atom kinds:** `add`, `sub` (a carve within its instance), `paint`, `cell` (Voronoi paint). These are exactly the
  creature renderer's kinds. Which of them the game's blob model keeps is [05](05-blob-model.md)'s call.
- **Parts are bones.** Each has a pivot; a pose is a rotation per part (builder `rotate`). Mirroring and tubes stay in
  the source for editability and expand at compile time.
- **The material parameters** are the creature atoms' (`col`, `col2`, `rough`, `spec`, `fuzz`, `wrap`, `trans`,
  `shag`, `patch`, `tscale`). The final material model is [05](05-blob-model.md) and [13](13-lighting-atmosphere-and-sky.md)'s.

**`tools/blobc`** compiles a source model into the packed form a plugin embeds:
- the blob records;
- a part (bone) table and a material table;
- bounds;
- **merged LOD levels**: moment-matched merges as in `world/js/lod.js`. Creatures need them at a distance; "the bear
  shows ~100 well-placed atoms go a long way".

It validates (no NaN, positive radii, a known material per atom) and reports counts per level. The bundle format it
writes into is [08](08-plugin-system.md)'s.

**Migration of the prototypes.** Export the five creatures from `blobs/creatures/*.py` through `builder.py`'s
`to_json`, then convert to `blobmodel/1`. They become the first test assets and a regression set (golden images of
each, posed).

**An in-browser editor** comes later, at the creatures milestone. It would add, move, scale and rotate blobs, edit
`k`, materials, parts and pivots, and mirroring, with a live preview in the real renderer. It's a tool for plugin
authors, and maybe for players. Until then, models are authored as code (Python builder or a JS port) or as JSON
(Q19.6).

### 4.9 The canonical scenarios as tests

| scenario | test | assertions |
|---|---|---|
| 1 Orbit to grass | GPU: a scripted search → jump → orbit → embark → walk, with snapshots | no frame after warm-up above budget × 1.5; after arrival the cut converges within 1 s (pixels over tolerance < 1%, the ray cut's measure); no jitter (E7 of 02) |
| 2 The storm and the flood | headless: a storm forced upstream, an observer at a river leaf | discharge at the observer rises ≥ 5× and falls back within days (`planet/` measured 6.8 → 98 → 6.9 m³/s); water conserved exactly (`q64`) or to ≤ 1e-12 |
| 3 The beaver dam | headless with the beaver, vegetation and hydrology plugins | a pond forms within T; after unloading and returning a year later the dam is still there (edit log or residual present) and discharge downstream has changed |
| 4 The asteroid | headless replay of the deterministic schedule, plus a GPU shot of the strike | the crater's removed volume is within tolerance of what the event specified; parcels conserve mass; the crater persists across unload |
| 5 Dig, leave, return | headless | depth restored with the expected decay; felled trees gone; the damaged tree's fate consistent with the ranks |
| 6 A community plugin | headless: the "hut" plugin built from the SDK template | it loads isolated; its flammability composes (the phase graph checks out); a fire burns it; its malicious variants are rejected or contained |

### 4.10 The plugin SDK and templates

`sdk/templates/{zig,c}` each contain:
- a manifest;
- a source file with one blob type, one field provider, one process and one `split`;
- a build script producing the `.wasm`;
- a test.

`tools/plugin-test.mjs` loads the core and the plugin in Node, builds a tiny world, runs N ticks, applies the contract
checks (§4.3) and prints a fingerprint. The SDK's README walks through the ABI ([08](08-plugin-system.md) owns its
content). Rust and AssemblyScript templates follow once the ABI is stable (M3+).

### 4.11 Documentation conventions

- **`docs/plan/`:** living design docs, each with a status line. A proposal becomes a decision when Joel confirms it.
  Questions are numbered `Qnn.m` and collected in `questions.md`, which groups related questions into clusters
  (`C1`–`C22`) so that one answer settles several docs.
- **`docs/decisions/NNNN-title.md`:** one short ADR per confirmed decision, with context, decision, consequences,
  status and date, linking to the plan section. The README's decision table points at these. A decision changes only
  through a new ADR that supersedes the old one.
- **`docs/notes/`:** a write-up per milestone or experiment in the prototype docs' style (what was asked, the short
  answer, measurements, what was learned, what's left), plus a running `log.md` of bugs and numbers, like
  `world/notes/log.md`.
- **`docs/journey/<milestone>/NN_name.png`:** numbered iteration images, including failures. Notes link to them.
- **Commits** say what and why. A changed golden fingerprint or image is explained in the message.
- **`CLAUDE.md`** at the root, for AI sessions:
  - read `docs/plan/README.md` first;
  - the build and test commands;
  - GPU tests need the sandbox off and the real GPU;
  - ask before deciding anything a plan doc marks as open;
  - keep every iteration's images.

### 4.12 The collaborative workflow

The cycle for each step:
1. **Pick a small step** from the current milestone's build order (each doc's §8).
2. **Settle its questions first.** Joel answers in `questions.md` or in conversation, and the answer becomes an ADR.
3. **When unsure, run a small experiment** with its success criterion written *before* running it (every doc lists
   some).
4. **Implement** in small, reviewable commits with tests: unit, then headless, then GPU if it's visual.
5. **Measure and write it up.** A note, journey images, benchmark JSON.
6. **Joel reviews** the code and how it looks, and decides the next step.

Rules:
- **No large unreviewed drops.** If a step grows beyond a day's review, split it.
- **The prototypes are references, not copy sources.** Port on purpose, with tests, citing where it came from.
- **Measure before optimising, and before arguing.** The prototypes repeatedly found the expected cause was wrong
  (M7's "far canopies too bright" were darker).

### 4.13 Disagreement with the README

None.

## 5. Interfaces

The tools consume a few engine-facing core exports beyond [01](01-runtime-and-toolchain.md)'s. They use the `eng_`
prefix, which keeps them apart from the plugin ABI's `ss_` functions ([08](08-plugin-system.md) §5.2):

```c
uint64_t eng_fingerprint(uint32_t scope_mask);                 // 02
uint32_t eng_field_totals(double *dst, uint32_t cap);
uint32_t eng_inspect(const eng_key *key, eng_inspect_out *out);   // lineage, fields, requesters (06, 09)
uint32_t eng_series(const eng_key *key, const uint32_t *fields, uint32_t nf, double *dst, uint32_t cap);
uint32_t eng_events(eng_event *dst, uint32_t cap, int64_t since);   // the event log (17)
void     eng_debug_set(uint32_t opt, const void *val, uint32_t len);   // debug options, shuffle mode, ...
uint32_t eng_replay_begin(const uint8_t *snapshot, uint32_t len);   // 15
```

The debug API seen from the console and shot lists (JS, main-thread proxy):

```js
await dbg.call('setDebugView', 'level')
await dbg.call('pick', x, y)            // → { body, key, pos }
await dbg.call('inspect', key)          // → lineage rows with fields
await dbg.call('waitConverged', { maxMs: 5000 })
await dbg.call('fingerprint')
await dbg.call('warp', 0)               // a command, recorded
```

## 6. Risks and unknowns

| risk | likelihood | impact | mitigation |
|---|---|---|---|
| GPU rendering isn't repeatable, so goldens are flaky | medium | high | sort GPU lists; freeze time; wait for convergence; E2 before any golden |
| goldens drift across Chrome versions | high | medium | per-shot thresholds; record the browser version; update goldens on purpose with contact sheets |
| GPU tests never get a CI home, and rot | medium | medium | `zig build test-gpu` runs locally before renderer merges; a self-hosted runner later (Q19.1) |
| headless suites get slow as the world grows | high | medium | split PR and nightly suites; tiny worlds for PR tests; time budgets enforced |
| tooling eats the time meant for the game | medium | medium | build each tool when the first bug needs it; the inspector and detdiff first, because the prototypes show they pay |
| images bloat the repo | high | low | small goldens; journey images in LFS or external storage (Q19.3) |

## 7. First experiments

| id | experiment | success criterion |
|---|---|---|
| E1 | **Dev loop latency** in M0: edit Zig, WGSL, plugin. | page back with state < 3 s (Zig), < 1 s (WGSL), plugin swapped < 2 s |
| E2 | **Repeatable shots:** port the CDP harness; render one M1 scene 10 times, and twice across a browser restart. | 0 pixels differ |
| E3 | **Cross-engine runner** time in CI. | the determinism subset under Node, Bun and `sm` in < 3 min on a hosted runner |
| E4 | **Golden stability** across a Chrome update (keep the M1 goldens through the next release). | under thresholds without regenerating |
| E5 | **Safari automation:** `safaridriver` loads the WebGPU page in a worker and takes screenshots. | a reduced shot list runs unattended |
| E6 | **detdiff** on a planted bug: a handle-order dependence in a toy reduction. | it reports the first divergent tick, system and key |

## 8. Build order

- **M0:**
  - the repo layout; `build.zig` with the pinned version;
  - the dev server and loop (E1);
  - the unit and headless harnesses; the fingerprint tool and engine runner (E3);
  - CI for layers 1–3;
  - `CLAUDE.md`, an ADR template, `docs/notes/log.md`;
  - logging; a minimal HUD.
- **M1:** the CDP harness and shot lists (E2); the debug-view registry; an inspector v0 for bodies; journey images
  begin.
- **M2:** the blob inspector with lineage; level, sim-tier-vs-render-tier, node and error views; the benchmark runner
  (§4.6).
- **M3:** `plugin-test`, SDK templates, contract checks with malicious fixtures, the plugin inspector with the phase
  graph.
- **M4:** the full performance HUD; detdiff on the real sim (E6); scenario 1 and Safari shots (E5).
- **M5:** scenarios 2 and 5; nightly observer-effect runs; time series in the inspector.
- **M6:** the event log viewer; scenario 4.
- **Later:** the blob editor; scenario 3 (with creatures); a self-hosted GPU runner.

## 9. Open questions

- **Q19.1 Do GPU tests run only on your machine, or on a self-hosted Mac runner in CI?**
  - Why it matters: GPU tests that only run "when remembered" rot.
  - Recommended default: local `zig build test-gpu` before renderer merges until M4, then a self-hosted runner (a Mac
    mini or the M1 Max) once the shot set is stable.
- **Q19.2 Who else will contribute, and when?**
  - Why it matters: it sets how formal review, the SDK docs and the release process need to be, and when the plugin
    ABI must freeze.
  - Recommended default: you plus AI sessions now; a public SDK preview after M3.
- **Q19.3 Where do images live?** Goldens and journey images: plain git, git LFS, or external storage?
  - Why it matters: you want every iteration's images kept; the prototype repo's `renders/` already holds thousands.
  - Recommended default: small goldens in plain git; journey images in git LFS.
- **Q19.4 Mono-repo for the first-party plugins?**
  - Recommended default: yes. Community plugins in their own repos from the templates.
- **Q19.5 Licence for the core and the SDK?**
  - Why it matters: community plugins depend on the SDK's terms.
  - Recommended default: a permissive SDK (MIT or Apache-2.0) so plugins can use any licence. The core's licence is
    your call.
- **Q19.6 Authoring before the editor exists:** keep the Python builder for the first creatures, port it to JS/TS, or
  author JSON directly?
  - Recommended default: export the prototype creatures via `builder.py` now; port the builder to JS when the
    creatures milestone starts; the in-browser editor after that.
