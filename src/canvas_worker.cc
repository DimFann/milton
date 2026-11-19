// Included by renderer.cc. Only complete, immutable textures cross GL contexts.

struct CanvasSnapshotStroke
{
    SDL_atomic_t refs;
    TileStrokeKey key;
    Stroke data;
};

struct CanvasSnapshotLayer
{
    Layer* source;
    i32 id, flags;
    float alpha;
    DArray<CanvasSnapshotStroke*> strokes;
    DArray<ReplayCacheEffect> effects;
};

struct CanvasSnapshot
{
    SDL_atomic_t refs;
    DArray<CanvasSnapshotLayer> layers;
    CanvasSnapshotStroke* working;
};

struct CanvasWorkerLayer
{
    Layer layer;
    Layer* source;
    bool used;
    i64 slots;
    DArray<CanvasSnapshotStroke*> previous;
    DArray<LayerEffect> effects;
};

struct CanvasRasterPreview
{
    GLuint texture;
    CanvasView view;
    u64 touched;
    bool preview, selected;
};

#if defined(TESTING)
struct CanvasWorkerProfile
{
    double install_ms, render_ms, publish_ms;
    int builds, composites, batches, replay_hits, blur_runs;
};

static double
canvas_profile_elapsed(u64 start)
{
    return 1000.0 * (SDL_GetPerformanceCounter() - start) / SDL_GetPerformanceFrequency();
}
#endif

struct CanvasWorker
{
    SDL_Window* window;
    SDL_GLContext context;
    SDL_Thread* thread;
    SDL_mutex* mutex;
    SDL_cond* condition;
    SDL_atomic_t generation, minimum_generation, stop, status, result_ready;
    Uint32 wake_event;
    CanvasSnapshot* pending;
    CanvasView pending_view;
    int pending_generation;
    bool pending_preview;
    u64 pending_scene;
    GLuint result;
    int result_generation;
    CanvasView result_view;
    bool result_preview;
    u64 result_scene;
    CanvasSnapshot* scene;           // Main-thread-owned reference and request state.
    CanvasView requested_view;
    int requested_generation;
    bool requested;
    bool requested_preview;
    GLuint front;
    int front_generation;
    GLuint preview;
    CanvasView preview_view;
    CanvasView front_view;
    u64 scene_revision, front_scene, preview_scene;
    DArray<CanvasRasterPreview> history;
    DArray<u8> coverage;
    u64 history_bytes, history_clock;
    CanvasView initial_view;
#if defined(TESTING)
    SDL_atomic_t hold, entered, completed, render_thread, hold_after_completion, reference_only;
    CanvasWorkerProfile profile;
#endif
};

static u64
canvas_raster_bytes(const CanvasView& view)
{
    return (u64)view.screen_size.w * view.screen_size.h * 4;
}

static void
canvas_raster_remove(CanvasWorker* worker, i64 index)
{
    CanvasRasterPreview& frame = worker->history[index];
    worker->history_bytes -= canvas_raster_bytes(frame.view);
    glDeleteTextures(1, &frame.texture);
    for ( i64 i = index + 1; i < worker->history.count; ++i ) {
        worker->history[i - 1] = worker->history[i];
    }
    --worker->history.count;
}

static void
canvas_raster_trim(CanvasWorker* worker, u64 budget)
{
    while ( worker->history_bytes > budget ) {
        i64 oldest = 0;
        for ( i64 i = 1; i < worker->history.count; ++i ) {
            if ( worker->history[i].touched < worker->history[oldest].touched ) { oldest = i; }
        }
        canvas_raster_remove(worker, oldest);
    }
}

static void
canvas_raster_retain(RenderBackend* r, GLuint* texture, const CanvasView& view, bool preview, u64 scene)
{
    CanvasWorker* worker = r->canvas_worker;
    u64 bytes = canvas_raster_bytes(view);
    if ( !*texture ) { return; }
    if ( scene != worker->scene_revision || bytes > r->navigation_cache_budget ) {
        glDeleteTextures(1, texture);
    } else {
        for ( i64 i = worker->history.count; i-- > 0; ) {
            if ( memcmp(&worker->history[i].view, &view, sizeof(view)) == 0 ) {
                if ( preview && !worker->history[i].preview ) {
                    glDeleteTextures(1, texture); *texture = 0;
                    return;
                }
                canvas_raster_remove(worker, i);
            }
        }
        canvas_raster_trim(worker, r->navigation_cache_budget - bytes);
        CanvasRasterPreview frame = {*texture, view, ++worker->history_clock, preview, false};
        push(&worker->history, frame);
        worker->history_bytes += bytes;
    }
    *texture = 0;
}

void
gpu_set_navigation_cache_budget(RenderBackend* r, u64 bytes)
{
    r->navigation_cache_budget = bytes;
    if ( r->canvas_worker ) { canvas_raster_trim(r->canvas_worker, bytes); }
}

