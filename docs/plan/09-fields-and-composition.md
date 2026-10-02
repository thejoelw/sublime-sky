# 09. Fields and composition

Status: draft · Depends on: [05](05-blob-model.md), [06](06-blob-hierarchy-and-cut.md),
[08](08-plugin-system.md) · Used by: [10](10-simulation-and-scheduling.md), [11](11-physics.md),
[12](12-rendering.md), [14](14-planet-generation.md), [15](15-persistence.md), [16](16-entities-and-creatures.md)

## 0. Short answers to Joel's questions

**"Somehow we'll have to determine the ordering for everything reading and writing to a field F ... maybe there needs
to be some kind of `@checkpoint` specification ... Do you have a better idea?"**

Your instinct is right. Underneath, `@checkpoint`s define a dependency graph over points in a field's composition, and
the loader can sort and validate that graph. The proposal keeps that idea and changes three things:

1. **The field's definer declares the checkpoints up front, as an ordered list of named phases.** For example
   `std:ground_height` has `base → shape → cover → override → final`. Contributors pick a phase. They don't invent
   checkpoint names that other plugins then have to guess.
2. **Each phase declares how its contributions combine.**
   - Most phases are *commutative*, with a declared operator (`add`, `mul`, `min`, `max`). Contributions in them
     can't depend on each other, so their order doesn't matter. The core combines them in a fixed canonical order,
     so float rounding is reproducible too.
   - Only phases marked `ordered` run contributors one after another. There, `before` / `after` constraints, or a
     deterministic tie-break, set the order.
3. **The loader builds one graph whose nodes are `(field, phase)` pairs.**
   - A read like `ground_height@shape` ("the value after the `shape` phase") is an edge to that node.
   - This resolves your "field A calls field B which calls field A" case: B may read "A as of an earlier phase"
     while A's later phase reads B. A real cycle is a load error that names the path (§4.5).

Most fields need **no ordering at all**:
- **Owner dispatch:** the core calls each blob type's owner only for its own blobs.
- **Type tables:** most per-type values are constants, so the core reads them from a table without calling the
  plugin.
- **Deltas:** stored state is changed by deltas, which commute.

Your flammability example needs zero code from the hut plugin (§4.12a). That matches what `planet/` found: "no
ordering was ever needed, and neither were the phases (add / scale / clamp) I proposed for computed fields". There,
everything was a stored field written by deltas. Phases are for the few cases where that isn't enough, such as two
plugins both flattening terrain.

**"We might need an export providing the `@init` checkpoint ... not sure if the core can since it doesn't know the
datatype."**

The core will know the datatype. Every field has a **schema** (dtype, shape, default, unit, aggregation...), declared
in a manifest. The core fills `dst` with the default before the first stage runs. A field read but defined by nobody
is a constant at its declared default, which the load report lists (the `planet/` precedent). No `@init` export is
needed.

**Your notation and the proposal, side by side:**

| | Joel's checkpoints | phases (proposal) |
|---|---|---|
| a contributor | export `ground_field@d@e`: "I supply checkpoints d and e" | `{ "field": "std:ground_height", "phase": "shape", "op": "add" }` |
| reading the value at a point | import `ground_field@a@b@c`: "no supplier of a, b or c may come after my read" | read `std:ground_height@shape`: "after every contribution up to and including `shape`" |
| reading the final value | list every checkpoint | `std:ground_height` (implicitly `@final`) |
| who names checkpoints | every contributor | the field's definer, once |
| two contributors with no relation | order undefined, so the loader must pick one | in a commutative phase the order provably doesn't matter; in an ordered phase you get `before` / `after`, or a tie-break by plugin id plus a load-report warning |
| initialising `dst` | an export at the start of the chain (`@init`) | the core, from the schema's dtype and default |
| how a stage gets "the value so far" | it calls the imported previous `ground_field` | the core passes it in (`dst` pre-filled, §4.4) |
| validation | a DAG over checkpoints | a DAG over `(field, phase)` nodes, plus a commutativity contract |

The two have the **same expressive power**:
- A phase is a checkpoint whose suppliers are exactly the contributors declared in it.
- Any partial order of checkpoints fits into ordered phases with `before` / `after`.

What phases add:
- one shared vocabulary per field;
- a contract that lets most contributions be order-free;
- a default "final";
- clearer errors.

## 1. Purpose and scope

**In scope:**
- what a field is: its schema and **domains** (per blob, per point, per body);
- stored fields: how they are written, restricted, lifted, pushed down and persisted;
- computed fields: base sources (type tables, owner dispatch) and composition by phases;
- the load-time `(field, phase)` graph;
- evaluation: batching, caching, and when values are read;
- names, namespaces and the `std` vocabulary;
- computed fields on coarse blobs (closures);
- fields the renderer sees.

**Out of scope:**
- the plugin ABI and the manifest's overall shape: [08](08-plugin-system.md) (this doc specifies the field parts);
- the hierarchy's split protocol and the generic smooth lift on irregular blob layouts:
  [06](06-blob-hierarchy-and-cut.md);
- tick stages, stage snapshots, canonical order and commit points: [10](10-simulation-and-scheduling.md);
- GPU packing: [12](12-rendering.md);
- residual storage: [15](15-persistence.md);
- the terrain field's actual content: [14](14-planet-generation.md).

## 2. Requirements

| | requirement | from |
|---|---|---|
| must | Several plugins can contribute to a shared quantity (flammability, ground height) without knowing about each other. | pillar 4, Joel's flammability example |
| must | Composition order is defined, validated at load, and identical on every machine. | D5, D12 |
| must | Fields default to their declared default (usually zero). Missing contributors are fine. | Joel |
| must | Stored state is consistent across levels: restrict∘lift = identity, exactly to float precision. | D8, `planet/` (2e-16) |
| must | Writes from many plugins commute (deltas). | D17, `planet/PLUGINS.md` §1.2 |
| must | Batched evaluation (handles in, arrays out). | Joel's ABI style |
| should | Coarse blobs give unbiased values for computed fields (closures), keeping the observer effect small. | pillar 5, `planet/DESIGN.md` §2.8 |
| should | A shared vocabulary of common physical fields with units. | pillar 4 |
| should | The renderer can show fields (wetness, snow, char, glow, sediment) on any blob at any level. | pillar 2 |
| later | Cross-tick caching of computed fields with dependency tracking. | performance |

## 3. What the prototypes taught

From `blobby-test/planet/` (`js/world.js`, `js/plugin.js`, `js/plugins/*.js`, `DESIGN.md`, `PLUGINS.md`):

1. **A schema per field, declared once, is enough.**
   - `{ name, agg: sum|mean|max, lift, push, unit, persist, finest, liftReads }`.
   - Several plugins may declare the same field only if `agg`, `lift` and `unit` agree. `resolvePlugins` checks
     this.
