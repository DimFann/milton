// Copyright (c) 2015 Sergio Gonzalez. All rights reserved.
// License: https://github.com/serge-rgb/milton#license


#include "gui.h"

#include "localization.h"
#include "color.h"
#include "renderer.h"
#include "milton.h"
#include "persist.h"
#include "platform.h"


void settings_init(MiltonSettings* s);  // milton.cc

#define NUM_BUTTONS 5
#define GUI_PANEL_MIN_WIDTH 160
#define GUI_PANEL_MAX_WIDTH 480

// Moves `src` to the position `dst` currently occupies in the layer list (as displayed, top first).
static void
layer_reorder(CanvasState* canvas, Layer* src, Layer* dst)
{
    // Is src above dst? (next = higher layer)
    bool src_above = false;
    for ( Layer* l = src->prev; l; l = l->prev ) {
        if ( l == dst ) { src_above = true; break; }
    }

    // Unlink src.
    if ( src->prev ) { src->prev->next = src->next; }
    if ( src->next ) { src->next->prev = src->prev; }

    if ( src_above ) {
        // Take dst's place; dst moves up. Insert directly below dst.
        Layer* below = dst->prev;
        src->prev = below;
        src->next = dst;
        dst->prev = src;
        if ( below ) { below->next = src; }
    }
    else {
        // Take dst's place; dst moves down. Insert directly above dst.
        Layer* above = dst->next;
        src->prev = dst;
        src->next = above;
        dst->next = src;
        if ( above ) { above->prev = src; }
    }

    Layer* root = dst;
    while ( root->prev ) { root = root->prev; }
    canvas->root_layer = root;
}

// A thin invisible strip that can be dragged to resize the panel. Returns true while dragging and
// stores the drag offset (from where the drag started) in `delta`.
static bool
panel_resize_handle(const char* id, ImVec2 pos, ImVec2 size, ImVec2* delta)
{
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1, 1));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus;
    bool dragging = false;
    if ( ImGui::Begin(id, NULL, flags) ) {
        ImGui::InvisibleButton("##handle", size);
        bool active = ImGui::IsItemActive();
        if ( ImGui::IsItemHovered() || active ) {
            ImGui::GetWindowDrawList()->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                                                      ImGui::GetColorU32(ImVec4(0.4f, 0.6f, 0.9f, active ? 0.8f : 0.5f)));
        }
        if ( active ) {
            *delta = ImGui::GetMouseDragDelta(0, 0.0f);
            dragging = true;
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    return dragging;
}

// Moves `src` to the position `dst` currently occupies in the layer list (as displayed, top first).
// (layer_reorder is defined above.)

// The layers panel sits directly under the color picker, with the same width. The left edge of the
// panel resizes it horizontally and the bottom edge of the layers panel resizes it vertically.
void
gui_layer_window(MiltonInput* input, PlatformState* platform, Milton* milton, f32 brush_window_height, PlatformSettings* prefs, b32 reset_gui)
{
    float ui_scale = milton->gui->scale;
    MiltonGui* gui = milton->gui;
    CanvasState* canvas = milton->canvas;
    const ImVec2 display = ImGui::GetIO().DisplaySize;

    static b32 first_frame = true;
    if ( first_frame ) {
        first_frame = false;
        if ( prefs->layer_window_width > 0 ) { gui->panel_width = (f32)prefs->layer_window_width; }
        if ( prefs->layer_window_height > 0 ) { gui->layers_height = (f32)prefs->layer_window_height; }
        gui_anchor_picker_top_right(gui, milton->view->screen_size.w);
        gpu_update_picker(milton->renderer, &gui->picker);
    }
    if ( reset_gui ) {
        gui->panel_width = ui_scale*240;
        gui->layers_height = 0;
        gui_anchor_picker_top_right(gui, milton->view->screen_size.w);
        gpu_update_picker(milton->renderer, &gui->picker);
    }

    Rect pbounds = get_bounds_for_picker_and_colors(&gui->picker);
    const f32 min_height = ui_scale*140;
    const f32 layers_top = (f32)pbounds.bottom;
    const f32 available = max(display.y - layers_top, min_height);
    f32 height = (gui->layers_height > 0) ? clamp(gui->layers_height, min_height, available) : available;
    f32 width = (f32)(pbounds.right - pbounds.left);
    const f32 grip = ui_scale*6;

    // Resize handles. The panel is anchored on the right, so dragging left makes it wider.
    {
        static f32 start_width = 0;
        static f32 start_height = 0;
        ImVec2 delta = {};
        if ( ImGui::IsMouseClicked(0) ) {
            start_width = gui->panel_width;
            start_height = height;
        }
        ImVec2 hpos = ImVec2((f32)pbounds.left - grip*0.5f, (f32)pbounds.top);
        ImVec2 hsize = ImVec2(grip, layers_top + height - pbounds.top);
        if ( panel_resize_handle("##panel_width_handle", hpos, hsize, &delta) ) {
            gui->panel_width = start_width - delta.x;
            gui_anchor_picker_top_right(gui, milton->view->screen_size.w);
            gpu_update_picker(milton->renderer, &gui->picker);
            pbounds = get_bounds_for_picker_and_colors(&gui->picker);
            width = (f32)(pbounds.right - pbounds.left);
        }
        ImVec2 vpos = ImVec2((f32)pbounds.left, layers_top + height - grip);
        ImVec2 vsize = ImVec2(width, grip);
        if ( panel_resize_handle("##panel_height_handle", vpos, vsize, &delta) ) {
            f32 new_height = clamp(start_height + delta.y, min_height, available);
            // Dragging all the way down goes back to filling the window.
            gui->layers_height = (new_height >= available - 2) ? 0.0f : new_height;
            height = new_height;
        }
    }
    prefs->layer_window_width = (i32)gui->panel_width;
    prefs->layer_window_height = (i32)gui->layers_height;

    ImGui::SetNextWindowPos(ImVec2((f32)pbounds.left, (f32)(pbounds.bottom)));
    ImGui::SetNextWindowSize(ImVec2(width, height));

    static b32 is_renaming = false;
    static i32 layer_renaming_idx = -1;
    static b32 focus_rename_field = false;
    static b32 deleting = false;

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse;
    if ( ImGui::Begin(loc(TXT_layers), NULL, flags) ) {
        // Opacity of the current layer.
        {
            ImGui::AlignTextToFramePadding();
            ImGui::Text(loc(TXT_opacity));
            ImGui::SameLine();
            f32 percent = canvas->working_layer->alpha * 100.0f;
            ImGui::PushItemWidth(-1);
            if ( ImGui::SliderFloat("##opacity", &percent, 0.0f, 100.0f, "%.0f%%") ) {
                // Used the slider. Ask if it's OK to convert the binary format.
                if ( milton->persist->mlt_binary_version < 3 ) {
                    milton_log("Modified milton file from %d to 3\n", milton->persist->mlt_binary_version);
                    milton->persist->mlt_binary_version = 3;
                }
                input->flags |= (i32)MiltonInputFlags_FULL_REFRESH;
                canvas->working_layer->alpha = clamp(percent / 100.0f, 0.0f, 1.0f);
            }
            ImGui::PopItemWidth();
        }

        // Effects of the current layer.
        {
            if ( ImGui::SmallButton(loc(TXT_blur)) ) {
                LayerEffect* e = arena_alloc_elem(&milton->canvas_arena, LayerEffect);
                e->next = milton->canvas->working_layer->effects;
                milton->canvas->working_layer->effects = e;
                e->enabled = true;
                e->blur.original_scale = milton->view->scale;
                e->blur.kernel_size = 10;
                input->flags |= (i32)MiltonInputFlags_FULL_REFRESH;
            }

            LayerEffect* prev = NULL;
            int effect_id = 1;
            for ( LayerEffect* e = milton->canvas->working_layer->effects; e != NULL; e = e->next ) {
                ImGui::PushID(effect_id);
                if ( ImGui::Checkbox("##enabled", (bool*)&e->enabled) ) {
                    input->flags |= MiltonInputFlags_FULL_REFRESH;
                }
                ImGui::SameLine();
                ImGui::PushItemWidth(-ui_scale*34);
                if ( ImGui::SliderInt("##level", &e->blur.kernel_size, 2, 100, "Blur %.0f") ) {
                    if ( e->blur.kernel_size % 2 == 0 ) {
                        --e->blur.kernel_size;
                    }
                    input->flags |= MiltonInputFlags_FULL_REFRESH;
                }
                ImGui::PopItemWidth();
                ImGui::SameLine();
                bool removed = false;
                if ( ImGui::SmallButton("x") ) {
                    if ( prev ) {
                        prev->next = e->next;
                    } else {  // Was the first.
                        milton->canvas->working_layer->effects = e->next;
                    }
                    input->flags |= (i32)MiltonInputFlags_FULL_REFRESH;
                    removed = true;
                }
                ImGui::PopID();
                if ( removed ) { break; }
                prev = e;
                effect_id++;
            }
        }

        // Layer list, top layer first. Drag a row to reorder.
        Layer* drag_src = NULL;
        Layer* drag_dst = NULL;
        ImGui::BeginChild("layer_list", ImVec2(0, -(ImGui::GetFrameHeightWithSpacing() + ui_scale*4)), true);
        if ( !ImGui::IsWindowFocused() ) {
            is_renaming = false;
        }

        const f32 row_h = ui_scale*34;
        const f32 box = ImGui::GetFrameHeight();

        Layer* layer = milton->canvas->root_layer;
        while ( layer->next ) { layer = layer->next; }  // Move to the top layer.
        while ( layer ) {
            ImGui::PushID(layer->id);

            const ImVec2 p = ImGui::GetCursorScreenPos();
            const f32 row_w = ImGui::GetContentRegionAvailWidth();

            // Visibility checkbox, centered in the row.
            ImGui::SetCursorScreenPos(ImVec2(p.x + ui_scale*4, p.y + (row_h - box)*0.5f));
            bool v = layer->flags & LayerFlags_VISIBLE;
            if ( ImGui::Checkbox("##visible", &v) ) {
                layer::layer_toggle_visibility(layer);
                input->flags |= (i32)MiltonInputFlags_FULL_REFRESH;
            }

            const f32 name_x = p.x + ui_scale*4 + box + ui_scale*6;
            ImGui::SetCursorScreenPos(ImVec2(name_x, p.y));
            const f32 name_w = p.x + row_w - name_x;

            if ( is_renaming && layer->id == layer_renaming_idx ) {
                ImGui::SetCursorScreenPos(ImVec2(name_x, p.y + (row_h - box)*0.5f));
                if ( focus_rename_field ) {
                    ImGui::SetKeyboardFocusHere(0);
                    focus_rename_field = false;
                }
                ImGui::PushItemWidth(name_w);
                if ( ImGui::InputText("##rename", layer->name, 13, ImGuiInputTextFlags_EnterReturnsTrue) ) {
                    is_renaming = false;
                }
                ImGui::PopItemWidth();
            }
            else {
                if ( ImGui::Selectable("##row",
                                       milton->canvas->working_layer == layer,
                                       ImGuiSelectableFlags_AllowDoubleClick,
                                       ImVec2(name_w, row_h)) ) {
                    if ( ImGui::IsMouseDoubleClicked(0) ) {
                        layer_renaming_idx = layer->id;
                        is_renaming = true;
                        focus_rename_field = true;
                    }
                    else {
                        is_renaming = false;
                    }
                    milton_set_working_layer(milton, layer);
                }
                if ( ImGui::BeginDragDropSource() ) {
                    ImGui::SetDragDropPayload("MILTON_LAYER", &layer, sizeof(Layer*));
                    ImGui::Text("%s", layer->name);
                    ImGui::EndDragDropSource();
                }
                if ( ImGui::BeginDragDropTarget() ) {
                    if ( const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MILTON_LAYER") ) {
                        drag_src = *(Layer**)payload->Data;
                        drag_dst = layer;
                    }
                    ImGui::EndDragDropTarget();
                }
                ImGui::GetWindowDrawList()->AddText(
                    ImVec2(name_x + ui_scale*4, p.y + (row_h - ImGui::GetFontSize())*0.5f),
                    ImGui::GetColorU32(ImGuiCol_Text), layer->name);
            }

            ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + row_h + ui_scale*2));
            ImGui::PopID();
            layer = layer->prev;
        }
        ImGui::EndChild();

        if ( drag_src && drag_dst && drag_src != drag_dst ) {
            layer_reorder(milton->canvas, drag_src, drag_dst);
            input->flags |= (i32)MiltonInputFlags_FULL_REFRESH;
        }

        // Bottom bar: new / delete.
        const bool can_delete = milton->canvas->working_layer->next || milton->canvas->working_layer->prev;
        if ( !can_delete ) { deleting = false; }
        if ( deleting ) {
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%s", loc(TXT_are_you_sure));
            ImGui::SameLine();
            if ( ImGui::Button(loc(TXT_yes)) ) {
                milton_delete_working_layer(milton);
                input->flags |= MiltonInputFlags_FULL_REFRESH;
                deleting = false;
            }
            if ( ImGui::IsItemHovered() ) { ImGui::SetTooltip("%s", loc(TXT_cant_be_undone)); }
            ImGui::SameLine();
            if ( ImGui::Button(loc(TXT_no)) ) {
                deleting = false;
            }
        }
        else {
            const f32 bw = ImGui::GetFrameHeight() * 1.5f;
            const ImGuiStyle& st = ImGui::GetStyle();
            ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 2*bw - st.ItemSpacing.x - st.WindowPadding.x);
            if ( ImGui::Button("+", ImVec2(bw, 0)) ) {
                milton_new_layer(milton);
            }
            if ( ImGui::IsItemHovered() ) { ImGui::SetTooltip("%s", loc(TXT_new_layer)); }
            ImGui::SameLine();
            if ( !can_delete ) { ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.4f); }
            if ( ImGui::Button("-", ImVec2(bw, 0)) && can_delete ) {
                deleting = true;
            }
            if ( !can_delete ) { ImGui::PopStyleVar(); }
            if ( ImGui::IsItemHovered() ) { ImGui::SetTooltip("%s", loc(TXT_delete)); }
        }
    } ImGui::End();
}

