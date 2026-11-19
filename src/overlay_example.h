// Example demonstrating how to use the draggable overlay icon feature
// Add this code to your main loop (e.g., in milton.cc or sdl_milton.cc)

#include "renderer.h"
#include "utils.h"

// Initialize a draggable overlay icon
// Position is in canvas coordinates, so it maintains position when zooming/panning/rotating
void init_overlay_icon(RenderBackend* r) {
    OverlayIcon icon = {};
    
    // Set the icon properties:
    // - canvas_position: Position in canvas space (maintains with transforms)
    // - size: Size in canvas units
    // - color: RGBA color of the icon
    // - visible: Whether to render it
    
    icon.canvas_position = v2f{100.0f, 150.0f};  // Position in canvas coordinates
    icon.size = v2f{40.0f, 40.0f};                // Size in canvas units
    icon.color = v4f{1.0f, 0.5f, 0.0f, 0.8f};    // Reddish color with transparency
    icon.visible = true;                          // Make it visible
    
    gpu_update_overlay_icon(r, &icon);
}

// Update the overlay icon position (useful for dragging)
void update_overlay_icon_position(RenderBackend* r, float x, float y) {
    OverlayIcon icon = {};
    
    // Set new position in canvas coordinates
    icon.canvas_position = v2f{x, y};
    icon.size = v2f{40.0f, 40.0f};
    icon.color = v4f{1.0f, 0.5f, 0.0f, 0.8f};
    icon.visible = true;
    
    gpu_update_overlay_icon(r, &icon);
}

// Hide/show the overlay icon
void toggle_overlay_icon(RenderBackend* r, b32 visible) {
    OverlayIcon icon = {};
    
    // Copy existing icon properties
    icon.canvas_position = r->overlay_icon.canvas_position;
    icon.size = r->overlay_icon.size;
    icon.color = r->overlay_icon.color;
    icon.visible = visible;
    
    gpu_update_overlay_icon(r, &icon);
}

// Example: Dragging the overlay icon with mouse input
void handle_overlay_drag(RenderBackend* r, MiltonInput* input) {
    // Check if left mouse button is pressed and over the icon
    v2f canvas_pos = r->overlay_icon.canvas_position;
    float half_size = r->overlay_icon.size.x * 0.5f;
    
    b32 hovering = (input->mouse_x >= canvas_pos.x - half_size &&
                   input->mouse_x <= canvas_pos.x + half_size &&
                   input->mouse_y >= canvas_pos.y - half_size &&
                   input->mouse_y <= canvas_pos.y + half_size);
    
    if (hovering && input->mouse_left_pressed) {
        // Update icon position to follow mouse in screen space
        // Convert screen mouse position to canvas coordinates using the same transform
        v2f screen_pos = v2f(input->mouse_x, input->mouse_y);
        
        // Inverse of the forward transform (same as in gpu_render)
        float zoom_center_x = 800.0f; // viewport width / 2
        float zoom_center_y = 600.0f; // viewport height / 2
        
        v2l relative_to_screen = v2i_to_v2l(VEC2L(screen_pos - v2f(zoom_center_x, zoom_center_y)));
        v2f scaled_rel = v2l_to_v2f(relative_to_screen) / (float)r->scale;
        
        float cos_angle = cosf(r->view_rotation);
        float sin_angle = sinf(r->view_rotation);
        v2l rotated = v2i_to_v2l(VEC2L(scaled_rel.x * cos_angle + scaled_rel.y * sin_angle,
                                -scaled_rel.x * sin_angle + scaled_rel.y * cos_angle));
        
        v2f pan_center = v2i_to_v2f(r->render_center);
        v2f new_canvas_pos = zoom_center_x - v2l_to_v2f(rotated) * (float)r->scale;
        
        update_overlay_icon_position(r, new_canvas_pos.x, new_canvas_pos.y);
    }
}