// Fully covered screen blocks need no older textures. Test pixel centers so
// rotated/mirrored edges never claim pixels outside their actual raster.
static bool
canvas_raster_cover(RenderBackend* r, const CanvasView& view, int columns, int* remaining)
{
    if ( !*remaining ) { return false; }
    CanvasWorker* worker = r->canvas_worker;
    float vertices[16];
    gpu_view_texture_vertices(r, view, vertices);
    v2f points[4];
    float left = (float)r->width, right = 0, bottom = (float)r->height, top = 0;
    for ( int i = 0; i < 4; ++i ) {
        points[i] = {(vertices[4 * i] + 1) * r->width / 2,
                     (vertices[4 * i + 1] + 1) * r->height / 2};
        left = min(left, points[i].x); right = max(right, points[i].x);
        bottom = min(bottom, points[i].y); top = max(top, points[i].y);
    }
    if ( right <= 0 || left >= r->width || top <= 0 || bottom >= r->height ) { return false; }
    const int block = 32;
    int x0 = (int)(max(0.0f, left) / block);
    int x1 = (int)(min((float)r->width - 1, right) / block);
    int y0 = (int)(max(0.0f, bottom) / block);
    int y1 = (int)(min((float)r->height - 1, top) / block);
    bool needed = false;
    for ( int y = y0; y <= y1; ++y ) {
        for ( int x = x0; x <= x1; ++x ) {
            if ( worker->coverage[y * columns + x] ) { continue; }
            needed = true;
            bool inside = true;
            for ( int corner = 0; corner < 4 && inside; ++corner ) {
                v2f p = {corner & 1 ? min((x + 1) * block, r->width) - 0.5f : x * block + 0.5f,
                         corner & 2 ? min((y + 1) * block, r->height) - 0.5f : y * block + 0.5f};
                bool positive = false, negative = false;
                for ( int edge = 0; edge < 4; ++edge ) {
                    v2f a = points[edge], b = points[(edge + 1) % 4];
                    float cross = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
                    positive |= cross > 0; negative |= cross < 0;
                }
                inside = !(positive && negative);
            }
            if ( inside ) { worker->coverage[y * columns + x] = 1; --*remaining; }
        }
    }
    return needed;
}

static void
canvas_raster_present(RenderBackend* r)
{
    CanvasWorker* worker = r->canvas_worker;
    CanvasTileCache* cache = r->tiles;
    int columns = (r->width + 31) / 32, rows = (r->height + 31) / 32;
    reset(&worker->coverage);
    for ( int i = 0; i < columns * rows; ++i ) { push(&worker->coverage, (u8)0); }
    int remaining = columns * rows;
    ++worker->history_clock;
    if ( cache->navigation_valid ) { canvas_raster_cover(r, cache->navigation_view, columns, &remaining); }
    for ( i64 i = worker->history.count; i-- > 0; ) {
        CanvasRasterPreview& frame = worker->history[i];
        frame.selected = !frame.preview && canvas_raster_cover(r, frame.view, columns, &remaining);
    }
    bool preview = worker->preview && canvas_raster_cover(r, worker->preview_view, columns, &remaining);
    for ( i64 i = worker->history.count; i-- > 0; ) {
        CanvasRasterPreview& frame = worker->history[i];
        if ( frame.preview ) { frame.selected = canvas_raster_cover(r, frame.view, columns, &remaining); }
        if ( frame.selected ) { frame.touched = worker->history_clock; }
    }
    for ( int quality = 0; quality < 2; ++quality ) {
        for ( CanvasRasterPreview& frame : worker->history ) {
            if ( frame.selected && frame.preview == (quality == 0) ) { gpu_draw_view_texture(r, frame.texture, frame.view); }
        }
        if ( quality == 0 && preview ) { gpu_draw_view_texture(r, worker->preview, worker->preview_view); }
    }
    if ( cache->navigation_valid ) { gpu_draw_view_texture(r, cache->navigation_texture, cache->navigation_view); }
}

static void
canvas_snapshot_stroke_release(CanvasSnapshotStroke* stroke)
{
    if ( stroke && SDL_AtomicDecRef(&stroke->refs) ) {
        free(stroke->data.points);
        free(stroke->data.pressures);
#if STROKE_DEBUG_VIZ
        free(stroke->data.debug_flags);
#endif
        free(stroke);
    }
}

static void
canvas_snapshot_release(CanvasSnapshot* scene)
{
    if ( !scene || !SDL_AtomicDecRef(&scene->refs) ) { return; }
    for ( CanvasSnapshotLayer& layer : scene->layers ) {
        for ( CanvasSnapshotStroke* stroke : layer.strokes ) { canvas_snapshot_stroke_release(stroke); }
        release(&layer.strokes);
        release(&layer.effects);
    }
    release(&scene->layers);
    canvas_snapshot_stroke_release(scene->working);
    free(scene);
}

static bool
canvas_snapshot_stroke_matches(CanvasSnapshotStroke* previous, Stroke* stroke, i64 position, bool working)
{
    if ( !previous || !tile_key_equal(previous->key, tile_stroke_key(stroke, position)) ) { return false; }
    return !working || (memcmp(previous->data.points, stroke->points, stroke->num_points * sizeof(v2l)) == 0
                        && memcmp(previous->data.pressures, stroke->pressures, stroke->num_points * sizeof(f32)) == 0);
}