static char
hotkey_sanitize_char(char c)
{
    if ( c >= 'A' && c <= 'Z' ) { c = c - 'A' + 'a'; }
    return (c >= 33 && c <= 126) ? c : (char)0;
}

// Human readable binding, e.g. "Ctrl+Shift+Z". "Unb" when unbound.
static void
hotkey_label(const Binding* b, char* out, size_t size)
{
    out[0] = 0;
    if ( b->bound_key == Binding::UNBOUND ) {
        snprintf(out, size, "Unb");
        return;
    }
    char key[8] = {};
    if ( b->bound_key == Binding::TAB ) { snprintf(key, sizeof(key), "Tab"); }
    else if ( b->bound_key == Binding::ESC ) { snprintf(key, sizeof(key), "Esc"); }
    else if ( b->bound_key <= Binding::F1 && b->bound_key >= Binding::F12 ) { snprintf(key, sizeof(key), "F%d", -b->bound_key - 1); }
    else if ( b->bound_key > 32 ) {
        key[0] = (b->bound_key >= 'a' && b->bound_key <= 'z') ? (char)(b->bound_key - 'a' + 'A') : (char)b->bound_key;
    }
    else { snprintf(key, sizeof(key), "?"); }
    snprintf(out, size, "%s%s%s%s",
             (b->modifiers & Modifier_CTRL) ? "Ctrl+" : "",
             (b->modifiers & Modifier_ALT) ? "Alt+" : "",
             (b->modifiers & Modifier_SHIFT) ? "Shift+" : "",
             key);
}

// Copies a loc() string without its baked " - [key]" suffix.
static void
loc_base_name(Texts id, char* out, size_t size)
{
    snprintf(out, size, "%s", loc(id));
    char* cut = strstr(out, " - [");
    if ( cut ) { *cut = 0; }
}

// Button whose label shows the current binding of `action`.
static bool
binding_button(Milton* milton, Texts id, BindableAction action)
{
    char name[64];
    char key[64];
    char label[160];
    loc_base_name(id, name, sizeof(name));
    hotkey_label(&milton->settings->bindings.bindings[action], key, sizeof(key));
    snprintf(label, sizeof(label), "%s [%s]###binding_btn_%d", name, key, (int)action);
    return ImGui::Button(label);
}

// The newest assignment wins: any other action using the same key+modifiers gets unbound.
static void
hotkey_resolve_conflicts(Milton* milton, BindableAction action)
{
    MiltonSettings* st = milton->settings;
    Binding* nb = &st->bindings.bindings[action];
    nb->action = action;

    if ( nb->bound_key != Binding::UNBOUND ) {
        for ( int i = Action_FIRST; i < Action_COUNT; ++i ) {
            Binding* o = &st->bindings.bindings[i];
            if ( i != action && o->bound_key == nb->bound_key && o->modifiers == nb->modifiers ) {
                o->bound_key = Binding::UNBOUND;
                o->modifiers = Modifier_NONE;
            }
        }
        if ( nb->modifiers == Modifier_NONE && st->rotate_key == nb->bound_key ) {
            st->rotate_key = 0;
        }
    }
    // Press/release pair shares one key.
    Binding* rel = &st->bindings.bindings[ActionRelease_PEEK_OUT];
    Binding* press = &st->bindings.bindings[Action_PEEK_OUT];
    rel->bound_key = press->bound_key;
    rel->modifiers = press->modifiers;
}

static void
hotkey_resolve_rotate_conflicts(Milton* milton)
{
    MiltonSettings* st = milton->settings;
    if ( st->rotate_key == 0 ) { return; }
    for ( int i = Action_FIRST; i < Action_COUNT; ++i ) {
        Binding* o = &st->bindings.bindings[i];
        if ( o->bound_key == st->rotate_key && o->modifiers == Modifier_NONE ) {
            o->bound_key = Binding::UNBOUND;
        }
    }
    Binding* rel = &st->bindings.bindings[ActionRelease_PEEK_OUT];
    rel->bound_key = st->bindings.bindings[Action_PEEK_OUT].bound_key;
}
// Thin tool selector fixed to the left edge, under the menu bar.
void picker_set_fixed_triangle(ColorPicker* picker, int fixed);

