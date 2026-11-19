#undef main // SDL does things we don't want
#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui_internal.h"

#if defined(_WIN32)
static POINTER_PEN_INFO test_ink_history[4];
static UINT32 test_ink_count;

static BOOL WINAPI
test_ink_type(UINT32, POINTER_INPUT_TYPE* type)
{
    *type = PT_PEN;
    return TRUE;
}

static BOOL WINAPI
test_ink_latest(UINT32, POINTER_PEN_INFO* pen)
{
    *pen = test_ink_history[0];
    return TRUE;
}

static BOOL WINAPI
test_ink_get_history(UINT32, UINT32* count, POINTER_PEN_INFO* history)
{
    memcpy(history, test_ink_history, min(*count, test_ink_count) * sizeof(*history));
    *count = test_ink_count;
    return TRUE;
}

static void
test_ink_message(HWND hwnd, UINT message, int first_x, UINT32 count, bool release)
{
    test_ink_count = count;
    for ( UINT32 i = 0; i < count; ++i ) {
        auto& pen = test_ink_history[count - i - 1];
        pen = {};
        pen.pointerInfo.historyCount = count;
        pen.pointerInfo.pointerFlags = POINTER_FLAG_INRANGE;
        if ( !release || i < count - 1 ) {
            pen.pointerInfo.pointerFlags |= POINTER_FLAG_INCONTACT;
        }
        pen.pointerInfo.ptPixelLocation = POINT{first_x + (LONG)i, 20};
        ClientToScreen(hwnd, &pen.pointerInfo.ptPixelLocation);
        pen.penMask = PEN_MASK_PRESSURE;
        pen.pressure = 256 * (i + 1);
    }
    ink_capture_message(hwnd, hwnd, message, 1, 0);
}

static bool
test_windows_ink()
{
    if ( SDL_Init(SDL_INIT_EVENTS) != 0 ) {
        fprintf(stderr, "Could not initialize SDL event queue: %s\n", SDL_GetError());
        return false;
    }
    ImGui::CreateContext();
    ink_event_type = SDL_RegisterEvents(1);
    ink_GetPointerType = test_ink_type;
    ink_GetPointerPenInfo = test_ink_latest;
    ink_GetPointerPenInfoHistory = test_ink_get_history;
    EasyTabInfo tablet = {};
    EasyTab = &tablet;
    HWND hwnd = GetDesktopWindow();
    PlatformState platform = {};
    MiltonSettings settings = {};
    Milton milton = {};
    milton.current_mode = MiltonMode::PEN;
    milton.settings = &settings;

    // Simulate one slow render frame with several native messages, including a lift
    // and the start of the next stroke. Capture must not defer reading message data.
    test_ink_message(hwnd, WM_POINTERDOWN, 10, 1, false);
    test_ink_message(hwnd, WM_POINTERUPDATE, 11, 3, false);
    test_ink_message(hwnd, WM_POINTERUP, 14, 2, true);
    test_ink_message(hwnd, WM_POINTERDOWN, 30, 1, false);
    MiltonInput input = sdl_event_loop(&milton, &platform);
    bool passed = platform.num_point_results == 5 && platform.num_pressure_results == 5
                  && (input.flags & MiltonInputFlags_END_STROKE) && !platform.is_pointer_down;
    const f32 pressures[] = {0.25f, 0.25f, 0.5f, 0.75f, 0.25f};
    for ( int i = 0; passed && i < 5; ++i ) {
        passed = input.points[i].x == 10 + i && input.points[i].y == 20
                 && input.pressures[i] == pressures[i];
    }
    ink_GetPointerPenInfoHistory = NULL;
    test_ink_message(hwnd, WM_POINTERUPDATE, 31, 2, false);
    input = sdl_event_loop(&milton, &platform);
    passed = passed && platform.num_point_results == 2 && !(input.flags & MiltonInputFlags_END_STROKE)
             && input.points[0].x == 30 && input.points[1].x == 32 && input.pressures[1] == 0.5f;

    ink_GetPointerPenInfoHistory = test_ink_get_history;
    for ( int i = 0; i < 10; ++i ) {
        test_ink_message(hwnd, WM_POINTERUPDATE, 33 + i * 4, 4, false);
    }
    input = sdl_event_loop(&milton, &platform);
    passed = passed && platform.num_point_results == 40 && platform.num_pressure_results == 40
             && input.points[0].x == 33 && input.points[39].x == 72 && input.pressures[39] == 1.0f;

    ink_deinit();
    EasyTab = NULL;
    ImGui::DestroyContext();
    SDL_Quit();
    if ( !passed ) {
        fprintf(stderr, "Windows Ink regression: samples, pressure, or stroke boundaries lost\n");
    } else {
        puts("Windows Ink regression test passed");
    }
    return passed;
}
#endif

#define INVALIDATE_COUNT(ptr, count) memset((u8*)(ptr), -1, sizeof(*(ptr)) * (count))

#define INVALIDATE(ptr) INVALIDATE_COUNT(ptr, 1)


#define COMPARE_BYTES_COUNT(ptrA, ptrB, count) \
    assert(sizeof(*(ptrA)) == sizeof(*(ptrB))), \
    compare_bytes((u8*)(ptrA), (u8*)(ptrB), sizeof(*(ptrA)) * (count) )

#define COMPARE_BYTES(ptrA, ptrB) \
    COMPARE_BYTES_COUNT(ptrA, ptrB, 1)

#define EXPECT_TRUE(expr) \
    if (!(expr)) { \
        failed_test(#expr); \
    }


void
failed_test(char* expr)
{
    fprintf(stderr, "Failed expression: [ %s ]\n", expr);
}

b32
compare_bytes(u8* bufA, u8* bufB, sz size)
{
    b32 equal = true;

    for (sz i = 0; i < size; ++i) {
        if (bufA[i] != bufB[i]) {
            equal = false;
            break;
        }
    }

    return equal;
}

static bool
test_save_load()
{
    Milton milton = {};

    PATH_CHAR* path = TO_PATH_STR("TEST_loading.mlt");

    milton_init(&milton, 0, 0, 1, path, MiltonInit_FOR_TEST);
    milton_reset_canvas_and_set_default(&milton);
    auto& picker = milton.gui->picker;
    picker.controls_height = 80;
    bool layout_ok = true;
    const i32 widths[] = {160, 240, 480};
    for ( i32 width : widths ) {
        milton.gui->panel_width = (f32)width;
        gui_anchor_picker_top_right(milton.gui, 1000);
        const ColorButton* prev = NULL;
        for ( auto* swatch = picker.color_buttons; swatch; swatch = swatch->next ) {
            layout_ok = layout_ok && swatch->x >= picker.bounds.left
                        && swatch->x + swatch->w <= picker.bounds.right
                        && swatch->w == swatch->h && swatch->w > 0
                        && swatch->y >= picker.bounds.bottom + picker.controls_height
                        && (!prev || (prev->x + prev->w <= swatch->x && prev->y == swatch->y));
            prev = swatch;
        }
    }
    int index = 0;
    for ( auto* swatch = picker.color_buttons; swatch; swatch = swatch->next ) {
        swatch->rgba = v4f{0.25f, index++ / 10.0f, 0.75f, 1.0f};
    }
    milton.persist->mlt_file_path = path;
    milton_save(&milton);

    Milton loaded_milton = {};

    milton_init(&loaded_milton, 0, 0, 1, TO_PATH_STR("TEST_loading.mlt"), MiltonInit_FOR_TEST);

    INVALIDATE(loaded_milton.view);
    loaded_milton.view->screen_size = milton.view->screen_size;

    INVALIDATE_COUNT(loaded_milton.brushes, 3);
    INVALIDATE_COUNT(loaded_milton.brush_sizes, 3); // We only save PEN, ERASER and PRIMITIVE

    milton_load(&loaded_milton);

    EXPECT_TRUE( COMPARE_BYTES(milton.view, loaded_milton.view) );

    EXPECT_TRUE( milton.canvas->layer_guid == loaded_milton.canvas->layer_guid );

    EXPECT_TRUE( COMPARE_BYTES_COUNT(milton.brushes, loaded_milton.brushes, BrushEnum_COUNT) );
    EXPECT_TRUE( COMPARE_BYTES_COUNT(milton.brush_sizes, loaded_milton.brush_sizes, BrushEnum_COUNT) );

    auto* loaded = loaded_milton.gui->picker.color_buttons;
    bool history_ok = index == 10;
    for ( auto* swatch = picker.color_buttons; swatch && loaded; swatch = swatch->next, loaded = loaded->next ) {
        history_ok = history_ok && (COMPARE_BYTES(&swatch->rgba, &loaded->rgba));
    }
    if ( !layout_ok || !history_ok ) {
        fprintf(stderr, "Color picker regression: swatches did not fit or did not survive save/load\n");
        return false;
    }
    puts("Color picker layout and history round-trip passed");
    return true;
}

static bool
test_picker_slider_input()
{
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = NULL;
    io.DisplaySize = ImVec2(320, 160);
    io.KeyMap[ImGuiKey_Enter] = 13;
    io.Fonts->AddFontDefault();
    io.Fonts->Build();
    v3f hsv = {240.0f, 0.0f, 1.0f};
    ImVec2 track = {}, field = {};
    auto frame = [&](ImVec2 mouse, bool down, bool enter = false) {
        io.MousePos = mouse;
        io.MouseDown[0] = down;
        io.KeysDown[13] = enter;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(5, 5));
        ImGui::SetNextWindowSize(ImVec2(300, 140));
        ImGui::Begin("##test_picker_slider", NULL, ImGuiWindowFlags_NoDecoration);
        const auto& style = ImGui::GetStyle();
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const f32 y = start.y + ImGui::GetFrameHeight() * 0.5f;
        const f32 x = start.x + ImGui::CalcTextSize("S").x + style.ItemSpacing.x;
        track = ImVec2(x + 50, y);
        field = ImVec2(x + 100 + style.ItemSpacing.x + 15, y);
        gui_picker_slider_row("S", "%", &hsv, 1, 100, 30);
        ImGui::End();
        ImGui::Render();
    };
    frame(ImVec2(0, 0), false);
    frame(track, true);
    bool passed = fabsf(hsv.s - 0.5f) < 0.01f;
    frame(track, false);
    frame(field, true);
    frame(field, false);
    io.AddInputCharactersUTF8("75");
    frame(field, false);
    frame(field, false, true);
    passed = passed && fabsf(hsv.s - 0.75f) < 0.01f && hsv.h == 240.0f && hsv.v == 1.0f;
    ImGui::DestroyContext();
    if ( !passed ) {
        fprintf(stderr, "HSV slider did not respond to dragging or typed input\n");
    }
    return passed;
}