static CanvasSnapshotStroke*
canvas_snapshot_stroke(Stroke* stroke, i64 position, CanvasSnapshotStroke* previous, bool working = false)
{
    if ( canvas_snapshot_stroke_matches(previous, stroke, position, working) ) {
        SDL_AtomicIncRef(&previous->refs);
        return previous;
    }
    CanvasSnapshotStroke* copy = (CanvasSnapshotStroke*)calloc(1, sizeof(*copy));
    if ( !copy ) { milton_die_gracefully("Cannot allocate canvas stroke snapshot."); }
    SDL_AtomicSet(&copy->refs, 1);
    copy->key = tile_stroke_key(stroke, position);
    copy->data = *stroke;
    copy->data.render_handle = 0;
    size_t count = (size_t)stroke->num_points;
    copy->data.points = (v2l*)calloc(count, sizeof(v2l));
    copy->data.pressures = (f32*)calloc(count, sizeof(f32));
    if ( !copy->data.points || !copy->data.pressures ) {
        milton_die_gracefully("Cannot allocate canvas sample snapshot.");
    }
    memcpy(copy->data.points, stroke->points, count * sizeof(v2l));
    memcpy(copy->data.pressures, stroke->pressures, count * sizeof(f32));
#if STROKE_DEBUG_VIZ
    copy->data.debug_flags = (int*)calloc(count, sizeof(int));
    if ( !copy->data.debug_flags ) { milton_die_gracefully("Cannot allocate stroke debug snapshot."); }
    if ( stroke->debug_flags ) { memcpy(copy->data.debug_flags, stroke->debug_flags, count * sizeof(int)); }
#endif
    return copy;
}

static bool
canvas_snapshot_matches(CanvasSnapshot* scene, RenderBackend* r)
{
    if ( !scene ) { return false; }
    i64 index = 0;
    for ( Layer* layer = r->tile_root; layer; layer = layer->next, ++index ) {
        if ( index >= scene->layers.count ) { return false; }
        CanvasSnapshotLayer& old = scene->layers[index];
        if ( old.source != layer || old.id != layer->id || old.flags != layer->flags
             || old.alpha != layer->alpha || old.strokes.count != layer->strokes.count ) { return false; }
        StrokeIterator iterator = {};
        i64 position = 0;
        for ( Stroke* stroke = stroke_iter_init(&layer->strokes, &iterator); stroke;
              stroke = stroke_iter_next(&iterator), ++position ) {
            if ( !canvas_snapshot_stroke_matches(old.strokes[position], stroke, position, false) ) { return false; }
        }
        i64 effect_index = 0;
        for ( LayerEffect* effect = layer->effects; effect; effect = effect->next, ++effect_index ) {
            if ( effect_index >= old.effects.count ) { return false; }
            ReplayCacheEffect key = {effect->type, effect->enabled, effect->blur.original_scale, effect->blur.kernel_size};
            if ( memcmp(&key, &old.effects[effect_index], sizeof(key)) != 0 ) { return false; }
        }
        if ( effect_index != old.effects.count ) { return false; }
    }
    if ( index != scene->layers.count ) { return false; }
    return r->tile_working->num_points
        ? canvas_snapshot_stroke_matches(scene->working, r->tile_working, 0, true)
        : scene->working == NULL;
}

static CanvasSnapshot*
canvas_snapshot_create(RenderBackend* r, CanvasSnapshot* previous)
{
    CanvasSnapshot* scene = (CanvasSnapshot*)calloc(1, sizeof(*scene));
    if ( !scene ) { milton_die_gracefully("Cannot allocate canvas snapshot."); }
    SDL_AtomicSet(&scene->refs, 1);
    for ( Layer* source = r->tile_root; source; source = source->next ) {
        CanvasSnapshotLayer layer = {};
        layer.source = source; layer.id = source->id; layer.flags = source->flags; layer.alpha = source->alpha;
        CanvasSnapshotLayer* old = NULL;
        if ( previous ) {
            for ( CanvasSnapshotLayer& candidate : previous->layers ) {
                if ( candidate.source == source && candidate.id == source->id ) { old = &candidate; break; }
            }
        }
        StrokeIterator iterator = {};
        i64 position = 0;
        for ( Stroke* stroke = stroke_iter_init(&source->strokes, &iterator); stroke;
              stroke = stroke_iter_next(&iterator), ++position ) {
            CanvasSnapshotStroke* prior = old && position < old->strokes.count ? old->strokes[position] : NULL;
            push(&layer.strokes, canvas_snapshot_stroke(stroke, position, prior));
        }
        for ( LayerEffect* effect = source->effects; effect; effect = effect->next ) {
            push(&layer.effects, ReplayCacheEffect{effect->type, effect->enabled,
                                                  effect->blur.original_scale, effect->blur.kernel_size});
        }
        push(&scene->layers, layer);
    }
    if ( r->tile_working->num_points ) {
        scene->working = canvas_snapshot_stroke(r->tile_working, 0, previous ? previous->working : NULL, true);
    }
    return scene;
}

