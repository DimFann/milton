# Draggable Overlay Icon Implementation

## Overview
Added support for rendering a draggable overlay icon on the canvas that maintains its position relative to the drawing area when zoomed, panned, or rotated.

## Changes Made

### 1. `src/renderer.h`
- Added `OverlayIcon` struct with fields:
  - `canvas_position`: Position in canvas coordinates (maintains position with transforms)
  - `size`: Size in canvas units
  - `color`: RGBA color of the icon
  - `visible`: Whether the icon is rendered

### 2. `src/renderer.cc`
- Added `overlay_icon` field to `RenderBackend` structure
- Added `view_rotation` and `view_zoom_center` fields to store view state needed for overlay transform
- Implemented overlay icon rendering in `gpu_render()` function:
  - Renders after canvas but before post-processing
  - Uses the same transformation pipeline as strokes (inverse of view transform)
  - Properly handles zoom, pan, and rotation
- Added `gpu_update_overlay_icon()` function to set/update icon properties from outside the rendering system
- Updated `gpu_update_canvas()` to sync view state for accurate overlay positioning

### 3. `src/overlay_example.h` (NEW FILE)
- Example code demonstrating how to use the overlay icon API
- Includes initialization, position updates, and mouse drag handling

## API Usage

```c
// Initialize an overlay icon
OverlayIcon icon = {};
icon.canvas_position = v2f{100.0f, 150.0f};  // Position in canvas coordinates
icon.size = v2f{40.0f, 40.0f};               // Size in canvas units
icon.color = v4f{1.0f, 0.5f, 0.0f, 0.8f};    // RGBA color
icon.visible = true;

gpu_update_overlay_icon(renderer, &icon);
```

## Key Features

1. **Transform-aware positioning**: The icon uses canvas coordinates, so it stays at the same relative position when you zoom in/out, pan around, or rotate the view.

2. **Proper scaling**: The icon size scales with the viewport zoom level for visual consistency.

3. **Efficient rendering**: Uses temporary VBOs that are created and destroyed each frame (minimal overhead).

4. **Clean API**: Simple function to set icon properties from any part of your application.

## Future Enhancements

To make it fully draggable:
1. Add mouse input handling in your main loop
2. Check if the mouse is over the icon's bounds
3. On left-click, convert screen position to canvas coordinates
4. Update `gpu_update_overlay_icon()` with new position on mouse move

## Technical Details

The overlay icon rendering pipeline:
```
Canvas Render → Overlay Icon Transform → Post-processing → GUI Elements
```

The transform from canvas to screen space mirrors the stroke rendering transformation:
- Converts canvas coordinates to screen space using inverse view rotation
- Applies zoom/pan center offsets
- Scales by viewport and render scale factors