2. **Restrict is the only relation written by hand.** One word (`sum`, area-weighted `mean`, `max`).
   - The lift is a prior followed by an exact projection (`projectMean`), so a plugin's prior can never break
     consistency.
   - Measured worst relative error: 2e-16 to 6e-15 after 20 simulated years.
3. **Push-down modes:**
   - `mean`: every descendant shifts by the same delta;
   - `scale`: multiplicative, for non-negative intensive fields;
   - `amount`: negative deltas proportional to the current amount, positive ones by area;
   - named plugin weights, such as vegetation's room-to-grow vs stress;
   - `none`: written only where stored.

   Additive push-down of precipitation turned it negative, then runoff, then discharge, then NaN. Hence `scale`.
4. **Priors may only read persisted state.** A tree-count prior that read discharge (regenerated after an unload)
   brought back the wrong number of trees through a stored residual.
5. **Lift order was the only order that mattered.** `liftReads` was sorted topologically. A temperature lift needs
   the children's elevations first.
6. **Constants.** A field read but owned by no installed plugin is a constant, reported at load.
7. **Coarse closures must be the expectation of the fine function under the lift prior.**
   - The coastal cell's tree capacity uses the expected land fraction (a normal CDF over the relief the lift will add),
     not "mean elevation > 0".
   - That took a coarse capacity from 1.97 M to 1.29 M trees, against 1.30 M summed over its refined children.
8. **Core structure vs plugin terms.**
   - River conditioning had to ignore volcano cones.
   - The fix tracked the plugins' share of the terrain average separately (`nzT`).
   - In phase terms, rivers needed "ground as of before the plugins' shape terms", which is
     `std:ground_height@base` below.
9. **Shape terms** (contributions to the continuous terrain field) were additive, at any point, with a value and a
   gradient, and a `band` limiting how fine the evaluation goes (`surfaceAt(dir, band)`: a 10 m parcel feels 10 m
   slopes inside a 10 km cell).

## 4. Design

### 4.1 The field schema

Every field has exactly one **definer**: the core's `std` vocabulary, or one plugin, in its manifest's `fields`.

| attribute | values | meaning |
|---|---|---|
| `name` | `ns:name` | `std:` or the defining plugin's id; interned to a `u32` id at load (sorted by name) |
| `domain` | `blob` \| `point` \| `body` | indexed by blob handles, by positions in a body frame (with a `band`), or by body handles (§4.11) |
| `kind` | `stored` \| `computed` | state, or a function of state (§4.2, §4.3) |
| `dtype` | `f32 f64 i32 u32 u16 u8 i8`, `f32x2 f32x3 f32x4`, `bytes:N` (N ≤ 64) | `bytes:N` only for owned, non-aggregated stored fields: the fallback for per-entity state if 16's entity tables are not adopted (R10, `questions.md` C10). `conserved` fields use f64 or a fixed-point q64, pending Q02.5 |
| `default` | a literal | what the core fills before the first stage, and the constant if no one defines the field |
| `unit` | an SI string (`m3`, `K`, `m/s`, `kg/m3`, `1`) | checked for agreement; shown by tools |
| `clamp` | `[lo, hi]` | computed fields: applied by the core at `final`; stored intensive fields: at commit (§4.2) |
| `finite`, `nonneg` | flags | validated at commit; a violation is a fault of the writer ([08](08-plugin-system.md) §4.11) |
| **stored only** | | |
| `agg` | `sum` \| `mean` \| `max` \| `min` \| `none` | restrict. `mean` is weighted by the blob's **measure** (area for surface blobs, volume for volume blobs; [06](06-blob-hierarchy-and-cut.md) defines it) |
| `lift` | a generic prior (`uniform`, `by_measure`, `smooth`, `copy`, `zero`) or `export:<name>` | the prior. The core always projects afterwards so that restrict(children) = parent (§4.6) |
| `lift_reads` | field names | fields whose children must be lifted first (the order of lifts) |
| `push` | `mean` \| `scale` \| `amount` \| `export:<name>` \| `none` | how a delta on a coarse blob reaches its leaves |
| `write` | `delta` \| `transfer` \| `owned` | the write kinds (R13, §4.2): `delta`, commutative changes from many writers; `transfer`, conserved moves between fields, scaled so no source goes negative; `owned`, only the definer, which may `set` |
| `op` | `add` (default) \| `max` \| `min` | the combinator for `delta` writes |
| `conserved` | flag | the field's total changes only by declared sources and sinks: the core keeps a ledger of `delta` totals per writer, `transfer`s move amounts exactly, and push-down and restrict preserve totals (10 §4.4 checks them in regression tests) |
| `persist` | `residual` \| `absolute` \| `never` | 15's keys: `residual` stores `x − L(R(x))` on touched subtrees (needs linear aggregation); `absolute` stores the value; `never` regenerates |
| `persist_tau` | seconds of game time | e-folding time of a stored residual after unloading; 0 = regenerated below the top level (15) |
| `persist_on` | `write` \| `touch` | `write`: any write touches the blob (salient fields such as lava); `touch`: only explicit `ss_touch` and edits (15) |
| `finest` | a level | not stored below this level: blobs finer than it have no value (they read their ancestor's), and push-down stops there |
| `steward` | a plugin id | for `std` stored fields: the plugin supplying specialised lift and push exports (at most one) |
| **computed only** | | |
| `phases` | an ordered list of `{ name, mode }` (§4.4) | the composition backbone. Defaults to `[base]` |
| `cache` | `none` \| `tick` | per-tick materialisation (§4.7) |
| `render` | `{ slot, encode, range }` | the field drives a renderer slot (§4.10); allowed on stored fields too |

### 4.2 Stored fields

Stored fields are the state: water, snow, temperature, a beaver's hunger.

**Writing: three kinds** (R13; 10 §4.4 owns how they are scheduled and merged).
- **`delta`.** `ss_field_add(field, deltas, handles, n)` from any plugin that declares the field in a process's or
  stage's `writes`.
  - The op is the field's `op`. `add` is the default. `max` / `min` mean "raise to at least" / "lower to at most",
    e.g. `std:fire` intensity.
  - All three commute. The core combines them in **10's canonical order** (`(plugin id, process name, call index)`,
    then position in the batch), so float rounding is identical on every machine and under any load order.
- **`transfer`.** `ss_field_transfer(from_field, from, to_field, to, amount, n)` moves an amount between fields (or
  within one, along routing edges, or into a parcel). The pair must be declared in the process's `transfers`.
  - At the merge barrier, each source's outgoing amounts are summed in canonical order and scaled by
    `min(1, stock / Σ out)`, so no source goes negative and mass is conserved exactly (10 §4.4).
  - This is how melt (snow → water), routing (flow → flow) and entrainment (snow → parcel) are written. The
    prototype's "take what's there" calls became order-independent this way.
- **`owned`.** `ss_field_set` from the definer only. With a single writer, call order is already deterministic. This
  is a refinement of D17 ("processes write deltas"): **shared state is written by deltas and transfers; a plugin's
  private state may be set by its single owner.**
  - For a `std` field marked `owned` (such as `std:emitted_power`, §4.8), "the definer" means **the blob's owner**:
    each plugin sets the value only on blobs of its own types, so there is still exactly one writer per blob.