static bool
canvas_snapshot_continues_drawing(CanvasSnapshot* previous, CanvasSnapshot* scene)
{
    if ( !previous || previous->layers.count != scene->layers.count ) { return false; }
    bool committed = false;
    for ( i64 i = 0; i < scene->layers.count; ++i ) {
        CanvasSnapshotLayer& old = previous->layers[i];
        CanvasSnapshotLayer& next = scene->layers[i];
        if ( old.source != next.source || old.id != next.id || old.flags != next.flags
             || old.alpha != next.alpha || old.effects.count != next.effects.count
             || next.strokes.count < old.strokes.count ) { return false; }
        for ( i64 j = 0; j < old.effects.count; ++j ) {
            if ( memcmp(&old.effects[j], &next.effects[j], sizeof(ReplayCacheEffect)) != 0 ) { return false; }
        }
        for ( i64 j = 0; j < old.strokes.count; ++j ) {
            if ( old.strokes[j] != next.strokes[j] ) { return false; }
        }
        if ( next.strokes.count != old.strokes.count ) {
            if ( previous->working ) {
                if ( committed || previous->working->data.layer_id != next.id
                     || next.strokes.count != old.strokes.count + 1 ) { return false; }
                Stroke& stroke = next.strokes[old.strokes.count]->data;
                Stroke& working = previous->working->data;
                if ( stroke.flags != working.flags
                     || memcmp(&stroke.brush, &working.brush, sizeof(Brush)) != 0 ) { return false; }
            }
            committed = true;
        }
    }
    if ( !committed && previous->working && scene->working ) {
        Stroke& old = previous->working->data;
        Stroke& next = scene->working->data;
        if ( old.layer_id != next.layer_id || old.flags != next.flags || next.num_points < old.num_points
             || memcmp(&old.brush, &next.brush, sizeof(Brush)) != 0 ) { return false; }
    }
    // Removing an uncommitted stroke must discard frames that still contain it.
    return committed || scene->working != NULL;
}

static void
canvas_worker_cancel(CanvasWorker* worker)
{
    if ( !worker->requested ) { return; }
    int generation = SDL_AtomicAdd(&worker->generation, 1) + 1;
    SDL_AtomicSet(&worker->minimum_generation, generation);
    worker->requested = false;
    SDL_LockMutex(worker->mutex);
    CanvasSnapshot* pending = worker->pending;
    worker->pending = NULL;
    SDL_UnlockMutex(worker->mutex);
    canvas_snapshot_release(pending);
}

static void
canvas_worker_invalidate(RenderBackend* r)
{
    if ( !r->canvas_worker ) { return; }
    CanvasWorker* worker = r->canvas_worker;
    canvas_worker_cancel(worker);
    ++worker->scene_revision;
    canvas_raster_trim(worker, 0);
    canvas_snapshot_release(worker->scene);
    worker->scene = NULL;
}

static bool
canvas_worker_has_result(RenderBackend* r)
{
    return SDL_AtomicGet(&r->canvas_worker->result_ready)
           || SDL_AtomicGet(&r->canvas_worker->status) < 0;
}

static Layer*
canvas_worker_install(RenderBackend* r, Arena* arena, DArray<CanvasWorkerLayer*>* owned,
                      CanvasSnapshot* scene, Stroke* working, CanvasSnapshotStroke* old_working, u64* revision)
{
    for ( CanvasWorkerLayer* layer : *owned ) { layer->used = false; }
    Layer* root = NULL;
    Layer* last = NULL;
    for ( CanvasSnapshotLayer& snapshot : scene->layers ) {
        CanvasWorkerLayer* target = NULL;
        for ( CanvasWorkerLayer* candidate : *owned ) {
            if ( candidate->source == snapshot.source && candidate->layer.id == snapshot.id ) {
                target = candidate; break;
            }
        }
        if ( !target ) {
            for ( CanvasWorkerLayer* candidate : *owned ) {
                if ( !candidate->used && candidate->layer.strokes.count == 0 ) { target = candidate; break; }
            }
        }
        if ( !target ) {
            target = (CanvasWorkerLayer*)calloc(1, sizeof(*target));
            if ( !target ) { milton_die_gracefully("Cannot allocate worker layer."); }
            target->layer.strokes.arena = arena;
            push(owned, target);
        }
        if ( target->source != snapshot.source || target->layer.id != snapshot.id ) {
            target->source = snapshot.source;
            target->layer.id = snapshot.id;
        }
        target->used = true;
        Layer* layer = &target->layer;
        for ( i64 i = snapshot.strokes.count; i < layer->strokes.count; ++i ) {
            gpu_free_strokes(get(&layer->strokes, i), 1, r);
        }
        i64 old_count = layer->strokes.count;
        reset(&layer->strokes);
        for ( i64 i = 0; i < snapshot.strokes.count; ++i ) {
            CanvasSnapshotStroke* payload = snapshot.strokes[i];
            Stroke old = i < target->slots ? *get(&layer->strokes, i) : Stroke{};
            bool same = i < target->previous.count && target->previous[i] == payload;
            if ( !same && i < old_count ) { gpu_free_strokes(&old, 1, r); }
            Stroke copy = payload->data;
            copy.render_handle = old.render_handle;
            copy.content_revision = same ? old.content_revision : ++*revision;
            push(&layer->strokes, copy);
        }
        target->slots = max(target->slots, snapshot.strokes.count);
        tile_assign(&target->previous, snapshot.strokes);
        reset(&target->effects);
        for ( const ReplayCacheEffect& effect : snapshot.effects ) {
            LayerEffect copy = {};
            copy.type = effect.type; copy.enabled = effect.enabled;
            copy.blur.original_scale = effect.scale; copy.blur.kernel_size = effect.kernel;
            push(&target->effects, copy);
        }
        for ( i64 i = 0; i < target->effects.count; ++i ) {
            target->effects[i].next = i + 1 < target->effects.count ? &target->effects[i + 1] : NULL;
        }
        layer->effects = target->effects.count ? &target->effects[0] : NULL;
        layer->flags = snapshot.flags; layer->alpha = snapshot.alpha;
        layer->prev = last; layer->next = NULL;
        if ( last ) { last->next = layer; } else { root = layer; }
        last = layer;
    }
    for ( CanvasWorkerLayer* layer : *owned ) {
        if ( !layer->used ) {
            for ( i64 i = 0; i < layer->layer.strokes.count; ++i ) {
                gpu_free_strokes(get(&layer->layer.strokes, i), 1, r);
            }
            reset(&layer->layer.strokes);
            reset(&layer->previous);
        }
    }
    if ( old_working != scene->working ) { gpu_free_strokes(working, 1, r); }
    RenderHandle handle = working->render_handle;
    *working = scene->working ? scene->working->data : Stroke{};
    working->render_handle = handle;
    return root;
}

