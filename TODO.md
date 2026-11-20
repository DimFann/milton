# Canvas-space tile renderer

Implemented on `feature/canvas-tile-cache`. Original strokes remain authoritative;
there is no canvas-format change or permanent flattening.

## Implemented

- [x] Windows OS-managed brush outlines, cached by size/shape/orientation rather
  than pointer position, independent of canvas rendering. Oversized footprints
  retain an independently tracked native tip; their canvas preview can lag.
- [x] Prioritize brush resizing over reconstruction and fades. Hold the displayed
  canvas during scrubbing, brush-preview sliders, and size-shortcut settling,
  while updating size feedback; resume the fade without counting paused time.
- [x] Canvas pan/zoom/rotation preempt fades, including wheel and direct view
  changes. Show the transformed preview immediately without old-view blending.
- [x] Undo/redo, including selection history, preempt active and resize-paused
  fades. Show updated preview pixels without tile jobs; refine after settling.
- [x] Pump input before foreground compatibility tile jobs and yield when keyboard/mouse events
  await processing. Wake frame-cap waits on events and match shortcut modifiers
  from each queued key event rather than the latest keyboard state.
- [x] Keep window/event pumping, tablet/keyboard input, UI state, and navigation
  previews on the main thread. Run expensive reconstruction on a lower-priority
  worker with a separate shared OpenGL context and independently owned programs,
  buffers, framebuffer objects, and tile cache. The worker never pumps SDL input.
- [x] Transfer immutable scene snapshots with reference-counted stroke samples.
  Reuse unchanged sample arrays and worker geometry; retain only the latest
  queued request. Runtime content revisions do not change the canvas format.
- [x] Cancel obsolete requests at job boundaries without joining the worker.
  Publish only GPU-complete textures, reject stale generations, and
  wake the UI via completion events. Input handling never waits for the worker.
- [x] Preserve resize priority, previews, deterministic exports, and
  resident resolution reuse. Cancel pending work and release all worker-owned
  GL resources before deleting its context; report worker failures explicitly.
- [x] Avoid reallocating foreground viewport textures on same-size pan updates.
- [x] Initialize renderer dimensions together with its viewport textures, so
  same-size startup resume has a valid presentation size before its first
  frame without restoring per-pan resize housekeeping.
- [x] Retain full-quality same-view paint while drawing and committing strokes.
  Let in-flight drawing finish and display intermediate progress while keeping
  only the latest queued update. Navigation/history and incompatible edits still
  invalidate older frames immediately; new ink remains asynchronous.
- [x] Preserve the worker's full-quality replay prefix across same-view sample
  updates. Updating scene snapshots must not unconditionally invalidate cached
  blur replay prefixes; camera changes still update view state.
- [x] Reproject a pinned GPU snapshot during camera pan/zoom/rotation/flip/resize,
  instead of replaying foreground strokes. Skip the redundant foreground scene
  traversal while the worker owns rendering; do not resize textures or update
  picker resources on a same-size pan.
- [x] Fill newly exposed regions with asynchronous half-resolution, effect-free
  worker frames, reprojecting older compatible coverage rather than waiting for
  the newest camera request. Keep the original snapshot immutable through
  navigation and settling, then replace it as accurate paint completes.
- [x] Invalidate camera snapshots for history, selection transforms, and
  incompatible scene edits. Never pin placeholder background as artwork.
- [x] Retain full-quality views and asynchronous gap previews across camera
  gestures, with scene-revision checks, LRU texture eviction, and conservative
  screen-coverage culling. Clear flattened history for artwork/background edits.
- [x] Add a persistent Settings navigation-cache target (64–2048 MiB, default
  512 MiB), applied live and allocated on demand. This is additional to render
  buffers, the tile pool, and workspace, not a total-process VRAM cap. Preserve
  older settings and binding migrations when appending the new field.
- [x] Verify full/low-resolution revisits with the worker blocked, transformed
  coverage culling against unculled composition, actual GL texture deletion on
  budget reduction, edit/background invalidation, and old/new settings loading.
