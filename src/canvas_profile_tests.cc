// Read-only, hidden-window drawing benchmark for a supplied canvas.
static bool
test_profile_canvas(const char* path)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    PATH_CHAR filename[MAX_PATH] = {};
#if defined(_WIN32)
    if ( !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, filename, MAX_PATH) ) {
        fprintf(stderr, "Cannot decode canvas path\n");
        return false;
    }
#else
    if ( strlen(path) >= sizeof(filename) ) { fprintf(stderr, "Canvas path is too long\n"); return false; }
    memcpy(filename, path, strlen(path) + 1);
#endif
    FILE* file = platform_fopen(filename, TO_PATH_STR("rb"));
    u32 header[2] = {};
    CanvasView saved = {};
    bool valid = file && fread(header, sizeof(header), 1, file) == 1
                 && header[0] == MILTON_MAGIC_NUMBER && header[1] == MILTON_MINOR_VERSION
                 && fread(&saved.size, sizeof(saved.size), 1, file) == 1
                 && saved.size >= offsetof(CanvasView, scale) && saved.size <= sizeof(saved)
                 && fread((u8*)&saved + offsetof(CanvasView, screen_size), saved.size - sizeof(u32), 1, file) == 1;
    if ( file ) { fclose(file); }
    if ( !valid || saved.screen_size.w <= 0 || saved.screen_size.h <= 0 ) {
        fprintf(stderr, "Profiling requires a readable current-version canvas with a valid saved viewport\n");
        return false;
    }
    if ( SDL_Init(SDL_INIT_VIDEO) != 0 ) {
        fprintf(stderr, "Cannot initialize profiling video: %s\n", SDL_GetError());
        return false;
    }