static void
canvas_worker_release_renderer(RenderBackend* r)
{
    gpu_release_data(r);
    GLuint textures[] = {r->canvas_texture, r->eraser_texture, r->helper_texture, r->stencil_texture,
                         r->stroke_info_texture, r->opacity_stroke_texture, r->opacity_history_texture,
                         r->blur_brush_textures[0], r->blur_brush_textures[1]};
    glDeleteTextures(array_count(textures), textures);
    GLuint buffers[] = {r->vbo_screen_quad, r->vbo_screen_uv, r->vbo_picker, r->vbo_picker_norm,
                        r->vbo_outline, r->vbo_outline_sizes, r->vbo_exporter, r->exporter_indices};
    for ( GLuint buffer : buffers ) { if ( buffer ) { DEBUG_gl_unmark_buffer(buffer); } }
    glDeleteBuffers(array_count(buffers), buffers);
    glDeleteFramebuffersEXT(1, &r->fbo);
    if ( r->vao ) { glDeleteVertexArrays(1, &r->vao); }
    GLuint programs[] = {r->stroke_program, r->stroke_eraser_program, r->stroke_clear_program,
        r->stroke_info_program, r->stroke_fill_program_pressure, r->stroke_fill_program_distance,
        r->stroke_fill_program_pressure_distance, r->quad_program, r->picker_program,
        r->layer_blend_program, r->outline_program, r->exporter_program, r->texture_fill_program,
        r->postproc_program, r->blur_program, r->blur_brush_program
#if MILTON_DEBUG
        , r->simple_program
#endif
    };
    for ( GLuint program : programs ) {
        if ( !program ) { continue; }
        GLuint shaders[8] = {};
        GLsizei count = 0;
        glGetAttachedShaders(program, array_count(shaders), &count, shaders);
        for ( GLsizei i = 0; i < count; ++i ) { glDeleteShader(shaders[i]); }
        glDeleteProgram(program);
    }
}

static void
canvas_worker_wake(CanvasWorker* worker)
{
    SDL_Event event = {};
    event.type = worker->wake_event;
    if ( SDL_PushEvent(&event) != 1 ) {
        milton_log("Cannot queue background canvas completion: %s\n", SDL_GetError());
    }
}