static void
gui_tools_window(MiltonInput* input, Milton* milton)
{
    const f32 s = milton->gui->scale;
    const f32 top = milton->gui->menu_visible ? ImGui::GetFrameHeight() : 0.0f;
    const ImVec2 display = ImGui::GetIO().DisplaySize;

    ImGui::SetNextWindowPos(ImVec2(0, top));
    ImGui::SetNextWindowSize(ImVec2(s*68, display.y - top));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(s*4, s*4));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    if ( ImGui::Begin("Tools", NULL, flags) ) {
        struct Tool { Texts text; BindableAction action; MiltonMode mode; const char* label; };
        const Tool tools[] = {
            { TXT_switch_to_brush,               Action_MODE_PEN,                 MiltonMode::PEN,                 "Brush"  },
            { TXT_switch_to_eraser,              Action_MODE_ERASER,              MiltonMode::ERASER,              "Eraser" },
            { TXT_switch_to_primitive_line,      Action_MODE_PRIMITIVE_LINE,      MiltonMode::PRIMITIVE_LINE,      "Line"   },
            { TXT_switch_to_primitive_rectangle, Action_MODE_PRIMITIVE_RECTANGLE, MiltonMode::PRIMITIVE_RECTANGLE, "Rect"   },
            { TXT_switch_to_primitive_grid,      Action_MODE_PRIMITIVE_GRID,      MiltonMode::PRIMITIVE_GRID,      "Grid"   },
        };
        const f32 w = ImGui::GetContentRegionAvailWidth();
        for ( const Tool& t : tools ) {
            const bool active = milton->current_mode == t.mode;
            if ( active ) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
            }
            if ( ImGui::Button(t.label, ImVec2(w, s*30)) ) {
                input->mode_to_set = t.mode;
            }
            if ( active ) { ImGui::PopStyleColor(); }
            if ( ImGui::IsItemHovered() ) {
                char name[64];
                char key[64];
                loc_base_name(t.text, name, sizeof(name));
                hotkey_label(&milton->settings->bindings.bindings[t.action], key, sizeof(key));
                ImGui::SetTooltip("%s [%s]", name, key);
            }
        }
        {
            const bool armed = selection_lasso_armed(milton);
            if ( armed ) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
            }
            if ( ImGui::Button("Lasso", ImVec2(w, s*30)) ) {
                selection_command(milton, SelCmd_ARM_LASSO);
            }
            if ( armed ) { ImGui::PopStyleColor(); }
            if ( ImGui::IsItemHovered() ) {
                char key[64];
                hotkey_label(&milton->settings->bindings.bindings[Action_SELECT_LASSO], key, sizeof(key));
                ImGui::SetTooltip("Lasso select [%s]", key);
            }
        }

        // Background colour swatch pinned to the bottom of the panel.
        {
            const f32 box = w - s*8;
            const f32 label_h = ImGui::GetTextLineHeightWithSpacing();
            ImGui::SetCursorPosY(ImGui::GetWindowHeight() - box - label_h - s*10);
            ImGui::Text("Bg Clr");
            v3f bg = milton->view->background_color;
            ImVec4 col = ImVec4(bg.r, bg.g, bg.b, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1, 1, 1, 1));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, s*2);
            if ( ImGui::ColorButton("##bg_swatch", col, ImGuiColorEditFlags_NoAlpha | ImGuiColorEditFlags_NoTooltip, ImVec2(box, box)) ) {
                ImGui::OpenPopup("bg_picker");
            }
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
            if ( ImGui::BeginPopup("bg_picker") ) {
                if ( ImGui::ColorPicker3("##bg_color", bg.d) ) {
                    milton_set_background_color(milton, clamp_01(bg));
                    input->flags |= (i32)MiltonInputFlags_FULL_REFRESH;
                }
                ImGui::EndPopup();
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

// Compact options for the selected tool, floating at the top left next to the tool panel.
// ---- Brush presets (pen and eraser). Stored in presets.bin next to the settings.
struct BrushPreset
{
    char name[32];
    i32  kind;  // BrushEnum_PEN or BrushEnum_ERASER
    i32  size;
    f32  alpha;
    f32  hardness;
    f32  pressure_opacity_min;
    i32  shape;
    i32  pressure_size;
    f32  pressure_size_min;
    f32  shape_aspect;
    f32  shape_angle;
    i32  pressure_opacity;
    i32  relative_to_canvas;
};

#define MAX_BRUSH_PRESETS 64
static BrushPreset g_presets[MAX_BRUSH_PRESETS];
static i32  g_num_presets = 0;
static bool g_presets_loaded = false;

static void
presets_path(PATH_CHAR* fname)
{
    PATH_CHAR tmp[MAX_PATH] = TO_PATH_STR("presets.bin");
    platform_fname_at_config(tmp, MAX_PATH);
    memcpy(fname, tmp, sizeof(tmp));
}

static void
presets_save()
{
    PATH_CHAR fname[MAX_PATH];
    presets_path(fname);
    FILE* fd = platform_fopen(fname, TO_PATH_STR("wb"));
    if ( fd ) {
        u32 sz = sizeof(BrushPreset);
        fwrite(&sz, sizeof(sz), 1, fd);
        fwrite(&g_num_presets, sizeof(g_num_presets), 1, fd);
        fwrite(g_presets, sizeof(BrushPreset), g_num_presets, fd);
        fclose(fd);
    }
}

static void
presets_load()
{
    g_presets_loaded = true;
    PATH_CHAR fname[MAX_PATH];
    presets_path(fname);
    FILE* fd = platform_fopen(fname, TO_PATH_STR("rb"));
    if ( fd ) {
        u32 sz = 0;
        i32 n = 0;
        if ( fread(&sz, sizeof(sz), 1, fd) == 1 && sz == sizeof(BrushPreset)
             && fread(&n, sizeof(n), 1, fd) == 1 && n >= 0 && n <= MAX_BRUSH_PRESETS ) {
            g_num_presets = (i32)fread(g_presets, sizeof(BrushPreset), n, fd);
        }
        fclose(fd);
    }
}

static void
preset_capture(Milton* milton, BrushPreset* p, int kind)
{
    const Brush& b = milton->brushes[kind];
    p->kind = kind;
    p->size = milton->brush_sizes[kind];
    p->alpha = b.alpha;
    p->hardness = b.hardness;
    p->pressure_opacity_min = b.pressure_opacity_min;
    p->shape = b.shape;
    p->pressure_size = b.pressure_size;
    p->pressure_size_min = b.pressure_size_min;
    p->shape_aspect = b.shape_aspect;
    p->shape_angle = b.shape_angle;
    p->pressure_opacity = (kind == BrushEnum_ERASER) ? (milton->eraser_pressure_opacity ? 1 : 0)
                                                      : ((milton->working_stroke.flags & StrokeFlag_PRESSURE_TO_OPACITY) ? 1 : 0);
    p->relative_to_canvas = (milton->working_stroke.flags & StrokeFlag_RELATIVE_TO_CANVAS) ? 1 : 0;
}

static void
preset_apply(Milton* milton, const BrushPreset* p)
{
    int kind = p->kind;
    Brush* b = &milton->brushes[kind];
    if ( kind == BrushEnum_PEN ) { b->alpha = clamp(p->alpha, 0.0f, 1.0f); }
    b->hardness = clamp(p->hardness, 1.0f, k_max_hardness);
    b->pressure_opacity_min = p->pressure_opacity_min;
    b->shape = p->shape;
    b->pressure_size = p->pressure_size;
    b->pressure_size_min = clamp(p->pressure_size_min, 0.0f, 1.0f);
    b->shape_aspect = clamp(p->shape_aspect, 0.05f, 1.0f);
    b->shape_angle = p->shape_angle;

    if ( kind == BrushEnum_ERASER ) { milton->eraser_pressure_opacity = p->pressure_opacity != 0; }
    else if ( p->pressure_opacity ) { milton->working_stroke.flags |= StrokeFlag_PRESSURE_TO_OPACITY; }
    else { milton->working_stroke.flags &= ~StrokeFlag_PRESSURE_TO_OPACITY; }

    if ( p->relative_to_canvas ) { milton->working_stroke.flags |= StrokeFlag_RELATIVE_TO_CANVAS; }
    else { milton->working_stroke.flags &= ~StrokeFlag_RELATIVE_TO_CANVAS; }

    milton_set_brush_size(milton, clamp(p->size, 1, MILTON_MAX_BRUSH_SIZE));  // Also refreshes the brushes.
}

static void
gui_presets_section(Milton* milton, f32 s)
{
    int kind = milton_get_brush_enum(milton);
    if ( kind != BrushEnum_PEN && kind != BrushEnum_ERASER ) { return; }
    if ( !g_presets_loaded ) { presets_load(); }

    static int rename_idx = -1;
    static int rename_grace = 0;

    ImGui::Separator();
    ImGui::Text("Presets");
    ImGui::SameLine();
    if ( ImGui::SmallButton("+##add_preset") && g_num_presets < MAX_BRUSH_PRESETS ) {
        BrushPreset* p = &g_presets[g_num_presets++];
        memset(p, 0, sizeof(*p));
        int n = 0;
        for ( int i = 0; i < g_num_presets; ++i ) { n += g_presets[i].kind == kind; }
        snprintf(p->name, sizeof(p->name), "%s %d", kind == BrushEnum_PEN ? "Brush" : "Eraser", n + 1);
        preset_capture(milton, p, kind);
        presets_save();
    }

    const f32 remove_w = ImGui::CalcTextSize("-").x + ImGui::GetStyle().FramePadding.x * 2;
    int to_remove = -1;
    for ( int i = 0; i < g_num_presets; ++i ) {
        BrushPreset* p = &g_presets[i];
        if ( p->kind != kind ) { continue; }
        ImGui::PushID(i);
        if ( rename_idx == i ) {
            if ( rename_grace == 2 ) { ImGui::SetKeyboardFocusHere(); }
            ImGui::PushItemWidth(ImGui::GetContentRegionAvailWidth() - remove_w - ImGui::GetStyle().ItemSpacing.x);
            bool done = ImGui::InputText("##rename", p->name, sizeof(p->name), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
            ImGui::PopItemWidth();
            if ( rename_grace > 0 ) { --rename_grace; }
            else if ( !ImGui::IsItemActive() ) { done = true; }
            if ( done ) {
                if ( p->name[0] == 0 ) { snprintf(p->name, sizeof(p->name), "Preset"); }
                rename_idx = -1;
                presets_save();
            }
        }
        else {
            f32 w = ImGui::GetContentRegionAvailWidth() - remove_w - ImGui::GetStyle().ItemSpacing.x;
            if ( ImGui::Selectable(p->name, false, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(w, 0)) ) {
                if ( ImGui::IsMouseDoubleClicked(0) ) {
                    rename_idx = i;
                    rename_grace = 2;
                }
                else {
                    preset_apply(milton, p);
                    milton->gui->flags |= (i32)MiltonGuiFlags_SHOWING_PREVIEW;
                }
            }
        }
        ImGui::SameLine();
        if ( ImGui::SmallButton("-") ) { to_remove = i; }
        ImGui::PopID();
    }
    if ( to_remove >= 0 ) {
        for ( int i = to_remove; i < g_num_presets - 1; ++i ) { g_presets[i] = g_presets[i+1]; }
        --g_num_presets;
        rename_idx = -1;
        presets_save();
    }
}
// Returns the height of the window.
i32
gui_brush_window(MiltonInput* input, PlatformState* platform, Milton* milton, PlatformSettings* prefs, b32 reset_gui)
{
    if ( !current_mode_is_for_drawing(milton) ) {
        return 0;
    }
    MiltonGui* gui = milton->gui;
    const f32 s = gui->scale;
    i32 window_height = 0;

    gui_tools_window(input, milton);

    const f32 top = gui->menu_visible ? ImGui::GetFrameHeight() : 0.0f;
    ImGui::SetNextWindowPos(ImVec2(s*68 + s*6, top + s*6));
    ImGui::SetNextWindowSize(ImVec2(s*230, 0));

    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(s*6, s*4));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(s*6, s*3));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(s*4, s*2));

    const char* title = "Brush Options";
    switch ( milton->current_mode ) {
        case MiltonMode::PEN:                 title = "Brush Options";  break;
        case MiltonMode::ERASER:              title = "Eraser Options"; break;
        case MiltonMode::PRIMITIVE_LINE:      title = "Line Options";   break;
        case MiltonMode::PRIMITIVE_RECTANGLE: title = "Rect Options";   break;
        case MiltonMode::PRIMITIVE_GRID:      title = "Grid Options";   break;
        default: break;
    }
    char title_id[64];
    snprintf(title_id, sizeof(title_id), "%s###brush_options", title);

    static bool options_collapsed = false;
    if ( ImGui::Begin(title_id, NULL, flags) ) {
        // Clicking the title bar toggles the panel's contents.
        {
            ImVec2 wp = ImGui::GetWindowPos();
            ImVec2 mp = ImGui::GetIO().MousePos;
            f32 title_h = ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.y * 2;
            if ( ImGui::IsMouseClicked(0)
                 && mp.x >= wp.x && mp.x < wp.x + ImGui::GetWindowWidth()
                 && mp.y >= wp.y && mp.y < wp.y + title_h ) {
                options_collapsed = !options_collapsed;
            }
        }
        if ( options_collapsed ) {
            window_height = (i32)ImGui::GetWindowSize().y;
            ImGui::End();
            ImGui::PopStyleVar(3);
            return window_height;
        }
        ImGui::PushItemWidth(s*110);

        if ( milton->current_mode == MiltonMode::PEN || mode_is_for_primitives(milton->current_mode) ) {
            const float pen_alpha = milton_get_brush_alpha(milton);
            mlt_assert(pen_alpha >= 0.0f && pen_alpha <= 1.0f);
            float mut_alpha = pen_alpha*100;
            ImGui::SliderFloat(loc(TXT_opacity), &mut_alpha, 1, 100, "%.0f%%");

            mut_alpha /= 100.0f;
            if (mut_alpha > 1.0f ) mut_alpha = 1.0f;
            if ( mut_alpha != pen_alpha ) {
                milton_set_brush_alpha(milton, mut_alpha);
                gui->flags |= (i32)MiltonGuiFlags_SHOWING_PREVIEW;
            }
        }

        const auto size = milton_get_brush_radius(milton);
        auto mut_size = size;
        ImGui::SliderInt(loc(TXT_brush_size), &mut_size, 1, MILTON_MAX_BRUSH_SIZE);
        if ( mut_size != size ) {
            milton_set_brush_size(milton, mut_size);
            gui->flags |= (i32)MiltonGuiFlags_SHOWING_PREVIEW;
        }

        if ( milton->current_mode == MiltonMode::PEN || milton->current_mode == MiltonMode::ERASER ) {
            Brush* shape_brush = &milton->brushes[milton_get_brush_enum(milton)];
            bool size_pressure = shape_brush->pressure_size != 0;
            if ( ImGui::Checkbox("Size from pressure", &size_pressure) ) {
                shape_brush->pressure_size = size_pressure ? 1 : 0;
            }
            if ( size_pressure ) {
                f32 min_percent = shape_brush->pressure_size_min * 100.0f;
                if ( ImGui::SliderFloat("Min size", &min_percent, 0.0f, 100.0f, "%.0f%%") ) {
                    shape_brush->pressure_size_min = clamp(min_percent, 0.0f, 100.0f) / 100.0f;
                }
            }
            int shape = shape_brush->shape;
            ImGui::RadioButton("Round", &shape, BrushShape_ROUND);
            ImGui::SameLine();
            ImGui::RadioButton("Rectangle", &shape, BrushShape_RECTANGLE);
            shape_brush->shape = shape;
            if ( shape == BrushShape_RECTANGLE ) {
                f32 aspect_percent = shape_brush->shape_aspect * 100.0f;
                if ( ImGui::SliderFloat("Aspect", &aspect_percent, 5.0f, 100.0f, "%.0f%%") ) {
                    shape_brush->shape_aspect = clamp(aspect_percent, 5.0f, 100.0f) / 100.0f;
                }
                shape_brush->shape_aspect = clamp(shape_brush->shape_aspect, 0.05f, 1.0f);
                ImGui::SliderFloat("Angle", &shape_brush->shape_angle, 0.0f, 180.0f, "%.0f deg");
                shape_brush->shape_angle = clamp(shape_brush->shape_angle, 0.0f, 180.0f);
            }
        }

        if ( milton->current_mode == MiltonMode::PRIMITIVE_GRID ) {
            ImGui::SliderInt(loc(TXT_grid_columns), &milton->grid_columns, 1, MILTON_MAX_GRID_SIZE);
            ImGui::SliderInt(loc(TXT_grid_rows), &milton->grid_rows, 1, MILTON_MAX_GRID_SIZE);
        }

        if ( !(milton->working_stroke.flags & StrokeFlag_ERASER) ) {
            int brush_enum = milton_get_brush_enum(milton);
            f32* hardness = &milton->brushes[brush_enum].hardness;
            // Very soft edges show seams between nearby passes, so the slider starts at a minimum.
            const f32 k_min_hardness_percent = clamp(milton->settings->hardness_min_percent, 0.0f, 95.0f);
            f32 percent = (*hardness - 1.0f) / (k_max_hardness - 1.0f) * 100.0f;
            if ( percent < k_min_hardness_percent ) {
                percent = k_min_hardness_percent;
                *hardness = 1.0f + percent / 100.0f * (k_max_hardness - 1.0f);
            }
            // The slider shows 0-100%, mapped onto the real range above the minimum.
            f32 shown = (percent - k_min_hardness_percent) / (100.0f - k_min_hardness_percent) * 100.0f;
            if ( ImGui::SliderFloat(loc(TXT_hardness), &shown, 0.0f, 100.0f, "%.0f%%") ) {
                percent = k_min_hardness_percent + shown / 100.0f * (100.0f - k_min_hardness_percent);
                *hardness = 1.0f + percent / 100.0f * (k_max_hardness - 1.0f);
            }
            // A hard brush uses the fast path; anything softer needs the feathered path.
            if ( *hardness < k_max_hardness ) {
                milton->working_stroke.flags |= StrokeFlag_DISTANCE_TO_OPACITY;
            } else {
                milton->working_stroke.flags &= ~StrokeFlag_DISTANCE_TO_OPACITY;
            }

            ImGui::CheckboxFlags(loc(TXT_opacity_pressure), reinterpret_cast<u32*>(&milton->working_stroke.flags), StrokeFlag_PRESSURE_TO_OPACITY);
            if ( milton->working_stroke.flags & StrokeFlag_PRESSURE_TO_OPACITY ) {
                f32* min_opacity = &milton->brushes[brush_enum].pressure_opacity_min;
                ImGui::SliderFloat(loc(TXT_minimum), min_opacity, 0.0f, milton->brushes[brush_enum].alpha);
            }
        }

        if ( milton->working_stroke.flags & StrokeFlag_ERASER ) {
            ImGui::Checkbox(loc(TXT_opacity_pressure), &milton->eraser_pressure_opacity);
            if ( milton->eraser_pressure_opacity ) {
                ImGui::SliderFloat(loc(TXT_minimum), &milton->brushes[BrushEnum_ERASER].pressure_opacity_min, 0.0f, 1.0f);
            }
        }

        ImGui::CheckboxFlags(loc(TXT_size_relative_to_canvas),
                             reinterpret_cast<u32*>(&milton->working_stroke.flags),
                             StrokeFlag_RELATIVE_TO_CANVAS);

        ImGui::PopItemWidth();

        gui_presets_section(milton, s);

        ImVec2 pos  = ImGui::GetWindowPos();
        ImVec2 size_w = ImGui::GetWindowSize();
        window_height = (i32)size_w.y;
        if ( gui->flags & MiltonGuiFlags_SHOWING_PREVIEW ) {
            gui->preview_pos = {
                (i32)(pos.x + size_w.x + milton_get_brush_radius(milton)),
                (i32)(pos.y),
            };
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(3);

    return window_height;
}
void
gui_menu(MiltonInput* input, PlatformState* platform, Milton* milton, b32& show_settings, b32* reset_gui)
{
    // Menu ----
    int menu_style_stack = 0;

    MiltonGui* gui = milton->gui;
    CanvasState* canvas = milton->canvas;

    // TODO: translate

    if ( gui->menu_visible ) {
        if ( ImGui::BeginMainMenuBar() ) {
            if ( ImGui::BeginMenu(loc(TXT_file)) ) {
                if ( ImGui::MenuItem(loc(TXT_new_milton_canvas)) ) {
                    b32 save_file = false;
                    if ( layer::count_strokes(milton->canvas->root_layer) > 0 ) {
                        if ( milton->flags & MiltonStateFlags_DEFAULT_CANVAS ) {
                            save_file = platform_dialog_yesno(loc(TXT_default_will_be_cleared), "Save?");
                        }
                    }
                    if ( save_file ) {
                        PATH_CHAR* name = platform_save_dialog(FileKind_MILTON_CANVAS);
                        if ( name ) {
                            milton_log("Saving to %s\n", name);
                            milton_set_canvas_file(milton, name);
                            milton_save(milton);
                            b32 del = platform_delete_file_at_config(TO_PATH_STR("MiltonPersist.mlt"), DeleteErrorTolerance_OK_NOT_EXIST);
                            if ( del == false ) {
                                platform_dialog("Could not delete contents. The work will be still be there even though you saved it to a file.",
                                                "Info");
                            }
                        }
                    }

                    // New Canvas
                    milton_reset_canvas_and_set_default(milton);
                    canvas = milton->canvas;
                    input->flags |= MiltonInputFlags_FULL_REFRESH;
                    milton->flags |= MiltonStateFlags_DEFAULT_CANVAS;
                }
                b32 save_requested = false;
                if ( ImGui::MenuItem(loc(TXT_open_milton_canvas)) ) {
                    // If current canvas is MiltonPersist, then prompt to save
                    if ( ( milton->flags & MiltonStateFlags_DEFAULT_CANVAS ) ) {
                        b32 save_file = false;
                        if ( layer::count_strokes(milton->canvas->root_layer) > 0 ) {
                            save_file = platform_dialog_yesno(loc(TXT_default_will_be_cleared), "Save?");
                        }
                        if ( save_file ) {
                            PATH_CHAR* name = platform_save_dialog(FileKind_MILTON_CANVAS);
                            if ( name ) {
                                milton_log("Saving to %s\n", name);
                                milton_set_canvas_file(milton, name);
                                milton_save(milton);
                                b32 del = platform_delete_file_at_config(TO_PATH_STR("MiltonPersist.mlt"),
                                                                         DeleteErrorTolerance_OK_NOT_EXIST);
                                if ( del == false ) {
                                    platform_dialog("Could not delete default canvas. Contents will be still there when you create a new canvas.",
                                                    "Info");
                                }
                            }
                        }
                    }
                    PATH_CHAR* fname = platform_open_dialog(FileKind_MILTON_CANVAS);
                    if ( fname ) {
                        milton_set_canvas_file(milton, fname);
                        input->flags |= MiltonInputFlags_OPEN_FILE;
                    }
                }
                if ( ImGui::MenuItem(loc(TXT_save_milton_canvas_as_DOTS)) || save_requested ) {
                    // NOTE(possible refactor): There is a copy of this at milton.c end of file
                    PATH_CHAR* name = platform_save_dialog(FileKind_MILTON_CANVAS);
                    if ( name ) {
                        milton_log("Saving to %s\n", name);
                        milton_set_canvas_file(milton, name);
                        input->flags |= MiltonInputFlags_SAVE_FILE;
                        b32 del = platform_delete_file_at_config(TO_PATH_STR("MiltonPersist.mlt"),
                                                                 DeleteErrorTolerance_OK_NOT_EXIST);
                        if ( del == false ) {
                            platform_dialog(loc(TXT_could_not_delete_default_canvas),
                                            "Info");
                        }
                    }
                }
                if ( ImGui::MenuItem(loc(TXT_export_to_image_DOTS)) ) {
                    milton_enter_mode(milton, MiltonMode::EXPORTING);
                }
                if ( ImGui::MenuItem(loc(TXT_settings)) && !show_settings ) {
                    milton_set_gui_visibility(milton, true);
                    show_settings = true;
                    *gui->original_settings = *milton->settings;
                    for (sz i = Action_FIRST; i < Action_COUNT; ++i) {
                        gui->scratch_binding_key[i][0] = gui->original_settings->bindings.bindings[i].bound_key;
                    }
                }
                if ( ImGui::MenuItem(loc(TXT_quit)) ) {
                    milton_try_quit(milton);
                }
                ImGui::EndMenu();
            } // file menu
            if ( ImGui::BeginMenu(loc(TXT_edit)) ) {
                if ( ImGui::MenuItem(loc(TXT_undo) ) ) {
                    input->flags |= MiltonInputFlags_UNDO;
                }
                if ( ImGui::MenuItem(loc(TXT_redo)) ) {
                    input->flags |= MiltonInputFlags_REDO;
                }
                ImGui::EndMenu();
            }

            if ( ImGui::BeginMenu(loc(TXT_canvas), /*enabled=*/true) ) {
                if ( ImGui::BeginMenu(loc(TXT_set_background_color)) ) {
                    v3f bg = milton->view->background_color;
                    if (ImGui::ColorEdit3(loc(TXT_color), bg.d))
                    {
                        milton_set_background_color(milton, clamp_01(bg));
                        input->flags |= (i32)MiltonInputFlags_FULL_REFRESH;
                    }
                    ImGui::EndMenu();
                }
                if ( ImGui::MenuItem(loc(TXT_zoom_in)) ) {
                    input->scale++;
                    milton_set_zoom_at_screen_center(milton);
                }
                if ( ImGui::MenuItem(loc(TXT_zoom_out)) ) {
                    input->scale--;
                    milton_set_zoom_at_screen_center(milton);
                }
                ImGui::EndMenu();
            }
            if ( ImGui::BeginMenu(loc(TXT_tools)) ) {
                if ( ImGui::MenuItem("Hotkeys...") ) {
                    gui->show_hotkeys = true;
                    milton_set_gui_visibility(milton, true);
                }
                if ( ImGui::MenuItem("Pen & Input Settings...") ) {
                    gui->show_pen_settings = true;
                    milton_set_gui_visibility(milton, true);
                }
                // Brush
                if ( ImGui::MenuItem(loc(TXT_brush)) ) {
                    input->mode_to_set = MiltonMode::PEN;
                }
                if ( ImGui::BeginMenu(loc(TXT_brush_options)) ) {

                    // Toggling brush smoothing is barely noticeable...

                    // b32 smoothing_enabled = milton_brush_smoothing_enabled(milton);
                    // char* entry_str = smoothing_enabled? loc(TXT_disable_stroke_smoothing) : loc(TXT_enable_stroke_smoothing);

                    // if ( ImGui::MenuItem(entry_str) ) {
                    //     milton_toggle_brush_smoothing(milton);
                    // }

                    // Decrease / increase brush size
                    if ( ImGui::MenuItem(loc(TXT_decrease_brush_size)) ) {
                        for (int i=0;i<5;++i) milton_decrease_brush_size(milton);
                    }
                    if ( ImGui::MenuItem(loc(TXT_increase_brush_size)) ) {
                        for (int i=0;i<5;++i) milton_increase_brush_size(milton);
                    }
                    // Opacity shortcuts

                    f32 opacities[] = { 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 1.0f };
                    for ( i32 i = 0; i < array_count(opacities); ++i ) {
                        char entry[128] = {};
                        snprintf(entry, array_count(entry), "%s %d%% - [%d]",
                                 loc(TXT_set_opacity_to), (int)(100 * opacities[i]), i == 9 ? 0 : i+1);
                        if ( ImGui::MenuItem(entry) ) {
                            milton_set_brush_alpha(milton, opacities[i]);
                        }
                    }
                    ImGui::EndMenu();
                }
                // Eraser
                if ( ImGui::MenuItem(loc(TXT_eraser)) ) {
                    input->mode_to_set = MiltonMode::ERASER;
                }
                // Panning
                char* move_str = platform->is_panning==false? loc(TXT_move_canvas) : loc(TXT_stop_moving_canvas);
                if ( ImGui::MenuItem(move_str) ) {
                    platform->waiting_for_pan_input = true;
                }
                // Eye Dropper
                if ( ImGui::MenuItem(loc(TXT_eye_dropper)) ) {
                    input->mode_to_set = MiltonMode::EYEDROPPER;
                }
                ImGui::EndMenu();
            }
            if ( ImGui::BeginMenu(loc(TXT_view)) ) {
                if ( ImGui::MenuItem(loc(TXT_toggle_gui_visibility)) ) {
                    milton_toggle_gui_visibility(milton);
                }

                if ( ImGui::MenuItem(loc(TXT_peek_out)) ) {
                    peek_out_trigger_start(milton, PeekOut_CLICK_TO_EXIT);
                }

                if ( ImGui::MenuItem(loc(TXT_reset_view_at_origin)) ) {
                    reset_transform_at_origin(
                        &milton->view->pan_center,
                        &milton->view->scale,
                        &milton->view->angle);
                    gpu_update_canvas(milton->renderer, milton->canvas, milton->view);
                }

                if ( ImGui::MenuItem(loc(TXT_reset_GUI)) ) {
                    *reset_gui = true;
                }

#if MILTON_ENABLE_PROFILING
                if ( ImGui::MenuItem("Toggle Debug Data [BACKQUOTE]") ) {
                    milton->viz_window_visible = !milton->viz_window_visible;
                }
#endif
                ImGui::EndMenu();
            }
            if ( ImGui::BeginMenu(loc(TXT_help)) ) {
                if ( ImGui::MenuItem(loc(TXT_help_me)) ) {
                    //platform_open_link("https://www.youtube.com/watch?v=g27gHio2Ohk");
                    platform_open_link("http://www.miltonpaint.com/help/");
                }
                if ( ImGui::MenuItem(loc(TXT_milton_version)) ) {
                    char buffer[1024];
                    snprintf(buffer, array_count(buffer),
                             "Milton version %d.%d.%d",
                             MILTON_MAJOR_VERSION, MILTON_MINOR_VERSION, MILTON_MICRO_VERSION);
                    platform_dialog(buffer, "Milton Version");
                }
                if (  ImGui::MenuItem(loc(TXT_website))  ) {
                    platform_open_link("http://miltonpaint.com");
                }
                ImGui::EndMenu();
            }
            PATH_CHAR* utf16_name = str_trim_to_last_slash(milton->persist->mlt_file_path);

            char file_name[MAX_PATH] = {};
            utf16_to_utf8_simple(utf16_name, file_name);

            char msg[1024];
            WallTime lst = milton->persist->last_save_time;

            // TODO: Translate!
            snprintf(msg, 1024, "\t%s -- Last saved: %.2d:%.2d:%.2d\t\tZoom level %.2f",
                     (milton->flags & MiltonStateFlags_DEFAULT_CANVAS) ? loc(TXT_OPENBRACKET_default_canvas_CLOSE_BRACKET):
                     file_name,
                     lst.hours, lst.minutes, lst.seconds,
                     // We divide by MILTON_DEFAULT_SCALE to give a frame of
                     // reference to the user, where 1.0 is the default. For
                     // our calculations in other places, we don't do the
                     // divide.
                     log2(1 + milton->view->scale / (double)MILTON_DEFAULT_SCALE) / log2(SCALE_FACTOR));

            if ( ImGui::BeginMenu(msg, /*bool enabled = */false) ) {
                ImGui::EndMenu();
            }
            // Canvas rotation and flip.
            {
                CanvasView* view = milton->view;
                i32 angle = (i32)roundf(view->angle / PI * 180.0f) % 360;
                if ( angle < 0 ) { angle += 360; }
                ImGui::AlignTextToFramePadding();
                ImGui::Text("%s", loc(TXT_rotation));
                ImGui::PushItemWidth(gui->scale * 44);
                if ( ImGui::InputInt("##rotation", &angle, 0, 0,
                                     ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsDecimal) ) {
                    view->angle = (f32)angle / 180.0f * PI;
                    milton_set_zoom_at_screen_center(milton);
                }
                ImGui::PopItemWidth();
                if ( ImGui::Button("Reset") ) {
                    milton_reset_rotation(milton);
                }
                if ( ImGui::Button("Flip") ) {
                    milton_flip_canvas_horizontal(milton);
                }
            }
            ImGui::EndMainMenuBar();
        }
        ImGui::PopStyleColor(menu_style_stack);
    }
}


void
milton_imgui_tick(MiltonInput* input, PlatformState* platform,  Milton* milton, PlatformSettings* prefs)
{
    CanvasState* canvas = milton->canvas;
    MiltonGui* gui = milton->gui;
    // ImGui Section

    // Spawn below the picker
    const Rect pbounds = get_bounds_for_picker_and_colors(&gui->picker);

    int color_stack = 0;

    static auto color_window_background = ImVec4{.929f, .949f, .957f, 1};
    //static auto color_title_bg        = ImVec4{.957f,.353f, .286f,1};
    static auto color_title_bg          = color_window_background;
    static auto color_title_fg          = ImVec4{151/255.f, 184/255.f, 210/255.f, 1};

    static auto color_buttons         = ImVec4{.686f, .796f, 1.0f, 1};
    static auto color_buttons_active  = ImVec4{.886f, .796f, 1.0f, 1};
    static auto color_buttons_hovered = ImVec4{.706f, .816f, 1.0f, 1};

    static auto color_menu_bg        = ImVec4{.784f, .392f, .784f, 1};
    static auto color_text           = ImVec4{.2f,.2f,.2f,1};
    static auto color_slider         = ImVec4{ 148/255.f, 182/255.f, 182/255.f,1};
    static auto frame_background     = ImVec4{ 0.862745f, 0.862745f, 0.862745f,1};
    static auto color_text_selected  = ImVec4{ 0.509804f, 0.627451f, 0.823529f,1};
    static auto color_header_hovered = color_buttons;

    picker_set_fixed_triangle(&milton->gui->picker, milton->settings->picker_triangle_rotates ? 0 : 1);

    if ( !milton->settings->light_theme ) {
        // VS Code Dark+ inspired palette.
        color_window_background = ImVec4{ 0x25/255.f, 0x25/255.f, 0x26/255.f, 1 };
        color_title_bg          = ImVec4{ 0x1e/255.f, 0x1e/255.f, 0x1e/255.f, 1 };
        color_title_fg          = ImVec4{ 0x33/255.f, 0x33/255.f, 0x37/255.f, 1 };
        color_buttons           = ImVec4{ 0x3c/255.f, 0x3c/255.f, 0x3c/255.f, 1 };
        color_buttons_hovered   = ImVec4{ 0x50/255.f, 0x50/255.f, 0x52/255.f, 1 };
        color_buttons_active    = ImVec4{ 0x0e/255.f, 0x63/255.f, 0x9c/255.f, 1 };
        color_menu_bg           = color_title_bg;
        color_text              = ImVec4{ 0xcc/255.f, 0xcc/255.f, 0xcc/255.f, 1 };
        color_slider            = ImVec4{ 0x00/255.f, 0x7a/255.f, 0xcc/255.f, 1 };
        frame_background        = ImVec4{ 0x3c/255.f, 0x3c/255.f, 0x3c/255.f, 1 };
        color_text_selected     = ImVec4{ 0x26/255.f, 0x4f/255.f, 0x78/255.f, 1 };
        color_header_hovered    = color_buttons_hovered;
    }
    else {
        color_window_background = ImVec4{.929f, .949f, .957f, 1};
        color_title_bg          = color_window_background;
        color_title_fg          = ImVec4{151/255.f, 184/255.f, 210/255.f, 1};
        color_buttons           = ImVec4{.686f, .796f, 1.0f, 1};
        color_buttons_active    = ImVec4{.886f, .796f, 1.0f, 1};
        color_buttons_hovered   = ImVec4{.706f, .816f, 1.0f, 1};
        color_menu_bg           = ImVec4{.784f, .392f, .784f, 1};
        color_text              = ImVec4{.2f,.2f,.2f,1};
        color_slider            = ImVec4{ 148/255.f, 182/255.f, 182/255.f,1};
        frame_background        = ImVec4{ 0.862745f, 0.862745f, 0.862745f,1};
        color_text_selected     = ImVec4{ 0.509804f, 0.627451f, 0.823529f,1};
        color_header_hovered    = color_buttons;
    }

    // Helper Imgui code to select color scheme
#if 0
    ImGui::ColorEdit3("Window Background", (float*)&color_window_background);
    ImGui::ColorEdit3("Title background", (float*)&color_title_bg);
    ImGui::ColorEdit3("Title background active", (float*)&color_title_fg);

    ImGui::ColorEdit3("Buttons", (float*)&color_buttons);
    ImGui::ColorEdit3("Menu BG", (float*)&color_menu_bg);
    ImGui::ColorEdit3("text", (float*)&color_text);
    ImGui::ColorEdit3("frame background", (float*)&frame_background);
    ImGui::ColorEdit3("selected", (float*)&color_text_selected);
    if ( ImGui::Button("Print out") ) {
        auto print_color = [&](char* label, ImVec4 c) {
            milton_log("%s : %f, %f, %f \n", label, c.x, c.y, c.z);
        };
        print_color("window bg", color_window_background);
        print_color("title bg", color_title_bg);
        print_color("buttons", color_buttons);
        print_color("menu bg", color_menu_bg);
        print_color("text", color_text);
        print_color("selected", frame_background);
        print_color("selected", color_text_selected);
    }
#endif

    ImGui::PushStyleColor(ImGuiCol_WindowBg,        color_window_background); ++color_stack;
    ImGui::PushStyleColor(ImGuiCol_PopupBg,         color_window_background); ++color_stack;
    ImGui::PushStyleColor(ImGuiCol_TitleBg,         color_title_bg); ++color_stack;
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive,   color_title_fg); ++color_stack;
    ImGui::PushStyleColor(ImGuiCol_Text,            color_text); ++color_stack;
    ImGui::PushStyleColor(ImGuiCol_MenuBarBg,       color_title_bg); ++color_stack;
    ImGui::PushStyleColor(ImGuiCol_TextSelectedBg,  color_buttons_active); ++color_stack;
    ImGui::PushStyleColor(ImGuiCol_FrameBg,      frame_background); ++color_stack;

    ImGui::PushStyleColor(ImGuiCol_SliderGrab,      color_buttons); ++color_stack;
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive,      color_buttons_active); ++color_stack;

    ImGui::PushStyleColor(ImGuiCol_Button,          color_buttons); ++color_stack;
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,      color_buttons_hovered); ++color_stack;
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,          color_buttons_active); ++color_stack;

    ImGui::PushStyleColor(ImGuiCol_Header,          color_buttons); ++color_stack;
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,   color_buttons_hovered); ++color_stack;
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,    color_buttons_active); ++color_stack;

    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab,   color_buttons); ++color_stack;
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered,      color_buttons_hovered); ++color_stack;
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive,      color_buttons_active); ++color_stack;

    ImGui::PushStyleColor(ImGuiCol_CheckMark,      color_slider); ++color_stack;


    static b32 show_settings = false;
    b32 reset_gui = false;

    gui_menu(input, platform, milton, show_settings, &reset_gui);

    float ui_scale = milton->gui->scale;

    // Rotation gizmo: a circle around the pivot with a red line pointing at true (canvas) up.
    if ( milton->current_mode == MiltonMode::TRANSFORM ) {
        CanvasView* view = milton->view;
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        const ImVec2 c = ImVec2((f32)view->screen_size.w * 0.5f, (f32)view->screen_size.h * 0.5f);
        const f32 r = ui_scale * 70;

        v2l canvas_c = raster_to_canvas(view, v2l{ (i64)c.x, (i64)c.y });
        v2l canvas_up = { canvas_c.x, canvas_c.y - 1000 * view->scale };
        v2l up_raster = canvas_to_raster(view, canvas_up);
        f32 dx = (f32)(up_raster.x - (i64)c.x);
        f32 dy = (f32)(up_raster.y - (i64)c.y);
        f32 len = sqrtf(dx*dx + dy*dy);
        if ( len > 0.0f ) { dx /= len; dy /= len; }

        ImDrawList* dl = ImGui::GetOverlayDrawList();
        const ImU32 dark = IM_COL32(0, 0, 0, 200);
        const ImU32 light = IM_COL32(255, 255, 255, 200);
        const ImU32 red = IM_COL32(230, 30, 30, 255);
        dl->AddCircle(c, r, light, 64, ui_scale * 4);
        dl->AddCircle(c, r, dark, 64, ui_scale * 2);
        dl->AddCircleFilled(c, ui_scale * 4, dark, 16);
        ImVec2 tip = ImVec2(c.x + dx * r * 1.3f, c.y + dy * r * 1.3f);
        dl->AddLine(c, tip, light, ui_scale * 5);
        dl->AddLine(c, tip, red, ui_scale * 3);
        (void)display;
    }

    // GUI Windows ----

    b32 should_show_windows = milton->current_mode != MiltonMode::EXPORTING
                                && milton->current_mode != MiltonMode::HISTORY;

    if ( gui->visible && should_show_windows ) {
        // While rotating (Alt held) keep showing the panels of the mode we came from.
        MiltonMode real_mode = milton->current_mode;
        if ( (real_mode == MiltonMode::TRANSFORM || real_mode == MiltonMode::EYEDROPPER) && milton->n_mode_stack > 0 ) {
            milton->current_mode = milton->mode_stack[milton->n_mode_stack - 1];
        }
        i32 brush_window_height = gui_brush_window(input, platform, milton, prefs, reset_gui);
        milton->current_mode = real_mode;

        gui_layer_window(input, platform, milton, brush_window_height, prefs, reset_gui);

        if ( gui->show_hotkeys ) {
            ImGui::SetNextWindowSize(ImVec2(ui_scale*420, ui_scale*400), ImGuiSetCond_FirstUseEver);
            bool open = true;
            if ( ImGui::Begin("Hotkeys", &open) ) {
                MiltonSettings* st = milton->settings;
                ImGui::TextWrapped("Type a key to assign it. Clear the field to unbind. Assigning a key that is already in use unbinds the old action.");
                ImGui::Separator();
                ImGui::BeginChild("HotkeyList");

                // Rotate (hold + drag)
                {
                    char buf[2] = { st->rotate_key, 0 };
                    ImGui::PushID("rotate_key");
                    ImGui::PushItemWidth(ui_scale*30);
                    if ( ImGui::InputText("##key", buf, sizeof(buf), ImGuiInputTextFlags_AutoSelectAll) ) {
                        st->rotate_key = hotkey_sanitize_char(buf[0]);
                        hotkey_resolve_rotate_conflicts(milton);
                    }
                    ImGui::PopItemWidth();
                    ImGui::SameLine();
                    ImGui::Text("Rotate canvas (hold + drag)");
                    ImGui::PopID();
                }

                MiltonBindings* bs = &st->bindings;
                for ( int i = Action_FIRST; i < Action_COUNT; ++i ) {
                    if ( i == Action_DRAG_BRUSH_SIZE || i == Action_DRAG_ZOOM || i == Action_TRANSFORM || i == Action_SELECT_COMMIT ) {
                        continue;  // Fixed modifier gestures.
                    }
                    Binding* b = &bs->bindings[i];
                    bool changed = false;
                    ImGui::PushID(i);

                    char buf[2] = {};
                    if ( b->bound_key >= 33 && b->bound_key <= 126 ) { buf[0] = b->bound_key; }

                    ImGui::PushItemWidth(ui_scale*30);
                    if ( ImGui::InputText("##key", buf, sizeof(buf), ImGuiInputTextFlags_AutoSelectAll) ) {
                        char c = hotkey_sanitize_char(buf[0]);
                        b->bound_key = c;
                        if ( c == 0 ) { b->modifiers = Modifier_NONE; }
                        changed = true;
                    }
                    ImGui::PopItemWidth();
                    ImGui::SameLine();
                    changed |= ImGui::CheckboxFlags("Ctrl", (unsigned int*)&b->modifiers, Modifier_CTRL);
                    ImGui::SameLine();
                    changed |= ImGui::CheckboxFlags("Alt", (unsigned int*)&b->modifiers, Modifier_ALT);
                    ImGui::SameLine();
                    changed |= ImGui::CheckboxFlags("Shift", (unsigned int*)&b->modifiers, Modifier_SHIFT);
                    ImGui::SameLine();
                    char label[64];
                    hotkey_label(b, label, sizeof(label));
                    ImGui::Text("%s [%s]", loc((Texts)(TXT_Action_FIRST + i - Action_FIRST)), label);
                    ImGui::PopID();

                    if ( changed ) {
                        if ( b->bound_key == Binding::UNBOUND ) { b->modifiers = Modifier_NONE; }
                        hotkey_resolve_conflicts(milton, (BindableAction)i);
                    }
                }
                ImGui::EndChild();
            } ImGui::End();
            if ( !open ) {
                gui->show_hotkeys = false;
                milton_settings_save(milton->settings);
            }
        }
        // Pen & input settings window. Changes apply live; Save persists them.
        if ( gui->show_pen_settings ) {
            MiltonSettings* s = milton->settings;
            ImGui::SetNextWindowSize(ImVec2(ui_scale*380, ui_scale*300), ImGuiSetCond_FirstUseEver);
            bool open = true;
            if ( ImGui::Begin("Pen & Input Settings", &open) ) {
                ImGui::Text("Pressure");
                ImGui::SliderFloat("Pressure smoothing", &s->pressure_smoothing, 0.05f, 1.0f, "%.2f");
                ImGui::TextWrapped("Lower = smoother, more lag. 1.0 = raw tablet pressure.");
                ImGui::SliderFloat("Minimum pressure", &s->pressure_min, 0.0f, 0.5f, "%.3f");
                ImGui::Separator();
                ImGui::Text("Stroke");
                ImGui::SliderFloat("Position smoothing", &s->position_smoothing, 0.05f, 1.0f, "%.2f");
                ImGui::TextWrapped("Applied when brush smoothing is enabled. 1.0 = no smoothing.");
                ImGui::Separator();
                ImGui::Text("Shortcuts");
                ImGui::InputFloat("Brush resize speed (Alt+RMB)", &s->brush_scrub_speed, 0.05f, 0.25f, 2);
                ImGui::InputFloat("Zoom drag speed (Ctrl+Space+LMB)", &s->zoom_drag_speed, 0.001f, 0.005f, 4);
                ImGui::Separator();
                ImGui::Text("Brush");
                ImGui::SliderFloat("Minimum hardness (%)", &s->hardness_min_percent, 0.0f, 95.0f, "%.0f");
                ImGui::TextWrapped("The brush panel hardness slider spans this value (shown as 0%) to 100%.");
                s->pressure_smoothing = clamp(s->pressure_smoothing, 0.05f, 1.0f);
                s->pressure_min = clamp(s->pressure_min, 0.0f, 0.5f);
                s->position_smoothing = clamp(s->position_smoothing, 0.05f, 1.0f);
                s->brush_scrub_speed = clamp(s->brush_scrub_speed, 0.05f, 5.0f);
                s->zoom_drag_speed = clamp(s->zoom_drag_speed, 0.0005f, 0.05f);
                ImGui::Separator();
                if ( ImGui::Button("Save") ) {
                    milton_settings_save(s);
                }
                ImGui::SameLine();
                if ( ImGui::Button("Reset to defaults") ) {
                    MiltonSettings defaults = {};
                    settings_init(&defaults);
                    s->pressure_smoothing = defaults.pressure_smoothing;
                    s->pressure_min = defaults.pressure_min;
                    s->position_smoothing = defaults.position_smoothing;
                    s->brush_scrub_speed = defaults.brush_scrub_speed;
                    s->zoom_drag_speed = defaults.zoom_drag_speed;
                    s->hardness_min_percent = defaults.hardness_min_percent;
                }
            } ImGui::End();
            if ( !open ) {
                gui->show_pen_settings = false;
                milton_settings_save(s);
            }
        }

        // Settings window
        if ( show_settings ) {
            ImGui::SetNextWindowSize(ImVec2(ui_scale*400, ui_scale*400),
                                     ImGuiSetCond_FirstUseEver);
            if ( ImGui::Begin(loc(TXT_settings)) ) {
                if (ImGui::Button(loc(TXT_ok))) {
                    milton_settings_save(milton->settings);
                    show_settings = false;
                }
                
                ImGui::SameLine();
                if (ImGui::Button(loc(TXT_cancel))) {
                    show_settings = false;
                    *milton->settings = *gui->original_settings;
                }
                
                ImGui::Separator();
                ImGui::BeginChild("ScrollRegion");

                {
                    bool dark = !milton->settings->light_theme;
                    if ( ImGui::Checkbox("Dark mode", &dark) ) {
                        milton->settings->light_theme = dark ? 0 : 1;
                    }
                    bool rotates = milton->settings->picker_triangle_rotates != 0;
                    if ( ImGui::Checkbox("Rotate color triangle with hue", &rotates) ) {
                        milton->settings->picker_triangle_rotates = rotates ? 1 : 0;
                    }
                    ImGui::Separator();
                }

                ImGui::Text(loc(TXT_default_background_color));

                v3f* bg = &milton->settings->background_color;
                if (ImGui::ColorEdit3(loc(TXT_color), bg->d)) {
                    // TODO: Let milton know that we need to save the settings
                }

                if ( ImGui::Button(loc(TXT_set_current_background_color_as_default)) ) {
                    milton->settings->background_color = milton->view->background_color;
                }

                const float peek_range = 20;
                int peek_out_percent = 100 * (milton->settings->peek_out_increment / peek_range);
                if (ImGui::SliderInt(loc(TXT_peek_out_increment_percent), &peek_out_percent, 0, 100)) {
                    milton->settings->peek_out_increment = (peek_out_percent / 100.0f) * peek_range;
                }

                ImGui::Separator();

                MiltonBindings* bs = &milton->settings->bindings;

                for (sz i = Action_FIRST; i < Action_COUNT; ++i ) {
                    Binding* b = bs->bindings + i;

                    char control_lbl[64] = {};
                    snprintf(control_lbl, array_count(control_lbl), "control##%d", (int)i);

                    char alt_lbl[64] = {};
                    snprintf(alt_lbl, array_count(alt_lbl), "alt##%d", (int)i);

                    char win_lbl[64] = {};
                    snprintf(win_lbl, array_count(win_lbl), "win##%d", (int)i);


                    ImGui::CheckboxFlags(control_lbl, (unsigned int*)&b->modifiers, Modifier_CTRL);
                    ImGui::SameLine();
                    ImGui::CheckboxFlags(alt_lbl, (unsigned int*)&b->modifiers, Modifier_ALT);
                    ImGui::SameLine();
                    ImGui::CheckboxFlags(win_lbl, (unsigned int*)&b->modifiers, Modifier_WIN);
                    ImGui::SameLine();

                    ImGui::PushItemWidth(60);
                    // mlt_assert(TXT_Action_FIRST + (int)i < TXT_Count);
                    char* action_str = (char*)loc((Texts)(TXT_Action_FIRST + (int)i - Action_FIRST));
                    if (ImGui::InputText(action_str,
                                         (char*)gui->scratch_binding_key[i],
                                         sizeof(gui->scratch_binding_key[i]),
                                         ImGuiInputTextFlags_AllowTabInput)) {
                        b->bound_key = gui->scratch_binding_key[i][0];
                    }
                    ImGui::PopItemWidth();
                }

                ImGui::EndChild();
            } ImGui::End();
        }

        // History window
        if ( milton->current_mode == MiltonMode::HISTORY ) {
            {
                Rect pb = picker_get_bounds(&gui->picker);
                auto width = 20 + pb.right - pb.left;
                ImGui::SetNextWindowPos(ImVec2(width, 30), ImGuiSetCond_FirstUseEver);
            }
            ImGui::SetNextWindowSize(ImVec2(ui_scale*500, ui_scale*100), ImGuiSetCond_FirstUseEver);
            if ( ImGui::Begin("History Slider") ) {
                ImGui::SliderInt("History", &gui->history, 0,
                                 layer::count_strokes(milton->canvas->root_layer));
            } ImGui::End();

        }
    } // Windows

    // Note: The export window is drawn regardless of gui visibility.
    if ( milton->current_mode == MiltonMode::EXPORTING ) {
        bool opened = true;
        b32 reset = false;

        ImGui::SetNextWindowPos(ImVec2(100, 30), ImGuiSetCond_FirstUseEver);
        ImGui::SetNextWindowSize({ui_scale*350, ui_scale*235}, ImGuiSetCond_FirstUseEver);  // We don't want to set it *every* time, the user might have preferences

        // Export window
        if ( ImGui::Begin(loc(TXT_export_DOTS), &opened, ImGuiWindowFlags_NoCollapse) ) {
            ImGui::Text(loc(TXT_MSG_click_and_drag_instruction));

            Exporter* exporter = &milton->gui->exporter;
            if ( exporter->state == ExporterState_SELECTED ) {
                i32 x = min( exporter->needle.x, exporter->pivot.x );
                i32 y = min( exporter->needle.y, exporter->pivot.y );
                int raster_w = MLT_ABS(exporter->needle.x - exporter->pivot.x);
                int raster_h = MLT_ABS(exporter->needle.y - exporter->pivot.y);

                ImGui::Text("%s: %dx%d\n",
                            loc(TXT_current_selection),
                            raster_w, raster_h);
                if (ImGui::InputInt(loc(TXT_scale_up), &exporter->scale, 1, /*step_fast=*/2)) {}
                if ( exporter->scale <= 0 ) {
                    exporter->scale = 1;
                }
                float viewport_limits[2] = {};
                gpu_get_viewport_limits(milton->renderer, viewport_limits);

                while ( exporter->scale*raster_w > viewport_limits[0]
                        || exporter->scale*raster_h > viewport_limits[1] ) {
                    --exporter->scale;
                }
                i32 max_scale = milton->view->scale / 2;
                if ( exporter->scale > max_scale) {
                    exporter->scale = max_scale;
                }
                ImGui::Text("%s: %dx%d\n", loc(TXT_final_image_resolution), raster_w*exporter->scale, raster_h*exporter->scale);

                ImGui::Text(loc(TXT_background_COLON));
                static int radio_v = 0;
                ImGui::RadioButton(loc(TXT_background_color), &radio_v, 0);
                ImGui::RadioButton(loc(TXT_transparent_background), &radio_v, 1);
                bool transparent_background = radio_v == 1;

                if ( ImGui::Button(loc(TXT_export_selection_to_image_DOTS)) ) {
                    // Render to buffer
                    int bpp = 4;  // bytes per pixel
                    i32 w = raster_w * exporter->scale;
                    i32 h = raster_h * exporter->scale;
                    size_t size = (size_t)w * h * bpp;
                    u8* buffer = (u8*)mlt_calloc(1, size, "Bitmap");
                    if ( buffer ) {
                        opened = false;
                        gpu_render_to_buffer(milton, buffer, exporter->scale,
                                             x,y, raster_w, raster_h, transparent_background ? 0.0f : 1.0f);
                        //milton_render_to_buffer(milton, buffer, x,y, raster_w, raster_h, exporter->scale);
                        PATH_CHAR* fname = platform_save_dialog(FileKind_IMAGE);
                        if ( fname ) {
                            milton_save_buffer_to_file(fname, buffer, w, h);
                        }
                        mlt_free (buffer, "Bitmap");
                    } else {
                        platform_dialog(loc(TXT_MSG_memerr_did_not_write), loc(TXT_error));
                    }
                }
            }
        }

        if ( ImGui::Button(loc(TXT_cancel)) ) {
            reset = true;
            milton_leave_mode(milton);
        }
        ImGui::End(); // Export...
        if ( !opened ) {
            reset = true;
            milton_leave_mode(milton);
        }
        if ( reset ) {
            exporter_init(&milton->gui->exporter);
        }
    } // exporting