static bool
test_blur_tool_ui()
{
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = NULL;
    io.DisplaySize = ImVec2(1000, 800);
    io.Fonts->AddFontDefault();
    io.Fonts->Build();
    Milton milton = {};
    milton_init(&milton, 1000, 800, 1, NULL, MiltonInit_FOR_TEST);
    milton.current_mode = MiltonMode::BLUR;
    milton.brushes[BrushEnum_BLUR].alpha = 0.9f;
    MiltonInput input = {};
    input.mode_to_set = MiltonMode::MODE_COUNT;
    auto frame = [&](ImVec2 mouse, bool down) {
        io.MousePos = mouse;
        io.MouseDown[0] = down;
        ImGui::NewFrame();
        gui_brush_window(&input, nullptr, &milton, nullptr, false);
        ImGui::Render();
    };
    frame(ImVec2(900, 700), false);
    frame(ImVec2(900, 700), false);
    ImGuiWindow* tools = ImGui::FindWindowByName("Tools");
    ImGuiWindow* options = ImGui::FindWindowByName("Tool Options (Blur)###brush_options");
    bool passed = tools && options;
    if ( passed ) {
        ImVec2 strength(options->Pos.x + 6 + 55,
                        options->Pos.y + options->TitleBarHeight() + 4 + ImGui::GetFrameHeight() * 0.5f);
        frame(strength, true);
        frame(strength, false);
        passed = milton.brushes[BrushEnum_BLUR].alpha > 0.4f
                 && milton.brushes[BrushEnum_BLUR].alpha < 0.6f;
        ImVec2 blur_button(tools->Pos.x + 34,
                           tools->Pos.y + 4 + 2 * (30 + ImGui::GetStyle().ItemSpacing.y) + 15);
        frame(blur_button, true);
        frame(blur_button, false);
        passed = passed && input.mode_to_set == MiltonMode::BLUR;
    }
    if ( !passed ) {
        fprintf(stderr, "Blur UI: tools=%p options=%p alpha=%f mode=%d scale=%f\n",
                tools, options, milton.brushes[BrushEnum_BLUR].alpha, (int)input.mode_to_set, milton.gui->scale);
    }
    fprintf(passed ? stdout : stderr, "Blur toolbar, tool-pane title and strength slider: %s\n", passed ? "passed" : "FAILED");
    arena_free(&milton.root_arena);
    ImGui::DestroyContext();
    return passed;
}

static bool
test_navigation_preview()
{
    RenderSettings settings = {};
    CanvasView view = {};
    view.scale = 512;
    auto update = [&](bool navigating, bool drawing, u32 now) {
        return update_navigation_preview(&settings, view, view.scale, navigating, drawing, now);
    };
    bool passed = !update(false, false, 0);
    view.pan_center.x = 100;
    passed = passed && update(false, false, 100);
    settings.do_full_redraw = false;
    passed = passed && update(false, false, 219);
    passed = passed && !update(false, false, 220)
             && settings.do_full_redraw;
    settings.do_full_redraw = false;
    passed = passed && !update(false, false, 221)
             && !settings.do_full_redraw;
    view.scale /= 2;
    passed = passed && update(false, false, 230);
    passed = passed && !update(false, true, 231);

    passed = passed && update(true, false, 300);
    passed = passed && update(true, false, 5300);
    passed = passed && update(true, true, 10300);
    passed = passed && update(false, false, 10301);
    view.angle = 0.05f;
    passed = passed && update(false, false, 10400);
    passed = passed && update(false, false, 10519);
    passed = passed && !update(false, false, 10520);
    passed = passed && update(true, false, 10600);
    passed = passed && !update(false, true, 10601);

    view.angle = 0.1f;
    passed = passed && update(true, false, 0xFFFFFFD0u);
    passed = passed && update(false, false, 20);
    passed = passed && !update(false, false, 80);
    passed = passed && update(true, false, 200);
    passed = passed && update(false, false, 319);
    passed = passed && !update(false, false, 320);

    Selection selection = {};
    Milton milton = {};
    milton.selection = &selection;
    auto selection_preview = [&](u32 now) {
        return update(selection_is_dragging(&milton), false, now);
    };
    selection.armed = true;
    selection.state = SelState_LASSO;
    passed = passed && !selection_preview(400);
    selection.state = SelState_MOVE;
    passed = passed && selection_preview(500) && selection_preview(5500);
    selection.state = SelState_IDLE;
    passed = passed && selection_preview(5619) && !selection_preview(5620);
    selection.xform_mode = true;
    passed = passed && !selection_preview(5700);
    selection.state = SelState_XFORM_DRAG;
    passed = passed && selection_preview(5800) && selection_preview(10800);
    selection.state = SelState_IDLE;
    passed = passed && selection_preview(10919) && !selection_preview(10920);
    fprintf(passed ? stdout : stderr, "Navigation preview held controls, settling, redraw, drawing cancellation and timer wrap: %s\n",
            passed ? "passed" : "FAILED");
    return passed;
}

