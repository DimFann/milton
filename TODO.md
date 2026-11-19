# Canvas-space tile renderer

Implemented on `feature/canvas-tile-cache`. Original strokes remain authoritative;
there is no canvas-format change or permanent flattening.

## Implemented

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
- [x] Reuse resident coarse and fine levels while navigating. Do not execute
  reconstruction jobs while navigation controls remain active.
- [x] Incremental idle reconstruction with an approximately 4 ms cooperative
  budget between GPU-completed jobs; publish completed tiles, not half-rendered
  intermediates. Revalidate jobs each frame instead of retaining obsolete view
  requests. Working strokes finish affected tiles immediately.
- [x] Full-quality deterministic export through the reference renderer,
  independent of display tiles and navigation previews.

## Limits and follow-ups

- [ ] Replace conservative accumulated-padding replay with an operation-level
  tile dependency graph for extremely long overlapping blur histories. Jobs
  exceeding the workspace/dependency budget currently log a diagnostic and use
  full replay for correctness.
- [ ] Preempt large individual jobs and working-stroke reconstruction. The
  current frame budget is cooperative, not a hard GPU-time guarantee.
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