static int
canvas_worker_thread(void* data)
{
    CanvasWorker* worker = (CanvasWorker*)data;
    if ( SDL_GL_MakeCurrent(worker->window, worker->context) != 0 ) {
        milton_log("Cannot activate background canvas context: %s\n", SDL_GetError());
        SDL_LockMutex(worker->mutex);
        SDL_AtomicSet(&worker->status, -1);
        SDL_CondSignal(worker->condition);
        SDL_UnlockMutex(worker->mutex);
        return 1;
    }
    if ( SDL_SetThreadPriority(SDL_THREAD_PRIORITY_LOW) != 0 ) {
        milton_log("Cannot lower canvas worker priority; using normal priority: %s\n", SDL_GetError());
    }
    Arena arena = arena_init(1024 * 1024);
    RenderBackend* r = gpu_allocate_render_backend(&arena);
    r->background_context = true;
    r->cancel_generation = &worker->minimum_generation;
    gpu_init(r, &worker->initial_view, NULL);
    gpu_resize(r, &worker->initial_view);
    GLenum startup_error = glGetError();
    if ( startup_error != GL_NO_ERROR ) {
        milton_log("Background canvas initialization OpenGL error: 0x%x\n", startup_error);
    }
    SDL_LockMutex(worker->mutex);
    SDL_AtomicSet(&worker->status, startup_error == GL_NO_ERROR ? 1 : -1);
    SDL_CondSignal(worker->condition);
    SDL_UnlockMutex(worker->mutex);
    DArray<CanvasWorkerLayer*> layers = {};
    CanvasSnapshot* active = NULL;
    CanvasView installed_view = {};
    bool view_installed = false;
    Stroke working = {};
    u64 revision = 0;
    for ( ;; ) {
        if ( SDL_AtomicGet(&worker->status) < 0 ) { break; }
        SDL_LockMutex(worker->mutex);
        while ( !worker->pending && !SDL_AtomicGet(&worker->stop) ) {
            SDL_CondWait(worker->condition, worker->mutex);
        }
        CanvasSnapshot* scene = worker->pending;
        CanvasView view = worker->pending_view;
        int generation = worker->pending_generation;
        bool preview = worker->pending_preview;
        u64 scene_revision = worker->pending_scene;
        worker->pending = NULL;
        SDL_UnlockMutex(worker->mutex);
        if ( SDL_AtomicGet(&worker->stop) ) { canvas_snapshot_release(scene); break; }
#if defined(TESTING)
        SDL_AtomicSet(&worker->entered, generation);
        SDL_AtomicSet(&worker->render_thread, (int)SDL_ThreadID());
        while ( SDL_AtomicGet(&worker->hold) && !SDL_AtomicGet(&worker->stop) ) { SDL_Delay(1); }
#endif
        if ( generation < SDL_AtomicGet(&worker->minimum_generation) ) { canvas_snapshot_release(scene); continue; }
        if ( preview && view.scale <= INT_MAX / 2 ) {
            // Guard texels cover odd viewport/center rounding when reprojected.
            view.screen_size = {max(1, (view.screen_size.w + 1) / 2) + 2,
                                max(1, (view.screen_size.h + 1) / 2) + 2};
            view.zoom_center = view.zoom_center / 2 + v2i{1, 1};
            view.scale *= 2;
        }
#if defined(TESTING)
        CanvasWorkerProfile profile = {};
        u64 install_started = SDL_GetPerformanceCounter();
#endif
        CanvasState canvas = {};
        canvas.root_layer = canvas_worker_install(r, &arena, &layers, scene, &working,
                                                   active ? active->working : NULL, &revision);
        canvas_snapshot_release(active);
        active = scene;
        if ( r->width != view.screen_size.w || r->height != view.screen_size.h ) { gpu_resize(r, &view); }
        gpu_update_background(r, view.background_color);
        if ( !view_installed || memcmp(&installed_view, &view, sizeof(view)) != 0 ) {
            gpu_update_canvas(r, &canvas, &view);
            installed_view = view;
            view_installed = true;
        }
        r->request_generation = generation;
#if defined(TESTING)
        profile.install_ms = canvas_profile_elapsed(install_started);
#endif
        bool failed = false;
        while ( !SDL_AtomicGet(&worker->stop) && generation >= SDL_AtomicGet(&worker->minimum_generation) ) {
#if defined(TESTING)
            u64 render_started = SDL_GetPerformanceCounter();
            r->replay_cache_hit = false;
            r->blur_filter_runs = 0;
#endif
            gpu_clip_strokes_and_update(&arena, r, &view, view.scale, canvas.root_layer, &working,
                                        0, 0, r->width, r->height, ClipFlags_TILE_SCENE);
            bool use_tiles = true;
#if defined(TESTING)
            use_tiles = SDL_AtomicGet(&worker->reference_only) == 0;
#endif
            bool tiled = !preview && use_tiles && gpu_render_tile_cache(r, 0, 0, r->width, r->height);
            if ( !tiled ) {
                gpu_clip_strokes_and_update(&arena, r, &view, view.scale, canvas.root_layer, &working,
                                            0, 0, r->width, r->height,
                                            preview ? ClipFlags_NAVIGATION_PREVIEW : ClipFlags_JUST_CLIP);
                gpu_render_canvas(r, 0, 0, r->width, r->height, 1, preview);
            }
#if defined(TESTING)
            profile.render_ms += canvas_profile_elapsed(render_started);
            profile.builds += r->tile_builds;
            profile.composites += r->tile_composites;
            profile.replay_hits += r->replay_cache_hit != 0;
            profile.blur_runs += r->blur_filter_runs;
            ++profile.batches;
#endif
            GLenum error = glGetError();
            if ( error != GL_NO_ERROR ) {
                milton_log("Background canvas renderer OpenGL error: 0x%x\n", error);
                failed = true; break;
            }
            if ( generation < SDL_AtomicGet(&worker->minimum_generation) ) { break; }
            bool complete = !tiled || !r->tiles->pending;
            if ( !complete && !r->tiles->display_ready ) { continue; }
#if defined(TESTING)
            u64 publish_started = SDL_GetPerformanceCounter();
#endif
            GLuint texture = gl::new_color_texture(r->width, r->height);
            glBindFramebufferEXT(GL_FRAMEBUFFER, r->fbo);
            glFramebufferTexture2DEXT(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->canvas_texture, 0);
            tile_copy_display_texture(texture, r->width, r->height);
            glFinish();  // Publish only after writes are visible to the presentation context.
#if defined(TESTING)
            profile.publish_ms += canvas_profile_elapsed(publish_started);
#endif
            error = glGetError();
            if ( error != GL_NO_ERROR ) {
                milton_log("Cannot publish background canvas texture: OpenGL error 0x%x\n", error);
                glDeleteTextures(1, &texture);
                failed = true; break;
            }
            SDL_LockMutex(worker->mutex);
            if ( generation >= SDL_AtomicGet(&worker->minimum_generation) && !SDL_AtomicGet(&worker->stop) ) {
                swap(texture, worker->result);
                worker->result_generation = generation;
                worker->result_view = view;
                worker->result_preview = preview;
                worker->result_scene = scene_revision;
                SDL_AtomicSet(&worker->result_ready, 1);
#if defined(TESTING)
                worker->profile = profile;
                if ( complete ) {
                    SDL_AtomicSet(&worker->completed, generation);
                    if ( SDL_AtomicCAS(&worker->hold_after_completion, 1, 0) ) {
                        SDL_AtomicSet(&worker->hold, 1);
                    }
                }
#endif
            }
            SDL_UnlockMutex(worker->mutex);
            if ( texture ) { glDeleteTextures(1, &texture); }
            canvas_worker_wake(worker);
            if ( complete ) { break; }
        }
        if ( failed ) { SDL_AtomicSet(&worker->status, -1); canvas_worker_wake(worker); break; }
    }
    for ( CanvasWorkerLayer* layer : layers ) {
        for ( i64 i = 0; i < layer->layer.strokes.count; ++i ) {
            gpu_free_strokes(get(&layer->layer.strokes, i), 1, r);
        }
        release(&layer->previous); release(&layer->effects);
        free(layer);
    }
    release(&layers);
    gpu_free_strokes(&working, 1, r);
    canvas_snapshot_release(active);
    canvas_worker_release_renderer(r);
    arena_free(&arena);
    if ( SDL_GL_MakeCurrent(worker->window, NULL) != 0 ) {
        milton_log("Cannot release background canvas context: %s\n", SDL_GetError());
    }
    return 0;
}