#if MILTON_ENABLE_PROFILING
    ImGui::SetNextWindowPos(ImVec2(ui_scale*300, ui_scale*205), ImGuiSetCond_FirstUseEver);
    ImGui::SetNextWindowSize({ui_scale*350, ui_scale*285}, ImGuiSetCond_FirstUseEver);  // We don't want to set it *every* time, the user might have preferences
    if ( milton->viz_window_visible ) {
        bool opened = true;
        if ( ImGui::Begin("Debug Data ([BACKQUOTE] to toggle)", &opened, ImGuiWindowFlags_NoCollapse) ) {
            float graph_height = 20;
            char msg[512] = {};

            float poll     = perf_count_to_sec(milton->graph_frame.polling) * 1000.0f;
            float update   = perf_count_to_sec(milton->graph_frame.update) * 1000.0f;
            float clipping = perf_count_to_sec(milton->graph_frame.clipping) * 1000.0f;
            float raster   = perf_count_to_sec(milton->graph_frame.raster) * 1000.0f;
            float GL       = perf_count_to_sec(milton->graph_frame.GL) * 1000.0f;
            float system   = perf_count_to_sec(milton->graph_frame.system) * 1000.0f;

            float sum = poll + update + raster + GL + system;

            snprintf(msg, array_count(msg),
                     "Input Polling %f ms\n",
                     poll);
            ImGui::Text(msg);

            snprintf(msg, array_count(msg),
                     "Milton Update %f ms\n",
                     update);
            ImGui::Text(msg);

            snprintf(msg, array_count(msg),
                     "Clipping & Update %f ms\n",
                     clipping);
            ImGui::Text(msg);

            snprintf(msg, array_count(msg),
                     "OpenGL commands %f ms\n",
                     GL);
            ImGui::Text(msg);

            snprintf(msg, array_count(msg),
                     "System %f ms \n",
                     system);
            ImGui::Text(msg);

            snprintf(msg, array_count(msg),
                     "Number of strokes in GPU memory: %d\n",
                     gpu_get_num_clipped_strokes(milton->canvas->root_layer));
            ImGui::Text(msg);

            float hist[] = { poll, update, raster, GL, system };
            ImGui::PlotHistogram("Graph",
                            (const float*)hist, array_count(hist));

            {
                static const int window_size = 100;
                static float moving_window[window_size] = {};
                static int window_i = 0;

                moving_window[window_i++] = sum;
                window_i %= window_size;

                float mavg = 0.0f;
                for ( int i =0; i < window_size; ++i ) {
                    mavg += moving_window[i];
                }
                mavg /= window_size;

                snprintf(msg, array_count(msg),
                         "Total %f ms (%f ms m. avg)\n",
                         sum,
                         mavg);

            }
            ImGui::Text(msg);

            ImGui::Dummy({0,30});

            i64 stroke_count = layer::count_strokes(milton->canvas->root_layer);

            auto* view = milton->view;
            int screen_height = view->screen_size.h * view->scale;
            int screen_width = view->screen_size.w * view->scale;

            if ( screen_height>0 && screen_height>0 ) {
                v2l pan = view->pan_center;

                i64 radius = ((i64)((1ull)<<63ull)-1);

                {
                    if ( pan.y > 0 ) {
                        i64 n_screens_below = ((i64)(radius) - (i64)pan.y)/(i64)screen_height;
                        snprintf(msg, array_count(msg),
                                 "Screens below: %I64d\n", n_screens_below);
                        ImGui::Text(msg);
                    } else {
                        i64 n_screens_above = ((i64)(radius) + (i64)pan.y)/(i64)screen_height;
                        snprintf(msg, array_count(msg),
                                 "Screens above: %I64d\n", n_screens_above);
                        ImGui::Text(msg);
                    }
                }
                {
                    if ( pan.x > 0 ) {
                        i64 n_screens_right = ((i64)(radius) - (i64)pan.x)/(i64)screen_width;
                        snprintf(msg, array_count(msg),
                                 "Screens to the right: %I64d\n", n_screens_right);
                        ImGui::Text(msg);
                    } else {
                        i64 n_screens_left = ((i64)(radius) + (i64)pan.x)/(i64)screen_width;
                        snprintf(msg, array_count(msg),
                                 "Screens to the left: %I64d\n", n_screens_left);
                        ImGui::Text(msg);
                    }
                }
            }
            snprintf(msg, array_count(msg),
                     "Scale: %lld", view->scale);
            ImGui::Text(msg);

            // Average stroke size.
            i64 avg = 0;
            {
                for ( Layer* l = milton->canvas->root_layer;
                      l != NULL;
                      l = l->next ) {
                    for ( i64 i = 0; i < l->strokes.count; ++i ) {
                        Stroke* s = get(&l->strokes, i);
                        avg += s->num_points;
                    }
                }
                if ( stroke_count > 0 ) {
                    avg /= stroke_count;
                }
            }
            snprintf(msg, array_count(msg),
                     "Average stroke size: %" PRIi64, avg);
            ImGui::Text(msg);

        } ImGui::End();
    } // profiling