static bool
test_input_priority()
{
    if ( SDL_Init(SDL_INIT_EVENTS) != 0 ) {
        fprintf(stderr, "Input priority: cannot initialize SDL events: %s\n", SDL_GetError());
        return false;
    }
    ImGui::CreateContext();
    SDL_SetModState(KMOD_NONE);
    SDL_Event key = {};
    key.type = SDL_KEYDOWN;
    key.key.keysym.sym = SDLK_z;
    key.key.keysym.scancode = SDL_SCANCODE_Z;
    key.key.keysym.mod = KMOD_LCTRL;
    bool passed = SDL_PushEvent(&key) == 1;
    CanvasTileCache cache = {};
    cache.can_build = true;
    cache.deadline = ~0ULL;
    passed = passed && !tile_time_available(&cache) && !cache.can_build;
    cache.can_build = true;
    cache.jobs = 1;
    passed = passed && !tile_time_available(&cache) && !cache.can_build;
    SDL_Event queued = {};
    passed = passed && SDL_PeepEvents(&queued, 1, SDL_GETEVENT, SDL_KEYDOWN, SDL_KEYDOWN) == 1
             && queued.key.keysym.sym == SDLK_z && queued.key.keysym.mod == KMOD_LCTRL;
    cache.can_build = true;
    passed = passed && tile_time_available(&cache);
    SDL_Event mouse = {};
    mouse.type = SDL_MOUSEMOTION;
    passed = passed && SDL_PushEvent(&mouse) == 1 && tile_time_available(&cache);
    passed = passed && SDL_PeepEvents(&queued, 1, SDL_GETEVENT, SDL_MOUSEMOTION, SDL_MOUSEMOTION) == 1;
    mouse.type = SDL_MOUSEWHEEL;
    passed = passed && SDL_PushEvent(&mouse) == 1
             && !tile_time_available(&cache) && !cache.can_build;
    passed = passed && SDL_PeepEvents(&queued, 1, SDL_GETEVENT, SDL_MOUSEWHEEL, SDL_MOUSEWHEEL) == 1;

    Milton milton = {};
    MiltonSettings settings = {};
    PlatformState platform = {};
    MiltonInput input = {};
    milton.settings = &settings;
    set_default_bindings(&settings.bindings);
    shortcut_handle_key(&milton, &platform, &key, &input, false);
    passed = passed && (input.flags & MiltonInputFlags_UNDO);
    SDL_SetModState(KMOD_LCTRL);
    key.key.keysym.mod = KMOD_NONE;
    input.flags = 0;
    shortcut_handle_key(&milton, &platform, &key, &input, false);
    passed = passed && !(input.flags & MiltonInputFlags_UNDO);

    SDL_SetModState(KMOD_NONE);
    binding(&settings.bindings, Modifier_SPACE, 'z', Action_UNDO);
    platform.is_space_down = true;
    shortcut_handle_key(&milton, &platform, &key, &input, false);
    passed = passed && (input.flags & MiltonInputFlags_UNDO);
    ImGui::GetIO().WantCaptureKeyboard = true;
    key.key.keysym.mod = KMOD_LALT;
    shortcut_handle_key(&milton, &platform, &key, &input, false);
    passed = passed && ImGui::GetIO().KeyAlt && !ImGui::GetIO().KeyCtrl;
    ImGui::DestroyContext();
    SDL_Quit();
    fprintf(passed ? stdout : stderr,
            "Input priority: queued keys preempt first/subsequent tile jobs without consumption; event-time shortcut modifiers: %s\n",
            passed ? "passed" : "FAILED");
    return passed;
}

static bool
test_brush_resize_priority()
{
    Milton milton = {};
    MiltonGui gui = {};
    CanvasView view = {};
    MiltonDragBrush drag = {};
    view.scale = 1024;
    milton.gui = &gui;
    milton.view = &view;
    milton.drag_brush = &drag;
    milton.current_mode = MiltonMode::PEN;
    milton.brush_sizes[BrushEnum_PEN] = 20;
    bool passed = !brush_resize_active(&milton, 0);
    milton_increase_brush_size(&milton);
    u32 changed_at = milton.render_settings.brush_resize_changed_at;
    passed = passed && milton_get_brush_radius(&milton) == 21
             && brush_resize_active(&milton, changed_at)
             && brush_resize_active(&milton, changed_at + 119)
             && !brush_resize_active(&milton, changed_at + 120);
    milton.current_mode = MiltonMode::DRAG_BRUSH_SIZE;
    drag.brush_idx = BrushEnum_PEN;
    passed = passed && brush_resize_active(&milton, 1000) && brush_resize_active(&milton, 6000);
    milton.brushes[BrushEnum_PEN].shape = BrushShape_RECTANGLE;
    milton.brushes[BrushEnum_PEN].shape_aspect = 0.25f;
    i32 shape = 0;
    f32 aspect = 1, angle = 0;
    get_outline_shape(&milton, &shape, &aspect, &angle);
    passed = passed && shape == BrushShape_RECTANGLE && aspect == 0.25f;
    milton.current_mode = MiltonMode::PEN;
    passed = passed && brush_resize_active(&milton, 6119) && !brush_resize_active(&milton, 6120);
    gui.flags |= MiltonGuiFlags_SHOWING_PREVIEW;
    passed = passed && brush_resize_active(&milton, 7000) && brush_resize_active(&milton, 12000);
    milton.working_stroke.num_points = 1;
    passed = passed && !brush_resize_active(&milton, 12001);
    milton.working_stroke.num_points = 0;
    gui.flags &= ~MiltonGuiFlags_SHOWING_PREVIEW;
    passed = passed && !brush_resize_active(&milton, 12002);
    fprintf(passed ? stdout : stderr, "Brush resizing: keyboard, held scrub, slider, shape, settling and drawing priority: %s\n",
            passed ? "passed" : "FAILED");
    return passed;
}

