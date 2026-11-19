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
  Publish only GPU-complete whole-view textures, reject stale generations, and
  wake the UI via completion events. Input handling never waits for the worker.
- [x] Preserve fades, resize priority, previews, deterministic exports, and
  resident resolution reuse. Cancel pending work and release all worker-owned
  GL resources before deleting its context; report worker failures explicitly.
- [x] Avoid reallocating foreground viewport textures on same-size pan updates.

- [x] Canvas-space 256-pixel tile cores with sampling gutters and power-of-two
  resolution levels, including negative coordinates.
- [x] Per-layer paint tiles and composited-prefix tiles, validated against exact
  stroke/brush/effect metadata and lower-layer dependencies.
- [x] Content revisions independent of GPU geometry eviction, so moving the
  viewport or render origin does not invalidate unchanged paint.
- [x] Dirty-tile detection and earliest changed history positions for drawing,
  stroke edits, undo/redo, and layer changes.
- [x] Chronological Preserve opacity, pressure, eraser, legacy-stroke, and blur
  replay using the reference stroke renderer.
- [x] Reverse chronological filter-footprint expansion: dependent blur operations
  accumulate their required input padding rather than sharing a fixed halo.
- [x] Cross-layer padded input assembly for erasers followed by blur, preserving
  the existing lower-layer sampling behavior.
- [x] Separate paint/composition caching: layer-opacity changes can recomposite
  retained paint; lower-layer edits invalidate upper paint only where required.
- [x] Lazy allocation, LRU eviction, canvas-replacement cleanup, and a 128 MiB
  tile-texture budget. The reusable workspace has a separate 128 MiB limit that
  includes its possible opacity-history and blur intermediates.
- [x] Retain resident coarse and fine levels across navigation. Stop starting
  reconstruction jobs when navigation controls activate.
- [x] Cross-fade the whole view over 50 ms toward preview and 100 ms toward
  rendered quality. Replace interrupted fades rather than queueing them, and
  hold preview mode throughout active transforms and settling. Hold the preview
  until cached tiles cover the whole view, including resident resolution
  fallbacks; do not reveal individually arriving tiles. Bypass fades for drawing.
  Two viewport-sized RGBA8 snapshots are separate from the tile/workspace budgets.
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