#endif
    ImGui::PopStyleColor(color_stack);
}

static Rect
color_button_as_rect(const ColorButton* button)
{
    Rect rect = {};
    rect.left = button->x;
    rect.right = button->x + button->w;
    rect.top = button->y;
    rect.bottom = button->y + button->h;
    return rect;
}

static void
picker_update_points(ColorPicker* picker, float angle)
{
    picker->data.hsv.h = radians_to_degrees(angle);
    if ( picker->fixed_triangle ) { angle = 0; }
    // Update the triangle
    float radius = 0.9f * (picker->wheel_radius - picker->wheel_half_width);
    v2f center = v2l_to_v2f(VEC2L(picker->center));
    {
        v2f point = polar_to_cartesian(-angle, radius);
        point = point + center;
        picker->data.c = point;
    }
    {
        v2f point = polar_to_cartesian(-angle + 2 * kPi / 3.0f, radius);
        point = point + center;
        picker->data.b = point;
    }
    {
        v2f point = polar_to_cartesian(-angle + 4 * kPi / 3.0f, radius);
        point = point + center;
        picker->data.a = point;
    }
}


void
picker_set_fixed_triangle(ColorPicker* picker, int fixed)
{
    if ( picker->fixed_triangle != fixed ) {
        picker->fixed_triangle = fixed;
        picker_update_points(picker, picker->data.hsv.h * kPi / 180.0f);
    }
}