- **Structured state** (where trees stand in a count, where snow lies inside a cell) is never written directly by
  other plugins. They call the owner's service ([08](08-plugin-system.md) §4.10, the `planet/` lesson). The schema
  can mark a delta field `structured: true`. Only its steward and services it exposes may then write it.

**Writes on coarse blobs are push-down.** `ss_field_push(field, delta, handles, n)` on a blob that has loaded
children distributes the delta to its leaves with the field's `push` mode:

| `push` | rule (delta δ on a coarse blob P with leaf descendants i) | use |
|---|---|---|
| `mean` | every descendant gets δ | intensive fields with structure worth keeping (temperature: the lapse-rate structure of the lifted children survives) |
| `scale` | `x_i *= (x_P + δ) / x_P` (falls back to `mean` when `x_P ≈ 0`) | non-negative intensive fields (precipitation) |
| `amount` | δ < 0: shares proportional to `x_i`; δ > 0: shares by measure | extensive amounts (water, snow) |
| `export:<w>` | shares proportional to plugin weights `w_i ≥ 0` (the fallback is by measure if Σw = 0) | vegetation: growth where there is room, death where trees are stressed |
| `none` | a push is an error; write at leaves only | fields only full-cut processes write (discharge) |

Push-down is recursive, level by level (as in `planet/js/world.js`'s `pushDown`). The sum over the leaves always
changes by exactly δ (extensive fields) or the measure-weighted mean by exactly δ (intensive fields).

**Commit and restrict.**
- Writes are staged per call and committed at 10's merge barrier (stage 5).
- After a commit, the core recomputes the ancestors of every touched leaf by the field's `agg`: incrementally,
  along dirty paths only, once per stage. That replaces the prototype's `restrictAll` after every process step.
- **Reads always see the stage-start snapshot** (10 §4.2, Jacobi semantics). No plugin observes another call's
  staged writes, or its own.

**Validation at commit.**
- `finite`: on by default for floats. `nonneg`: on by default for extensive fields.
- An extensive field going negative means a `delta` writer took more than existed (transfers can't, they are
  scaled). The fault is attributed to that call. This catches the `planet/` "negative precipitation → NaN" chain at
  its first link.

**Lift, projection, persistence:** §4.6 and [15](15-persistence.md). The residual of each persisted stored field is in
the null space of its restrict, as in `planet/DESIGN.md` §2.1.

### 4.3 Computed fields: base sources

A computed field starts from its **base phase**. For each queried blob the core picks the first base source that
applies:

1. **A type table.**
   - A plugin's manifest gives per-type constants: `"types": [{ "name": "wall_thatch", "fields": { "std:flammability": 0.95 } }]`.
   - The core builds one dense table per field, indexed by type id, and evaluation is a gather. No plugin call.
   - This covers most material properties: density, flammability, hardness, friction, porosity.
2. **Owner dispatch.**
   - A plugin declares `{ "field": F, "phase": "base", "types": "own", "export": "..." }`.
   - The core partitions the query's handles by type (a counting sort, O(n)), calls each owner's export with only its
     own blobs (one batched call per owner), and scatters the results back.
   - Used when the value depends on per-blob state: wetness from water content; a canopy's closure from its subgrid
     statistics (§4.9).
3. **The default**, from the schema.

Owner = type definer ([08](08-plugin-system.md) §4.4; a global type is `(plugin u16, type u16)`, R12), so
partitioning by type and partitioning by owner are the same partition. This is what makes "set flammability for my
types" cost O(n) total rather than O(n × plugins), with no ordering between plugins. Each blob gets exactly one base
source.

**Type attributes are not fields.** Some per-type constants are never modified by the environment and are not
numbers anyone composes: [11](11-physics.md)'s `settle` policy (`keep_instance` / `join_ground`), `contact_level`
and the `bedrock` flag, plus `restitution`. They live in the type entry's `physics` block ([08](08-plugin-system.md)
§4.5) and are read from the type table directly. `std:friction` and `std:density` stay computed fields, because wet,
icy or burnt states change them.

### 4.4 Computed fields: phases and how they are evaluated

A computed field's definer declares its phases. Every phase has a **mode**:

