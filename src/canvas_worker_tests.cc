static bool
test_canvas_worker(Milton* milton, SDL_Window* window)
{
    RenderBackend* r = milton->renderer;
    CanvasView* view = milton->view;
    CanvasView saved_view = *view;
    RenderSettings saved_settings = milton->render_settings;
    PlatformState* saved_platform = milton->platform;
    u32 saved_flags = milton->flags;
    bool saved_visible = milton->mode_gui_visibility[Milton::GuiVisibleCategory_DRAWING];
    bool saved_transitions = r->disable_display_transitions;
    int saved_render_flags = r->flags;
    SDL_GLContext foreground = SDL_GL_GetCurrentContext();
    size_t capacity = (size_t)r->width * r->height * 4;
    u8* actual = (u8*)calloc(capacity, 1);
    u8* expected = (u8*)calloc(capacity, 1);
    if ( !actual || !expected ) { milton_die_gracefully("Cannot allocate worker test pixels."); }
    bool passed = true;
    auto check = [&](bool condition, const char* message) {
        if ( !condition ) { fprintf(stderr, "Canvas worker FAILED: %s\n", message); passed = false; }
    };
    auto clip = [&](ClipFlags flags) {
        gpu_clip_strokes_and_update(&milton->root_arena, r, view, view->scale, milton->canvas->root_layer,
                                    &milton->working_stroke, 0, 0, r->width, r->height, flags);
    };
    auto frame = [&]() {
        clip(ClipFlags_TILE_SCENE);
        gpu_render(r, 0, 0, r->width, r->height);
    };
    auto read = [&](u8* pixels) {
        glBindFramebufferEXT(GL_FRAMEBUFFER, r->fbo);
        glFramebufferTexture2DEXT(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->canvas_texture, 0);
        glReadPixels(0, 0, r->width, r->height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    };
    auto wait = [&](SDL_atomic_t* value, int desired) {
        u32 began = SDL_GetTicks();
        while ( SDL_AtomicGet(value) != desired && (u32)(SDL_GetTicks() - began) < 10000 ) {
            SDL_PumpEvents();
            SDL_Delay(1);
        }
        return SDL_AtomicGet(value) == desired;
    };
    auto compare = [&](bool preview, const char* label) {
        read(actual);
        if ( preview ) {
            clip(ClipFlags_NAVIGATION_PREVIEW);
            gpu_render_canvas(r, 0, 0, r->width, r->height, 1, true);
            read(expected);
        } else {
            CanvasWorker* worker = r->canvas_worker;
            CanvasTileCache* presentation = r->tiles;
            r->canvas_worker = NULL;
            r->tiles = NULL;
            clip(ClipFlags_TILE_SCENE);
            check(gpu_render_tile_cache(r, 0, 0, r->width, r->height), "synchronous reference must render");
            read(expected);
            gpu_clear_tile_cache(r);
            r->tiles = presentation;
            r->canvas_worker = worker;
        }
        int worst = 0;
        for ( size_t i = 0; i < (size_t)r->width * r->height * 4; ++i ) {
            worst = max(worst, abs((int)actual[i] - (int)expected[i]));
        }
        fprintf(stdout, "Canvas worker %s: worst difference %d LSB\n", label, worst);
        check(worst <= (preview ? 0 : 2), label);
    };
    const size_t export_bytes = 192 * 192 * 4;
    u8 export_before[export_bytes], export_after[export_bytes];
    gpu_render_to_buffer(milton, export_before, 2, 224, 280, 96, 96, 1);
    bool started = gpu_start_canvas_worker(r, window);
    check(started, "shared-context renderer must start");
    if ( started ) {
        CanvasWorker* worker = r->canvas_worker;
        r->disable_display_transitions = true;
        r->flags = RenderBackendFlags_NONE;
        SDL_AtomicSet(&worker->hold, 1);
        frame();
        int first = worker->requested_generation;
        check(wait(&worker->entered, first), "worker must receive its first snapshot");
        check(SDL_AtomicGet(&worker->render_thread) != (int)SDL_ThreadID(),
              "reconstruction must execute on another thread");
        Stroke* source = get(&milton->canvas->root_layer->strokes, 0);
        v2l original_point = source->points[0];
        Rect original_bounds = source->bounding_rect;
        CanvasSnapshotStroke* frozen = worker->scene->layers[0].strokes[0];
        source->points[0].x += view->scale;
        check(frozen->data.points != source->points && frozen->data.points[0] == original_point
              && frozen->data.render_handle == 0,
              "worker samples and render handles must not alias mutable UI state");
        source->points[0] = original_point;

        ImGuiContext* previous_imgui = ImGui::GetCurrentContext();
        ImGuiContext* input_imgui = ImGui::CreateContext();
        ImGui::SetCurrentContext(input_imgui);
        PlatformState platform = {};
        platform.width = r->width; platform.height = r->height;
        platform.window_id = SDL_GetWindowID(window);
        milton->platform = &platform;
        milton->flags |= MiltonStateFlags_AUTOSAVE_BLOCKED;
        milton->mode_gui_visibility[Milton::GuiVisibleCategory_DRAWING] = false;
        SDL_Event key = {};
        key.type = SDL_KEYDOWN; key.key.windowID = platform.window_id;
        key.key.keysym.sym = SDLK_SPACE; key.key.keysym.scancode = SDL_SCANCODE_SPACE;
        check(SDL_PushEvent(&key) == 1, "must queue navigation key while worker is blocked");
        SDL_Event mouse = {};
        mouse.type = SDL_MOUSEBUTTONDOWN; mouse.button.windowID = platform.window_id;
        mouse.button.button = SDL_BUTTON_LEFT; mouse.button.x = 500; mouse.button.y = 350;
        check(SDL_PushEvent(&mouse) == 1, "must queue pan contact");
        MiltonInput input = sdl_event_loop(milton, &platform);
        panning_update(&platform, false);
        input.flags |= MiltonInputFlags_PANNING | MiltonInputFlags_IMGUI_GRABBED_INPUT;
        input.input_count = 0;
        milton_update_and_render(milton, &input);
        SDL_Event motion = {};
        motion.type = SDL_MOUSEMOTION; motion.motion.windowID = platform.window_id;
        motion.motion.x = 532; motion.motion.y = 374;
        check(SDL_PushEvent(&motion) == 1, "must queue pan movement");
        input = sdl_event_loop(milton, &platform);
        panning_update(&platform, false);
        v2l delta = platform.pan_point - platform.pan_start;
        milton_resize_and_pan(milton, delta, {platform.width, platform.height});
        input.flags |= MiltonInputFlags_PANNING | MiltonInputFlags_IMGUI_GRABBED_INPUT;
        input.input_count = 0;
        milton_update_and_render(milton, &input);
        check(platform.is_space_down && platform.is_panning && delta == v2l{32, 24}
              && view->pan_center != saved_view.pan_center
              && SDL_AtomicGet(&worker->hold) && SDL_AtomicGet(&worker->completed) != first
              && !worker->requested && !r->tiles->display_fading
              && r->tile_builds == 0 && r->tile_composites == 0,
              "real queued navigation must update and display without waiting for a blocked worker");
        compare(true, "navigation while worker blocked");
        key.type = SDL_KEYUP;
        mouse.type = SDL_MOUSEBUTTONUP;
        check(SDL_PushEvent(&key) == 1 && SDL_PushEvent(&mouse) == 1, "must queue navigation release");
        sdl_event_loop(milton, &platform);
        panning_update(&platform, false);
        Layer* undo_layer = milton->canvas->root_layer;
        i64 undo_count = undo_layer->strokes.count;
        push(&milton->canvas->history, HistoryElement{HistoryElement_STROKE_ADD, undo_layer->id});
        key.type = SDL_KEYDOWN;
        key.key.keysym.sym = SDLK_z;
        key.key.keysym.scancode = SDL_SCANCODE_Z;
        key.key.keysym.mod = KMOD_LCTRL;
        check(SDL_PushEvent(&key) == 1, "must queue undo while worker is blocked");
        input = sdl_event_loop(milton, &platform);
        input.flags |= MiltonInputFlags_IMGUI_GRABBED_INPUT;
        milton_update_and_render(milton, &input);
        check(undo_layer->strokes.count == undo_count - 1
              && SDL_AtomicGet(&worker->hold) && !worker->requested && !r->tiles->display_fading,
              "queued undo must register without waiting for the worker");
        compare(true, "undo while worker blocked");
        key.key.keysym.mod = KMOD_LCTRL | KMOD_LSHIFT;
        check(SDL_PushEvent(&key) == 1, "must queue redo while worker is blocked");
        input = sdl_event_loop(milton, &platform);
        input.flags |= MiltonInputFlags_IMGUI_GRABBED_INPUT;
        milton_update_and_render(milton, &input);
        check(undo_layer->strokes.count == undo_count && !worker->requested,
              "queued redo must restore history without waiting for the worker");
        pop(&milton->canvas->history);
        ImGui::DestroyContext();
        ImGui::SetCurrentContext(previous_imgui);
        milton->platform = saved_platform;
        milton->flags = saved_flags;
        milton->mode_gui_visibility[Milton::GuiVisibleCategory_DRAWING] = saved_visible;
        milton->gui->visible = saved_visible;
        milton->render_settings = saved_settings;

        r->flags = RenderBackendFlags_NONE;
        view->angle = 0.1f;
        frame();
        int intermediate = worker->requested_generation;
        *view = saved_view;
        view->scale = 512;
        view->screen_size = {512, 512};
        view->zoom_center = {256, 256};
        view->pan_center = {16 * view->scale, 0};
        gpu_resize(r, view);
        gpu_update_canvas(r, milton->canvas, view);
        frame();
        int latest = worker->requested_generation;
        check(latest != first && latest != intermediate, "new view requests must replace queued work");
        SDL_AtomicSet(&worker->hold, 0);
        check(wait(&worker->completed, latest), "latest zoom/resize request must complete");
        r->disable_display_transitions = false;
        frame();
        check(r->tiles->display_fading && !r->tiles->display_preview,
              "a completed worker frame must use the existing rendered-quality fade");
        r->flags = RenderBackendFlags_BRUSH_RESIZING;
        frame();
        check(r->tiles->display_paused, "resizing must still pause worker-result fades");
        r->flags = RenderBackendFlags_NAVIGATION_PREVIEW | RenderBackendFlags_CANVAS_TRANSFORMING;
        frame();
        check(!r->tiles->display_paused && !r->tiles->display_fading && r->tiles->display_preview,
              "navigation must preempt even a paused worker-result fade");
        r->disable_display_transitions = true;
        r->flags = RenderBackendFlags_NONE;
        frame();
        latest = worker->requested_generation;
        check(wait(&worker->completed, latest), "refinement must resume after interrupting a worker-result fade");
        frame();
        check(r->tiles->display_ready && worker->front_generation == latest
              && r->tiles->entries.count == 0 && r->tile_builds == 0 && r->tile_composites == 0,
              "only the latest complete result may be displayed, with no foreground tile jobs");
        compare(false, "latest zoom/resize result");
        frame();
        check(worker->requested_generation == latest, "unchanged frames must not resubmit snapshots");

        SDL_AtomicSet(&worker->hold, 1);
        gpu_free_strokes(source, 1, r);
        source->points[0].x += 8 * view->scale;
        source->bounding_rect = bounding_box_for_stroke(source);
        frame();
        int edited = worker->requested_generation;
        check(wait(&worker->entered, edited), "worker must receive edited snapshot");
        frozen = worker->scene->layers[0].strokes[0];
        v2l frozen_point = frozen->data.points[0];
        gpu_free_strokes(source, 1, r);
        source->points[0].x += 8 * view->scale;
        source->bounding_rect = bounding_box_for_stroke(source);
        frame();
        latest = worker->requested_generation;
        check(frozen->data.points[0] == frozen_point && frozen_point != source->points[0],
              "edits must not mutate samples held by an in-flight request");
        SDL_AtomicSet(&worker->hold, 0);
        check(wait(&worker->completed, latest), "latest edited scene must complete after cancellation");
        frame();
        compare(false, "latest immutable edit");

        for ( int i = 0; i < 16; ++i ) {
            view->angle = 0.005f * (i % 3);
            view->pan_center.x = (i % 5) * 8 * view->scale;
            gpu_update_canvas(r, milton->canvas, view);
            frame();
            check(!r->tiles->display_ready || worker->front_generation == worker->requested_generation,
                  "rapid unblocked requests must never display an obsolete generation");
        }
        view->angle = 0;
        view->pan_center.x = 16 * view->scale;
        gpu_update_canvas(r, milton->canvas, view);
        frame();
        latest = worker->requested_generation;
        check(wait(&worker->completed, latest), "worker must converge after rapid camera requests");
        frame();
        compare(false, "rapid camera requests");

        SDL_AtomicSet(&worker->hold, 1);
        gpu_free_strokes(source, 1, r);
        source->points[0] = original_point;
        source->bounding_rect = original_bounds;
        *view = saved_view;
        gpu_resize(r, view);
        gpu_update_canvas(r, milton->canvas, view);
        gpu_render_to_buffer(milton, export_after, 2, 224, 280, 96, 96, 1);
        check(memcmp(export_before, export_after, export_bytes) == 0,
              "export must remain pixel-identical and independent of pending worker requests");
        gpu_stop_canvas_worker(r);
        check(!r->canvas_worker && SDL_GL_GetCurrentContext() == foreground,
              "shutdown must cancel pending work without losing the UI context");
        check(gpu_start_canvas_worker(r, window), "worker must restart after complete resource cleanup");
        gpu_stop_canvas_worker(r);
    }
    r->flags = saved_render_flags;
    r->disable_display_transitions = saved_transitions;
    free(expected); free(actual);
    fprintf(passed ? stdout : stderr, "Isolated canvas worker: %s\n", passed ? "passed" : "FAILED");
    return passed;
}