static void
picker_update_wheel(ColorPicker* picker, v2f polar_point)
{
    float angle = picker_wheel_get_angle(picker, polar_point);
    picker_update_points(picker, angle);
}

static b32
picker_hits_triangle(ColorPicker* picker, v2f fpoint)
{
    b32 result = is_inside_triangle(fpoint, picker->data.a, picker->data.b, picker->data.c);
    return result;
}

static void
picker_deactivate(ColorPicker* picker)
{
    picker->flags = ColorPickerFlags_NOTHING;
}

static b32
is_inside_picker_rect(ColorPicker* picker, v2i point)
{
    return is_inside_rect(picker->bounds, point);
}

static Rect
picker_color_buttons_bounds(const ColorPicker* picker)
{
    Rect bounds = {};
    bounds.right = INT_MIN;
    bounds.left = INT_MAX;
    bounds.top = INT_MAX;
    bounds.bottom = INT_MIN;
    const ColorButton* button = picker->color_buttons;
    while ( button ) {
        bounds = rect_union(bounds, color_button_as_rect(button));
        button = button->next;
    }
    return bounds;
}

static b32
is_inside_picker_button_area(ColorPicker* picker, v2i point)
{
    Rect button_rect = picker_color_buttons_bounds(picker);
    b32 is_inside = is_inside_rect(button_rect, point);
    return is_inside;
}