#if USE_GL_3_2
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#endif
    int width = saved.screen_size.w, height = saved.screen_size.h;
    SDL_Window* window = SDL_CreateWindow("Canvas profile", 0, 0, width, height,
                                          SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    SDL_GLContext context = window ? SDL_GL_CreateContext(window) : NULL;
    if ( !context || !gl::load() ) {
        fprintf(stderr, "Cannot initialize profiling OpenGL: %s\n", SDL_GetError());
        if ( context ) { SDL_GL_DeleteContext(context); }
        if ( window ) { SDL_DestroyWindow(window); }
        SDL_Quit();
        return false;
    }
    Milton milton = {};
    milton.root_arena = arena_init(8 * 1024 * 1024);
    milton_init(&milton, width, height, 1, filename, MiltonInit_FOR_TEST);
    milton_load(&milton);
    milton.flags |= MiltonStateFlags_AUTOSAVE_BLOCKED;
    RenderBackend* r = milton.renderer;
    CanvasView* view = milton.view;
    bool passed = (milton.flags & MiltonStateFlags_JUST_SAVED) != 0;
    if ( !passed ) { fprintf(stderr, "Canvas loading failed\n"); }
    if ( !gpu_init(r, view, &milton.gui->picker) ) {
        fprintf(stderr, "Cannot initialize profiling renderer\n");
        arena_free(&milton.root_arena);
        SDL_GL_DeleteContext(context);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return false;
    }
    gpu_resize(r, view);
    gpu_update_background(r, view->background_color);
    gpu_update_canvas(r, milton.canvas, view);
    glViewport(0, 0, width, height);
    gpu_update_brush_outline(r, -100, -100, -1);
    r->tile_cache_unlimited = true;
    r->disable_display_transitions = true;
    i64 strokes = 0, points = 0, blur = 0, preserving = 0;
    for ( Layer* layer = milton.canvas->root_layer; layer; layer = layer->next ) {
        StrokeIterator iterator = {};
        for ( Stroke* s = stroke_iter_init(&layer->strokes, &iterator); s; s = stroke_iter_next(&iterator) ) {
            ++strokes; points += s->num_points;
            blur += (s->flags & StrokeFlag_BLUR) != 0;
            preserving += (s->flags & StrokeFlag_PRESERVE_OPACITY) != 0;
        }
    }
    fprintf(stdout, "Canvas profile: %dx%d scale %lld angle %.3f; %lld strokes, %lld samples, %lld blur, %lld preserving; GPU %s\n",
            width, height, (long long)view->scale, view->angle, (long long)strokes, (long long)points,
            (long long)blur, (long long)preserving, (const char*)glGetString(GL_RENDERER));
    auto clip = [&](ClipFlags flags) {
        gpu_clip_strokes_and_update(&milton.root_arena, r, view, view->scale, milton.canvas->root_layer,
                                    &milton.working_stroke, 0, 0, width, height, flags);
    };
    auto wait = [&](int generation) {
        u32 started = SDL_GetTicks();
        while ( SDL_AtomicGet(&r->canvas_worker->completed) != generation
                && SDL_AtomicGet(&r->canvas_worker->status) >= 0
                && (u32)(SDL_GetTicks() - started) < 60000 ) {
            SDL_PumpEvents();
            SDL_Delay(1);
        }
        bool done = SDL_AtomicGet(&r->canvas_worker->completed) == generation;
        if ( !done ) { fprintf(stderr, "Canvas profiling worker timed out or failed\n"); }
        return done;
    };
    const u32 flags[] = {0, StrokeFlag_ERASER, StrokeFlag_BLUR, StrokeFlag_PRESERVE_OPACITY};
    const char* tools[] = {"pen", "eraser", "blur", "preserve"};
    const char* modes[] = {"direct", "tiles", "worker"};
    size_t pixel_bytes = (size_t)width * height * 4;
    u8* actual = (u8*)calloc(pixel_bytes, 1);
    u8* reference = (u8*)calloc(pixel_bytes, 1);
    if ( !actual || !reference ) { milton_die_gracefully("Cannot allocate profiling pixels."); }
    auto read = [&](u8* pixels) {
        glBindFramebufferEXT(GL_FRAMEBUFFER, r->fbo);
        glFramebufferTexture2DEXT(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->canvas_texture, 0);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    };
    for ( int mode = 0; passed && mode < 3; ++mode ) {
        gpu_clear_tile_cache(r);
        reset_working_stroke(&milton);
        if ( mode == 2 ) {
            clip(ClipFlags_TILE_SCENE);
            passed = gpu_start_canvas_worker(r, window);
            if ( !passed ) { fprintf(stderr, "Cannot start profiling worker\n"); break; }
        }
        for ( int tool = 0; passed && tool < array_count(flags); ++tool ) {
            reset_working_stroke(&milton);
            double elapsed = 0, maximum = 0, submit = 0;
            CanvasWorkerProfile totals = {};
            for ( int frame = 0; passed && frame < 8; ++frame ) {
                Stroke* working = &milton.working_stroke;
                working->layer_id = milton.canvas->working_layer->id;
                working->brush = milton.brushes[BrushEnum_PEN];
                working->brush.radius = 14 * view->scale;
                working->flags = flags[tool];
                working->num_points = 2 + frame;
                for ( int i = 0; i < working->num_points; ++i ) {
                    working->points[i] = raster_to_canvas(view, v2l{width * 53 / 100 + i * 3, height * 40 / 100 + i * 2});
                    working->pressures[i] = 0.8f;
                }
                working->bounding_rect = bounding_box_for_stroke(working);
                u64 started = SDL_GetPerformanceCounter();
                clip(mode == 0 ? ClipFlags_JUST_CLIP : ClipFlags_TILE_SCENE);
                if ( mode == 0 ) { gpu_render_canvas(r, 0, 0, width, height, 1, false); }
                if ( mode == 1 && !gpu_render_tile_cache(r, 0, 0, width, height) ) {
                    clip(ClipFlags_JUST_CLIP);
                    gpu_render_canvas(r, 0, 0, width, height, 1, false);
                }
                if ( mode == 2 ) { gpu_render(r, 0, 0, width, height); }
                double submitted = canvas_profile_elapsed(started);
                if ( mode == 2 ) {
                    passed = wait(r->canvas_worker->requested_generation);
                    SDL_LockMutex(r->canvas_worker->mutex);
                    CanvasWorkerProfile profile = r->canvas_worker->profile;
                    SDL_UnlockMutex(r->canvas_worker->mutex);
                    if ( frame >= 2 ) {
                        totals.install_ms += profile.install_ms;
                        totals.render_ms += profile.render_ms;
                        totals.publish_ms += profile.publish_ms;
                        totals.builds += profile.builds;
                        totals.composites += profile.composites;
                        totals.batches += profile.batches;
                        totals.replay_hits += profile.replay_hits;
                        totals.blur_runs += profile.blur_runs;
                    }
                    gpu_render(r, 0, 0, width, height);
                }
                glFinish();
                double ms = canvas_profile_elapsed(started);
                if ( frame >= 2 ) { elapsed += ms; submit += submitted; maximum = max(maximum, ms); }
            }
            fprintf(stdout, "%s %s: update avg %.2f ms max %.2f; foreground %.2f; worker install %.2f render %.2f publish %.2f; builds %.1f composites %.1f batches %.1f replay hits %.1f blur passes %.1f\n",
                    modes[mode], tools[tool], elapsed / 6, maximum, submit / 6,
                    totals.install_ms / 6, totals.render_ms / 6, totals.publish_ms / 6,
                    totals.builds / 6.0, totals.composites / 6.0, totals.batches / 6.0,
                    totals.replay_hits / 6.0, totals.blur_runs / 6.0);
            if ( passed && mode == 2 ) {
                read(actual);
                CanvasWorker* worker = r->canvas_worker;
                CanvasTileCache* presentation = r->tiles;
                r->canvas_worker = NULL;
                r->tiles = NULL;
                r->replay_valid = false;
                clip(ClipFlags_TILE_SCENE);
                if ( !gpu_render_tile_cache(r, 0, 0, width, height) ) {
                    clip(ClipFlags_JUST_CLIP);
                    gpu_render_canvas(r, 0, 0, width, height, 1, false);
                }
                read(reference);
                gpu_clear_tile_cache(r);
                r->canvas_worker = worker;
                r->tiles = presentation;
                int worst = 0;
                size_t worst_at = 0, differences = 0;
                for ( size_t i = 0; i < pixel_bytes; ++i ) {
                    int difference = abs((int)actual[i] - (int)reference[i]);
                    if ( difference > worst ) { worst = difference; worst_at = i; }
                    differences += difference > 2;
                }
                fprintf(stdout, "worker %s vs synchronous pixels: %d LSB at %zu,%zu (%u/%u), %zu differing channels\n",
                        tools[tool], worst, worst_at / 4 % width, worst_at / 4 / width,
                        actual[worst_at], reference[worst_at], differences);
                passed = worst <= 2;
                if ( !passed ) { fprintf(stderr, "Profiled worker output differs from synchronous rendering\n"); }
            }
        }
        gpu_stop_canvas_worker(r);
    }
    gpu_free_strokes(r, milton.canvas);
    gpu_release_data(r);
    free(reference);
    free(actual);
    arena_free(&milton.root_arena);
    if ( glGetError() != GL_NO_ERROR ) { fprintf(stderr, "Canvas profiling OpenGL error\n"); passed = false; }
    SDL_GL_DeleteContext(context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return passed;
}
