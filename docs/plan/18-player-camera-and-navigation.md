# 18. Player, camera and navigation

Status: draft · Depends on: [01](01-runtime-and-toolchain.md) (threads, messaging), [02](02-numerics-time-and-determinism.md)
(ticks, input log), [03](03-universe-and-identity.md) (ids, search, sky stars), [04](04-bodies-and-orbits.md) (frames,
bodies), [06](06-blob-hierarchy-and-cut.md) (requesters), [07](07-spatial-queries.md) (surface queries, casts),
[11](11-physics.md) (character controller), [12](12-rendering.md) (layers, picking readback) · Used by:
[13](13-lighting-atmosphere-and-sky.md), [17](17-events-and-discovery.md), [19](19-tooling-and-testing.md)

## 1. Purpose and scope

This doc covers how the player moves through the universe and the world. It spans about twenty orders of magnitude:
from a galaxy seen from outside, down to the grass at their feet.

**In scope:**
- the camera across all scales, and its precision (anchors);
- the views: galaxy browser, system view, planet orbit view, surface, and a debug free-fly;
- transitions ("jumps") between views, and prefetching for them;
- picking bodies, sky stars and surface points;
- search, bookmarks and links;
- the embark and un-embark sequences;
- the player's avatar on a sphere: local up, heading without pole problems, first and third person;
- the interface to the character controller;
- the input pipeline from the main thread to the engine;
- the DOM UI layer, and time controls.

**Out of scope:**
- the character controller's physics (doc 11; only the interface is defined here);
- what the renderer does with the camera (doc 12);
- the journal's content and the rules for discoveries (doc 17; only the UI hooks are here);
- id formats and generation (doc 03);
- frames and bodies (doc 04).

## 2. Requirements

| | requirement | from |
|---|---|---|
| must | jump to any planet or star the player can see, by clicking it or by searching its id; the camera zooms to it | user |
| must | a planet view where the camera slowly rotates around the planet, using the in-game renderer with LOD capped at some level | user |
| must | embark from a chosen location on the planet | user |
| must | walk on a sphere: local up is radial, no gimbal or pole problems, all the way around | constraint "spherical planet you can walk around" |
| must | no jitter at any scale (precision) | D4 |
| must | the player's inputs reach the sim as tick-stamped commands (deterministic, replayable) | D5, multiplayer later |
| should | transitions feel continuous ("powers of ten"), and hide loading | pillar 2; idea 6 |
| should | the UI is minimal; the world is the interface | pillar 1 |
| should | shareable places: id + location + time | pillar 1 (discovery is social) |
| later | other players' avatars; ships; photo mode | |

## 3. What the prototypes taught

- **The prototype's heading was relative to north** (`planet/js/app.js` `cameraState`: `tangentFrame(p)` gives east
  and north, and yaw is measured from north). That breaks at the poles, where north is undefined and yaw flips. The
  game needs a heading that is parallel-transported, not re-derived (§4.7).
- **Camera clearance was a hack:** "3 m: the GPU surface interpolates the cells differently from the CPU query"
  (`app.js`; `PLUGINS.md` §7). The surface used to place the camera and the avatar must be the surface the renderer
  draws: doc 07's `ss_surface_along`, on the same blobs.
- **Double-click to teleport, right-drag to orbit, wheel to zoom from 2 m to orbit** (`planet/index.html`) worked
  well enough to keep as the orbit-view controls. A teleport refined the cut in 15–45 ms (`DESIGN.md` §5, §6), so the
  *sim* side of an embark is cheap.
- **The view side of a new location is not cheap:**
  - the ray cut needed 0.9 s to converge after a 180° turn with prediction rays, 1.2 s without
    (`RAYCUT.md` §6);
  - a newcomer's far view in the streamed pyramid took about two minutes (`BLOBMAP3.md` §3).

  So arriving somewhere must be choreographed: the descent is the loading screen.
- **Prediction rays are the difference between a blurry screen and a finished one** after a snap turn: 63% of pixels
  over 4× the tolerance without them, 0.05% with them (`RAYCUT.md` §6). During a planned transition we know the
  camera's *future path exactly*, which is better than extrapolating it.
