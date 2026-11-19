// Hidden-window integration tests for the canvas tile renderer.
static bool
test_tile_cache()
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const int size = 768;
    const size_t bytes = (size_t)size * size * 4;
    if ( SDL_Init(SDL_INIT_VIDEO) != 0 ) {
        fprintf(stderr, "Tile cache: cannot initialize SDL video: %s\n", SDL_GetError());
        return false;
    }
#if USE_GL_3_2
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#endif
    SDL_Window* window = SDL_CreateWindow("Tile cache test", 0, 0, size, size,
                                          SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    SDL_GLContext context = window ? SDL_GL_CreateContext(window) : NULL;
    if ( !context || !gl::load() ) {
        fprintf(stderr, "Tile cache: cannot initialize OpenGL: %s\n", SDL_GetError());
        if ( context ) { SDL_GL_DeleteContext(context); }
        if ( window ) { SDL_DestroyWindow(window); }
        SDL_Quit();
        return false;
    }

    Milton milton = {};
    milton_init(&milton, size, size, 1, NULL, MiltonInit_FOR_TEST);
    milton_reset_canvas_and_set_default(&milton);
    RenderBackend* r = milton.renderer;
    CanvasView* view = milton.view;
    view->screen_size = {size, size};
    view->zoom_center = {size / 2, size / 2};
    view->pan_center = {0, 0};
    view->scale = 1024;
    view->angle = 0;
    view->flipped = 0;
    gui_anchor_picker_top_right(milton.gui, size);
    if ( !gpu_init(r, view, &milton.gui->picker) ) {
        fprintf(stderr, "Tile cache: cannot initialize renderer\n");
        arena_free(&milton.root_arena);
        SDL_GL_DeleteContext(context);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return false;
    }
    gpu_resize(r, view);
    gpu_update_canvas(r, milton.canvas, view);
    gpu_update_brush_outline(r, -100, -100, 1);
    glViewport(0, 0, size, size);
    r->tile_cache_unlimited = true;

    u8* tiled = (u8*)mlt_calloc(bytes, 1, "Tile cache test");
    u8* reference = (u8*)mlt_calloc(bytes, 1, "Tile cache test");
    u8* before = (u8*)mlt_calloc(bytes, 1, "Tile cache test");
    bool passed = true;
    auto check = [&](bool condition, const char* message) {
        if ( !condition ) {
            fprintf(stderr, "Tile cache FAILED: %s\n", message);
            passed = false;
        }
    };
    auto add = [&](Layer* layer, u32 flags, v3f color, float alpha, int radius,
                   v2l a, v2l b, float p0, float p1) {
        Stroke stroke = {};
        stroke.id = milton.canvas->stroke_id_count++;
        stroke.layer_id = layer->id;
        stroke.flags = flags;
        stroke.num_points = 2;
        stroke.points = arena_alloc_array(&milton.canvas->arena, 2, v2l);
        stroke.pressures = arena_alloc_array(&milton.canvas->arena, 2, f32);
        stroke.points[0] = raster_to_canvas(view, a);
        stroke.points[1] = raster_to_canvas(view, b);
        stroke.pressures[0] = p0;
        stroke.pressures[1] = p1;
        stroke.brush = default_brush();
        stroke.brush.radius = radius * view->scale;
        stroke.brush.alpha = alpha;
        stroke.brush.color = to_premultiplied(color, alpha);
        stroke.brush.pressure_opacity_min = 0.0f;
        stroke.brush.pressure_size = 0;
        stroke.bounding_rect = bounding_box_for_stroke(&stroke);
        layer::layer_push_stroke(layer, stroke);
    };
    auto clip = [&]() {
        gpu_clip_strokes_and_update(&milton.root_arena, r, view, view->scale, milton.canvas->root_layer,
                                    &milton.working_stroke, 0, 0, size, size, ClipFlags_JUST_CLIP);
    };
    auto clip_scene = [&]() {
        gpu_clip_strokes_and_update(&milton.root_arena, r, view, view->scale, milton.canvas->root_layer,
                                    &milton.working_stroke, 0, 0, size, size, ClipFlags_TILE_SCENE);
    };
    auto read = [&](u8* pixels) {
        glBindFramebufferEXT(GL_FRAMEBUFFER, r->fbo);
        glFramebufferTexture2DEXT(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->canvas_texture, 0);
        glReadPixels(0, 0, size, size, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    };
    auto draw_tiles = [&](const char* label) {
        clip();
        bool rendered = gpu_render_tile_cache(r, 0, 0, size, size);
        check(rendered, label);
        if ( rendered ) { read(tiled); }
        return rendered;
    };
    auto compare = [&](const char* label) {
        clip();
        r->replay_valid = false;
        gpu_render_canvas(r, 0, 0, size, size, 1.0f, false);
        read(reference);
        int worst = 0;
        size_t worst_at = 0;
        for ( size_t i = 0; i < bytes; ++i ) {
            int diff = abs((int)tiled[i] - (int)reference[i]);
            if ( diff > worst ) { worst = diff; worst_at = i; }
        }
        fprintf(worst <= 2 ? stdout : stderr,
                "Tile cache %s: worst difference %d LSB at (%zu,%zu), channel %zu (tile %u, legacy %u)\n",
                label, worst, (worst_at / 4) % size, (worst_at / 4) / size, worst_at % 4,
                (unsigned)tiled[worst_at], (unsigned)reference[worst_at]);
        check(worst <= 2, label);
    };
    auto aligned = [&](const char* label) {
        if ( draw_tiles(label) ) { compare(label); }
    };
    auto changed = [&](const char* label) {
        check(memcmp(before, tiled, bytes) != 0, label);
    };

    // At this view, tile boundaries fall at raster coordinates 128, 384, and 640.
    Layer* lower = milton.canvas->working_layer;
    add(lower, 0, {0.1f, 0.35f, 0.9f}, 0.85f, 34, {175, 340}, {585, 410}, 1, 1);
    add(lower, StrokeFlag_PRESERVE_OPACITY | StrokeFlag_PRESSURE_TO_OPACITY,
        {0.9f, 0.15f, 0.1f}, 0.8f, 30, {190, 385}, {570, 385}, 0.25f, 1);
    add(lower, StrokeFlag_PRESERVE_OPACITY | StrokeFlag_PRESSURE_TO_OPACITY,
        {0.9f, 0.15f, 0.1f}, 0.8f, 30, {195, 388}, {565, 388}, 1, 0.35f);
    add(lower, StrokeFlag_BLUR, {1, 1, 1}, 0.8f, 60, {220, 380}, {540, 380}, 1, 1);
    add(lower, StrokeFlag_BLUR, {1, 1, 1}, 0.7f, 52, {260, 390}, {510, 390}, 1, 1);
    add(lower, StrokeFlag_ERASER, {1, 1, 1}, 0.55f, 18, {365, 320}, {405, 450}, 1, 1);

    milton_new_layer(&milton);
    Layer* upper = milton.canvas->working_layer;
    add(upper, 0, {0.1f, 0.8f, 0.25f}, 0.7f, 32, {240, 350}, {530, 425}, 1, 1);
    add(upper, StrokeFlag_ERASER, {1, 1, 1}, 0.8f, 20, {350, 325}, {415, 450}, 1, 1);
    add(upper, StrokeFlag_BLUR, {1, 1, 1}, 0.9f, 48, {280, 390}, {490, 390}, 1, 1);

    if ( draw_tiles("cold render") ) {
        check(r->tile_builds > 0 && r->tile_composites > 0, "cold render must build and composite tiles");
        check(!gpu_tile_cache_pending(r), "unlimited cold render must finish rebuilding");
        compare("mixed pressure, repeated blur, erasers and layers");
    }
    if ( draw_tiles("warm redraw") ) {
        check(r->tile_builds == 0 && r->tile_hits > 0 && r->tile_composites == 0,
              "warm redraw must reuse tiles without rebuilding or recompositing");
        compare("warm redraw");
    }
    gpu_clear_tile_cache(r);
    auto timed_tile_frame = [&]() {
        clip_scene();
        glBindFramebufferEXT(GL_FRAMEBUFFER, r->fbo);
        glFramebufferTexture2DEXT(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->canvas_texture, 0);
        u8 pixel[4];
        glReadPixels(size / 2, size / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        u64 start = SDL_GetPerformanceCounter();
        bool rendered = gpu_render_tile_cache(r, 0, 0, size, size);
        glBindFramebufferEXT(GL_FRAMEBUFFER, r->fbo);
        glFramebufferTexture2DEXT(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->canvas_texture, 0);
        glReadPixels(size / 2, size / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        double ms = 1000.0 * (SDL_GetPerformanceCounter() - start) / SDL_GetPerformanceFrequency();
        check(rendered && !gpu_tile_cache_pending(r), "metadata-only benchmark render must complete");
        return ms;
    };
    double cold_ms = timed_tile_frame();
    int cold_builds = r->tile_builds, cold_composites = r->tile_composites;
    double warm_ms = timed_tile_frame();
    int warm_builds = r->tile_builds, warm_composites = r->tile_composites, warm_hits = r->tile_hits;
    u64 cached_bytes = 0;
    for ( const CanvasTile& tile : r->tiles->entries ) {
        cached_bytes += (u64)(!!tile.texture + !!tile.paint) * CANVAS_TILE_TEXTURE_SIZE
                        * CANVAS_TILE_TEXTURE_SIZE * 4;
    }
    fprintf(stdout, "Tile cache 768x768 cold %.2f ms (%d builds, %d composites), warm %.2f ms "
            "(%d builds, %d composites, %d hits); cached textures %llu bytes\n",
            cold_ms, cold_builds, cold_composites, warm_ms, warm_builds, warm_composites, warm_hits,
            (unsigned long long)cached_bytes);
    check(cold_builds > 0 && cold_composites > 0 && warm_builds == 0
          && warm_composites == 0 && warm_hits > 0, "metadata-only clipping must build then reuse tiles");
    read(tiled);
    compare("metadata-only clipping");

    auto zoom_reuse = [&](i64 resident_scale, i64 target_scale, const char* label) {
        gpu_clear_tile_cache(r);
        r->flags |= RenderBackendFlags_NAVIGATION_PREVIEW;
        view->scale = target_scale;
        gpu_update_canvas(r, milton.canvas, view);
        clip_scene();
        gpu_render(r, 0, 0, size, size);
        read(before);
        check(gpu_tile_cache_pending(r) && r->tile_builds == 0 && r->tile_composites == 0,
              "cold zoom preview must not reconstruct tiles");

        r->flags &= ~RenderBackendFlags_NAVIGATION_PREVIEW;
        view->scale = resident_scale;
        gpu_update_canvas(r, milton.canvas, view);
        clip_scene();
        gpu_render(r, 0, 0, size, size);
        check(!gpu_tile_cache_pending(r) && r->tile_builds > 0,
              "resident zoom level must be built completely");

        view->scale = target_scale;
        r->flags |= RenderBackendFlags_NAVIGATION_PREVIEW;
        gpu_update_canvas(r, milton.canvas, view);
        clip_scene();
        gpu_render(r, 0, 0, size, size);
        read(tiled);
        fprintf(stdout, "Tile cache %s: %d hits, %d builds, %d composites, pending %d\n",
                label, r->tile_hits, r->tile_builds, r->tile_composites, (int)gpu_tile_cache_pending(r));
        check(gpu_tile_cache_pending(r) && r->tile_hits > 0 && r->tile_builds == 0
              && r->tile_composites == 0, label);
        check(memcmp(before, tiled, bytes) != 0,
              "resident zoom tiles must visibly contribute beyond the cold preview");
        r->flags &= ~RenderBackendFlags_NAVIGATION_PREVIEW;
    };
    zoom_reuse(1024, 512, "coarser resident zoom reuse");
    zoom_reuse(512, 1024, "finer resident zoom reuse");
    view->scale = 1024;
    gpu_update_canvas(r, milton.canvas, view);
    aligned("after zoom reuse");

    view->pan_center.x += 8 * view->scale;
    gpu_update_canvas(r, milton.canvas, view);
    if ( draw_tiles("pan") ) { check(r->tile_hits > 0, "pan must reuse tiles"); }
    view->angle = 0.12f;
    gpu_update_canvas(r, milton.canvas, view);
    if ( draw_tiles("rotation") ) { check(r->tile_hits > 0, "rotation must reuse tiles"); }
    view->flipped = 1;
    gpu_update_canvas(r, milton.canvas, view);
    if ( draw_tiles("flip") ) { check(r->tile_hits > 0, "flip must reuse tiles"); }
    view->pan_center = {0, 0};
    view->angle = 0;
    view->flipped = 0;
    gpu_update_canvas(r, milton.canvas, view);
    aligned("restored aligned view");

    // Export itself remains on the legacy path; cached navigation must not affect it.
    const int export_w = 320, export_h = 240, export_scale = 2;
    const size_t export_bytes = (size_t)export_w * export_h * export_scale * export_scale * 4;
    u8* export_before = (u8*)mlt_calloc(export_bytes, 1, "Tile export test");
    u8* export_after = (u8*)mlt_calloc(export_bytes, 1, "Tile export test");
    CanvasView saved_view = *view;
    gpu_clear_tile_cache(r);
    gpu_render_to_buffer(&milton, export_before, export_scale, 224, 280, export_w, export_h, 1.0f);
    check(memcmp(view, &saved_view, sizeof(saved_view)) == 0, "export must restore the view");
    bool exported_paint = false;
    for ( size_t i = 0; i < export_bytes; i += 4 ) {
        if ( export_before[i] < 245 || export_before[i + 1] < 245 || export_before[i + 2] < 245 ) {
            exported_paint = true;
            break;
        }
    }
    check(exported_paint, "cropped export must contain painted pixels");
    clip_scene();
    gpu_render(r, 0, 0, size, size);
    check(r->tile_hits > 0 && r->tile_builds == 0, "export setup must leave warm tiles");
    r->flags |= RenderBackendFlags_NAVIGATION_PREVIEW;
    view->pan_center.x += 8 * view->scale;
    gpu_update_canvas(r, milton.canvas, view);
    clip_scene();
    gpu_render(r, 0, 0, size, size);
    check(r->tile_builds == 0 && r->tile_composites == 0 && r->tile_hits > 0,
          "export setup navigation must reuse tiles without reconstruction");
    *view = saved_view;
    gpu_update_canvas(r, milton.canvas, view);
    gpu_render_to_buffer(&milton, export_after, export_scale, 224, 280, export_w, export_h, 1.0f);
    check((r->flags & RenderBackendFlags_NAVIGATION_PREVIEW) != 0
          && memcmp(view, &saved_view, sizeof(saved_view)) == 0,
          "export must restore view and preserve navigation state");
    int export_worst = 0;
    for ( size_t i = 0; i < export_bytes; ++i ) {
        int diff = abs((int)export_before[i] - (int)export_after[i]);
        if ( diff > export_worst ) { export_worst = diff; }
    }
    fprintf(stdout, "Tile cache cropped 2x export before/after navigation: worst difference %d LSB\n", export_worst);
    check(export_worst == 0, "cropped scaled export must be pixel-identical after tile activity");
    mlt_free(export_before, "Tile export test");
    mlt_free(export_after, "Tile export test");
    r->flags &= ~RenderBackendFlags_NAVIGATION_PREVIEW;
    aligned("after export");

    memcpy(before, tiled, bytes);
    Stroke* edited = get(&lower->strokes, 0);
    gpu_free_strokes(edited, 1, r);
    edited->brush.color = to_premultiplied(v3f{0.95f, 0.65f, 0.05f}, edited->brush.alpha);
    if ( draw_tiles("edit earlier stroke") ) {
        check(r->tile_builds > 0, "editing an earlier stroke must rebuild affected tiles");
        changed("editing an earlier stroke must change pixels");
        compare("edit earlier stroke");
    }
    memcpy(before, tiled, bytes);
    Stroke undone = pop(&upper->strokes);
    if ( draw_tiles("undo") ) {
        changed("undo must change pixels");
        compare("undo");
    }
    memcpy(before, tiled, bytes);
    push(&upper->strokes, undone);
    if ( draw_tiles("redo") ) {
        changed("redo must change pixels");
        compare("redo");
    }
    memcpy(before, tiled, bytes);
    lower->alpha = 0.4f;
    if ( draw_tiles("layer alpha") ) { changed("layer alpha must change pixels"); compare("layer alpha"); }
    memcpy(before, tiled, bytes);
    upper->flags &= ~LayerFlags_VISIBLE;
    if ( draw_tiles("layer visibility") ) { changed("hiding layer must change pixels"); compare("layer visibility"); }
    upper->flags |= LayerFlags_VISIBLE;
    aligned("restore layer visibility");
    memcpy(before, tiled, bytes);
    LayerEffect effect = {};
    effect.type = LayerEffectType_BLUR;
    effect.enabled = true;
    effect.blur.original_scale = (i32)view->scale;
    effect.blur.kernel_size = 5;
    upper->effects = &effect;
    if ( draw_tiles("enabled layer blur effect") ) {
        changed("enabled layer blur effect must change pixels");
        compare("enabled layer blur effect");
    }
    upper->effects = NULL;
    aligned("disabled layer blur effect");
    memcpy(before, tiled, bytes);
    milton.canvas->root_layer = upper;
    upper->prev = NULL;
    upper->next = lower;
    lower->prev = upper;
    lower->next = NULL;
    if ( draw_tiles("layer order") ) { changed("layer order must change pixels"); compare("layer order"); }

    Stroke* working = &milton.working_stroke;
    working->layer_id = upper->id;
    working->num_points = 2;
    working->points[0] = raster_to_canvas(view, {290, 405});
    working->points[1] = raster_to_canvas(view, {440, 435});
    working->pressures[0] = 0.35f;
    working->pressures[1] = 1;
    working->flags = StrokeFlag_PRESERVE_OPACITY | StrokeFlag_PRESSURE_TO_OPACITY;
    working->brush = default_brush();
    working->brush.radius = 24 * view->scale;
    working->brush.alpha = 0.7f;
    working->brush.pressure_opacity_min = 0;
    working->brush.pressure_size = 0;
    working->brush.color = to_premultiplied(v3f{0.1f, 0.8f, 0.25f}, working->brush.alpha);
    working->bounding_rect = bounding_box_for_stroke(working);
    memcpy(before, tiled, bytes);
    // Drawing must finish its dirty tiles even when the ordinary time budget is enabled.
    r->tile_cache_unlimited = false;
    if ( draw_tiles("working preserve") ) {
        check(r->tile_builds > 0 && r->tile_hits > 0 && !gpu_tile_cache_pending(r),
              "working preserve must rebuild dirty tiles immediately and reuse others");
        changed("working preserve must change pixels");
        compare("working preserve");
    }
    memcpy(before, tiled, bytes);
    working->points[1].y += 35 * view->scale;
    working->bounding_rect = bounding_box_for_stroke(working);
    if ( draw_tiles("moved working preserve") ) {
        check(r->tile_builds > 0 && r->tile_hits > 0 && !gpu_tile_cache_pending(r),
              "moving working preserve must rebuild dirty tiles immediately");
        changed("moving working preserve must change pixels");
        compare("moved working preserve");
    }
    memcpy(before, tiled, bytes);
    working->flags = StrokeFlag_BLUR;
    working->brush.radius = 52 * view->scale;
    working->brush.alpha = 0.9f;
    working->bounding_rect = bounding_box_for_stroke(working);
    if ( draw_tiles("working blur") ) {
        check(r->tile_builds > 0 && r->tile_hits > 0 && !gpu_tile_cache_pending(r),
              "working blur must rebuild dirty tiles immediately");
        changed("working blur must change pixels");
        compare("working blur");
    }
    memcpy(before, tiled, bytes);
    working->points[1].x += 45 * view->scale;
    working->bounding_rect = bounding_box_for_stroke(working);
    if ( draw_tiles("moved working blur") ) {
        check(r->tile_builds > 0 && r->tile_hits > 0 && !gpu_tile_cache_pending(r),
              "moving working blur must rebuild dirty tiles immediately");
        changed("moving working blur must change pixels");
        compare("moved working blur");
    }
    reset_working_stroke(&milton);
    r->tile_cache_unlimited = true;
    aligned("after working stroke");
    gpu_clear_tile_cache(r);
    r->flags |= RenderBackendFlags_NAVIGATION_PREVIEW;
    for ( int i = 0; i < 3; ++i ) {
        clip_scene();
        gpu_render(r, 0, 0, size, size);
        check(gpu_tile_cache_pending(r) && r->tile_builds == 0 && r->tile_composites == 0,
              "held navigation must remain pending without building or recompositing tiles");
    }
    check(r->tiles && r->tiles->worker.fbo == 0 && r->tiles->worker.canvas_texture == 0,
          "cold held navigation must not allocate a worker workspace");
    gpu_clear_tile_cache(r);
    glBindFramebufferEXT(GL_FRAMEBUFFER, r->fbo);
    check(glCheckFramebufferStatusEXT(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE,
          "clearing a preview-only cache must preserve the main framebuffer");
    r->flags &= ~RenderBackendFlags_NAVIGATION_PREVIEW;
    r->tile_cache_unlimited = false;
    bool complete = false;
    bool saw_pending = false;
    int total_builds = 0;
    int frames = 0;
    for ( ; frames < 256; ++frames ) {
        clip_scene();
        gpu_render(r, 0, 0, size, size);
        total_builds += r->tile_builds;
        if ( !gpu_tile_cache_pending(r) ) {
            complete = true;
            ++frames;
            read(tiled);
            break;
        }
        saw_pending = true;
    }
    fprintf(stdout, "Tile cache main-route refinement: %d frames, %d builds, pending observed: %s\n",
            frames, total_builds, saw_pending ? "yes" : "no");
    check(complete && total_builds > 0, "main-route budgeted tile refinement did not build and complete within 256 frames");
    if ( complete ) {
        compare("budgeted completion");
        clip_scene();
        gpu_render(r, 0, 0, size, size);
        read(tiled);
        check(!gpu_tile_cache_pending(r) && r->tile_builds == 0
              && r->tile_composites == 0 && r->tile_hits > 0,
              "completed main-route render must reuse warm tiles");
        compare("budgeted warm redraw");
    }
    r->tile_cache_unlimited = true;

    // Save, alter the scene, then load over the warm cache.
    char filename[128];
    snprintf(filename, sizeof(filename), "TEST_tile_cache_%llu.mlt", (unsigned long long)SDL_GetPerformanceCounter());
    PATH_CHAR path[128] = {};
    str_to_path_char(filename, path, sizeof(path));
    FILE* existing = platform_fopen(path, TO_PATH_STR("rb"));
    check(!existing, "refusing to overwrite an existing tile test file");
    if ( existing ) {
        fclose(existing);
    } else {
        milton.persist->mlt_file_path = path;
        milton_save(&milton);
        bool saved = !(milton.flags & MiltonStateFlags_LAST_SAVE_FAILED);
        check(saved, "saving tile test scene failed");
        if ( saved ) {
            memcpy(before, tiled, bytes);
            upper->flags &= ~LayerFlags_VISIBLE;
            aligned("before load");
            changed("pre-load scene must differ from saved scene");
            milton_load(&milton);
            gpu_update_canvas(r, milton.canvas, view);
            if ( draw_tiles("load") ) {
                check(r->tile_builds > 0, "load must clear cached tiles");
                compare("load");
                int worst = 0;
                for ( size_t i = 0; i < bytes; ++i ) {
                    int diff = abs((int)before[i] - (int)tiled[i]);
                    if ( diff > worst ) { worst = diff; }
                }
                fprintf(worst <= 2 ? stdout : stderr, "Tile cache save/load: worst difference %d LSB\n", worst);
                check(worst <= 2, "load must restore saved tile pixels");
            }
        }
        remove(filename);
    }

    gpu_clear_tile_cache(r);
    if ( draw_tiles("explicit cache clear") ) {
        check(r->tile_builds > 0, "explicit clear must force rebuilding");
        compare("explicit cache clear");
    }
    gpu_free_strokes(r, milton.canvas);
    if ( draw_tiles("free all strokes") ) {
        check(r->tile_builds > 0, "freeing all strokes must clear cached tiles");
        compare("free all strokes");
    }
    milton_reset_canvas_and_set_default(&milton);
    view->screen_size = {size, size};
    view->zoom_center = {size / 2, size / 2};
    view->pan_center = {0, 0};
    view->scale = 1024;
    gpu_update_canvas(r, milton.canvas, view);
    if ( draw_tiles("reset") ) {
        check(r->tile_builds > 0, "canvas reset must clear cached tiles");
        changed("canvas reset must remove saved paint");
        compare("reset");
    }

    // Reset followed by explicit cache clearance must permit a cold empty-canvas render.
    fprintf(stdout, "Tile cache blank reset-clear cold render\n");
    gpu_clear_tile_cache(r);
    gpu_update_canvas(r, milton.canvas, view);
    if ( draw_tiles("blank reset-clear cold render") ) {
        check(r->tile_builds > 0 && !gpu_tile_cache_pending(r),
              "blank reset-clear cold render must rebuild and complete");
        compare("blank reset-clear cold render");
    }

    // Visit enough disjoint tile positions to force eviction under the pool cap.
    bool pool_rendered = true;
    for ( int i = 0; i < 40; ++i ) {
        view->pan_center = {(i64)i * 256 * view->scale, (i64)i * 256 * view->scale};
        gpu_update_canvas(r, milton.canvas, view);
        clip();
        if ( !gpu_render_tile_cache(r, 0, 0, size, size) || gpu_tile_cache_pending(r) ) {
            check(false, "unlimited pool-pressure render did not complete");
            pool_rendered = false;
            break;
        }
        u64 pool_bytes = (u64)r->tiles->entries.count * CANVAS_TILE_BYTES;
        if ( pool_bytes > CANVAS_TILE_BUDGET ) {
            check(false, "tile pool exceeded its byte budget");
            pool_rendered = false;
            break;
        }
    }
    if ( pool_rendered ) {
        CanvasTileCache* cache = r->tiles;
        u64 pool_bytes = (u64)cache->entries.count * CANVAS_TILE_BYTES;
        u64 workspace_bytes = (u64)cache->worker.width * cache->worker.height * 4;
        u64 workspace_max = workspace_bytes
            ? workspace_bytes * (11 + min((u64)OPACITY_HISTORY_CACHE_SLOTS,
                                          OPACITY_HISTORY_CACHE_BYTES / workspace_bytes))
            : 0;
        fprintf(stdout, "Tile cache pool: %llu / %llu bytes; workspace allowance: %llu / %llu bytes\n",
                (unsigned long long)pool_bytes, (unsigned long long)CANVAS_TILE_BUDGET,
                (unsigned long long)workspace_max, (unsigned long long)CANVAS_TILE_WORKSPACE_BUDGET);
        check((u64)(cache->entries.count + 1) * CANVAS_TILE_BYTES > CANVAS_TILE_BUDGET,
              "pool-pressure test must reach eviction");
        check(workspace_bytes > 0 && workspace_max <= CANVAS_TILE_WORKSPACE_BUDGET,
              "workspace allowance must remain within its byte budget");
        if ( draw_tiles("pool-pressure warm redraw") ) {
            check(r->tile_builds == 0 && r->tile_composites == 0 && r->tile_hits > 0,
                  "warm redraw after eviction must reuse tiles");
        }
    }

    GLenum error = glGetError();
    if ( error != GL_NO_ERROR ) {
        fprintf(stderr, "Tile cache: OpenGL error 0x%x\n", error);
        passed = false;
    }
    mlt_free(before, "Tile cache test");
    mlt_free(reference, "Tile cache test");
    mlt_free(tiled, "Tile cache test");
    gpu_free_strokes(r, milton.canvas);
    gpu_release_data(r);
    arena_free(&milton.root_arena);
    SDL_GL_DeleteContext(context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    fprintf(passed ? stdout : stderr, "Tile cache integration: %s\n", passed ? "passed" : "FAILED");
    return passed;
}