void
gpu_stop_canvas_worker(RenderBackend* r)
{
    CanvasWorker* worker = r->canvas_worker;
    if ( !worker ) { return; }
    SDL_AtomicSet(&worker->stop, 1);
    int generation = SDL_AtomicAdd(&worker->generation, 1) + 1;
    SDL_AtomicSet(&worker->minimum_generation, generation);
    SDL_LockMutex(worker->mutex);
    SDL_CondSignal(worker->condition);
    SDL_UnlockMutex(worker->mutex);
    if ( worker->thread ) { SDL_WaitThread(worker->thread, NULL); }
    canvas_snapshot_release(worker->pending);
    canvas_snapshot_release(worker->scene);
    GLuint textures[] = {worker->result, worker->front, worker->preview};
    glDeleteTextures(array_count(textures), textures);
    canvas_raster_trim(worker, 0);
    release(&worker->history);
    release(&worker->coverage);
    SDL_GL_DeleteContext(worker->context);
    SDL_DestroyCond(worker->condition);
    SDL_DestroyMutex(worker->mutex);
    free(worker);
    r->canvas_worker = NULL;
    gpu_clear_tile_cache(r);
}

bool
gpu_start_canvas_worker(RenderBackend* r, SDL_Window* window)
{
    if ( r->canvas_worker ) { return true; }
    SDL_GLContext foreground = SDL_GL_GetCurrentContext();
    int sharing = 0;
    if ( SDL_GL_GetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, &sharing) != 0
         || SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1) != 0 ) {
        milton_log("Cannot enable canvas context sharing: %s\n", SDL_GetError());
        return false;
    }
    SDL_GLContext context = SDL_GL_CreateContext(window);
    if ( SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, sharing) != 0 ) {
        milton_log("Cannot restore OpenGL context-sharing attribute: %s\n", SDL_GetError());
    }
    if ( SDL_GL_MakeCurrent(window, foreground) != 0 ) {
        milton_die_gracefully("Cannot restore foreground OpenGL context.");
    }
    if ( !context ) {
        milton_log("Cannot create background canvas context: %s\n", SDL_GetError());
        return false;
    }
    CanvasWorker* worker = (CanvasWorker*)calloc(1, sizeof(*worker));
    if ( !worker ) { milton_die_gracefully("Cannot allocate background canvas worker."); }
    worker->window = window; worker->context = context;
    worker->initial_view = r->tile_view;
    worker->initial_view.screen_size = {r->width, r->height};
    worker->mutex = SDL_CreateMutex();
    worker->condition = SDL_CreateCond();
    worker->wake_event = SDL_RegisterEvents(1);
    if ( !worker->mutex || !worker->condition || worker->wake_event == (Uint32)-1 ) {
        milton_log("Cannot initialize background canvas synchronization: %s\n", SDL_GetError());
        if ( worker->condition ) { SDL_DestroyCond(worker->condition); }
        if ( worker->mutex ) { SDL_DestroyMutex(worker->mutex); }
        SDL_GL_DeleteContext(context); free(worker);
        return false;
    }
    r->canvas_worker = worker;
    worker->thread = SDL_CreateThread(canvas_worker_thread, "Canvas renderer", worker);
    if ( worker->thread ) {
        SDL_LockMutex(worker->mutex);
        while ( SDL_AtomicGet(&worker->status) == 0 ) { SDL_CondWait(worker->condition, worker->mutex); }
        SDL_UnlockMutex(worker->mutex);
    }
    if ( !worker->thread || SDL_AtomicGet(&worker->status) < 0 ) {
        milton_log("Cannot start background canvas renderer: %s\n", SDL_GetError());
        gpu_stop_canvas_worker(r);
        return false;
    }
    gpu_clear_tile_cache(r);
    return true;
}