- **Picking from the rendered frame** (`M7.md` §7: one frame with a pick pixel; the shade pass records depth, normal
  and kind) makes "dig where I clicked" hit what the player sees. The same applies to "embark where I clicked".
- **The "follow" camera** (`planet/` scenarios: the camera follows a fast-layer group's centroid) made avalanches and
  eruptions watchable. Keep it for events.

## 4. Design

### 4.1 One continuous camera, several contexts

There is one camera with a **focus**: a body, or a point on a body. The camera orbits its focus. The "views" are not
separate modes with separate cameras. They are contexts derived from the focus and the distance, so the zoom is
continuous from the surface to outside the galaxy, like a map going from street to globe:

| context | focus | typical distance | what clicks do |
|---|---|---|---|
| galaxy browser | the galaxy, or a star at interstellar range | 10¹⁵–10²¹ m | select a star; jump to it |
| system view | a star or barycenter | 10⁹–10¹⁴ m | select a planet, moon or star; jump |
| planet orbit view | a planet or moon | ~1.5–20 R | select a surface point; embark; jump to a moon |
| surface | the avatar | metres | interact (doc 17/Q18.1) |
| free-fly (debug) | none | any | debug picks (doc 19) |

**Focus changes:**
- **Zooming out** past the focus's *context distance* re-focuses to its parent. The context distance is the larger of
  the body's SOI and 50× its radius.
- **Zooming in** with the cursor over a body re-focuses to that body.
- **Selecting and jumping** (§4.3) sets the focus explicitly.

The context decides the UI panels, which labels are drawn, and what a click means.

### 4.2 Camera state and precision: anchors

```
Camera {
  anchor: body handle + frame kind (inertial | body-fixed)     // doc 04 §4.2
  offset: f64[3]          // position in the anchor frame
  orient: f64 quat        // orientation in the anchor frame
  fov, exposure hints
}
```

**The anchor is always a nearby body.** When the camera's distance to a body drops below that body's anchor radius,
it re-anchors (exactly, via doc 04's `frame_xform` at the current tick):
- the galaxy, until the camera is within a system's SOI (half the thinning clearance, ~10¹⁴ m);
- then the star (or barycenter) frame;
- then a planet's inertial frame, within its SOI;
- then the planet's body-fixed frame, whenever the camera should move with the ground (embarked, or during a descent).

f64 precision in the anchor frame is then at worst metres in interstellar space, and sub-millimetre near a planet
(doc 04 §4.2).

**Per frame,** the engine computes the camera and every visible body's position in f64, relative to the camera's
anchor. Doc 12 turns these into its integer node keys with per-level camera cells, with no rebasing (doc 02, 12), and
draws them per render layer (galaxy, system, planet, surface; doc 12 owns the layering and the depth ranges).

### 4.3 Transitions ("jumps")

A jump moves the focus to a target (a star, planet or moon, or a surface point) with a framing distance d1:
- a star: ~30 radii;
- a planet: ~3 R, so it fills ~40° of view;
- a surface point: eye height (embark, §4.6).

The path is defined **in the target's frame**, so it follows a target that moves on rails during the transition.

```
d(s)   = exp((1 − s)·ln d0 + s·ln d1)            // log-distance: uniform "powers of ten" speed
u(s)   = slerp(u0, u1, ease(s))                   // direction target→camera: from where we are to the framing direction
pos(s) = target(t) + d(s)·u(s)                    // target(t) evaluated at the current tick, every frame
orient: look at the target; in the last 15% blend to the final framing (a horizon view for an embark)
s(t)   = ease(t / T),   T = clamp(1 + 0.12·|ln(d0/d1)|, 1.5 s, 8 s)
```

- From the galaxy browser to a planet (ln ratio ~40) takes ~6 s. From orbit to the surface (ln ~13) takes ~2.6 s,
  stretched by the readiness gate (§4.6).
- The rotation rate is capped. A comfort option shortens transitions to a quick fade.
- **u1, the framing direction,** puts the sun three-quarters behind the camera for a planet, so it is lit and shows
  its terminator (pillar 2). For a star, the system's plane is seen at ~30°.

**Prefetch (idea 6): a transition's path is known in advance**, so the engine registers requests for it before and
while it plays:
- **render-tier requests along the future camera poses**: the path's samples go to doc 06's `eng_render_prefetch(body,
  path)`, and doc 12 casts prediction rays from poses sampled ahead on the path, at the normal tolerance near the end of the path and at 3× before. This is
  `RAYCUT.md`'s prediction, with an exact path instead of an extrapolated one;
- **generation requests** (doc 03, doc 14): the destination system, the destination planet's top level, and the
  destination's sky stars, issued the moment the jump starts;
- **a pending observer** at an embark destination (§4.6), through doc 06's pending-observer API.

The split into a sim tier and a render tier (doc 06) makes this cheap. Render-only blobs from the path never touch the
sim, and can be dropped the moment the path changes. Generating a planet's top level when its system is entered (doc
14) is separate from *activating* the planet's simulation (doc 10), which only an embark or an event does.

### 4.4 Picking

The main thread sends the click's canvas coordinates. The engine resolves them in this order:

1. **The rendered surface**, if the pixel shows blobs. The pick pixel is read back from the shade pass (depth, normal,
   instance or kind, and the body), as in `M7.md` §7. This takes 1–3 frames of latency, which is fine for clicks.
   It gives a body-fixed point on a planet (orbit view, surface).
2. **Bodies.** Build the f64 ray from the camera. For each loaded body, take the angular distance from the ray to its
   centre. It is a hit within max(its angular radius, the marker radius of 8 px), so planets that are sub-pixel at
   true scale can still be clicked. Pick the smallest normalised distance, and on a tie, the nearest.
3. **Sky stars** (doc 03's `sky_stars` list for the current system): the screen-space nearest within 6 px; on a tie,
   the brightest. This works from the surface too: **looking up at the night sky and clicking a star selects that
   star** (Q18.11). The user's "jump to stars he sees", literally.

Hover runs steps 2 and 3 every frame, which is cheap. Surface hover reuses the last readback.

**Selection** opens a small panel: name, id, type, and for planets embarkable, gravity, day length and local time.
It has "jump", "bookmark" and "copy link" buttons. Jumping is always explicit (one more click, or Enter), so a stray
click never sends the player across the galaxy.

### 4.5 Search, bookmarks, links

- **Search box:**
  - The text goes to the engine's `id_parse` (doc 03). A valid id resolves, the system is generated if needed, and
    the panel is shown. A checksum failure offers doc 03's one-character correction ("did you mean …").
  - Names are searched in a local index of bodies the save knows: visited, bookmarked, journaled, or generated
    nearby. Matches are ranked by edit distance, then nearness. Procedural names aren't unique (doc 03 §4.5), so the
    results show the id beside each name.
- **Bookmarks and recent places** live in the save (doc 15): `{id, p?, label, created tick}`. "Return to" is a jump
  to the body, followed by an embark at `p` if it has one.
- **Links** (doc 03 §4.5) open straight into a transition:
  - with only `b`: to the body's orbit view;
  - with `p`: on into an embark at `p`;
  - with `t` (a tick): an info line offers to show the orbital configuration of that moment. The receiver's own save
    clock is never changed by a link.

### 4.6 Embark

In the planet orbit view, hovering shows a reticle on the surface. The tooltip reads latitude and longitude,
elevation, biome, local time of day, and current weather, from coarse fields (doc 09, 10).

Double-click, or select then "embark here", and the sequence runs:

1. **Validate the spot.**
   - Not deep water and not lava. Otherwise snap to the nearest standing spot within 5 km, by `ss_surface_along` on a
     coarse grid of candidate points. Whether swimming starts are allowed is Q18.8.
   - The spot is a point in the planet's **body-fixed** frame.
2. **Place a pending observer** at the spot (doc 06's pending-observer API; doc 10). It is sim state, and it activates
   the planet (doc 10) if it wasn't already. The sim tier starts refining there at once, through doc 06's
   `refine_request` (effective after the split latency `L_split`): lifts plus the owner plugins' splits. `planet/` measured 15–45 ms per teleport for the lifts; plugin splits add to that.
   - Touched subtrees (doc 15) load with their residuals.
   - The player isn't there yet, so nothing is simulated finer than the bands need.
3. **Re-anchor the camera** to the planet's body-fixed frame (§4.2). The planet stops turning under the camera, and
   the sky starts turning instead. With the ~1 h working-default day (questions.md C2), that is ~0.1°/s, barely
   noticeable. The view's
   angular velocity is blended over 0.5 s.
4. **Descend** (§4.3) from the orbit view to a framing at eye height above the spot, with path prefetch.
5. **Readiness gate.** Before the last 5% of log-distance, the descent requires two things:
   - the sim tier has converged within the observer's radius at the finest band (doc 06's convergence query);
   - the render-tier requests at the final pose are within 2× the tolerance.

   Until both hold, the descent slows asymptotically, up to 10 s extra. If the time runs out, it lands anyway, and
   the remaining detail fades in (doc 12's level fade, `BLOBMAP3.md` §3), never holes.
6. **Spawn the avatar.**
   - Run doc 07's `ss_surface_along` down the radial line through the spot, from the camera's altitude. It returns
     the hit height, the surface normal n and the level, from the rendered surface.
   - If a standing capsule at x overlaps anything (a tree, a rock; doc 07's `ss_overlap` with a `CAPSULE` shape) or is
     under water, search a small outward spiral for a clear standing spot.
   - Create the avatar. For physics it is a dynamic body (doc 04 §4.12): integrated, in the planet's body-fixed
     frame, feet at x. Its appearance is a model instance (doc 16: a shared asset plus bone poses, outside the blob
     store; pending questions.md C10).
   - Up is radial (gravity's direction), not n. Heading is the descent's horizontal direction.
   - The pending observer becomes the avatar's observer.
7. **Attach the camera** to the avatar, in first or third person per preference, and fade in the HUD.

**Un-embark** ("leave") reverses this:
- the avatar body is destroyed and its observer removed;
- the camera ascends to the orbit view (a reverse transition) and re-anchors to the planet's inertial frame;
- the sim tier around the old spot is evicted under the budget as the requests lapse (doc 06). What was touched is
  persisted (doc 15);
- the coarse levels keep simulating the whole planet (D8, doc 10).

### 4.7 On the surface: up, heading, views

**Up** is `u = normalize(position)` in the body-fixed frame. That is radial, matching gravity (doc 04 §4.8), not the
terrain normal.

**Heading without poles.**
- The avatar and the camera carry a heading frame (f, r, u), with forward f ⟂ u.
- After each move, the frame is **parallel-transported**: rotate f by the shortest-arc rotation q(u_old → u_new),
  then re-orthogonalise it against u_new.
- Nothing is ever derived from north, so there is no singularity at the poles. The player can walk over a pole and
  keep walking straight.
- Mouse or stick yaw rotates f about u. Pitch is a separate angle from the tangent plane, clamped to ±89°.
- The compass computes the heading from north for *display only*, and shows "pole" within a few metres of one.
- One side effect is real geometry, not a bug: walking a large closed loop and returning leaves the heading rotated
  by the solid angle the loop encloses (holonomy). E2 confirms it.

**First person:** the eye is at the avatar's eye height, and the view is the heading frame plus pitch.

**Third person:**
- A spring arm behind the avatar, along −f rotated by pitch, with length L ∈ [2, 8] m on the scroll wheel.
- It collides through a sphere sweep, doc 07's `ss_sweep`. Its minimum
  clearance comes from the rendered surface, so no "3 m hack" is needed.
- The arm shortens instantly when blocked and recovers smoothly.

**Free-fly** (debug, doc 19):
- six degrees of freedom in the heading frame, with ×10 speed steps (the prototypes' Shift);
- like every camera, it only makes render-tier requests, so flying around doesn't refine the sim tier;
- a debug toggle creates a separate debug observer that follows the camera, to test sim refinement. The observer is
  sim state; the camera still isn't.

**Follow camera** (doc 17): for an event, orbit a moving point such as a parcel group's centroid or an impactor's
position. It uses the same orbit controls as the planet view, at metre to kilometre distances. The time governor
(doc 10) keeps watched fast events at real time.

### 4.8 The character controller interface

Doc 11 implements the controller as a body-level component. This doc fixes what goes in and out. It is driven
**per tick** from commands, so it is deterministic and replayable:

```c
typedef struct {
  int64_t  tick;            // the sim tick this command applies to
  float    move[2];         // desired motion in the heading frame's tangent plane, |move| ≤ 1
  float    heading_yaw;     // absolute yaw of f, as transported at the end of the previous tick (§4.7)
  float    pitch;           // look pitch (used by interaction rays; doc 17)
  uint32_t buttons;         // JUMP, SPRINT, CROUCH, SWIM_UP, SWIM_DOWN, INTERACT, USE, …
} MoveCommand;

typedef struct {
  double   pos[3], vel[3];  // body-fixed frame
  float    heading[4];      // quat: the transported frame
  uint32_t grounded, medium; // AIR, WATER, SNOW, LAVA …
  float    ground_normal[3];
  float    eye_height;
} ControllerState;
```

The controller resolves movement against the blob field: sweeps, step-up, slope limit, ground snap, swimming
(doc 11). It reports `ControllerState`. The verbs beyond walk, run, jump and swim (climb, glide, fly) are Q18.4.

### 4.9 Input

- **The main thread captures input:**
  - keyboard and mouse events, pointer lock on the canvas element (the element stays in the DOM after
    `transferControlToOffscreen`), and gamepad polling;
  - it accumulates mouse deltas and key states per animation frame;
  - and writes raw input records (kind, value, `performance.now()`) into a **single-producer, single-consumer
    SharedArrayBuffer ring** (doc 01).
  - Without cross-origin isolation, it falls back to `postMessage` per frame.
- **The engine worker** drains the ring each frame and splits it two ways:
  - **View input** (look deltas) is applied to the camera immediately, at render rate, so looking around has no
    tick latency.
  - **Sim input** becomes `MoveCommand`s for the next tick, carrying the heading as of that tick. Commands are
    stamped with the tick they apply to and appended to the **input log** (doc 02). The log is what a replay, a test
    (doc 19) or, later, a network peer consumes.
- **UI commands** (search, jump, embark, time controls) are also tick-stamped commands, and go into the same log.
  Replaying a session reproduces the jumps too.
- **The budget:** input to photon within one frame of a main-thread baseline. E5 measures it.

### 4.10 The UI layer

- **The DOM sits over the canvas, on the main thread.** It never draws per-frame 3D content.
  - Labels, markers, orbit lines and reticles are drawn by the engine (WebGPU). DOM elements positioned from worker
    messages would lag a frame and swim during camera motion.
  - The engine posts UI state at ~10 Hz (the selected body's info, time and date, nearby notables), and events at
    once (discoveries, doc 17).
- **The HUD is minimal:** a crosshair in first person, discovery toasts, a small sun and moon clock. The compass is
  optional.
- **Panels:** search, selection, bookmarks and recent places, the journal (doc 17), settings, the map.
- **The map:** the engine renders the planet's coarse levels into an equirectangular or globe image with doc 12's map
  pass, at a low rate, and transfers it as an `ImageBitmap`. The DOM overlays pins: bookmarks, discoveries, the
  player, and events in progress.
- **System view markers:** at true scale planets are sub-pixel (doc 03 §4.9). They get a ring marker with a minimum
  pixel size and a label, and their orbits are drawn as thin conics from doc 04's `body_orbit`. True scale stays the
  truth; markers are the UI (Q18.9).
- **Time controls:**
  - pause, 1×, and warp steps (×4, ×30, ×600), plus "until dawn" and "until dusk";
  - doc 10's governor caps the rate while fast events are watched, and doc 04 §4.11 decides what integrated bodies
    do at warp;
  - whether players get warp at all, and how fast, is Q18.6.
- **Comfort and accessibility:** a shorter or fade-only transition, an FOV slider, head-bob off, invert Y, key
  rebinding, and "reduce motion" (no auto-rotation in orbit view).

### 4.11 Observer vs camera: who drives the sim

The rule, agreed across docs (06, 10, 12):
- **the sim observer is the avatar**, or a pending observer during an embark. Observers are sim state;
- **cameras only make render-tier requests**: the orbit camera, free-fly, and photo mode alike.

Why:
- **Browsing must not simulate.** The orbit view and the galaxy browser can point anywhere, fast. If the camera were
  the observer, sweeping across a continent would refine and simulate wherever it looked. That is expensive, and it
  is an observer effect driven by browsing.
- **Detail without simulation.** With the sim tier and render tier (doc 06), the camera still gets visual detail
  wherever it looks, through render-only blobs, without the sim noticing.
- **Interest management.** In multiplayer, each peer's interest set (idea 8) should be where its avatar *is*, not
  where its camera *looks*.
- **Free-fly** gets an explicit debug observer (§4.7) for when we want to drive the sim from where the camera is.

The **orbit view's LOD floor** (user: "won't generate LoD below a certain level") falls out of this. With no observer
on the planet, the sim tier doesn't refine; the planet is activated (doc 10) only by an embark or an event. Only the
camera's render-tier requests refine, capped at `ℓ_view_floor` = ℓ10 (doc 06's level table; Q18.7, questions.md
C11). The renderer is the same pipeline; only the cap differs.

### 4.12 Canonical scenarios

**1. Orbit to grass, end to end.**
1. Search `7KQ2-XM9D-3PA4/3` → parse → generate the system (doc 03). The panel shows "rocky, temperate, 0.98 g,
   day 2 h". Press Enter.
2. Jump: about 6 s from wherever the player is. Generation requests for the planet's top level and the destination
   sky start immediately. The camera re-anchors galaxy → star → planet inertial, and the path prefetch fills the
   planet's far levels before arrival.
3. Orbit view: slow auto-rotation, the terminator, the atmosphere's limb. Hovering over a ridge shows "temperate
   forest, 1,240 m, 17:40 local, clear".
4. Double-click → validate → pending observer → body-fixed anchor → descent with path prefetch → the readiness gate
   (sim tier converged, render within 2× tolerance) → spawn by `ss_surface_along` → first person.
5. The player walks to a stream. The heading is transported; the controller works against the blob field.

**2. The storm and the flood.**
- The player watches the storm cross the horizon from the ridge, then uses "×600" until the river rises. Doc 10's
  governor drops back toward 1× if fast events start nearby.
- The avatar is at rest, so doc 04 puts its body in the `resting` mode during the warp.
- The map shows the storm cell as a pin.

**3. The beaver dam.**
- The player notices a beaver (doc 16). With INTERACT on it, the follow camera orbits the beaver while it works.
- The player bookmarks the creek and leaves (un-embark). A year later, "return to" (jump + embark at `p`) shows the
  pond.

**4. The asteroid.**
- A discovery toast says "a moving light in the sky" (doc 17). Clicking it in the sky selects the impactor body
  (step 2 of picking). Its panel shows its predicted impact region and time.
- The follow camera tracks the fireball. The governor holds real time while it is watched.

**5. Dig, leave, return.**
- Un-embark, jump in orbit view to another continent, and embark. Ten years pass with time controls (if Q18.6
  allows it; otherwise via the save's clock).
- "Return to" the bookmark: the descent re-lifts the old region, and its touched subtrees come back with their
  residuals (doc 15). The hole is there.

## 5. Interfaces

**Engine ↔ main thread messages** (doc 01 owns the transport):

```
main → engine   InputRing (SAB)        raw input records
main → engine   UiCommand              {kind: SEARCH|SELECT|JUMP|EMBARK|LEAVE|BOOKMARK|TIME|SETTING, payload}
engine → main   UiState (~10 Hz)       {context, focus info, selection info, time, warp, nearby notables}
engine → main   UiEvent (immediate)    {discovery, notification, link to copy, error}
engine → main   MapImage (low rate)    ImageBitmap + projection parameters
```

**Inside the engine** (core plus renderer glue; C ABI where the core is involved). These are engine-internal names,
not plugin ABI (no `ss_` prefix):

```c
// camera
void camera_set(const Camera* c);
void camera_get(Camera* dst);
void camera_relative(double* dst_pos, const body_h* bodies, uint32_t n, int64_t tick);  // f64, for doc 12

// transitions
uint32_t transition_start(const TransitionDesc* d);     // target, framing, kind (JUMP | EMBARK | LEAVE)
void     transition_poses(Pose* dst, uint32_t n, uint32_t transition, float s_from, float s_to);  // for prefetch rays

// picking (results arrive asynchronously for readback picks)
uint32_t pick_request(float x_px, float y_px, uint32_t flags);   // SURFACE | BODIES | SKY
int      pick_result(PickResult* dst, uint32_t request);         // 0 pending, 1 ready
typedef struct { uint32_t kind; key128 key; double pos_fixed[3]; float normal[3]; uint32_t instance; } PickResult;

// observers and refinement: doc 06's APIs, used as-is (doc 10 owns activation)
//   eng_observer_pending_add/remove   a pending observer at a body-fixed point; promoted to the avatar's (06 §5)
//   eng_cut_converged(...)            is the sim tier converged within the observer ring at the finest band? (06 §5)
//   ss_refine_request(body, reqs)     sim tier, effective after L_split
//   eng_render_prefetch(body, path)       render tier, from transition_poses() samples

// the avatar: a dynamic body for physics (doc 04, 11) whose appearance is a model instance (doc 16; pending C10)
body_h avatar_spawn(body_h planet, const double pos_fixed[3], const float heading[4], key128 model_asset);
void   avatar_command(const MoveCommand* cmds, uint32_t n);    // appended to the input log
void   avatar_state(ControllerState* dst);
```

**Expected from other docs:**
- doc 03: `id_parse`, `id_format`, `name_of`, `sky_stars` with keys;
- doc 04: `frame_xform`, `ss_body_states`, `body_orbit`, dynamic body creation, the `resting` mode;
- doc 06: `eng_render_level_cap` (for `ℓ_view_floor`), `eng_render_prefetch`, `ss_refine_request`,
  `eng_observer_pending_add/remove` and `eng_cut_converged`;
- doc 07: `ss_surface_along` (radial), `ss_sweep` with a sphere (spring arm), `ss_overlap` with a `CAPSULE` shape
  (standing spots);
- doc 16: the avatar's model instance (asset plus bone poses);
- doc 11: the controller;
- doc 12: layers, the pick-pixel readback, the map pass, markers and labels, prediction rays from given poses, level
  fade-in;
- doc 17: discovery events for toasts and the journal.

## 6. Risks and unknowns

- **The readiness gate depends on generation throughput.** The ray cut converged a new 16 km panorama in ~20 s
  (`RAYCUT.md` §10) and a usable view in about 1 s. If plugin splits in WASM are slower than hoped, descents get
  long. Mitigation: start the requests at the first click, not at the descent, and keep the far field coarse at
  first.
- **Re-anchoring discontinuities.** Each re-anchor is an exact f64 conversion. Doc 12's integer node keys and
  per-level camera cells need no rebasing, so the only requirement is that the camera handed to doc 12 is continuous
  across the re-anchor frame. E1 checks it.
- **Engine-drawn UI** (labels, markers) means text rendering in WebGPU. It's a modest amount of work (an SDF font
  atlas), but it is real work in M1.
- **A worker-hosted engine and pointer lock or input latency** are unmeasured in this architecture. E5 checks them in
  M0.
- **Holonomy** may surprise players who walk large loops by compass. It is real geometry, and only the display uses
  north.

## 7. First experiments

| # | experiment | success criterion |
|---|---|---|
| E1 | "Powers of ten": scripted transitions galaxy → star → planet → 2 m above ground, with re-anchoring, in a stub renderer (points and spheres) | no visible jitter at any stage: the frame-to-frame f32 camera-relative position noise of a static object stays < 0.1 px; no pop at re-anchors |
| E2 | Parallel transport: random walks of 10⁵ steps over and around both poles; plus a walk around a 90°–90°–90° triangle | heading continuous, no NaN; the triangle loop returns rotated by π/2 (its solid angle), as geometry predicts |
| E3 | Embark latency on the reference planet (M4): click → playable, with path prefetch and pending observer vs without | with prefetch, playable when the descent ends, ≤ 5 s total, and no holes |
| E4 | Picking at true scale in a generated system: every planet and moon with a marker is clickable; sky-star clicks from the surface select the right key | 100% of visible bodies selectable; sky picks match `sky_stars` keys |
| E5 | Input latency: engine in a worker (SAB ring) vs a main-thread baseline, measured with a test pattern | ≤ 1 frame extra mouse-to-photon |

## 8. Build order

- **M0 foundations:** the input ring and the UI message channel (with doc 01); E5.
- **M1 the sky:**
  - the focus camera, anchors and re-anchoring, the derived contexts;
  - jumps with log-distance transitions;
  - picking of bodies and sky stars; search via `id_parse`;
  - engine-drawn labels, markers and orbit lines;
  - basic links (`b` only);
  - E1, E4.
- **M2 a planet from orbit:**
  - the orbit view with auto-rotation and `ℓ_view_floor` (render-tier requests only);
  - surface picking via readback; the hover tooltip from coarse fields;
  - path prefetch.
- **M4 embark:**
  - the embark and un-embark sequences, pending observers, the readiness gate;
  - avatar spawn; the controller interface; first and third person; parallel transport;
  - free-fly;
  - E2, E3.
- **M5 a living world:** bookmarks and recent places, the map, time controls with the governor, links with `p`
  and `t`.
- **M6 events:** the follow camera, discovery toasts, the journal panel (doc 17).

## 9. Open questions

**Q18.1 What can the player do on the surface?** (merged into questions.md C3)
- Examples: look, walk, swim; interact with creatures (feed, follow, tame?); collect or photograph; dig and build
  (the prototypes' tools)?
- This decides how much of doc 15 (edits) and doc 11 (objects) M4–M5 must support.
- *Recommended:* M4 = look, walk, run, jump, swim, a journal camera ("photograph" = record a discovery); M5 = dig and
  plant. Building later.

**Q18.2 What is the avatar?**
- *Recommended:* a blob creature. For physics it is a dynamic body; its appearance is a model instance (doc 16: a
  shared asset plus bone poses; pending questions.md C10). A few cosmetic choices. In first person, only hands or
  shadow are visible.
- Customisable at all? Visible to other players later, so it needs a key and a description in the save.

**Q18.3 Can the player be hurt, or die?** (merged into questions.md C3)
- Lava, falls, cold, an avalanche.
- *Recommended:* no death. Hazards push the player back or "faint" them to a safe spot, to keep exploration relaxed.

**Q18.4 Movement verbs.** (merged into questions.md C3)
- *Recommended:* walk, run, jump and swim at M4. Climbing smooth blob terrain (Breath-of-the-Wild-like) and gliding
  are strong exploration verbs to consider for M5. Flying stays a debug or creative toggle.

**Q18.5 Is travel between bodies only a jump (a camera move), or ever physical (a ship)?** (merged into questions.md
C19)
- *Recommended:* jump only. It matches the user's description, and doc 04 Q04.5 keeps the door open.

**Q18.6 Time warp for players.** (merged into questions.md C6)
- Waiting for night, a storm or an eruption is part of discovery, but warp is hard in multiplayer later.
- *Recommended:* yes in single player, up to ×600 on the surface and higher in orbit view, governed by doc 10.

**Q18.7 The orbit view's LOD floor.** (merged into questions.md C11)
- *Recommended:* a fixed level, `ℓ_view_floor` = ℓ10 (S2 depth 10; blobs of ~1–2 km). These come from the top-down
  terrain's cheap levels (doc 14), with no plugin fine generators, plus the normal render budget.
- Or should it be set by budget only?

**Q18.8 Where can the player embark?**
- Water (spawn swimming, or on a raft), mountain tops, caves (only if visible from above), the night side.
- *Recommended:* anywhere a standing spot exists within 5 km of the click; water snaps to shore.

**Q18.9 System view scale.**
- *Recommended:* true scale with minimum-size markers and orbit lines.
- Or should bodies be drawn exaggerated, as in many space games?

**Q18.10 Default view: first or third person?**
- *Recommended:* third person for exploration and screenshots of a blobby avatar, with first person a key away.

**Q18.11 Jumping from the ground.**
- Should clicking a star in the night sky from the surface select it, and offer a jump?
- *Recommended:* yes. It makes "jump to a star you see" literal, and the night sky an index of places.