| mode | who runs | what a contributor sees and returns | order |
|---|---|---|---|
| `base` | type tables, owners (§4.3) | writes the value for its own blobs | one source per blob, none needed |
| `add`, `mul`, `min`, `max` | contributors declared in this phase | reads `in_value` (the value at the **start** of the phase, read-only); returns an **operand** per blob | none: operands are combined with the op, in canonical plugin-id order (the plugin part of 10's canonical order), then applied once |
| `ordered` | contributors declared in this phase | reads and rewrites `dst` in place (Joel's "call the previous, then modify") | topological order from `before` / `after`; ties by plugin id, with a warning |
| `final` | the core | `clamp` from the schema | — |

**Evaluation of `F@p` for a batch of handles** (blob domain; the point domain is the same, with positions instead
of handles):

```
dst[n] := schema default
for phase in plan(F), up to and including p:
  base:       for each type group:      table hit  -> gather
                                        owner export -> call(owner, sub-batch); scatter
  op in {add, mul, min, max}:
              acc[n] := identity(op)                               // 0, 1, +inf, -inf
              for c in contributors(phase), in canonical plugin order:
                 sub := c.filter(handles)                          // by declared types; default: all
                 operand := identity; c.export(operand, in_value = dst[sub], sub)
                 acc[sub] := combine(acc[sub], operand, op)        // fixed order -> fixed rounding
              dst := apply(dst, acc, op)                           // dst+acc, dst*acc, min(dst,acc), max(dst,acc)
  ordered:    for c in topo_order(contributors(phase)):
                 sub := c.filter(handles); c.export(dst[sub], sub)  // in place
  final:      dst := clamp(dst)
return dst
```

**Why operands in commutative phases.**
- The contributor can't see other contributors' work, only `in_value`. So the commutativity contract is enforced by
  construction, not by trusting plugin authors.
- In-place stages ("I promise I only multiply") can't be checked. Operand stages can.

**Prefer commutative phrasings.** Many "overrides" are commutative after all:
- "fireproof" is `mul` by 0;
- "cap at 0.3" is `min`;
- "at least frozen" is `max`.

Ordered phases are for genuinely order-dependent operations, such as interpolating towards a target (§4.12c).

**What a stage may read.**
- `in_value` (or `dst`) is provided.
- Any other field, or this field at an *earlier* phase, through `ss_field_compute(G, q, ...)`, declared in the stage's
  `reads` (a missing declaration is an error at the call). Each declared read is a graph edge (§4.5).
- Stored fields through `ss_field_read`: committed state, no graph edge.
- Resources and queries ([07](07-spatial-queries.md)).

**Filters.** A contributor declares `"types": "all" | "own" | [type names]`. The core passes it only matching
handles: one type partition per evaluation, shared by all stages. A wetness modifier that only applies to porous
types lists them, and blobs of other types never cross the ABI.

### 4.5 The `(field, phase)` graph

Built at resolution ([08](08-plugin-system.md) §4.6, step 5):

- **Nodes:** `(F, p)` for every computed field `F` and every phase `p` in its plan, including `final`.
- **Edges:**
  - `(F, pᵢ) ← (F, pᵢ₋₁)`: the backbone;
  - for every contributor at `(F, p)` that reads `G@q`: `(F, p) ← (G, q)`;
  - for every base owner export of `F` that reads `G@q`: `(F, base) ← (G, q)`;
  - reading `F` itself at an earlier phase `q < p` is just an edge back along the backbone, so it's legal.
- **Stored fields are leaves.** Reading them is reading state, which creates no edge.
- **Checks:**
  - **cycles:** an error with the path, shown as plugins and phases;
  - **reads of undeclared phases:** an error;
  - **contributions to undeclared phases:** an error;
  - **ordered phases with unconstrained contributors:** a warning, with a deterministic tie-break by plugin id (a
    strict mode turns it into an error, Q09.4);
  - **reads of fields nobody defines:** turned into constants, and reported.
- **The output:** an evaluation plan per field (the stages per phase, in order), used by every evaluation at
  runtime.

**Joel's "A calls B calls A".** Snow both reads the ground and adds to it:
- `std:ground_height` (point domain) has phases `base → shape(add) → cover(add) → override(ordered) → final`;
- `snow:depth` (computed, point domain) has a base stage by snow that needs the ground's slope;
- snow contributes to `std:ground_height` in phase `cover`, adding `snow:depth`.

```
(ground, base) ← (ground, shape) ← (ground, cover) ← (ground, override) ← (ground, final)
                        ↑                  ↑
                        │                  └── snow's cover stage reads snow:depth@final
                        │                                     │
                        └──────── snow:depth base reads ground@shape ← ┘      acyclic: valid
```

If snow's base had read `std:ground_height` (that is, `@final`) instead, the loader reports:

```
error: field cycle
  std:ground_height@cover   ← snow (stage 'add_cover')        reads snow:depth@final
  snow:depth@base           ← snow (stage 'depth_base')       reads std:ground_height@final
  std:ground_height@final   includes @cover
hint: snow:depth probably wants std:ground_height@shape (the ground before snow cover)
```

### 4.6 Lift priors and push weights

These belong to stored fields, and they are where plugins put their knowledge of structure.

- **The prior** is `lift` in the schema:
  - a **generic core prior**:
    - `uniform`: children equal the parent (intensive), or share by measure (extensive);
    - `by_measure`;
    - `copy`;
    - `zero`;
    - `smooth`: a linear prediction from the parent's neighbours, evaluated at each child's centroid. That is
      Harten's stencil generalised to irregular blob neighbourhoods; [06](06-blob-hierarchy-and-cut.md) defines the
      stencil;
  - or a **plugin export**: `fill` (bathtub), `transit` (by river length), `lapse` (temperature),
    `snowline`, `capacity`.
- **The projection is always the core's.** After the prior, children are corrected so that restrict(children)
  equals the parent:
  - `sum`: add the shortfall in proportion to each child's prior, or by measure if the priors sum to 0;
  - `mean`: shift uniformly, as in `planet/`'s `projectMean`;
  - `max`: clamp children to ≤ the parent and raise the one with the largest prior to equal it;
  - `min`: likewise, mirrored.
  - So no plugin can break consistency, and restrict∘lift = identity holds by construction.
- **Order:** lifts run per split batch in `lift_reads` topological order (resolved at load), so a field's prior can
  read its children's values of the fields it depends on (temperature reads the children's ground height).
- **The rules from `planet/`, now enforced:**
  - A prior may read only **persisted state**: the parent's persisted fields, the children's lifted persisted fields
    that it declares in `lift_reads`, resources marked persisted, and hashes. Reads of non-persisted fields are
    rejected for `lift` exports. That makes lifts reproducible after an unload, so stored residuals stay valid.
  - A prior must agree with what coarse processes assume: lifted detail in equilibrium with its parent. This is a
    review rule, not a check. Experiment 7.5 and doc 10's observer-effect tests measure it.
- **The neighbour invariant** ([06](06-blob-hierarchy-and-cut.md)): a lift may read the parent's same-level
  neighbours, which the cut guarantees exist. So a lift is a function of state alone, never of refinement order.

### 4.7 When values are read, batching, and caching

- **Values are read from a snapshot.**
  - Computed fields read committed stored state as of the current stage's start: 10's stage snapshots (§4.2 there),
    which this doc doesn't redefine.
  - So `F@p` for a handle set is a pure function of (state version, handles) within a stage, and any two
    evaluations in that stage agree.
- **Always batched.** One evaluation:
  - partitions by type once;
  - calls each base owner once;
  - calls each phase contributor once per evaluation, never per blob.
- **Cost estimates** (to be measured, experiments 7.1 and 7.2):
  - a counting sort by type: ~1–2 ns per handle;
  - a type-table gather: ~1 ns per handle;
  - each isolated contributor call costs ~0.1–0.3 µs of glue plus copying `in_value` in and the operand out (4–8 B per
    handle; [08](08-plugin-system.md) §4.2.1).
  - Flammability over 100 k blobs with 1 base owner and 2 `mul` contributors: about 0.1–0.3 ms plus the contributors'
    own maths.
- **`cache: tick`.**
  - The first evaluation of `F@final` in a stage stores its results in a per-field column, stamped with the stage
    id. Later evaluations in the same stage gather stamped values and evaluate only the rest.
  - Worth it for fields read by many processes, or many times (flammability read by fire on every front step).
  - Invalidation is trivial: the stamp changes every phase.
- **Cross-tick caching (later).** Keep values while the inputs that feed them are unchanged:
  - the types;
  - the stored fields named in the stages' `reads`;
  - the resources read.

  It needs per-blob version tracking on stored fields, so it waits until profiles ask for it.
- **Async jobs** ([08](08-plugin-system.md) §4.9) get computed fields **pre-evaluated into their snapshot** at their
  declared phases. Workers never run composition themselves, unless the job's own plugin set covers every stage, as
  for split jobs that evaluate `std:ground_height`.

### 4.8 Names and the `std` vocabulary

- **Namespaces.** `std:` is the core's curated vocabulary, versioned with the ABI. Every other field is
  `<plugin id>:<name>`: plugin-private by convention, but readable by anyone who declares the read.
- **`std` fields have a fixed schema and phases**, so any plugin can contribute without a dependency on another
  plugin. A `std` stored field may have one **steward** plugin that supplies its specialised lift and push
  (hydrology for `std:water`). Without a steward, the generic prior applies.
- **Fields are added, never repurposed.** A deprecated field keeps working until the next ABI major version.
- **A seed list** (a proposal; units SI; temperatures in kelvin; display units are the UI's job):

| field | domain, kind | dtype, unit | agg / phases | steward / typical contributors |
|---|---|---|---|---|
| `std:density` | blob, computed | f32, kg/m3 | base | type tables |
| `std:hardness` | blob, computed | f32, Pa | base · `mul` (weathering, frozen) | type tables; snow, climate |
| `std:friction` | blob, computed | f32, 1 (Coulomb μ) | base · `mul` (wet, icy) | type tables; hydro, snow |
| `std:porosity` | blob, computed | f32, 1 | base | type tables |
| `std:flammability` | blob, computed | f32, 1 (0–1, relative spread / ignition propensity) | base · `environment(mul)` · `final` clamp [0, 1] | type tables, vegetation closures; hydro, snow |
| `std:fuel` | blob, computed | f32, kg/m3 combustible | base · `mul` (burnt) | type tables; fire |
| `std:wetness` | blob, computed | f32, 1 | base · `max` (rain on surfaces) | hydro (owner dispatch from `std:water`); climate; `render: wet` |
| `std:snow_cover` | blob, computed | f32, 1 | base | snow; `render: snow` |
| `std:burnt` | blob, stored | f32, 1 | mean, push `mean`, persist `residual` with τ | fire; `render: char` |
| `std:emission_temperature` | blob, computed | f32, K | base (owner dispatch) · `max` | lava parcels and lobes (volcano), flames (fire), glowing creatures; `render: glow` through the blackbody table (13) |
| `std:emitted_power` | blob, stored, `write: owned` per blob owner | f32, W | **sum** (coarse blobs carry their descendants' light, 13 §4.10), push `none`, persist `never` | each emitter's owner sets it on its own blobs |
| `std:sediment` | blob, computed | f32, kg/m3 suspended | base | hydro (from discharge and erosion); `render: sediment` (a flooding river turns brown, 13) |
| `std:temperature` | blob, stored | f32, K | mean, lift `smooth`, push `mean`, persist `residual` | steward climate (`lapse`); fire, volcano deltas |
| `std:water` | blob, stored, `conserved` | f64 (or q64, Q02.5), m3 | sum, push `amount`, persist `residual` with τ | steward hydro (`fill`); transfers from snow, parcels |
| `std:snow` | blob, stored, structured, `conserved` | f64 (or q64), m3 water equivalent | sum, push `amount`, persist `residual` | steward snow (`snowline`) |
| `std:precipitation` | blob, stored | f32, m/s water equivalent | mean, lift `smooth`, push `scale`, `nonneg`, persist `never` | steward climate |
| `std:aerosol` | blob, stored | f32, 1 (optical depth at 550 nm) | mean, push `mean`, `finest` = `ℓ_sim_floor`, persist `never` below the top level | steward climate (relaxation); volcano, fire, impact add deltas; 13 reads the planet's mean for the Mie multiplier |
| `std:fire` | blob, stored | f32, 1 | max, op `max`, lift `zero`, push `none` | fire |
| `std:ground_height` | point, computed | f64, m above datum | base · `shape(add)` · `cover(add)` · `override(ordered)` · `final` | terrain; shape terms (volcano cones, lava lobes, crater rims); snow; structures (§4.12b) |
| `std:flow_velocity` | point, computed | f32x3, m/s | base | hydro (discharge over channel cross-section); [11](11-physics.md) reads it for drag on floating objects |
| `std:wind` | point, computed | f32x3, m/s | base · `add` | climate; local plugins (gusts) |
| `std:air_temperature` | point, computed | f32, K | base · `add` | climate; fire, lava (heat plumes) |

`std:restitution`, the `settle` policy, `contact_level` and `bedrock` are **not** fields: they are per-type physics
constants in the type entry (§4.3, [11](11-physics.md)).

Specific science (tree counts, river discharge, lava, a beaver's hunger) stays in plugin namespaces. Anyone can read
it by declaring the read.

### 4.9 Computed fields on coarse blobs: closures

A computed field is evaluated on whatever blobs are queried, at any level. On a coarse blob it must mean **the
expectation of the fine-level value over its descendants, under the lift prior** (`planet/DESIGN.md` §2.8 #4).
Otherwise refining a region changes its fire behaviour, and the observer effect grows.

**Example: the flammability of a level-6 forest canopy blob** (vegetation-owned).
- A type-table constant "canopy = 0.7" is wrong in both directions: it ignores the clearing fraction and the
  trunk/leaf fuel mix. So vegetation provides the base by owner dispatch.
- It computes `E[f] = Σ_types share_t · f_t` from the canopy's stored subgrid statistics: tree count, species mix,
  land fraction. These are the same quantities its own lift uses.
- The `environment(mul)` stage from hydro is `m(w) = 1 − 0.9·w`. That is linear in wetness, so `E[m(w)] = m(E[w])`,
  and the coarse wetness is fine.
- A nonlinear modifier is different: e.g. ignition as a sigmoid of moisture, `σ((w₀ − w)/s)`.
  - Its contributor must use the distribution of `w` under the lift prior, not just its mean. With a Gaussian
    spread σ_ℓ (the subgrid variance the lift adds at that level), `E[σ] ≈ Φ((w₀ − w̄)/√(s² + σ_ℓ²))`.
  - That is the same trick as `landFraction`'s normal CDF in `planet/js/world.js`.

**Core help.**
- `level` is available for every handle (`ss_blob_get_meta`), so stages can branch on it.
- **Virtual lift (should, M5).** `ss_virtual_children(parents, n, samples, out)` runs the owner's split and the
  field lifts *without inserting into the cut*, and returns sample children with their field values. Contributors can
  estimate or calibrate closures by sampling, and tests use it to measure closure error.
- **Closure-error report (should).** For regions loaded at fine levels, the inspector compares `F(parent)` with the
  measure-weighted mean of `F(children)` per computed field and level ([19](19-tooling-and-testing.md)). This is the
  computed-field counterpart of `planet/tools/observer.mjs`.

### 4.10 Fields the renderer sees

Some fields change how blobs look: snow cover, wetness (darker, glossier), char (burnt), glow (lava, embers),
sediment (muddy water). The renderer can't run plugin code ([08](08-plugin-system.md), Q08.7). It gets values:

- A field marked `"render": { "slot": "snow", "encode": "unorm8", "range": [0, 1] }` feeds one byte of the per-blob
  **modulation word**, which is **appearance words 2–3 of 05's 16-byte appearance record** (R12): 8 × `u8`.
- **Five standard slots, mapped from `std` fields** (names as 05's channels):

  | word | slot | source |
  |---|---|---|
  | 2 | `wet` | `std:wetness` |
  | 2 | `snow` | `std:snow_cover` |
  | 2 | `char` | `std:burnt` |
  | 2 | `glow` | `std:emission_temperature` through 13's blackbody table (from ~800 K; 0 below) |
  | 3 | `sediment` | `std:sediment` |

- **Up to three custom slots** (the rest of word 3) may be claimed by plugin fields, assigned at resolution in
  canonical order. The plugin's render material says what a custom slot does (tint toward a second colour,
  roughness, emissive) through the core's material parameters. That is data, not shaders.
- `std:emitted_power` is not a slot: 13 gathers emitters from the render tier using that stored sum field.
- **Packing** is the two appearance words, alongside the 32-byte geometry record (05).
  - The core re-encodes it for blobs whose source fields changed in the last commit, from the commit's dirty lists,
    and the renderer uploads the dirty ranges.
  - Coarse blobs carry their restricted (stored) or closure (computed, materialised) values, so far snowfields and
    burn scars show at every level.
  - [12](12-rendering.md) owns the buffer layout, the upload and the shading.
- **Computed render fields** are materialised for visible blobs once per *frame-relevant* tick (`cache: tick`), not
  per frame. Their cost is bounded by the commit dirty lists plus the cut's changes.

### 4.11 Fields on points and bodies

- **The point domain**: `ss_field_compute_at(F, phase, dst, points, n, body, band)`.
  - `points` are f64 positions in the body's frame. `band` is the finest level to resolve: contributors skip octaves
    and terms finer than the band, as `surfaceAt(dir, band)` did.
  - There is no type dispatch. The base is the definer, or the field's declared base plugin.
  - Contributors should declare a **support**, a resource listing bounding spheres such as each volcano cone's
    footprint, so the core calls them only for points inside it (should, M4). Otherwise every contributor sees every
    point.
  - The gradient is part of the value for fields that need it: `f64x4` = value + gradient for `std:ground_height`,
    because priors and parcels need slopes ([11](11-physics.md), [14](14-planet-generation.md)).
- **The point domain describes generator input, not everything in the world** (R7).
  - `std:ground_height` is what lift priors and realisation build kind-A content (ground plates) from, and what
    closures integrate over.
  - It **does** reflect everything that is generator input:
    - the terrain's stored fields and their residuals;
    - shape terms (cones, lava lobes, crater rims, §4.12b);
    - **kind-A edits**: a dig is a `z` residual or a 3D edit term ([14](14-planet-generation.md) §4.8), so digs and
      craters show in `std:ground_height` and every level re-derives them.
  - It does **not** show:
    - kind D placed content: logs, dams, the player's hut, settled objects;
    - kind B and C individuals: trees, boulders, rock outcrops;
    - anything moving: parcels, objects, creatures.
  - Those are visible only to **live queries** over the actual blobs (`ss_surface_along`, `ss_field_eval`,
    [07](07-spatial-queries.md)).
  - So physics, parcels, characters and gameplay use the live queries; priors, realisation and closures use the
    field (Q09.7).
- **The body domain**: a few per-body quantities, such as `std:sea_level` (m), `std:air_density_surface` (kg/m3) and
  a climate's global offset. They use the same machinery, with body handles as indices. Mostly core- and
  climate-defined.
- **Resources are not fields.** They're opaque, schema-tagged state with one writer ([08](08-plugin-system.md)
  §4.10).

### 4.12 Worked examples

**a) Flammability and the hut plugin (scenario 6).**

Joel's version, as described in his message:

```jsonc
{ "imports": { "core.flammability": "flammability" },
  "exports": { "flammability": "hut.flammability" } }
```
```c
// hut.flammability(dst, src, n): call the imported flammability to initialise dst,
// then for each i: if hut owns src[i], dst[i] = table[type(src[i])]
```

Under the proposal, the hut plugin writes **no code** for this:

```jsonc
{ "id": "hut",
  "types": [
    { "name": "wall_thatch", "material": "hut:thatch", "fields": { "std:flammability": 0.95, "std:density": 120, "std:hardness": 2e5 } },
    { "name": "wall_timber", "material": "hut:timber", "fields": { "std:flammability": 0.6,  "std:density": 550, "std:hardness": 4e6 } },
    { "name": "base_stone",  "material": "hut:stone",  "fields": { "std:flammability": 0.0,  "std:density": 2600, "std:hardness": 5e7 } } ] }
```

The plan the loader reports:

```
std:flammability  base[type tables: hut(3), terrain(4), vegetation(6); owner: vegetation(canopy_*)]
                  · environment(mul)[hydro 'wet_damp', snow 'snow_damp'] · final clamp [0,1]
```

- The fire plugin, unchanged, evaluates `std:flammability` for the blobs around its front and burns huts.
- Rain makes thatch harder to ignite through hydro's multiplier, and the hut plugin never hears about it.
- If the hut author later wants smouldering timber, they add an `environment(mul)` contributor filtered to
  `wall_timber`.

**b) Ground height: terrain, volcano, snow, and why rivers read `@base`.**

| phase | mode | contributors | content |
|---|---|---|---|
| `base` | base | terrain | the smooth part reconstructed from terrain's stored blob state, plus continuous fractal detail down to `band` (the `planet/` lift + `detail.js`, [14](14-planet-generation.md)) |
| `shape` | `add` | volcano: cones, lava lobes; crater rims (17's crater record); (later) any landform plugin | the `planet/` shape terms. **Shape terms are contributions to this phase** (R17), each with a support (§4.11); the terrain plugin averages `@shape − @base` into its cells' `nzT` (14) |
| `cover` | `add` | snow: `snow:depth` (reads ground `@shape`) | snow as part of the walked-on surface, where a plugin wants it |
| `override` | `ordered` | structure plugins: foundations, roads | non-commutative flattening (c) |
| `final` | — | core | — |

The readers:
- **river conditioning reads `@base`.** It is 14's `lift_conditioning` extension point (owned by the terrain plugin,
  R17): hydrology contributes river beds, valley floors and no-pit bounds that clamp the children's smooth part, and
  it reads the ground without shape terms. That is the `nzT` fix generalised, so a river never carves a cone flat
  (`PLUGINS.md` §1.2). 06's `std:shape_offset` and `std:river_bed` are not needed;
- snow reads `@shape`;
- realisation of ground plates ([14](14-planet-generation.md) §4.7) reads `@final` (or `@override`, as it chooses),
  so kind-A edits and shape terms reach every level;
- parcels sliding over unrefined ground read `@final` at their band.

**c) An ordered phase: two plugins flatten the same ground.**
- `hut:foundation`: `h ← lerp(h, h_pad, m_pad(x))` under each hut.
- `road:grade`: `h ← lerp(h, h_road(x), m_road(x))` along roads.
- Where a road meets a hut, the order matters. Foundation then road gives a road that cuts the pad's edge; the other
  way round, the pad sits on the road.
- The road plugin declares `"after": ["hut:foundation"]`. Without it, the loader warns and orders by plugin id
  (`hut` < `road`), which is the same order here, but by accident.
- Neither phrasing can be made commutative. That's the test for whether a phase needs to be `ordered`.

**d) A stored field with many writers: `std:snow`.** (This mirrors `planet/`, where `snow` and `flow` were written by
several plugins.)
- **Snow** (the steward): snowfall as a `delta`; melt as a `transfer` (`std:snow` → `std:water` or `hydro:flow`)
  through its `hydro:source` contribution; entrainment as a transfer into fast-layer parcels; its own structured
  stores.
