# Renderer TODO: canvas-space tile caching

## Goal

Reuse completed layer rendering across pan, zoom, and rotation instead of
replaying the visible stroke history after every transform. Keep saved strokes
authoritative: this must not permanently flatten artwork or require a new canvas
file format.

The current renderer has a view-specific replay checkpoint, bounded per-replay
color histories, and region-limited opacity passes. These reduce replay costs but
do not make completed results reusable at arbitrary views.

## Design and implementation

- [ ] Define canvas-space tile coordinates and resolution levels suitable for
  an effectively unbounded, stroke-based canvas.
- [ ] Cache per-layer rendered tiles, keyed by resolution, tile coordinates,
  layer identity, and validated stroke/effect revisions.
- [ ] Preserve chronological stroke processing and opacity-history semantics,
  including matching colors, other-color interruptions, erasers, pressure,
  legacy strokes, and replay checkpoints.
- [ ] Track dirty tiles and their earliest changed history positions for
  drawing, stroke edits, undo/redo, and layer changes.
- [ ] Model blur dependencies across tile boundaries. Render padded regions
  with sufficient neighboring input for every filter stage; propagate changes
  through subsequent blur operations rather than assuming one fixed halo is
  sufficient for an entire stroke history.
- [ ] Separate cached layer paint from final layer opacity/compositing where
  possible, so composition-only changes avoid repainting unchanged tiles.
- [ ] Add a bounded GPU-memory cache with eviction, lazy allocation, and correct
  cleanup on canvas replacement and graphics-context teardown.
- [ ] Reuse available resolution levels during transforms, then refine only
  visible tiles at the required quality. Respect held navigation controls.
- [ ] Schedule reconstruction within a frame budget, cancel obsolete work after
  view or artwork changes, and publish only coherent completed results.
- [ ] Keep export deterministic and full quality, independent of preview state
  or cached display resolution.

## Suggested milestones

1. Add a tile-rendering path alongside the existing renderer and compare output
   for ordinary paint and multiple layers.
2. Support Preserve opacity, erasers, and blur with correct cross-tile replay.
3. Add dirty-region invalidation, bounded caching, and multi-resolution reuse.
4. Add transform previews and budgeted refinement; retire the existing path only
   after correctness and performance checks pass.

## Acceptance checks

- [ ] Compare against uncached full replay at the same target resolution.
- [ ] Test tile seams, transparent backgrounds, large and repeated blurs,
  pressure/hardness, mixed colors, and erasing.
- [ ] Test pan, zoom, rotation, flip, viewport resize, and cropped/scaled exports.
- [ ] Test undo/redo, edits earlier in history, layer visibility/order/opacity/
  effects, opening another canvas, and reopening saved legacy artwork.
- [ ] Measure cold reconstruction, warm transforms, local edits, GPU-memory
  usage, and frame-time spikes on representative complex canvases.