- [x] Generate near-area and whole-artwork 512-square previews after 200 ms of
  worker idle time. Prioritize new requests between jobs, skip live strokes, and
  use independent overview programs/geometry/targets to preserve replay reuse.
  Keep the two overview levels preferentially within the configured cache target.
- [x] Stream completed native-tile coverage as transparent partial frames over
  the retained preview. Remove whole-view fades from worker presentation;
  history/camera changes discard obsolete partial coverage immediately. Full
  replay fallbacks remain atomic and replace the preview without a fade.
- [x] Explicitly restore worker viewport/VAO state for full and reduced-resolution
  jobs so preview raster pixels correspond to their published camera metadata.
- [x] Show a noninteractive pulsing blue dot beside the resource readout during
  queued or active rendering, including full replay and idle peripheral previews.
  Hide it when idle or the readout is disabled, without treating stale foreground
  upload counts as work.
- [x] Show the eyedropper's currently picked color in a small bordered native
  cursor swatch, updated after sampling and cached by color/DPI rather than
  pointer position. Keep motion independent of GPU presentation; retain the
  plain crosshair for lasso and Alt hover, and the normal cursor over UI.
- [x] Match native swatch packing to the HSV selection, including mixed colors
  and gray. Keep tablet picking authoritative over mouse aliases, synchronize
  the native cursor to the pen without flushing samples, and use zero-based
  pixel bounds when sampling near canvas edges.
- [x] Pin the last completed full-quality frame for CPU-only eyedropper sampling.
  Read pixels on the rendering worker, transfer ownership with completed frames,
  and keep previews/partial results out of the sampling cache. Picking does not
  replay strokes or wait for GPU readback; map camera changes into the captured
  view and defer sampling nonblockingly until the first complete frame exists.

- [x] Canvas-space 256-pixel tile cores with sampling gutters and power-of-two
  resolution levels, including negative coordinates.
- [x] Per-layer paint tiles and composited-prefix tiles, validated against exact
  stroke/brush/effect metadata and lower-layer dependencies.
- [x] Content revisions independent of GPU geometry eviction, so moving the
  viewport or render origin does not invalidate unchanged paint.
- [x] Dirty-tile detection and earliest changed history positions for drawing,
  stroke edits, undo/redo, and layer changes.
- [x] Chronological pressure, eraser, legacy-stroke, and blur
  replay using the reference stroke renderer.
- [x] Remove Preserve opacity rendering, per-color histories, brush options, and
  shortcuts. Ignore its old stroke bit and retire its persisted settings/binding
  slots without shifting other settings. Older artwork now uses ordinary paint.
- [x] Reverse chronological filter-footprint expansion: dependent blur operations
  accumulate their required input padding rather than sharing a fixed halo.
- [x] Cross-layer padded input assembly for erasers followed by blur, preserving
  the existing lower-layer sampling behavior.
- [x] Separate paint/composition caching: layer-opacity changes can recomposite
  retained paint; lower-layer edits invalidate upper paint only where required.
- [x] Lazy allocation, LRU eviction, canvas-replacement cleanup, and a 128 MiB
  tile-texture budget. The reusable workspace has a separate 128 MiB limit that
  includes its possible blur-mask and filter intermediates.
- [x] Retain resident coarse and fine levels across navigation. Stop starting
  full-quality tile reconstruction jobs when navigation controls activate.
- [x] Keep legacy 50/100 ms whole-view fades for the foreground compatibility
  renderer, replacing interrupted fades rather than queueing them. Normal worker
  rendering instead reveals completed native tiles immediately over previews.
  Transition snapshots are separate from tile/workspace budgets.
- [x] Incremental idle reconstruction with an approximately 4 ms cooperative
  budget between GPU-completed jobs; publish completed tiles, not half-rendered
  intermediates. Revalidate jobs each frame instead of retaining obsolete view
  requests. Working strokes reconstruct eagerly on the worker, without making
  the input thread wait; newer samples supersede older queued requests.
- [x] Full-quality deterministic export through the reference renderer,
  independent of display tiles and navigation previews.