static bool
canvas_worker_render(RenderBackend* r)
{
    CanvasWorker* worker = r->canvas_worker;
    if ( SDL_AtomicGet(&worker->status) < 0 ) {
        platform_dialog("Background canvas rendering failed. Using foreground rendering; see the log.", "Rendering error");
        gpu_stop_canvas_worker(r);
        return false;
    }
    if ( !r->tiles ) { r->tiles = (CanvasTileCache*)mlt_calloc(1, sizeof(CanvasTileCache), "Canvas tiles"); }
    CanvasTileCache* cache = r->tiles;
#if defined(TESTING)
    r->tile_builds = r->tile_composites = r->tile_hits = 0;
#endif
    bool navigating = (r->flags & RenderBackendFlags_NAVIGATION_PREVIEW) != 0;
    bool scene_control = (r->flags & (RenderBackendFlags_HISTORY_CHANGED
                                     | RenderBackendFlags_SELECTION_TRANSFORMING)) != 0
                         || (worker->requested && r->tile_view.background_color != worker->requested_view.background_color);
    if ( scene_control ) {
        ++worker->scene_revision;
        canvas_raster_trim(worker, 0);
        cache->navigation_valid = false;
        if ( worker->preview ) { glDeleteTextures(1, &worker->preview); worker->preview = 0; }
    }
    bool camera_preview = navigating && !scene_control
                          && ((r->flags & RenderBackendFlags_CANVAS_TRANSFORMING)
                              || cache->navigation_valid || (worker->requested && worker->requested_preview));
    if ( navigating && !camera_preview ) {
        canvas_worker_cancel(worker);
    } else {
        bool changed = !canvas_snapshot_matches(worker->scene, r);
        bool continuing = false;
        if ( changed ) {
            ++worker->scene_revision;
            canvas_raster_trim(worker, 0);
            CanvasSnapshot* scene = canvas_snapshot_create(r, worker->scene);
            continuing = canvas_snapshot_continues_drawing(worker->scene, scene);
            canvas_snapshot_release(worker->scene);
            worker->scene = scene;
        }
        if ( changed && !continuing ) {
            cache->navigation_valid = false;
            if ( worker->preview ) { glDeleteTextures(1, &worker->preview); worker->preview = 0; }
        }
        if ( camera_preview && (!changed || continuing) ) { gpu_capture_navigation(r); }
        bool view_changed = memcmp(&worker->requested_view, &r->tile_view, sizeof(CanvasView)) != 0;
        bool mode_changed = worker->requested_preview != camera_preview;
        if ( changed || !worker->requested || view_changed || mode_changed ) {
            SDL_AtomicAdd(&worker->generation, 1);
            worker->requested_generation = SDL_AtomicGet(&worker->generation);
            bool compatible = camera_preview && worker->requested_preview && (!changed || continuing);
            compatible |= !camera_preview && continuing && !view_changed;
            if ( !compatible || !worker->requested || mode_changed || scene_control ) {
                SDL_AtomicSet(&worker->minimum_generation, worker->requested_generation);
            }
            worker->requested_view = r->tile_view;
            worker->requested = true;
            worker->requested_preview = camera_preview;
            SDL_AtomicIncRef(&worker->scene->refs);
            SDL_LockMutex(worker->mutex);
            CanvasSnapshot* obsolete = worker->pending;
            worker->pending = worker->scene;
            worker->pending_view = r->tile_view;
            worker->pending_generation = worker->requested_generation;
            worker->pending_preview = camera_preview;
            worker->pending_scene = worker->scene_revision;
            SDL_CondSignal(worker->condition);
            SDL_UnlockMutex(worker->mutex);
            canvas_snapshot_release(obsolete);
        }
    }
    SDL_LockMutex(worker->mutex);
    GLuint result = worker->result;
    int generation = worker->result_generation;
    CanvasView result_view = worker->result_view;
    bool result_preview = worker->result_preview;
    u64 result_scene = worker->result_scene;
    worker->result = 0;
    SDL_AtomicSet(&worker->result_ready, 0);
    SDL_UnlockMutex(worker->mutex);
    if ( result ) {
        if ( worker->requested && generation >= SDL_AtomicGet(&worker->minimum_generation) ) {
            if ( result_preview ) {
                canvas_raster_retain(r, &worker->preview, worker->preview_view, true, worker->preview_scene);
                worker->preview = result;
                worker->preview_view = result_view;
                worker->preview_scene = result_scene;
            } else {
                canvas_raster_retain(r, &worker->front, worker->front_view, false, worker->front_scene);
                worker->front = result; worker->front_generation = generation;
                worker->front_view = result_view;
                worker->front_scene = result_scene;
            }
        } else { glDeleteTextures(1, &result); }
    }
    bool ready = !navigating && worker->requested && !worker->requested_preview && worker->front
                 && worker->front_generation >= SDL_AtomicGet(&worker->minimum_generation);
    cache->capture_ready = ready;
    if ( ready ) {
        cache->navigation_valid = false;
        canvas_raster_retain(r, &worker->preview, worker->preview_view, true, worker->preview_scene);
        glBindFramebufferEXT(GL_FRAMEBUFFER, r->fbo);
        glFramebufferTexture2DEXT(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->canvas_texture, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, worker->front);
        glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST);
        gpu_fill_with_texture(r);
    } else if ( camera_preview || cache->navigation_valid ) {
        glBindFramebufferEXT(GL_FRAMEBUFFER, r->fbo);
        glFramebufferTexture2DEXT(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->canvas_texture, 0);
        glViewport(0, 0, r->width, r->height);
        glScissor(0, 0, r->width, r->height);
        glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST);
        glClearColor(r->background_color.r, r->background_color.g, r->background_color.b, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        canvas_raster_present(r);
    } else {
        gpu_set_cook_budget(r, 0.002f);
        gpu_clip_strokes_and_update(r->tile_arena, r, &r->tile_view, r->tile_view.scale, r->tile_root,
                                    r->tile_working, 0, 0, r->width, r->height, ClipFlags_NAVIGATION_PREVIEW);
        gpu_set_cook_budget(r, 0);
        gpu_render_canvas(r, 0, 0, r->width, r->height, 1, true);
        cache->capture_ready = true;
    }
    cache->display_ready = ready;
    cache->pending = !ready;
    return true;
}