b32
gui_point_hovers(MiltonGui* gui, v2i point)
{
    b32 hovers = gui->visible &&
                    (is_inside_picker_rect(&gui->picker, point) ||
                     is_inside_picker_button_area(&gui->picker, point));
    return hovers;
}

void
gui_picker_from_rgb(ColorPicker* picker, v3f rgb)
{
    v3f hsv = rgb_to_hsv(rgb);
    picker->data.hsv = hsv;
    float angle = hsv.h * 2*kPi;
    picker_update_points(picker, angle);
}

static void
update_button_bounds(ColorPicker* picker, f32 ui_scale)
{
    i32 bounds_radius_px = picker->bounds_radius_px;

    i32 spacing = 4*ui_scale;
    i32 num_buttons = NUM_BUTTONS;

    i32 button_size = (2*bounds_radius_px - (num_buttons - 1) * spacing) / num_buttons;
    i32 current_x = picker->center.x - bounds_radius_px;

    for ( ColorButton* cur_button = picker->color_buttons;
          cur_button != NULL;
          cur_button = cur_button->next ) {
        cur_button->x = current_x;
        cur_button->y = picker->center.y + bounds_radius_px + spacing;
        cur_button->w = button_size;
        cur_button->h = button_size;

        current_x += spacing + button_size;
    }
}

static b32
picker_hit_history_buttons(ColorPicker* picker, f32 ui_scale, v2i point)
{
    b32 hits = false;
    ColorButton* first = picker->color_buttons;
    ColorButton* button = first;
    ColorButton* prev = NULL;
    while ( button ) {
        if ( button->rgba.a != 0 &&
             is_inside_rect(color_button_as_rect(button), point) ) {
            hits = true;

            gui_picker_from_rgb(picker, button->rgba.rgb);

            if ( prev ) {
                prev->next = button->next;
            }
            if ( button != picker->color_buttons ) {
                button->next = picker->color_buttons;
            }
            picker->color_buttons = button;

            update_button_bounds(picker, ui_scale);
            break;
        }
        prev = button;
        button = button->next;
    }
    return hits;
}