- **Volcano** melts snow under lava: it **calls `snow.melt(handles, volume)`**, because only snow knows where on a
  coarse blob the snow lies (its snowline closure). It doesn't write `std:snow` directly. Snow's service issues the
  transfer, so the meltwater reaches the rivers with mass conserved exactly.
- **Climate** never touches snow. It changes `std:temperature` and `std:precipitation`, and snow reacts on its next
  step.
- All writes commute: deltas are combined and transfers scaled in 10's canonical order. The only rule is the
  structured-field rule (§4.2). `std:snow` is `conserved`, so the ledger checks that its total changed only by
  snowfall and declared sinks.

### 4.13 The canonical scenarios

- **2. The storm and the flood.**
  - Climate's top-band process writes `std:precipitation` with `ss_field_push`. `scale` push keeps every leaf
    non-negative, which was lesson 3's NaN chain.
  - Hydrology's next route reads the leaves' precipitation through `hydro:source`.
  - `hydro:flow` (sum) carries the flood wave to the player's 1 m blobs.
  - Restrict keeps every level exact, so the top level "knows" the flood.
- **3. The beaver dam.**
  - The beaver's state is columns of 16's entity table (`action`, `hunger`, …; R10, the working assumption pending
    `questions.md` C10) plus an entity-scoped plan resource. The fallback without entity tables is owned fields on
    the beaver's root blob (`beaver:hunger` `f32`, `beaver:task` `bytes:N`).
  - The dam's logs are the beaver's kind D placed records, not field values; hydrology reads the blockage they make.
  - The pond is `std:water` that hydrology fills via the `fill` prior on refined blobs.
  - When the region unloads, `std:water` restricts into the coarse blob (sum), and the pond's *shape* is a residual
    in the null space of restrict ([15](15-persistence.md)).
