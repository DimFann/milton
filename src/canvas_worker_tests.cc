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
    auto compare_view_texture = [&](GLuint texture, CanvasView source, const char* label) {
        read(actual);
        glBindFramebufferEXT(GL_FRAMEBUFFER, r->tiles->fbo);
        glFramebufferTexture2DEXT(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
        glReadPixels(0, 0, source.screen_size.w, source.screen_size.h, GL_RGBA, GL_UNSIGNED_BYTE, expected);
        double cs = cos(source.angle), ss = sin(source.angle), fs = source.flipped ? -1 : 1;
        double ct = cos(view->angle), st = sin(view->angle), ft = view->flipped ? -1 : 1;
        int worst = 0;
        for ( int y = 0; y < r->height; ++y ) {
            for ( int x = 0; x < r->width; ++x ) {
                double dx = x + 0.5 - view->zoom_center.x;
                double dy = r->height - y - 0.5 - view->zoom_center.y;
                double wx = (view->pan_center.x - source.pan_center.x
                             + view->scale * (ct * ft * dx - st * dy)) / source.scale;
                double wy = (view->pan_center.y - source.pan_center.y
                             + view->scale * (st * ft * dx + ct * dy)) / source.scale;
                double sx = fs * (cs * wx + ss * wy) + source.zoom_center.x;
                double sy = -ss * wx + cs * wy + source.zoom_center.y;
                if ( fabs(sx) < 0.001 || fabs(sy) < 0.001
                     || fabs(sx - source.screen_size.w) < 0.001
                     || fabs(sy - source.screen_size.h) < 0.001 ) { continue; }
                bool inside = sx >= 0 && sy >= 0 && sx < source.screen_size.w && sy < source.screen_size.h;
                sx -= 0.5; sy = source.screen_size.h - sy - 0.5;
                int ix = (int)floor(sx), iy = (int)floor(sy);
                double tx = sx - ix, ty = sy - iy;
                for ( int channel = 0; channel < 4; ++channel ) {
                    double value = channel == 3 ? 255 : 255 * r->background_color.d[channel];
                    if ( inside ) {
                        value = 0;
                        for ( int j = 0; j < 2; ++j ) {
                            for ( int i = 0; i < 2; ++i ) {
                                int px = clamp(ix + i, 0, source.screen_size.w - 1);
                                int py = clamp(iy + j, 0, source.screen_size.h - 1);
                                value += expected[((size_t)py * source.screen_size.w + px) * 4 + channel]
                                         * (i ? tx : 1 - tx) * (j ? ty : 1 - ty);
                            }
                        }
                    }
                    worst = max(worst, abs((int)actual[((size_t)y * r->width + x) * 4 + channel]
                                           - (int)floor(value + 0.5)));
                }
            }
        }
        fprintf(stdout, "Canvas worker %s reprojection: %d LSB\n", label, worst);
        check(worst <= 2, label);
        glBindFramebufferEXT(GL_FRAMEBUFFER, r->fbo);
        glFramebufferTexture2DEXT(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->canvas_texture, 0);
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
              && worker->requested && worker->requested_preview && r->tiles->navigation_valid
              && r->clip_array.count == 0 && !r->tiles->display_fading
              && r->tile_builds == 0 && r->tile_composites == 0,
              "real queued navigation must update and display without waiting for a blocked worker");
        compare_view_texture(r->tiles->navigation_texture, r->tiles->navigation_view, "navigation while worker blocked");
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
              && SDL_AtomicGet(&worker->hold) && !worker->requested
              && !r->tiles->navigation_valid && !r->tiles->display_fading,
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

        frame();
        CanvasView navigation_start = *view;
        SDL_AtomicSet(&worker->hold, 1);
        GLuint frozen_texture = 0;
        for ( int transform = 0; transform < 5; ++transform ) {
            *view = navigation_start;
            if ( transform == 0 ) { view->pan_center.x += 8 * view->scale; }
            if ( transform == 1 ) { view->scale /= 2; }
            if ( transform == 2 ) { view->angle += 0.37f; }
            if ( transform == 3 ) { view->flipped = !view->flipped; }
            if ( transform == 4 ) { view->screen_size = {640, 480}; view->zoom_center = {320, 240}; }
            gpu_resize(r, view);
            gpu_update_canvas(r, milton->canvas, view);
            r->flags = RenderBackendFlags_NAVIGATION_PREVIEW | RenderBackendFlags_CANVAS_TRANSFORMING;
            frame();
            if ( !frozen_texture ) { frozen_texture = r->tiles->navigation_texture; }
            check(r->tiles->navigation_valid && r->tiles->navigation_texture == frozen_texture
                  && memcmp(&r->tiles->navigation_view, &navigation_start, sizeof(CanvasView)) == 0
                  && r->clip_array.count == 0 && r->cook_pending == 0,
                  "camera updates must reproject one immutable snapshot without traversing or cooking strokes");
            compare_view_texture(frozen_texture, navigation_start, "pan/zoom/rotation/flip/resize");
        }
        *view = navigation_start;
        view->pan_center.x += view->screen_size.w * view->scale;
        gpu_resize(r, view);
        gpu_update_canvas(r, milton->canvas, view);
        frame();
        latest = worker->requested_generation;
        SDL_AtomicSet(&worker->hold, 0);
        check(wait(&worker->completed, latest), "worker must fill newly exposed navigation regions");
        frame();
        check(worker->preview && r->tiles->navigation_valid && r->clip_array.count == 0,
              "exposed navigation regions must use asynchronous raster coverage, not foreground stroke replay");
        compare_view_texture(worker->preview, worker->preview_view, "newly exposed coverage");
        float navigation_alpha = milton->canvas->root_layer->alpha;
        milton->canvas->root_layer->alpha *= 0.5f;
        frame();
        frame();
        check(!r->tiles->navigation_valid && !r->tiles->capture_ready,
              "scene edits during navigation must not pin stale artwork or capture placeholder background");
        milton->canvas->root_layer->alpha = navigation_alpha;
        frame();
        r->flags = RenderBackendFlags_NAVIGATION_PREVIEW | RenderBackendFlags_SELECTION_TRANSFORMING;
        frame();
        check(!r->tiles->navigation_valid && !worker->requested,
              "selection transforms must invalidate camera snapshots and retain their live-stroke preview");
        r->flags = RenderBackendFlags_NONE;
        frame();
        latest = worker->requested_generation;
        check(wait(&worker->completed, latest), "accurate refinement must resume after raster navigation");
        frame();
        check(!r->tiles->navigation_valid && !worker->preview && r->tiles->display_ready,
              "accurate completion must retire navigation snapshots and asynchronous gap coverage");
        compare(false, "after raster navigation");
        *view = navigation_start;
        gpu_update_canvas(r, milton->canvas, view);
        frame();
        latest = worker->requested_generation;
        check(wait(&worker->completed, latest), "original view must refine after navigation tests");
        frame();

        Stroke* working = &milton->working_stroke;
        Stroke saved_working = *working;
        i32 saved_stroke_id = milton->canvas->stroke_id_count;
        Layer* drawing_layer = milton->canvas->root_layer;
        const u32 drawing_flags[] = {0, StrokeFlag_ERASER, StrokeFlag_BLUR, StrokeFlag_PRESERVE_OPACITY};
        const char* drawing_labels[] = {"drawing pen", "drawing eraser", "drawing blur", "drawing preserve opacity"};
        for ( int tool = 0; tool < array_count(drawing_flags); ++tool ) {
            frame();
            read(expected);
            SDL_AtomicSet(&worker->hold, 1);
            working->layer_id = drawing_layer->id;
            working->num_points = 2;
            working->flags = drawing_flags[tool];
            working->brush = default_brush();
            working->brush.radius = 24 * view->scale;
            working->brush.alpha = 0.7f;
            working->brush.color = to_premultiplied(v3f{0.25f, 0.45f, 0.75f}, 0.7f);
            working->points[0] = raster_to_canvas(view, v2l{240, 240});
            working->points[1] = raster_to_canvas(view, v2l{280, 280});
            working->pressures[0] = working->pressures[1] = 1;
            working->bounding_rect = bounding_box_for_stroke(working);
            frame();
            int drawing = worker->requested_generation;
            check(wait(&worker->entered, drawing), "worker must receive the initial drawing snapshot");
            read(actual);
            check(r->tiles->display_ready && !r->tiles->display_preview
                  && memcmp(actual, expected, (size_t)r->width * r->height * 4) == 0,
                  "starting a stroke must retain rendered paint while the worker is blocked");
            working->pressures[1] = 0.6f;
            frame();
            latest = worker->requested_generation;
            check(latest != drawing && SDL_AtomicGet(&worker->minimum_generation) <= drawing,
                  "new stroke samples must queue latest work without cancelling in-flight drawing");
            SDL_AtomicSet(&worker->hold_after_completion, 1);
            SDL_AtomicSet(&worker->hold, 0);
            check(wait(&worker->entered, latest) && SDL_AtomicGet(&worker->completed) == drawing,
                  "in-flight drawing must finish before the next queued snapshot begins");
            frame();
            check(r->tiles->display_ready && !r->tiles->display_preview && worker->front_generation == drawing,
                  "completed drawing progress must display even while newer samples are pending");
            SDL_AtomicSet(&worker->hold, 0);
            check(wait(&worker->completed, latest), "latest drawing samples must complete");
            frame();
            compare(false, drawing_labels[tool]);

            frame();
            read(expected);
            SDL_AtomicSet(&worker->hold, 1);
            working->points[1].x += 4 * view->scale;
            working->bounding_rect = bounding_box_for_stroke(working);
            frame();
            drawing = worker->requested_generation;
            check(wait(&worker->entered, drawing), "worker must receive the final drawing snapshot");
            Stroke committed = {};
            copy_stroke(&milton->canvas->arena, view, working, &committed);
            committed.id = milton->canvas->stroke_id_count++;
            layer::layer_push_stroke(drawing_layer, committed);
            reset_working_stroke(milton);
            frame();
            latest = worker->requested_generation;
            read(actual);
            check(r->tiles->display_ready && !r->tiles->display_preview
                  && SDL_AtomicGet(&worker->minimum_generation) <= drawing
                  && memcmp(actual, expected, (size_t)r->width * r->height * 4) == 0,
                  "committing a stroke must retain rendered paint and in-flight progress");
            SDL_AtomicSet(&worker->hold, 0);
            check(wait(&worker->completed, latest), "committed drawing must finish");
            frame();
            compare(false, "committed drawing");
        }
        frame();
        read(expected);
        SDL_AtomicSet(&worker->hold, 1);
        Stroke tap = {};
        copy_stroke(&milton->canvas->arena, view,
                    get(&drawing_layer->strokes, drawing_layer->strokes.count - 1), &tap);
        tap.id = milton->canvas->stroke_id_count++;
        layer::layer_push_stroke(drawing_layer, tap);
        frame();
        latest = worker->requested_generation;
        check(wait(&worker->entered, latest), "worker must receive a stroke committed between frames");
        read(actual);
        check(r->tiles->display_ready && !r->tiles->display_preview
              && memcmp(actual, expected, (size_t)r->width * r->height * 4) == 0,
              "a stroke committed between frames must not switch to preview");
        SDL_AtomicSet(&worker->hold, 0);
        check(wait(&worker->completed, latest), "stroke committed between frames must finish");
        frame();
        compare(false, "stroke committed between frames");
        SDL_AtomicSet(&worker->reference_only, 1);
        for ( int tool = 0; tool < array_count(drawing_flags); ++tool ) {
            for ( int update = 0; update < 2; ++update ) {
                working->num_points = 2;
                working->flags = drawing_flags[tool];
                working->points[0] = raster_to_canvas(view, v2l{240, 240});
                working->points[1] = raster_to_canvas(view, v2l{280, 280});
                working->pressures[0] = 1;
                working->pressures[1] = 0.8f - update * 0.2f;
                working->bounding_rect = bounding_box_for_stroke(working);
                frame();
                latest = worker->requested_generation;
                check(wait(&worker->completed, latest), "reference-path drawing must complete");
                if ( update != 0 ) {
                    fprintf(stdout, "Canvas worker reference %s: %d replay hits, %d dependent blur passes\n",
                            drawing_labels[tool], worker->profile.replay_hits, worker->profile.blur_runs);
                    check(worker->profile.replay_hits == 1,
                          "same-view worker drawing must reuse its replay prefix");
                }
                frame();
            }
        }
        reset_working_stroke(milton);
        SDL_AtomicSet(&worker->reference_only, 0);
        for ( int tool = 0; tool < array_count(drawing_flags) + 1; ++tool ) {
            Stroke removed = pop(&drawing_layer->strokes);
            gpu_free_strokes(&removed, 1, r);
        }
        *working = saved_working;
        milton->canvas->stroke_id_count = saved_stroke_id;

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