static bool
test_blur_performance(Milton* milton)
{
    const int width = 1920, height = 1080;
    auto* r = milton->renderer;
    auto* view = milton->view;
    view->screen_size = {width, height};
    view->zoom_center = {width / 2, height / 2};
    gpu_resize(r, view);
    gpu_update_canvas(r, milton->canvas, view);
    glViewport(0, 0, width, height);
    Layer* layer = milton->canvas->working_layer;
    auto add = [&](int index, u32 flags) {
        Stroke stroke = {};
        stroke.id = milton->canvas->stroke_id_count++;
        stroke.layer_id = layer->id;
        stroke.num_points = 2;
        stroke.points = arena_alloc_array(&milton->canvas->arena, 2, v2l);
        stroke.pressures = arena_alloc_array(&milton->canvas->arena, 2, f32);
        int y = 180 + (index % 12) * 55;
        stroke.points[0] = raster_to_canvas(view, v2l{400, y});
        stroke.points[1] = raster_to_canvas(view, v2l{1450, y + 100});
        stroke.pressures[0] = stroke.pressures[1] = 1;
        stroke.flags = flags;
        stroke.brush = default_brush();
        stroke.brush.radius = 64 * view->scale;
        stroke.brush.alpha = 0.6f;
        stroke.brush.color = to_premultiplied(v3f{0.2f + 0.04f * index, 0.5f, 0.75f}, 0.6f);
        stroke.brush.pressure_size = 0;
        stroke.bounding_rect = bounding_box_for_stroke(&stroke);
        layer::layer_push_stroke(layer, stroke);
    };
    for ( int i = 0; i < 12; ++i ) { add(i, 0); }
    for ( int i = 0; i < 12; ++i ) { add(i, StrokeFlag_BLUR); }
    float background_alpha = 0.0f;
    auto draw = [&](bool uncached = false, bool preview = false) {
        if ( uncached ) { r->replay_valid = false; }
        gpu_clip_strokes_and_update(&milton->root_arena, r, view, view->scale, milton->canvas->root_layer,
                                    &milton->working_stroke, 0, 0, width, height,
                                    preview ? ClipFlags_NAVIGATION_PREVIEW : ClipFlags_JUST_CLIP);
        gpu_render_canvas(r, 0, 0, width, height, background_alpha, preview);
        glFramebufferTexture2DEXT(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->canvas_texture, 0);
        u8 pixel[4];
        glReadPixels(width / 2, height / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    };
    auto measure = [&](bool uncached, bool preview = false) {
        draw(uncached, preview);
        u64 start = SDL_GetPerformanceCounter();
        for ( int i = 0; i < 8; ++i ) { draw(uncached, preview); }
        return 1000.0 * (SDL_GetPerformanceCounter() - start) / (8 * SDL_GetPerformanceFrequency());
    };
    double blur_ms = measure(true);
    double cached_blur_ms = measure(false);
    for ( int i = 0; i < 12; ++i ) { add(i, StrokeFlag_PRESERVE_OPACITY); }
    Stroke* working = &milton->working_stroke;
    working->layer_id = layer->id;
    working->num_points = 2;
    working->points[0] = raster_to_canvas(view, v2l{700, 500});
    working->points[1] = raster_to_canvas(view, v2l{1250, 570});
    working->pressures[0] = working->pressures[1] = 1;
    working->flags = StrokeFlag_PRESERVE_OPACITY;
    working->brush = default_brush();
    working->brush.radius = 32 * view->scale;
    working->brush.color = to_premultiplied(v3f{0.2f, 0.5f, 0.75f}, 0.6f);
    working->bounding_rect = bounding_box_for_stroke(working);
    double preserving_ms = measure(true);
    double cached_preserving_ms = measure(false);
    bool passed = r->replay_cache_hit && r->blur_filter_runs == 0;
    fprintf(stdout, "1080p blur replay uncached/cached: %.2f / %.2f ms; 12 preserving colors + moving pen: %.2f / %.2f ms\n",
            blur_ms, cached_blur_ms, preserving_ms, cached_preserving_ms);

    size_t bytes = (size_t)width * height * 4;
    auto* reference = (u8*)mlt_calloc(bytes, 1, "Blur cache test");
    auto* cached = (u8*)mlt_calloc(bytes, 1, "Blur cache test");
    auto read = [&](u8* pixels) { glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels); };
    auto matches_reference = [&]() {
        read(cached);
        r->disable_opacity_regions = true;
        r->disable_opacity_history_cache = true;
        draw(true);
        read(reference);
        r->disable_opacity_regions = false;
        r->disable_opacity_history_cache = false;
        return memcmp(reference, cached, bytes) == 0;
    };
    passed = passed && matches_reference();
    double start_ms = 0, end_ms = 0;
    for ( int i = 0; i < 3; ++i ) {
        Stroke committed = {};
        copy_stroke(&milton->canvas->arena, view, working, &committed);
        committed.id = milton->canvas->stroke_id_count++;
        layer::layer_push_stroke(layer, committed);
        reset_working_stroke(milton);
        u64 start = SDL_GetPerformanceCounter();
        draw();
        end_ms += 1000.0 * (SDL_GetPerformanceCounter() - start) / SDL_GetPerformanceFrequency();
        passed = passed && r->replay_cache_hit && r->blur_filter_runs == 0 && matches_reference();
        working->num_points = 2;
        working->bounding_rect = bounding_box_for_stroke(working);
        start = SDL_GetPerformanceCounter();
        draw();
        start_ms += 1000.0 * (SDL_GetPerformanceCounter() - start) / SDL_GetPerformanceFrequency();
        passed = passed && r->replay_cache_hit && r->blur_filter_runs == 0 && matches_reference();
    }
    fprintf(stdout, "Stroke start / commit average: %.2f / %.2f ms (no old blur passes)\n", start_ms / 3, end_ms / 3);

    read(reference);
    Stroke* hidden_blur = get(&layer->strokes, 12);
    gpu_free_strokes(hidden_blur, 1, r);
    double preview_ms = measure(true, true);
    read(cached);
    passed = passed && r->blur_filter_runs == 0 && r->opacity_stroke_runs == 0
             && get_render_element(hidden_blur->render_handle)->vbo_stroke == 0
             && r->cook_pending == 0 && gpu_needs_navigation_preview(r)
             && memcmp(reference, cached, bytes) != 0;
    working->flags = StrokeFlag_BLUR;
    u64 working_revision = get_render_element(working->render_handle)->revision;
    draw(true, true);
    passed = passed && get_render_element(working->render_handle)->revision == working_revision
             && r->blur_filter_runs == 0 && r->opacity_stroke_runs == 0;
    working->flags = StrokeFlag_PRESERVE_OPACITY;
    r->flags |= RenderBackendFlags_NAVIGATION_PREVIEW;
    draw(true);
    read(cached);
    passed = passed && r->blur_filter_runs > 0 && r->opacity_stroke_runs > 0
             && get_render_element(hidden_blur->render_handle)->vbo_stroke != 0
             && memcmp(reference, cached, bytes) == 0;
    r->flags &= ~RenderBackendFlags_NAVIGATION_PREVIEW;
    fprintf(stdout, "Navigation preview: %.2f ms; no opacity history or hidden blur uploads; full quality restored pixel-for-pixel\n", preview_ms);
    working->num_points = 0;
    draw();
    draw();
    passed = passed && r->replay_cache_hit && r->blur_filter_runs == 0 && matches_reference();
    working->num_points = 2;
    draw();
    passed = passed && r->replay_cache_hit && r->blur_filter_runs == 0;
    working->points[1].y += view->scale * 8;
    working->bounding_rect = bounding_box_for_stroke(working);
    draw();
    passed = passed && r->replay_cache_hit && r->blur_filter_runs == 0 && matches_reference();
    working->flags = StrokeFlag_BLUR;
    draw();
    passed = passed && r->replay_cache_hit && r->blur_filter_runs <= 2 && matches_reference();
    working->flags = StrokeFlag_PRESERVE_OPACITY;
    // A different working color reconstructs its history once, then reuses that snapshot.
    working->brush.color = to_premultiplied(v3f{0.24f, 0.5f, 0.75f}, 0.6f);
    draw();
    passed = passed && r->replay_cache_hit && r->blur_filter_runs > 0;
    draw();
    passed = passed && r->replay_cache_hit && r->blur_filter_runs == 0 && matches_reference();

    Stroke removed = pop(&layer->strokes);
    draw();
    passed = passed && !r->replay_cache_hit && matches_reference();
    push(&layer->strokes, removed);
    draw();
    passed = passed && r->replay_cache_hit && matches_reference();
    Stroke* changed = get(&layer->strokes, 0);
    gpu_free_strokes(changed, 1, r);
    changed->brush.color = to_premultiplied(v3f{0.8f, 0.1f, 0.2f}, 0.6f);
    draw();
    passed = passed && !r->replay_cache_hit && matches_reference();
    view->pan_center.x += 8 * view->scale;
    view->angle = 0.1f;
    view->flipped = 1;
    gpu_update_canvas(r, milton->canvas, view);
    draw();
    passed = passed && !r->replay_cache_hit && matches_reference();
    gpu_update_background(r, v3f{0.5f, 0.4f, 0.3f});
    draw();
    passed = passed && !r->replay_cache_hit && matches_reference();
    layer->flags &= ~LayerFlags_VISIBLE;
    draw();
    passed = passed && !r->replay_cache_hit && !gpu_needs_navigation_preview(r) && matches_reference();
    layer->flags |= LayerFlags_VISIBLE;
    draw();
    passed = passed && gpu_needs_navigation_preview(r) && matches_reference();
    background_alpha = 1.0f;
    draw();
    passed = passed && !r->replay_cache_hit && matches_reference();
    gpu_resize(r, view);
    draw();
    passed = passed && !r->replay_cache_hit && matches_reference();

    Layer* lower = layer;
    milton_new_layer(milton);
    layer = milton->canvas->working_layer;
    add(2, 0);
    add(2, StrokeFlag_BLUR);
    working->layer_id = layer->id;
    working->flags = StrokeFlag_ERASER;
    working->brush.alpha = 0.5f;
    draw();
    draw();
    passed = passed && r->replay_cache_hit && matches_reference();
    lower->alpha = 0.4f;
    draw();
    passed = passed && !r->replay_cache_hit && matches_reference();
    LayerEffect effect = {};
    effect.type = LayerEffectType_BLUR;
    effect.enabled = true;
    effect.blur.original_scale = (i32)view->scale;
    effect.blur.kernel_size = 3;
    lower->effects = &effect;
    draw();
    passed = passed && !r->replay_cache_hit && matches_reference();
    effect.blur.kernel_size = 5;
    draw();
    passed = passed && !r->replay_cache_hit && matches_reference();
    // Effects belong to a layer: moving the same effect must invalidate the prefix.
    lower->effects = nullptr;
    layer->effects = &effect;
    draw();
    passed = passed && !r->replay_cache_hit && matches_reference();
    layer->effects = nullptr;

    lower->flags &= ~LayerFlags_VISIBLE;
    layer->flags &= ~LayerFlags_VISIBLE;
    milton_new_layer(milton);
    layer = milton->canvas->working_layer;
    add(0, StrokeFlag_PRESERVE_OPACITY);
    add(0, StrokeFlag_PRESERVE_OPACITY);
    draw(true);
    read(reference);
    draw(true, true);
    read(cached);
    passed = passed && gpu_needs_navigation_preview(r) && r->opacity_stroke_runs == 0
             && memcmp(reference, cached, bytes) != 0;
    draw(true);
    read(cached);
    passed = passed && r->opacity_stroke_runs > 0 && memcmp(reference, cached, bytes) == 0;

    for ( int i = 0; i < 12; ++i ) {
        add(i % 3, StrokeFlag_PRESERVE_OPACITY | StrokeFlag_DISTANCE_TO_OPACITY | StrokeFlag_PRESSURE_TO_OPACITY);
        if ( i % 4 == 0 ) { add(1, StrokeFlag_BLUR); }
        if ( i % 4 == 2 ) { add(0, StrokeFlag_ERASER); }
    }
    r->disable_opacity_regions = true;
    r->disable_opacity_history_cache = true;
    double original_ms = measure(true);
    int original_masks = r->opacity_stroke_runs;
    read(reference);
    r->disable_opacity_regions = false;
    double regional_ms = measure(true);
    read(cached);
    passed = passed && memcmp(reference, cached, bytes) == 0;
    r->disable_opacity_history_cache = false;
    double optimized_ms = measure(true);
    int optimized_masks = r->opacity_stroke_runs;
    read(cached);
    passed = passed && r->opacity_history_cache_hits > 0 && optimized_masks < original_masks
             && memcmp(reference, cached, bytes) == 0;
    fprintf(stdout, "Opacity/blur replay original/regional/cached histories: %.2f / %.2f / %.2f ms; masks %d / %d; history cache hits %d\n",
            original_ms, regional_ms, optimized_ms, original_masks, optimized_masks, r->opacity_history_cache_hits);
    for ( int i = 0; i < 6; ++i ) { add(i, StrokeFlag_PRESERVE_OPACITY); }
    add(0, StrokeFlag_PRESERVE_OPACITY);
    draw(true);
    passed = passed && matches_reference();
    u64 allocated_history_bytes = 0;
    for ( GLuint texture : r->opacity_history_cache_textures ) {
        if ( texture ) { allocated_history_bytes += (u64)width * height * 4; }
    }
    passed = passed && allocated_history_bytes <= OPACITY_HISTORY_CACHE_BYTES;
    gpu_resize(r, view);
    for ( GLuint texture : r->opacity_history_cache_textures ) { passed = passed && texture == 0; }
    draw(true);
    passed = passed && matches_reference();
    milton_new_layer(milton);
    layer = milton->canvas->working_layer;
    add(1, StrokeFlag_PRESERVE_OPACITY);
    add(0, StrokeFlag_PRESERVE_OPACITY);
    add(2, StrokeFlag_PRESERVE_OPACITY);
    draw(true);
    passed = passed && matches_reference();

    view->screen_size = {3200, 900};
    view->zoom_center = view->screen_size / 2;
    gpu_resize(r, view);
    gpu_update_canvas(r, milton->canvas, view);
    glViewport(0, 0, 3200, 900);
    gpu_clip_strokes_and_update(&milton->root_arena, r, view, view->scale, milton->canvas->root_layer,
                                working, 0, 0, 3200, 900);
    gpu_render_canvas(r, 0, 0, 3200, 900, background_alpha);
    allocated_history_bytes = 0;
    for ( GLuint texture : r->opacity_history_cache_textures ) {
        if ( texture ) { allocated_history_bytes += 3200ULL * 900 * 4; }
    }
    passed = passed && allocated_history_bytes > 0 && allocated_history_bytes <= OPACITY_HISTORY_CACHE_BYTES;

    mlt_free(reference, "Blur cache test");
    mlt_free(cached, "Blur cache test");
    passed = passed && glGetError() == GL_NO_ERROR;
    fprintf(passed ? stdout : stderr, "Blur cache pixels, working pen/eraser, undo/redo, edits, view, background, layers and effects: %s\n",
            passed ? "passed" : "FAILED");
    return passed;
}