static ColorPickResult
picker_update(ColorPicker* picker, v2i point)
{
    ColorPickResult result = ColorPickResult_NOTHING;
    v2f fpoint = v2i_to_v2f(point);
    if ( picker->flags == ColorPickerFlags_NOTHING ) {
        if ( picker_hits_wheel(picker, fpoint) ) {
            picker->flags |= ColorPickerFlags_WHEEL_ACTIVE;
        }
        else if ( picker_hits_triangle(picker, fpoint) ) {
            picker->flags |= ColorPickerFlags_TRIANGLE_ACTIVE;
        }
    }
    if (( picker->flags & ColorPickerFlags_WHEEL_ACTIVE )) {
        if (!(picker->flags & ColorPickerFlags_TRIANGLE_ACTIVE))
        {
            picker_update_wheel(picker, fpoint);
            result = ColorPickResult_CHANGE_COLOR;
        }
    }
    if (( picker->flags & ColorPickerFlags_TRIANGLE_ACTIVE )) {
        PickerData& d = picker->data;  // Just shortening the identifier.

        // We don't want the chooser to "stick" if it goes outside the triangle
        // (i.e. picking black should be easy)
        f32 abp = orientation(d.a, d.b, fpoint);
        f32 bcp = orientation(d.b, d.c, fpoint);
        f32 cap = orientation(d.c, d.a, fpoint);
        int insideness = (abp < 0.0f) + (bcp < 0.0f) + (cap < 0.0f);

        // inside the triangle
        if ( insideness == 3 ) {
            float inv_area = 1.0f / orientation(d.a, d.b, d.c);
            d.hsv.v = 1.0f - (cap * inv_area);

            d.hsv.s = abp * inv_area / d.hsv.v;
            d.hsv.s = abp  / (orientation(d.a, d.b, d.c) - cap);
            d.hsv.s = 1 - (bcp * inv_area) / d.hsv.v;
        }
        // near a corner
        else if ( insideness < 2 ) {
            if (abp < 0.0f) {
                d.hsv.s = 1.0f;
                d.hsv.v = 1.0f;
            }
            else if (bcp < 0.0f) {
                d.hsv.s = 0.0f;
                d.hsv.v = 1.0f;
            }
            else {
                d.hsv.s = 0.5f;
                d.hsv.v = 0.0f;
            }
        }
        // near an edge
        else {
#define GET_T(A, B, C)                            \
    v2f perp = perpendicular(fpoint - C); \
    f32 t = DOT(C - A, perp) / DOT(B - A, perp);
            if (abp >= 0.0f) {
                GET_T(d.b, d.a, d.c)
                d.hsv.s = 0.0f;
                d.hsv.v = t;
            }
            else if (bcp >= 0.0f) {
                GET_T(d.b, d.c, d.a)
                d.hsv.s = 1.0f;
                d.hsv.v = t;
            }
            else {
                GET_T(d.a, d.c, d.b)
                d.hsv.s = t;
                d.hsv.v = 1.0f;
            }
        }
#undef GET_T
        result = ColorPickResult_CHANGE_COLOR;
    }

    return result;
}


b32
picker_hits_wheel(ColorPicker* picker, v2f point)
{
    v2f center = v2i_to_v2f(picker->center);
    v2f arrow = point - center;
    float dist = magnitude(arrow);

    b32 hits = (dist <= picker->wheel_radius + picker->wheel_half_width ) &&
               (dist >= picker->wheel_radius - picker->wheel_half_width );

    return hits;
}

float
picker_wheel_get_angle(ColorPicker* picker, v2f point)
{
    v2f direction = point - v2i_to_v2f(picker->center);
    f32 angle = atan2f(direction.y, -direction.x) + kPi;
    return angle;
}

void
picker_init(ColorPicker* picker)
{
    v2f fpoint = {
        (f32)picker->center.x + (int)(picker->wheel_radius),
        (f32)picker->center.y
    };
    picker_update_wheel(picker, fpoint);
    picker->data.hsv = v3f{ 0, 1, 0 };
}

Rect
picker_get_bounds(ColorPicker* picker)
{
    Rect picker_rect;
    {
        picker_rect.left   = picker->center.x - picker->bounds_radius_px;
        picker_rect.right  = picker->center.x + picker->bounds_radius_px;
        picker_rect.bottom = picker->center.y + picker->bounds_radius_px;
        picker_rect.top    = picker->center.y - picker->bounds_radius_px;
    }
    mlt_assert (picker_rect.left >= 0);
    mlt_assert (picker_rect.top >= 0);

    return picker_rect;
}

void
exporter_init(Exporter* exporter)
{
    *exporter = Exporter{};
    exporter->scale = 1;
}

b32
exporter_input(Exporter* exporter, MiltonInput const* input)
{
    b32 changed = false;
    if ( input->input_count > 0 ) {
        changed = true;
        v2i point = VEC2I(input->points[input->input_count - 1]);
        if ( exporter->state == ExporterState_EMPTY ||
             exporter->state == ExporterState_SELECTED ) {
            exporter->pivot = point;
            exporter->needle = point;
            exporter->state = ExporterState_GROWING_RECT;
        }
        else if ( exporter->state == ExporterState_GROWING_RECT ) {
            exporter->needle = point;
        }
    }
    if ( (input->flags & MiltonInputFlags_END_STROKE) && exporter->state != ExporterState_EMPTY ) {
        exporter->state = ExporterState_SELECTED;
        changed = true;
    }
    return changed;
}

void
gui_imgui_set_ungrabbed(MiltonGui* gui)
{
    gui->flags &= ~MiltonGuiFlags_SHOWING_PREVIEW;
}

Rect
get_bounds_for_picker_and_colors(ColorPicker* picker)
{
    Rect result = rect_union(picker_get_bounds(picker), picker_color_buttons_bounds(picker));
    return result;
}

static b32
picker_is_active(ColorPicker* picker)
{
    b32 active = (picker->flags & ColorPickerFlags_TRIANGLE_ACTIVE) ||
            (picker->flags & ColorPickerFlags_WHEEL_ACTIVE);
    return active;
}

v3f
picker_hsv_from_point(ColorPicker* picker, v2f point)
{
    float area = orientation(picker->data.a, picker->data.b, picker->data.c);
    v3f hsv = {};
    if ( area != 0 ) {
        float inv_area = 1.0f / area;
        float v = 1 - (orientation(point, picker->data.c, picker->data.a) * inv_area);
        float s = orientation(picker->data.b, point, picker->data.a) * inv_area / v;

        hsv = v3f
        {
            picker->data.hsv.h,
            s,
            v,
        };
    }
    return hsv;
}

v3f
gui_get_picker_rgb(MiltonGui* gui)
{
    v3f rgb = hsv_to_rgb(gui->picker.data.hsv);
    return rgb;
}

// Returns true if the Picker consumed input. False if the GUI wasn't affected
b32
gui_consume_input(MiltonGui* gui, MiltonInput const* input)
{
    b32 accepts = false;
    v2i point = VEC2I(input->points[0]);
    if ( gui->visible ) {
        accepts = gui_point_hovers(gui, point);
        if ( !picker_is_active(&gui->picker) &&
             !gui->did_hit_button &&
             picker_hit_history_buttons(&gui->picker, gui->scale, point) ) {
            accepts = true;
            gui->did_hit_button = true;
            gui->owns_user_input = true;
        }
        if ( accepts ) {
            ColorPickResult pick_result = picker_update(&gui->picker, point);
            if ( pick_result == ColorPickResult_CHANGE_COLOR ) {
                gui->owns_user_input = true;
            }
        }
    }
    return accepts;
}

void
gui_toggle_menu_visibility(MiltonGui* gui)
{
    gui->menu_visible = !gui->menu_visible;
}

void
gui_toggle_help(MiltonGui* gui)
{
    gui->show_help_widget = !gui->show_help_widget;
}

void
gui_init(Arena* root_arena, MiltonGui* gui, f32 ui_scale)
{
    gui->scale = ui_scale;
    gui->panel_width = ui_scale*240;
    gui->layers_height = 0;
    gui->panel_screen_w = (i32)gui->panel_width;
    gui->picker.data.hsv = v3f{ 0.0f, 1.0f, 0.7f };
    // The pixels buffer is not used for rendering. Size it for the largest panel.
    i32 max_radius = (i32)(ui_scale*GUI_PANEL_MAX_WIDTH/2);
    gui->picker.pixels = arena_alloc_array(root_arena, (4 * max_radius * max_radius), u32);
    gui->visible = true;
    gui->picker.color_buttons = arena_alloc_elem(root_arena, ColorButton);
    gui->original_settings = arena_alloc_elem(root_arena, MiltonSettings);

    i32 num_buttons = NUM_BUTTONS;
    auto* cur_button = gui->picker.color_buttons;
    for ( i64 i = 0; i != (num_buttons - 1); ++i ) {
        cur_button->next = arena_alloc_elem(root_arena, ColorButton);
        cur_button = cur_button->next;
    }
    gui_anchor_picker_top_right(gui, gui->panel_screen_w);
    picker_init(&gui->picker);

    gui->preview_pos      = v2i{-1, -1};
    gui->preview_pos_prev = v2i{-1, -1};


    exporter_init(&gui->exporter);
}

// Lays out the picker (and its color buttons) so that it spans the panel width at the top right.
void
gui_anchor_picker_top_right(MiltonGui* gui, i32 screen_width)
{
    ColorPicker* picker = &gui->picker;
    f32 ui_scale = gui->scale;

    gui->panel_screen_w = screen_width;
    f32 min_width = ui_scale*GUI_PANEL_MIN_WIDTH;
    f32 max_width = min(ui_scale*GUI_PANEL_MAX_WIDTH, (f32)(screen_width*0.6f));
    gui->panel_width = clamp(gui->panel_width, min_width, max(max_width, min_width));

    i32 r = (i32)(gui->panel_width / 2);
    if ( r > screen_width/2 ) { r = screen_width/2; }
    v2i new_center = { screen_width - r, r + (i32)(ui_scale*30) };

    picker->bounds_radius_px = r;
    picker->wheel_half_width = r * 0.15f;
    picker->wheel_radius = r - r*0.0625f - picker->wheel_half_width;
    picker->center = new_center;
    picker->bounds.left   = new_center.x - r;
    picker->bounds.right  = new_center.x + r;
    picker->bounds.top    = new_center.y - r;
    picker->bounds.bottom = new_center.y + r;

    // The triangle follows the wheel and the new size.
    picker_update_points(picker, picker->data.hsv.h * kPi / 180.0f);
    update_button_bounds(picker, ui_scale);
}// When a selected color is used in a stroke, call this to update the color
// button list.
b32
gui_mark_color_used(MiltonGui* gui)
{
    b32 changed = false;
    ColorButton* start = gui->picker.color_buttons;
    v3f picker_color  = hsv_to_rgb(gui->picker.data.hsv);
    // Search for a color that is already in the list
    ColorButton* button = start;
    while ( button ) {
        if ( button->rgba.a != 0 ) {
            v3f diff =
            {
                fabsf(button->rgba.r - picker_color.r),
                fabsf(button->rgba.g - picker_color.g),
                fabsf(button->rgba.b - picker_color.b),
            };
            float epsilon = 0.000001f;
            if ( diff.r < epsilon && diff.g < epsilon && diff.b < epsilon ) {
                // Move this button to the start and return.
                changed = true;
                v4f tmp_color = button->rgba;
                button->rgba = start->rgba;
                start->rgba = tmp_color;
            }
        }
        button = button->next;
    }
    button = start;

    // If not found, add to list.
    if ( !changed ) {
        changed = true;
        v4f button_color = color_rgb_to_rgba(picker_color,1);
        // Pass data to the next one.
        while ( button ) {
            v4f tmp_color = button->rgba;
            button->rgba = button_color;
            button_color = tmp_color;
            button = button->next;
        }
    }

    return changed;
}

void
gui_deactivate(MiltonGui* gui)
{
    picker_deactivate(&gui->picker);

    // Reset transient values
    gui->owns_user_input = false;
    gui->did_hit_button = false;
}
