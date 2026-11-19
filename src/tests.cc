#undef main // SDL does things we don't want

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

extern "C" int
main()
{
#if defined(_WIN32)
    if ( !test_windows_ink() ) { return 1; }
#endif
    if ( !test_picker_slider_input() ) { return 1; }
    return test_save_load() ? 0 : 1;
}