static bool
test_preserve_opacity(bool blur_tests = false, bool benchmark = false)
{
    if ( SDL_Init(SDL_INIT_VIDEO) != 0 ) {
        fprintf(stderr, "Cannot initialize opacity test video: %s\n", SDL_GetError());
        return false;
    }
#if USE_GL_3_2
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#endif
    const int size = 320;
    SDL_Window* window = SDL_CreateWindow("Opacity test", 0, 0, size, size, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    SDL_GLContext context = window ? SDL_GL_CreateContext(window) : NULL;
    if ( !context || !gl::load() ) {
        fprintf(stderr, "Cannot initialize opacity test OpenGL: %s\n", SDL_GetError());
        if ( context ) { SDL_GL_DeleteContext(context); }
        if ( window ) { SDL_DestroyWindow(window); }
        SDL_Quit();
        return false;
    }
    Milton milton = {};
    milton_init(&milton, size, size, 1, NULL, MiltonInit_FOR_TEST);
    milton_reset_canvas_and_set_default(&milton);
    auto* r = milton.renderer;
    CanvasView* view = milton.view;
    view->screen_size = {size, size};
    gui_anchor_picker_top_right(milton.gui, size);
    if ( !gpu_init(r, view, &milton.gui->picker) ) {
        fprintf(stderr, "Cannot initialize opacity test renderer\n");
        SDL_GL_DeleteContext(context);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return false;
    }
    gpu_resize(r, view);
    glViewport(0, 0, size, size);
    gpu_update_canvas(r, milton.canvas, view);
    if ( benchmark ) {
        bool passed = test_blur_performance(&milton);
        gpu_free_strokes(r, milton.canvas);
        gpu_release_data(r);
        arena_free(&milton.root_arena);
        SDL_GL_DeleteContext(context);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return passed;
    }
    Layer* layer = milton.canvas->working_layer;
    v2l line[] = {raster_to_canvas(view, v2l{100, 160}), raster_to_canvas(view, v2l{220, 160})};
    v2l offscreen[] = {raster_to_canvas(view, v2l{400, 160}), raster_to_canvas(view, v2l{460, 160})};
    f32 pressure[] = {1, 1};
    f32 low_pressure[] = {0.5f, 0.5f};
    auto add = [&](u32 flags, f32 alpha, v2l* points, f32* pressures,
                   v3f color = v3f{0.25f, 0.5f, 0.75f}, f32 hardness = k_max_hardness,
                   i32 shape = BrushShape_ROUND) {
        Stroke stroke = {};
        stroke.id = milton.canvas->stroke_id_count++;
        stroke.layer_id = layer->id;
        stroke.flags = flags;
        stroke.points = points;
        stroke.pressures = pressures;
        stroke.num_points = 2;
        stroke.brush = default_brush();
        stroke.brush.radius = 16 * view->scale;
        stroke.brush.alpha = alpha;
        stroke.brush.pressure_opacity_min = 0.0f;
        stroke.brush.color = to_premultiplied(color, alpha);
        stroke.brush.hardness = hardness;
        stroke.brush.shape = shape;
        stroke.bounding_rect = bounding_box_for_stroke(&stroke);
        return layer::layer_push_stroke(layer, stroke);
    };
    u8 pixel[4] = {};
    auto alpha_at_center = [&](float background = 0.0f, int offset_y = 0) {
        gpu_clip_strokes_and_update(&milton.root_arena, r, view, view->scale, milton.canvas->root_layer,
                                    &milton.working_stroke, 0, 0, size, size);
        gpu_render_canvas(r, 0, 0, size, size, background);
        glFramebufferTexture2DEXT(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->canvas_texture, 0);
        glReadPixels(size / 2, size / 2 - offset_y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        return (int)pixel[3];
    };
    if ( blur_tests ) {
        auto sample = [&](int offset_y) {
            return alpha_at_center(0.0f, offset_y);
        };
        auto add_blur = [&](float strength, u32 flags = 0, f32* pressures = nullptr) {
            Stroke* stroke = add(StrokeFlag_BLUR | flags, strength, line, pressures ? pressures : pressure);
            stroke->brush.radius = 64 * view->scale;
            stroke->brush.pressure_size = 0;
            stroke->bounding_rect = bounding_box_for_stroke(stroke);
        };
        add(0, 1.0f, line, pressure, v3f{1, 0, 0});
        int outside = sample(20);
        int center = sample(0);
        add_blur(0.0f);
        bool passed = sample(20) == outside && sample(0) == center;
        pop(&layer->strokes);
        add_blur(1.0f, StrokeFlag_PRESSURE_TO_OPACITY, low_pressure);
        int weak = sample(20);
        pop(&layer->strokes);
        add_blur(1.0f);
        int full = sample(20);
        passed = passed && outside == 0 && weak > outside && full > weak
                 && abs(pixel[0] - pixel[3]) <= 1 && pixel[1] == 0 && pixel[2] == 0
                 && sample(0) < center && sample(80) == 0;
        Stroke undone = pop(&layer->strokes);
        passed = passed && sample(20) == outside;
        milton.current_mode = MiltonMode::BLUR;
        milton.brushes[BrushEnum_BLUR].alpha = 0.75f;
        Brush ramped = milton_get_brush(&milton);
        add_blur(ramped.alpha);
        int at_75_percent = sample(20);
        passed = passed && abs(at_75_percent - full) <= 2;
        pop(&layer->strokes);
        fprintf(passed ? stdout : stderr, "Blur strength: new 75%% / legacy 100%% edge alpha %d / %d\n",
                at_75_percent, full);
        push(&layer->strokes, undone);
        passed = passed && sample(20) == full;
        add_blur(1.0f);
        int repeated = sample(20);
        passed = passed && repeated > full;
        pop(&layer->strokes);
        i64 original_scale = view->scale;
        view->scale /= 2;
        gpu_update_canvas(r, milton.canvas, view);
        int zoomed = sample(40);
        passed = passed && abs(zoomed - full) <= 8;
        view->scale = (i32)original_scale;
        gpu_update_canvas(r, milton.canvas, view);

        // Reconstruct opacity history through blur, not through its white mask color.
        add(StrokeFlag_PRESERVE_OPACITY, 0.1f, line, pressure, v3f{1, 0, 0});
        int protected_center = sample(0);
        passed = passed && protected_center < center;
        pop(&layer->strokes);
        Stroke blur = pop(&layer->strokes);
        add(StrokeFlag_PRESERVE_OPACITY, 1.0f, line, pressure, v3f{1, 0, 0});
        push(&layer->strokes, blur);
        passed = passed && abs(sample(20) - full) <= 1;
        // Drop the extra preserving stroke but retain blur for the save/load regression.
        pop(&layer->strokes);
        pop(&layer->strokes);
        push(&layer->strokes, blur);
        milton.persist->mlt_file_path = TO_PATH_STR("TEST_blur.mlt");
        milton_save(&milton);
        milton_load(&milton);
        layer = milton.canvas->working_layer;
        gpu_update_canvas(r, milton.canvas, view);
        passed = passed && sample(20) == full
                 && (get(&layer->strokes, layer->strokes.count - 1)->flags & StrokeFlag_BLUR);

        // A blur on an empty upper layer must not sample the paint below.
        milton_new_layer(&milton);
        layer = milton.canvas->working_layer;
        add_blur(1.0f);
        passed = passed && sample(20) == full;
        GLenum error = glGetError();
        passed = passed && error == GL_NO_ERROR;
        fprintf(passed ? stdout : stderr,
                "Blur: base/half/full/repeated edge alpha %d/%d/%d/%d; replay, pressure, transparent red, layers, opacity history: %s (GL 0x%x)\n",
                outside, weak, full, repeated, passed ? "passed" : "FAILED", error);
        gpu_free_strokes(r, milton.canvas);
        gpu_release_data(r);
        arena_free(&milton.root_arena);
        SDL_GL_DeleteContext(context);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return passed;
    }

    constexpr u32 preserve = StrokeFlag_PRESERVE_OPACITY;
    // The base stroke predates Preserve opacity and carries no new flags.
    add(0, 0.5f, line, pressure);
    int single = alpha_at_center();
    add(preserve, 0.5f, line, pressure);
    int repeated = alpha_at_center();
    add(preserve | StrokeFlag_PRESSURE_TO_OPACITY, 1.0f, line, low_pressure);
    int same_pressure_opacity = alpha_at_center();
    add(preserve | StrokeFlag_PRESSURE_TO_OPACITY, 0.75f, line, pressure);
    int stronger = alpha_at_center();
    add(StrokeFlag_ERASER, 1.0f, offscreen, pressure);
    add(preserve, 0.5f, line, pressure);
    int separated = alpha_at_center();
    add(0, 0.5f, line, pressure);
    int ordinary = alpha_at_center();
    bool passed = abs(single - 128) <= 1 && abs(repeated - single) <= 1
                  && abs(same_pressure_opacity - single) <= 1 && abs(stronger - 191) <= 1
                  && abs(separated - stronger) <= 1 && abs(ordinary - 223) <= 1;
    fprintf(passed ? stdout : stderr, "Opacity readback (single, repeat, pressure, stronger, separated, ordinary): %d %d %d %d %d %d\n",
            single, repeated, same_pressure_opacity, stronger, separated, ordinary);

    // Returning to a color after another preserving color rebuilds its history.
    add(preserve, 0.4f, line, pressure, v3f{0.75f, 0.25f, 0.5f});
    int with_other_color = alpha_at_center();
    u8 other_color_pixel[4];
    memcpy(other_color_pixel, pixel, sizeof(pixel));
    add(preserve, 0.5f, line, pressure);
    int returned_color = alpha_at_center();
    passed = passed && abs(returned_color - with_other_color) <= 1
             && memcmp(other_color_pixel, pixel, sizeof(pixel)) == 0;

    auto clear_strokes = [&]() {
        gpu_free_strokes(r, milton.canvas);
        gpu_release_data(r);
        reset(&layer->strokes);
    };
    // Soft and square strokes must protect their actual per-pixel coverage, not master alpha.
    clear_strokes();
    add(StrokeFlag_DISTANCE_TO_OPACITY, 0.5f, line, pressure, v3f{0.25f, 0.5f, 0.75f}, 2.0f, BrushShape_RECTANGLE);
    int soft = alpha_at_center(0.0f, 3);
    add(preserve | StrokeFlag_DISTANCE_TO_OPACITY, 0.5f, line, pressure, v3f{0.25f, 0.5f, 0.75f}, 2.0f, BrushShape_RECTANGLE);
    int soft_repeat = alpha_at_center(0.0f, 3);
    passed = passed && soft > 0 && abs(soft_repeat - soft) <= 1;

    // Erasing matching paint must lower its remembered opacity before a fresh pass.
    clear_strokes();
    add(0, 0.5f, line, pressure);
    add(StrokeFlag_ERASER, 0.5f, line, pressure);
    add(preserve, 0.5f, line, pressure);
    int after_erasing = alpha_at_center();
    passed = passed && after_erasing > single && after_erasing < ordinary;
    alpha_at_center(1.0f);
    // On white, the old 50% pass, half erasing, and preserving repaint must return to 50% color.
    passed = passed && abs(pixel[0] - 159) <= 2 && abs(pixel[1] - 191) <= 2 && abs(pixel[2] - 223) <= 2;

    clear_strokes();
    add(0, 0.5f, line, pressure);
    add(0, 0.5f, line, pressure);
    int old_overlap = alpha_at_center();
    add(preserve, 0.5f, line, pressure);
    passed = passed && abs(old_overlap - 191) <= 1 && abs(alpha_at_center() - old_overlap) <= 1;

    // Check true save/load: a legacy stroke remains protected after GPU handles are rebuilt.
    clear_strokes();
    add(0, 0.5f, line, pressure);
    milton.persist->mlt_file_path = TO_PATH_STR("TEST_preserve_opacity.mlt");
    milton_save(&milton);
    milton_load(&milton);
    layer = milton.canvas->working_layer;
    gpu_update_canvas(r, milton.canvas, view);
    int reopened = alpha_at_center();
    add(preserve, 0.5f, line, pressure);
    int reopened_repeat = alpha_at_center();
    passed = passed && reopened > 0 && abs(reopened_repeat - reopened) <= 1;
    fprintf(passed ? stdout : stderr,
            "Legacy/other-color/soft/erased/reopened opacity: %d %d / %d %d / %d / %d %d (%s)\n",
            with_other_color, returned_color, soft, soft_repeat, after_erasing,
            reopened, reopened_repeat, passed ? "passed" : "FAILED");

    milton_new_layer(&milton);
    layer = milton.canvas->working_layer;
    add(preserve, 0.5f, line, pressure);
    int other_layer = alpha_at_center();
    Stroke undone = pop(&layer->strokes);
    int undo_alpha = alpha_at_center();
    push(&layer->strokes, undone);
    int redo_alpha = alpha_at_center();
    bool layer_ok = abs(other_layer - 191) <= 1 && abs(undo_alpha - reopened) <= 1
                    && abs(redo_alpha - other_layer) <= 1;
    passed = passed && layer_ok;
    fprintf(layer_ok ? stdout : stderr, "Separate layer, undo, redo opacity: %d %d %d (%s)\n",
            other_layer, undo_alpha, redo_alpha, layer_ok ? "passed" : "FAILED");
    GLenum error = glGetError();
    if ( error != GL_NO_ERROR ) {
        fprintf(stderr, "OpenGL error after opacity history rendering: 0x%x\n", error);
        passed = false;
    }
    gpu_free_strokes(r, milton.canvas);
    arena_free(&milton.root_arena);
    SDL_GL_DeleteContext(context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return passed;
}

static bool
test_tool_pressure_persistence()
{
    Milton source = {}, restored = {};
    milton_init(&source, 320, 320, 1, NULL, MiltonInit_FOR_TEST);
    milton_init(&restored, 320, 320, 1, NULL, MiltonInit_FOR_TEST);
    bool passed = true;
    for ( int pen = 0; pen <= 1; ++pen ) {
        for ( int eraser = 0; eraser <= 1; ++eraser ) {
            source.flags_in_eraser_mode = true;
            source.pen_pressure_opacity_stash = pen != 0;
            source.eraser_pressure_opacity = eraser != 0;
            source.working_stroke.flags = StrokeFlag_ERASER | (eraser ? StrokeFlag_PRESSURE_TO_OPACITY : 0);
            BrushPreset saved[2] = {};
            preset_capture(&source, &saved[0], BrushEnum_PEN);
            preset_capture(&source, &saved[1], BrushEnum_ERASER);
            passed = passed && saved[0].pressure_opacity == pen && saved[1].pressure_opacity == eraser;

            restored.flags_in_eraser_mode = false;
            restored.working_stroke.flags = 0;
            preset_apply(&restored, &saved[0]);
            preset_apply(&restored, &saved[1]);
            passed = passed && ((restored.working_stroke.flags & StrokeFlag_PRESSURE_TO_OPACITY) != 0) == (pen != 0)
                     && restored.eraser_pressure_opacity == (eraser != 0);

            // Applying a pen preset while erasing must update the pen stash, not the eraser flag.
            restored.flags_in_eraser_mode = true;
            restored.pen_pressure_opacity_stash = pen == 0;
            restored.working_stroke.flags = StrokeFlag_ERASER | (eraser ? StrokeFlag_PRESSURE_TO_OPACITY : 0);
            preset_apply(&restored, &saved[0]);
            passed = passed && restored.pen_pressure_opacity_stash == (pen != 0)
                     && ((restored.working_stroke.flags & StrokeFlag_PRESSURE_TO_OPACITY) != 0) == (eraser != 0);
        }
    }
    source.flags_in_eraser_mode = false;
    source.flags_in_blur_mode = true;
    source.pen_pressure_opacity_stash = true;
    source.blur_pressure_opacity = false;
    source.working_stroke.flags = StrokeFlag_BLUR;
    source.brush_sizes[BrushEnum_BLUR] = 73;
    source.brushes[BrushEnum_BLUR].alpha = 0.37f;
    BrushPreset pen_options = {}, blur_options = {};
    preset_capture(&source, &pen_options, BrushEnum_PEN);
    preset_capture(&source, &blur_options, BrushEnum_BLUR);
    restored.flags_in_eraser_mode = false;
    restored.flags_in_blur_mode = false;
    preset_apply(&restored, &pen_options);
    preset_apply(&restored, &blur_options);
    passed = passed && pen_options.pressure_opacity && !blur_options.pressure_opacity
             && !restored.blur_pressure_opacity && restored.brush_sizes[BrushEnum_BLUR] == 73
             && restored.brushes[BrushEnum_BLUR].alpha == 0.37f
             && (restored.working_stroke.flags & StrokeFlag_PRESSURE_TO_OPACITY);
    BrushPreset records[3] = {pen_options, {}, blur_options};
    preset_capture(&source, &records[1], BrushEnum_ERASER);
    for ( int count = 2; count <= 3; ++count ) {
        FILE* file = tmpfile();
        if ( !file ) { passed = false; break; }
        u32 size = sizeof(BrushPreset);
        bool written = fwrite(&size, sizeof(size), 1, file) == 1
                       && fwrite(records, sizeof(BrushPreset), count, file) == count;
        rewind(file);
        restored.brush_sizes[BrushEnum_BLUR] = 19;
        passed = passed && written && read_tool_state(&restored, file)
                 && restored.brush_sizes[BrushEnum_BLUR] == (count == 3 ? 73 : 19)
                 && (restored.working_stroke.flags & StrokeFlag_PRESSURE_TO_OPACITY);
        fclose(file);
    }

    milton_reset_canvas_and_set_default(&source);
    source.persist->mlt_file_path = TO_PATH_STR("TEST_tool_pressure.mlt");
    restored.persist->mlt_file_path = source.persist->mlt_file_path;
    Stroke stroke = {};
    stroke.brush = default_brush();
    stroke.brush.radius = source.view->scale * 10;
    stroke.num_points = 2;
    stroke.points = arena_alloc_array(&source.canvas->arena, 2, v2l);
    stroke.pressures = arena_alloc_array(&source.canvas->arena, 2, f32);
    stroke.points[0] = {0, 0};
    stroke.points[1] = {1000, 1000};
    stroke.pressures[0] = stroke.pressures[1] = 1.0f;
    stroke.layer_id = source.canvas->working_layer->id;
    stroke.id = source.canvas->stroke_id_count++;
    stroke.bounding_rect = bounding_box_for_stroke(&stroke);
    Stroke* saved_stroke = layer::layer_push_stroke(source.canvas->working_layer, stroke);
    for ( int enabled = 0; enabled <= 1; ++enabled ) {
        saved_stroke->flags = enabled ? StrokeFlag_ERASER : StrokeFlag_PRESSURE_TO_OPACITY;
        milton_save(&source);
        restored.flags_in_eraser_mode = false;
        restored.working_stroke.flags = enabled ? StrokeFlag_PRESSURE_TO_OPACITY : 0;
        milton_load(&restored);
        passed = passed && ((restored.working_stroke.flags & StrokeFlag_PRESSURE_TO_OPACITY) != 0) == (enabled != 0);
    }
    arena_free(&source.root_arena);
    arena_free(&restored.root_arena);
    fprintf(passed ? stdout : stderr, "Pen/eraser/blur pressure options, legacy/current tool files, and canvas loading: %s\n", passed ? "passed" : "FAILED");
    return passed;
}

static bool
test_preserve_opacity_binding()
{
    MiltonSettings saved = {};
    settings_init(&saved);
    set_default_bindings(&saved.bindings);
    saved.preserve_opacity = 1;
    saved.position_smoothing = 0.73f;
    binding_with_release(&saved.bindings, Modifier_ALT, 'p', Action_PEEK_OUT, ActionRelease_PEEK_OUT);

    auto round_trip = [](const MiltonSettings& source, int missing, MiltonSettings* loaded) {
        FILE* file = tmpfile();
        if ( !file ) { return false; }
        bool ok = false;
        if ( missing ) {
            MiltonSettings fixture = source;
            for ( int i = ActionRelease_PEEK_OUT; i < Action_COUNT_WITH_RELEASE; ++i ) {
                fixture.bindings.bindings[i].action = (BindableAction)(i - missing);
            }
            const int first = missing == 2 ? Action_TOGGLE_PRESERVE_OPACITY : Action_MODE_BLUR;
            const size_t offset = offsetof(MiltonSettings, bindings) + first * sizeof(Binding);
            const u16 size = (u16)(sizeof(fixture) - missing * sizeof(Binding));
            const auto* bytes = reinterpret_cast<const u8*>(&fixture);
            ok = fwrite(&size, sizeof(size), 1, file) == 1
                 && fwrite(bytes, 1, offset, file) == offset
                 && fwrite(bytes + offset + missing * sizeof(Binding), 1, size - offset, file) == size - offset;
        } else {
            const u16 size = sizeof(source);
            ok = fwrite(&size, sizeof(size), 1, file) == 1 && fwrite(&source, size, 1, file) == 1;
        }
        rewind(file);
        ok = ok && read_milton_settings(loaded, file);
        fclose(file);
        return ok;
    };

    MiltonSettings loaded = {};
    bool passed = round_trip(saved, 2, &loaded);
    const Binding& toggle = loaded.bindings.bindings[Action_TOGGLE_PRESERVE_OPACITY];
    const Binding& release = loaded.bindings.bindings[ActionRelease_PEEK_OUT];
    passed = passed && toggle.bound_key == 'a' && !toggle.accepts_repeats
             && release.action == ActionRelease_PEEK_OUT && release.modifiers == Modifier_ALT
             && release.bound_key == 'p' && release.on_release
             && loaded.preserve_opacity && loaded.position_smoothing == saved.position_smoothing;
    binding(&saved.bindings, Modifier_NONE, 'a', Action_MODE_PEN);
    passed = passed && round_trip(saved, 2, &loaded) && toggle.bound_key == Binding::UNBOUND
             && loaded.bindings.bindings[Action_MODE_PEN].bound_key == 'a';
    binding(&saved.bindings, Modifier_CTRL, 'k', Action_TOGGLE_PRESERVE_OPACITY);
    passed = passed && round_trip(saved, false, &loaded) && toggle.bound_key == 'k'
             && toggle.modifiers == Modifier_CTRL && toggle.action == Action_TOGGLE_PRESERVE_OPACITY;
    passed = passed && round_trip(saved, 1, &loaded) && toggle.bound_key == 'k'
             && loaded.bindings.bindings[Action_MODE_BLUR].bound_key == 'f'
             && release.action == ActionRelease_PEEK_OUT
             && loaded.preserve_opacity && loaded.position_smoothing == saved.position_smoothing;
    binding(&saved.bindings, Modifier_NONE, 'f', Action_MODE_PEN);
    passed = passed && round_trip(saved, 1, &loaded)
             && loaded.bindings.bindings[Action_MODE_BLUR].bound_key == Binding::UNBOUND;
    binding(&saved.bindings, Modifier_NONE, 'g', Action_MODE_BLUR);
    passed = passed && round_trip(saved, 0, &loaded)
             && loaded.bindings.bindings[Action_MODE_BLUR].bound_key == 'g';
    MiltonInput input = {};
    binding_dispatch_action(Action_MODE_BLUR, &input, nullptr, {});
    passed = passed && input.mode_to_set == MiltonMode::BLUR && mode_is_for_drawing(input.mode_to_set);
    if ( !passed ) { fprintf(stderr, "Preserve opacity shortcut migration or rebinding failed\n"); }
    return passed;
}

#if defined(_WIN32)
static bool
test_vram_stats()
{
    if ( SDL_Init(SDL_INIT_VIDEO) != 0 ) {
        fprintf(stderr, "Cannot initialize VRAM test video: %s\n", SDL_GetError());
        return false;
    }
    SDL_Window* window = SDL_CreateWindow("VRAM stats test", 0, 0, 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    SDL_GLContext context = window ? SDL_GL_CreateContext(window) : NULL;
    if ( !context || !gl::load() ) {
        fprintf(stderr, "Cannot initialize VRAM test OpenGL: %s\n", SDL_GetError());
        if ( context ) { SDL_GL_DeleteContext(context); }
        if ( window ) { SDL_DestroyWindow(window); }
        SDL_Quit();
        return false;
    }
    GLuint texture = gl::new_color_texture(2048, 2048);
    GLuint framebuffer = gl::new_fbo(texture, 0, GL_TEXTURE_2D);
    glBindFramebufferEXT(GL_FRAMEBUFFER, framebuffer);
    glClearColor(0.25f, 0.5f, 0.75f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    u8 pixel[4];
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    SystemStats stats = {};
    platform_system_stats(&stats);
    SDL_Delay(1100);
    platform_system_stats(&stats);
    bool passed = stats.vram_total_mb > 0 && stats.vram_system_used_mb >= 0
                  && stats.vram_system_used_mb <= stats.vram_total_mb && stats.vram_used_mb > 0
                  && glGetError() == GL_NO_ERROR;
    fprintf(passed ? stdout : stderr, "VRAM system %.0f / %.0f MB, Milton %.0f MB: %s\n",
            stats.vram_system_used_mb, stats.vram_total_mb, stats.vram_used_mb, passed ? "passed" : "FAILED");
    glDeleteTextures(1, &texture);
    SDL_GL_DeleteContext(context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return passed;
}
#endif

#include "tile_tests.cc"
#include "canvas_profile_tests.cc"

#if MILTON_HARDWARE_BRUSH_CURSOR
static bool
test_brush_cursor()
{
    if ( SDL_Init(SDL_INIT_VIDEO) != 0 ) {
        fprintf(stderr, "Native cursor SDL initialization failed: %s\n", SDL_GetError());
        return false;
    }
    bool passed = true;
    auto check = [&](bool condition, const char* message) {
        if ( !condition ) { fprintf(stderr, "Native cursor FAILED: %s\n", message); passed = false; }
    };
    auto alpha = [](SDL_Surface* surface, int dx, int dy) {
        int x = surface->w / 2 + dx, y = surface->h / 2 + dy;
        u32 pixel = ((u32*)((u8*)surface->pixels + y * surface->pitch))[x];
        u8 r, g, b, a;
        SDL_GetRGBA(pixel, surface->format, &r, &g, &b, &a);
        return a;
    };
    SDL_Surface* circle = brush_cursor_surface(12, 0, 1, 0);
    check(circle != NULL, "round outline must allocate");
    if ( circle ) {
        check(alpha(circle, 12, 0) == 255 && alpha(circle, 0, 12) == 255
              && alpha(circle, 0, 0) == 0 && alpha(circle, 14, 0) == 0,
              "round outline must surround a transparent center at the brush radius");
        SDL_FreeSurface(circle);
    }
    for ( int angle = 0; angle <= 90; angle += 90 ) {
        SDL_Surface* rectangle = brush_cursor_surface(12, 1, 0.25f, (float)angle);
        check(rectangle != NULL, "rectangular outline must allocate");
        if ( rectangle ) {
            check((angle == 0
                       ? alpha(rectangle, 0, 3) == 255 && alpha(rectangle, 0, 8) == 0
                       : alpha(rectangle, 3, 0) == 255 && alpha(rectangle, 8, 0) == 0),
                  "rectangular outline must retain aspect and rotation");
            SDL_FreeSurface(rectangle);
        }
    }
    PlatformState platform = {};
    platform.cursor_brush = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_CROSSHAIR);
    check(platform.cursor_brush != NULL, "fallback tip must be available");
    if ( platform.cursor_brush ) {
        platform_update_brush_cursor(&platform, 24, 1, 0.5f, 35);
        SDL_Cursor* outline = platform.cursor_brush_outline;
        check(outline != NULL, "shaped outline must create a native cursor");
        platform.pointer = {140, 180};
        platform_update_brush_cursor(&platform, 24, 1, 0.5f, 35);
        check(platform.cursor_brush_outline == outline, "pointer movement must not rebuild cursor resources");
        platform_update_brush_cursor(&platform, 1000, 1, 0.5f, 35);
        check(!platform.cursor_brush_outline, "oversized brushes must retain the native tip and canvas footprint");
        platform_update_brush_cursor(&platform, 12, 0, 1, 0);
        check(platform.cursor_brush_outline != NULL, "returning to a normal brush must restore the native outline");
        if ( platform.cursor_brush_outline ) { SDL_FreeCursor(platform.cursor_brush_outline); }
        SDL_FreeCursor(platform.cursor_brush);
    }
    fprintf(passed ? stdout : stderr, "Native brush cursor shape, rotation, caching and oversized fallback: %s\n",
            passed ? "passed" : "FAILED");
    SDL_Quit();
    return passed;
}
#endif

extern "C" int
main(int argc, char** argv)
{
    if ( argc == 3 && strcmp(argv[1], "--profile-canvas") == 0 ) {
        return test_profile_canvas(argv[2]) ? 0 : 1;
    }
#if MILTON_HARDWARE_BRUSH_CURSOR
    if ( argc == 2 && strcmp(argv[1], "--brush-cursor") == 0 ) {
        return test_brush_cursor() ? 0 : 1;
    }
#endif
    if ( argc == 2 && strcmp(argv[1], "--tile-cache") == 0 ) {
        return test_tile_cache() ? 0 : 1;
    }
#if defined(_WIN32)
    if ( argc == 2 && strcmp(argv[1], "--vram-stats") == 0 ) {
        return test_vram_stats() ? 0 : 1;
    }
#endif
    if ( argc == 2 && strcmp(argv[1], "--blur-performance") == 0 ) {
        return test_navigation_preview() && test_input_priority() && test_brush_resize_priority()
               && test_preserve_opacity(false, true) ? 0 : 1;
    }
    if ( argc == 2 && strcmp(argv[1], "--blur") == 0 ) {
        bool ui = test_blur_tool_ui();
        bool gpu = test_preserve_opacity(true);
        return ui && gpu ? 0 : 1;
    }
    if ( argc == 2 && strcmp(argv[1], "--tool-pressure") == 0 ) {
        return test_tool_pressure_persistence() ? 0 : 1;
    }
    if ( argc == 2 && strcmp(argv[1], "--preserve-opacity-binding") == 0 ) {
        return test_preserve_opacity_binding() ? 0 : 1;
    }
    if ( argc == 2 && strcmp(argv[1], "--preserve-opacity") == 0 ) {
        return test_preserve_opacity() ? 0 : 1;
    }
#if defined(_WIN32)
    if ( !test_windows_ink() ) { return 1; }
#endif
    if ( !test_picker_slider_input() ) { return 1; }
    return test_save_load() ? 0 : 1;
}