- **5. Dig, leave, return.**
  - The dig is a kind-A edit: a `z` residual (`persist: residual`, `persist_tau` = 400 years, the `planet/` value),
    so `std:ground_height` and every level show the hole.
  - Felled trees are vegetation's structured overrides, not field values ([16](16-entities-and-creatures.md)).
  - The rule "priors read only persisted state" is what makes the return reproduce the hole to 1e-12 m, as measured
    in `planet/`.
- **6. A community plugin.** See §4.12a.

### 4.14 Alternatives considered

| alternative | why not |
|---|---|
| **pull chains**: each stage calls "the previous F" itself (Joel's import-the-previous) | the same meaning as the core-driven fold, but each stage costs an extra call and a recursion through every earlier stage, and a stage can forget to call its predecessor. The fold keeps Joel's semantics (`dst` arrives initialised) with fewer calls. |
| **free-form checkpoints** declared by contributors | equivalent power; but there's no shared vocabulary, unrelated contributors have undefined relative order, and nothing marks which stages are commutative (§0) |
| **numeric priorities** (Forge event priorities, WordPress hook priorities) | priorities collide across independent authors, can't express "read F as of before X", and give no commutativity guarantee |
| **materialise every property as stored columns** | 100 properties × 2 M blobs × 4 B = 800 MB, stale between updates, and every type change has to touch all of them. Per-tick materialisation of the few hot fields gets the speed without the cost. |
| **in-place stages everywhere**, trusting plugins to be commutative | can't be validated; one careless `dst[i] = x` silently makes results depend on plugin ids |
| **scalar per-blob calls** (`flammability(handle)`) | per-call overhead × millions; Joel's batch style is the right one |
| **fields only on blobs** | parcels, generation and closures need point queries (`surfaceAt`), and bodies need a few per-body quantities; one machinery with three index kinds is simpler than three machineries |

## 5. Interfaces

### 5.1 Manifest entries ([08](08-plugin-system.md) §4.5 has the whole manifest)

```jsonc
"fields": [                                   // definitions (schema, §4.1)
  { "name": "depth", "domain": "point", "kind": "computed", "dtype": "f32", "unit": "m", "default": 0,
    "phases": [ { "name": "base", "mode": "base" }, { "name": "wind", "mode": "add" } ],
    "cache": "none" },
  { "name": "over", "domain": "blob", "kind": "stored", "dtype": "f64", "unit": "m3", "agg": "sum",
    "lift": "by_measure", "push": "amount", "write": "transfer", "conserved": true,
    "persist": "never" }                       // snow load waiting to slide: regenerated, it lasts hours
],
"provides": [                                 // stages on any field
  { "field": "snow:depth", "phase": "base", "export": "depth_base",
    "reads": ["std:ground_height@shape", "std:air_temperature"] },
  { "field": "std:ground_height", "phase": "cover", "op": "add", "export": "add_cover",
    "reads": ["snow:depth"] },                // = snow:depth@final
  { "field": "std:flammability", "phase": "environment", "op": "mul", "types": "all",
    "export": "snow_damp", "reads": ["std:snow_cover"] },
  { "field": "std:ground_height", "phase": "override", "export": "grade", "after": ["hut:foundation"] }
],
"types": [ { "name": "drift", "fields": { "std:density": 300, "std:friction": 0.1 } } ],
"lifts": { "snowline": "lift_snowline" },
"push_weights": { }
```

### 5.2 Core imports for fields (module `ss`; [08](08-plugin-system.md) §5.2 has the full list)

```c
// computed or stored; phase = PHASE_FINAL (0xFFFFFFFF) for the final value. Reads must be declared.
i32 ss_field_compute   (u32 field, u32 phase, void* dst, const u32* handles, u32 n);
i32 ss_field_compute_at(u32 field, u32 phase, void* dst, const f64* points, u32 n, u32 body, i8 band);
// stored only: the committed value (an ancestor's value for blobs finer than 'finest')
i32 ss_field_read   (u32 field, void* dst, const u32* handles, u32 n);
// stored, write:delta, field declared in the caller's writes; combined with the field's op
i32 ss_field_add    (u32 field, const void* delta, const u32* handles, u32 n);
// stored, a declared transfer pair; scaled per source at 10's merge barrier so no source goes negative (R13)
i32 ss_field_transfer(u32 from_field, const u32* from, u32 to_field, const u32* to, const f64* amount, u32 n);
// stored, write:owned, definer only
i32 ss_field_set    (u32 field, const void* src, const u32* handles, u32 n);
// delta on coarse handles, distributed to the leaves by the field's push mode
i32 ss_field_push   (u32 field, const void* delta, const u32* handles, u32 n);
// should (M5): sample children without inserting them into the cut, for closures and tests
i32 ss_virtual_children(const u32* parents, u32 n, u32 samples, VirtualChild* out, u32 cap);
// ids
u32 ss_phase_id(u32 field, const char* name, u32 len);   // init only
```

### 5.3 Plugin exports for fields (signatures fixed per kind; names bound by the manifest)

```c
// base owner (blob domain): write dst for the given handles (all of this plugin's types)
i32 base_stage(u32 field, void* dst, const u32* handles, u32 n);
// commutative phase: write one operand per handle; in_value is the value at the phase start (read-only)
i32 op_stage(u32 field, u32 phase, void* operand, const void* in_value, const u32* handles, u32 n);
// ordered phase: rewrite dst in place
i32 ordered_stage(u32 field, u32 phase, void* dst, const u32* handles, u32 n);
// point-domain variants: (… , const f64* points, u32 n, u32 body, i8 band) in place of handles
// stored-field priors and weights
i32 lift_prior(u32 field, const u32* parents, u32 n, const u32* child_off, const u32* children, void* dst_children);
i32 push_weights(u32 field, const u32* handles, u32 n, const void* delta, f64* dst_w);
// support of a point-domain contributor (optional): bounding spheres, in the body frame
i32 support(u32 field, u32 phase, f64* dst_spheres /* x y z r */, u32 cap);
```

## 6. Risks and unknowns

- **Phase sprawl.** If every plugin wants its own phase on `std` fields, the vocabulary bloats. Mitigation: `std`
  phases are curated, and plugin-defined fields choose their own.
- **Closures done wrong**, e.g. type-table constants on coarse types: an observer effect where refinement changes
  behaviour. The closure-error report (§4.9) makes it visible. It needs owners to care about levels.
- **Point-field cost.** Without supports, every shape term sees every point. A split batch evaluating
  `std:ground_height` at 400 children × 8 samples × N contributors adds up. Supports should land with the first
  shape term (M4).
- **Copies on deep chains** in the isolated model: with ~5 stages per field over 100 k handles, copies are still
  well under a millisecond (estimate), but materialisation (`cache: tick`) matters for fields read repeatedly.
- **Combine order and floats.** Canonical plugin order fixes rounding only if every machine sees the same set and the
  same batch partitioning. The partition by type must therefore be stable: type ids from resolution, handles in
  canonical order within a type group ([07](07-spatial-queries.md) defines handle order).
- **Render slot scarcity.** Eight bytes per blob may not be enough once plugins want custom looks; doc 12 sizes it.
- **The `std` politics.** Who decides what flammability *means*? The doc's answer is a definition with units and a
  range, curated with the core. That needs an owner (Q09.5).
- **The generator-input vs live-query distinction** (§4.11) is a concept plugin authors must learn. Wrong use (priors
  reading live queries) silently breaks persistence reproducibility. Lift exports may not call live queries; the
  core enforces it.

## 7. First experiments

| # | experiment | success criterion |
|---|---|---|
| 7.1 | **Type tables + owner dispatch** in the core (WASM): `std:flammability` over 1 M handles of 20 types, 3 owners, 1 owner export | ≤ 2 ms total; ≤ 3 ns per handle for the table path |
| 7.2 | **A full plan with isolated contributors**: base + `environment(mul)` with 2 isolated plugins over 100 k handles | ≤ 0.5 ms including glue and copies; bit-identical to the statically linked build |
| 7.3 | **Resolution**: field manifests for the seven `planet/` plugins + hut + snow cover; then the deliberate `ground@final` cycle | correct plans in the load report; the cycle error names both stages and hints at `@shape` |
| 7.4 | **Determinism**: evaluate every field with plugin package order permuted and handle batches split differently | bit-identical outputs |
| 7.5 | **Consistency of stored fields on blobs**: port `planet/`'s fields (z, temp, precip, water, flow, snow, trees) onto a blob hierarchy with generic + plugin priors; split and merge randomly for 10 k steps | restrict∘lift = identity to ≤ 1e-12 relative; no negative extensive values |
| 7.6 | **The closure check**: vegetation's canopy flammability closure vs the measure-weighted mean of refined children over 100 coarse blobs | median relative error ≤ 5%, worst ≤ 20% (targets to tune) |

## 8. Build order

| milestone | fields work |
|---|---|
| M2 | stored fields in the core, minimal: schema, `agg`, generic lift and push, projection; terrain state on blobs (core-only, before plugins) |
| **M3** | **fields v1:** full schema and validation, `std` vocabulary v0, type tables, owner dispatch, commutative and ordered phases, the `(field, phase)` graph and load report, `ss_field_*` imports, write ops and commit validation, render slots (with 12); experiments 7.1–7.5 |
| M4 | the point domain (`ss_field_compute_at`) with bands and supports; `std:ground_height` with terrain + one shape term; `cache: tick` |
| M5 | stewards for `std:water` / `std:snow`; plugin priors (`fill`, `transit`, `snowline`, `lapse`); virtual lift and the closure-error report (7.6); field migrations |
| M6 | profile-driven: cross-tick caching if needed; custom render slots for event plugins (ash) |

## 9. Open questions

- **Q09.1 Do you approve phases (declared by the field's definer, each with a mode) as the formal version of
  `@checkpoint`s?** This includes operand-style commutative phases and the `(field, phase)` graph.
  - Why it matters: it's the composition contract every plugin author will learn.
  - Default: **yes**.
- **Q09.2 Should stored fields be changed only by deltas, owned sets and services, with no composition stages on
  stored fields?**
  - Why it matters: it keeps state and derivation separate, which is what made `planet/` order-free. It means "local
    heating near lava" is a delta on `std:temperature`, not a stage.
  - Default: **yes**.
- **Q09.3 Should the `std` vocabulary use SI everywhere (kelvin, m/s water equivalent for precipitation, m3 for
  volumes), with conversion only in the UI?**
  - Why it matters: unit mistakes were real bugs in `planet/` (m³ used as kg in the lava heat balance melted 14 Mm³ in
    an hour).
  - Default: **yes**.
- **Q09.4 For contributors in an ordered phase with no `before` / `after`, should the loader warn and tie-break by
  plugin id, or refuse to load?**
  - Default: **warn** locally; **refuse** in a strict mode used for registry builds and multiplayer sets.
- **Q09.5 Who curates `std`, and how open is it?**
  - Default: curated with the core; versioned with the ABI; proposals by pull request with a definition, a unit and a
    range; plugins otherwise use their own namespace.
- **Q09.6 Is a per-blob modulation word of 8 bytes (05's appearance words 2–3, R12) with 5 standard slots (wet, snow,
  char, glow, sediment) plus up to 3 custom slots enough for field-driven looks?**
  - Default: **yes for v1**. Doc 12 owns the GPU side.
- **Q09.7 Do you accept the distinction between generator-input fields and live queries?**
  - `std:ground_height` reflects generator input, including kind-A edits such as digs and craters (R7).
  - Live queries (`ss_surface_along`, `ss_field_eval`) see the world as it is now: placed content (logs, dams, huts),
    individuals (trees, rocks) and moving things too.
  - Default: **yes**. Priors, realisation and closures use fields; physics and gameplay use queries.