## Limits and follow-ups

- [ ] Replace conservative accumulated-padding replay with an operation-level
  tile dependency graph for extremely long overlapping blur histories. Jobs
  exceeding the workspace/dependency budget currently log a diagnostic and use
  full replay for correctness.
- [ ] Preempt large individual jobs and working-stroke reconstruction. The
  current cancellation boundary is between jobs, not inside GPU work. Driver
  contention and presentation can still delay visible pixels even though the
  input thread does not join reconstruction.
- [ ] Improve filtering and resolution selection during continuous zoom.
  Sampling cached raster tiles is not bit-identical to analytic stroke
  rasterization at arbitrary scales/rotations.
- [ ] Consider sharing empty tiles and storing reusable intermediate history
  checkpoints inside heavily edited tiles to reduce cold reconstruction.
- [ ] Broaden device coverage and benchmark very large canvases, many layers,
  sustained edits, cache eviction, and low-memory graphics adapters.

## Validation

The `--tile-cache` hidden-window GPU suite compares aligned tile rendering with
uncached full replay, checks internal boundaries, and reports the measured
maximum channel difference. Small RGBA8 resampling differences are expected;
native reference rendering remains available for exports and bounded fallbacks.
The suite also exercises mixed colors/pressure, repeated blur, erasers, lower
layer dependencies, transforms, working strokes, edits, undo/redo, layer
properties/order, save/load, budgeted refinement, and cache eviction.
Main-render integration checks held navigation without reconstruction, both
coarser and finer resident-level reuse, and preview-only cache cleanup. Cropped
2x exports are pixel-identical before and after tile/navigation activity, with
the view and navigation state preserved.
Whole-view fade checks cover endpoint/intermediate pixels, held previews, reversal,
timer wrap, immediate drawing, main-render undo/redo during active and resize-paused
fades, and main-render completion without tile replay.
Input-priority checks cover event-time shortcut modifiers, yielding before the
first/subsequent tile jobs without consuming queued keys, and resuming refinement.
Worker integration deliberately blocks reconstruction while queued pan and undo/
redo events are processed and displayed. It verifies immutable samples, latest-
request cancellation, zoom/resize, fade/resize interruption, unchanged-frame
reuse, output agreement with synchronous tiles within 2 LSB/channel, pixel-exact
exports during pending work, and shutdown/restart without losing the UI context.
Drawing checks retain rendered pixels with a blocked worker, display completed
intermediate samples despite a newer queued update, and compare pen, eraser,
blur, and committed strokes against synchronous tiles. Strokes
committed entirely between frames also retain rendered paint.
Worker reference-fallback checks verify that warmed pen, eraser, and blur
samples reuse their replay prefix. Blur after the active layer
still runs when its result depends on the new ink.

`--profile-canvas <path>` benchmarks a supplied current-version canvas in a
hidden window at its saved viewport, without saving it or loading user settings.
It reports a GPU-complete cold full replay and the number of blur applications.
It compares direct replay, the foreground tile/fallback path, and the worker
using growing strokes near the view's center-right. Each tool reports six warmed
updates after two warmup updates, including worker installation, reconstruction,
GPU-complete publication, and observed submission-to-display-texture time.
Worker reconstruction timing includes CPU submission; publication can include
waiting for previously submitted GPU work and is not isolated readback cost.
The worker's output is compared with synchronous rendering within 2 LSB/channel.
Warmed-update measurements exclude display refresh/presentation and the initial
cold rebuild, which can still be expensive after camera or history changes.
Navigation profiling also compares raw-stroke replay with snapshot presentation
for pan/zoom/rotation, both with a blocked worker and with live gap coverage.
It also waits for both idle overview levels and reports whole-artwork preview
content. Integration tests verify overview reprojection while the worker is
blocked, completed/transparent partial-frame pixels against full rendering,
immediate tile presentation/cancellation, and replay-prefix reuse after idle work.
Reprojection checks use an independent CPU texture sampler for pan, zoom,
rotation, mirroring, resize, and newly exposed regions (within 2 LSB/channel).
