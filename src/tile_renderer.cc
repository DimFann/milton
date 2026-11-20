// Included by renderer.cc: tile jobs reuse the reference stroke/effect renderer.

static const int CANVAS_TILE_SIZE = 256;
static const int CANVAS_TILE_TEXTURE_SIZE = CANVAS_TILE_SIZE + 2;
static const u64 CANVAS_TILE_BYTES = 2ULL * CANVAS_TILE_TEXTURE_SIZE * CANVAS_TILE_TEXTURE_SIZE * 4;
static const u64 CANVAS_TILE_BUDGET = 128ULL * 1024 * 1024;
static const u64 CANVAS_TILE_WORKSPACE_BUDGET = 128ULL * 1024 * 1024;

struct TileStrokeKey
{
    Stroke* stroke;
    i32 id;
    Brush brush;
    Rect bounds;
    i32 points;
    u32 flags;
    u64 revision;
    i64 position;
};

struct TileDependency
{
    i64 index;
    u64 serial;
};

struct CanvasTile
{
    Layer* layer;
    i32 layer_id;
    i64 x, y, scale;
    GLuint texture, paint;
    u64 serial, touched, checked;
    bool ready, paint_valid;
    float alpha;
    v3f background;
    i64 first_changed;
    DArray<TileStrokeKey> strokes;
    DArray<ReplayCacheEffect> effects;
    DArray<TileDependency> dependencies;
};

struct TileLayer
{
    Layer* layer;
    DArray<TileStrokeKey> strokes;
    DArray<ReplayCacheEffect> effects;
};

struct CanvasTileCache
{
    RenderBackend worker;
    DArray<CanvasTile> entries;
    DArray<TileLayer> layers;
    GLuint fbo, quad, underlay;
    u64 frame, serial, deadline;
    bool pending, failed, can_build, logged_limit, display_ready;
    int jobs;
    GLuint display_history, display_from;
    i32 display_width, display_height;
    u32 display_changed_at;
    bool display_valid, display_preview, display_fading;
    bool display_paused;
    u32 display_paused_at;
    CanvasView display_view, navigation_view;
    GLuint navigation_texture;
    bool navigation_valid, capture_ready;
    bool background_context;
    SDL_atomic_t* cancel_generation;
    int request_generation;
    u64 display_revision;
#if defined(TESTING)
    int job_limit;
#endif
};

struct TileBounds
{
    i64 left, top, right, bottom;
};

static i64
tile_floor(i64 value, i64 divisor)
{
    i64 q = value / divisor;
    return q - (value % divisor < 0 ? 1 : 0);
}

static bool
tile_intersects(TileBounds a, Rect b)
{
    return a.left <= b.right && a.right >= b.left && a.top <= b.bottom && a.bottom >= b.top;
}

static TileStrokeKey
tile_stroke_key(Stroke* stroke, i64 position)
{
    TileStrokeKey key = {};
    key.stroke = stroke;
    key.id = stroke->id;
    key.brush = stroke->brush;
    key.bounds = stroke->bounding_rect;
    key.points = stroke->num_points;
    key.flags = stroke->flags;
    key.revision = stroke->content_revision;
    key.position = position;
    return key;
}

static bool
tile_key_equal(const TileStrokeKey& a, const TileStrokeKey& b)
{
    return a.stroke == b.stroke && a.id == b.id && a.points == b.points && a.flags == b.flags
        && a.revision == b.revision && a.position == b.position
        && memcmp(&a.brush, &b.brush, sizeof(a.brush)) == 0
        && memcmp(&a.bounds, &b.bounds, sizeof(a.bounds)) == 0;
}

template<typename T>
static void
tile_assign(DArray<T>* target, const DArray<T>& source)
{
    reset(target);
    for ( const T& value : source ) { push(target, value); }
}

static void
tile_quad(RenderBackend* r, GLuint texture, float left, float top, float right, float bottom,
          float u0 = 0, float v0 = 1, float u1 = 1, float v1 = 0)
{
    CanvasTileCache* cache = r->tiles;
    float vertices[] = {left, top, u0, v0, left, bottom, u0, v1,
                        right, bottom, u1, v1, right, top, u1, v0};
    gl::use_program(r->quad_program);
    gl::set_uniform_i(r->quad_program, "u_canvas", 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glBindBuffer(GL_ARRAY_BUFFER, cache->quad);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);
    GLint point = glGetAttribLocation(r->quad_program, "a_point");
    GLint uv = glGetAttribLocation(r->quad_program, "a_uv");
    glEnableVertexAttribArray(point);
    glEnableVertexAttribArray(uv);
    glVertexAttribPointer(point, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), 0);
    glVertexAttribPointer(uv, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    // Attribute arrays must not outlive the evictable tile VBO.
    glDisableVertexAttribArray(point);
    glDisableVertexAttribArray(uv);
}

static void
tile_scene_release(CanvasTileCache* cache)
{
    for ( TileLayer& layer : cache->layers ) {
        release(&layer.strokes);
        release(&layer.effects);
    }
    reset(&cache->layers);
}

static void
tile_entry_release(CanvasTile* tile)
{
    GLuint textures[] = {tile->texture, tile->paint};
    glDeleteTextures(2, textures);
    release(&tile->strokes);
    release(&tile->effects);
    release(&tile->dependencies);
    *tile = {};
}

void
gpu_clear_tile_cache(RenderBackend* r)
{
    canvas_worker_invalidate(r);
    if ( !r->tiles ) { return; }
    CanvasTileCache* cache = r->tiles;
    for ( CanvasTile& tile : cache->entries ) { tile_entry_release(&tile); }
    release(&cache->entries);
    tile_scene_release(cache);
    release(&cache->layers);
    RenderBackend* w = &cache->worker;
    release(&w->clip_array);
    release(&w->replay_elements);
    release(&w->replay_effects);
    GLuint textures[] = {w->canvas_texture, w->eraser_texture, w->helper_texture, w->stroke_info_texture,
                         w->opacity_stroke_texture, w->opacity_history_texture, w->stencil_texture,
                         w->tile_paint_texture, cache->underlay, w->blur_brush_textures[0], w->blur_brush_textures[1]};
    glDeleteTextures(array_count(textures), textures);
    glDeleteTextures(array_count(w->opacity_history_cache_textures), w->opacity_history_cache_textures);
    GLuint fbos[] = {w->fbo, cache->fbo};
    glDeleteFramebuffersEXT(2, fbos);
    if ( cache->quad ) { glDeleteBuffers(1, &cache->quad); }
    GLuint display_textures[] = {cache->display_history, cache->display_from, cache->navigation_texture};
    glDeleteTextures(array_count(display_textures), display_textures);
    mlt_free(r->tiles, "Canvas tiles");
}

bool
gpu_tile_cache_pending(RenderBackend* r)
{
    if ( r->canvas_worker ) {
        return canvas_worker_has_result(r) || (r->tiles && r->tiles->display_fading);
    }
    return r->tiles && (r->tiles->pending || r->tiles->display_fading);
}

static void
tile_copy_display_texture(GLuint texture, i32 width, i32 height)
{
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, width, height);
}

static bool
gpu_hold_tile_display(RenderBackend* r, u32 now)
{
    CanvasTileCache* cache = r->tiles;
    if ( !cache || !cache->display_valid || cache->display_width != r->width
         || cache->display_height != r->height ) { return false; }
    if ( !cache->display_paused ) {
        cache->display_paused = true;
        cache->display_paused_at = now;
    }
    glBindFramebufferEXT(GL_FRAMEBUFFER, r->fbo);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->canvas_texture, 0);
    glActiveTexture(GL_TEXTURE0);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glBindTexture(GL_TEXTURE_2D, cache->display_history);
    gpu_fill_with_texture(r);
#if defined(TESTING)
    r->tile_builds = r->tile_composites = r->tile_hits = 0;
#endif
    return true;
}

static void
gpu_capture_navigation(RenderBackend* r)
{
    CanvasTileCache* cache = r->tiles;
    if ( cache->navigation_valid || !cache->display_valid || !cache->capture_ready ) { return; }
    if ( !cache->navigation_texture
         || cache->navigation_view.screen_size != cache->display_view.screen_size ) {
        if ( cache->navigation_texture ) { glDeleteTextures(1, &cache->navigation_texture); }
        cache->navigation_texture = gl::new_color_texture(cache->display_width, cache->display_height);
    }
    if ( !cache->fbo ) { glGenFramebuffersEXT(1, &cache->fbo); }
    glBindFramebufferEXT(GL_FRAMEBUFFER, cache->fbo);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, cache->display_history, 0);
    tile_copy_display_texture(cache->navigation_texture, cache->display_width, cache->display_height);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    cache->navigation_view = cache->display_view;
    cache->navigation_valid = true;
}

static void
gpu_view_texture_vertices(RenderBackend* r, const CanvasView& source, float* vertices)
{
    CanvasView& target = r->tile_view;
    double cs = cos(source.angle), ss = sin(source.angle), fs = source.flipped ? -1 : 1;
    double ct = cos(target.angle), st = sin(target.angle), ft = target.flipped ? -1 : 1;
    const int xx[] = {0, 0, 1, 1}, yy[] = {0, 1, 1, 0};
    for ( int i = 0; i < 4; ++i ) {
        double x = xx[i] * source.screen_size.w - source.zoom_center.x;
        double y = yy[i] * source.screen_size.h - source.zoom_center.y;
        double dx = ((double)(source.pan_center.x - target.pan_center.x)
                     + source.scale * (cs * fs * x - ss * y)) / target.scale;
        double dy = ((double)(source.pan_center.y - target.pan_center.y)
                     + source.scale * (ss * fs * x + cs * y)) / target.scale;
        double px = ft * (ct * dx + st * dy) + target.zoom_center.x;
        double py = -st * dx + ct * dy + target.zoom_center.y;
        vertices[4 * i] = (float)(2 * px / r->width - 1);
        vertices[4 * i + 1] = (float)(1 - 2 * py / r->height);
        vertices[4 * i + 2] = (float)xx[i];
        vertices[4 * i + 3] = (float)(1 - yy[i]);
    }
}

static void
gpu_draw_view_texture(RenderBackend* r, GLuint texture, const CanvasView& source)
{
    float vertices[16];
    gpu_view_texture_vertices(r, source, vertices);
    if ( !r->tiles->quad ) { glGenBuffers(1, &r->tiles->quad); }
    gl::use_program(r->quad_program);
    gl::set_uniform_i(r->quad_program, "u_canvas", 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glBindBuffer(GL_ARRAY_BUFFER, r->tiles->quad);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_DYNAMIC_DRAW);
    GLint point = glGetAttribLocation(r->quad_program, "a_point");
    GLint uv = glGetAttribLocation(r->quad_program, "a_uv");
    glEnableVertexAttribArray(point); glEnableVertexAttribArray(uv);
    glVertexAttribPointer(point, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), 0);
    glVertexAttribPointer(uv, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glDisableVertexAttribArray(point); glDisableVertexAttribArray(uv);
}

static void
gpu_present_tile_transition(RenderBackend* r, bool saved_preview, bool preview, u32 now)
{
    CanvasTileCache* cache = r->tiles;
    if ( !cache ) { return; }
    if ( cache->display_width != r->width || cache->display_height != r->height ) {
        GLuint textures[] = {cache->display_history, cache->display_from};
        glDeleteTextures(2, textures);
        cache->display_history = gl::new_color_texture(r->width, r->height);
        cache->display_from = gl::new_color_texture(r->width, r->height);
        cache->display_width = r->width; cache->display_height = r->height;
        cache->display_valid = cache->display_fading = false;
        cache->display_paused = false;
    }
    if ( cache->display_paused ) {
        cache->display_changed_at += (u32)(now - cache->display_paused_at);
        cache->display_paused = false;
    }
    bool immediate = (r->tile_working && r->tile_working->num_points > 0)
                     || r->canvas_worker != NULL
                     || (r->flags & (RenderBackendFlags_CANVAS_TRANSFORMING
                                    | RenderBackendFlags_HISTORY_CHANGED));
#if defined(TESTING)
    immediate |= r->disable_display_transitions;
#endif
    if ( !cache->display_valid || immediate ) {
        cache->display_fading = false;
    } else if ( preview != cache->display_preview ) {
        // Replace the previous fade from the displayed image, never queue it.
        swap(cache->display_history, cache->display_from);
        cache->display_changed_at = now;
        cache->display_fading = true;
    }
    cache->display_preview = preview;
    cache->display_valid = true;
    float duration = preview ? 50.0f : 100.0f;
    float t = min(1.0f, (u32)(now - cache->display_changed_at) / duration);
    if ( t >= 1 ) { cache->display_fading = false; }

    glBindFramebufferEXT(GL_FRAMEBUFFER, r->fbo);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->canvas_texture, 0);
    glViewport(0, 0, r->width, r->height);
    glScissor(0, 0, r->width, r->height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    GLuint target = saved_preview ? r->eraser_texture : r->canvas_texture;
    if ( cache->display_fading ) {
        // Keep the new image separate from the canvas being blended into.
        glFramebufferTexture2DEXT(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
        tile_copy_display_texture(r->helper_texture, r->width, r->height);
        glFramebufferTexture2DEXT(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->canvas_texture, 0);
        glBindTexture(GL_TEXTURE_2D, cache->display_from);
        gpu_fill_with_texture(r);
        glBindTexture(GL_TEXTURE_2D, r->helper_texture);
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        glBlendColor(0, 0, 0, t * t * (3 - 2 * t));
        glBlendFunc(GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA);
        gpu_fill_with_texture(r);
        glDisable(GL_BLEND);
    } else if ( target != r->canvas_texture ) {
        glBindTexture(GL_TEXTURE_2D, target);
        gpu_fill_with_texture(r);
    }
    tile_copy_display_texture(cache->display_history, r->width, r->height);
    cache->display_view = r->tile_view;
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glBindTexture(GL_TEXTURE_2D, r->canvas_texture);
}

static bool
tile_workspace(RenderBackend* r, int size)
{
    CanvasTileCache* cache = r->tiles;
    RenderBackend* w = &cache->worker;
    if ( w->width >= size && w->height >= size ) { return true; }
    int rounded = ((size + 127) / 128) * 128;
    u64 rounded_bytes = (u64)rounded * rounded * 4;
    if ( rounded_bytes * (11 + min((u64)OPACITY_HISTORY_CACHE_SLOTS, OPACITY_HISTORY_CACHE_BYTES / rounded_bytes))
         <= CANVAS_TILE_WORKSPACE_BUDGET ) { size = rounded; }
    // Include lazily allocated opacity/blur intermediates in the workspace allowance.
    u64 bytes = (u64)size * size * 4;
    if ( bytes * (11 + min((u64)OPACITY_HISTORY_CACHE_SLOTS, OPACITY_HISTORY_CACHE_BYTES / bytes)) > CANVAS_TILE_WORKSPACE_BUDGET
         || size > r->viewport_limits[0] || size > r->viewport_limits[1] ) {
        cache->failed = true;
        return false;
    }
    if ( w->width == size && w->height == size ) { return true; }
    if ( !w->canvas_texture ) {
        w->canvas_texture = gl::new_color_texture(size, size);
        w->eraser_texture = gl::new_color_texture(size, size);
        w->helper_texture = gl::new_color_texture(size, size);
        w->stroke_info_texture = gl::new_color_texture(size, size);
        w->stencil_texture = gl::new_depth_stencil_texture(size, size);
        w->fbo = gl::new_fbo(w->canvas_texture, w->stencil_texture, GL_TEXTURE_2D);
        w->tile_paint_texture = gl::new_color_texture(size, size);
        cache->underlay = gl::new_color_texture(size, size);
        w->allocated_width = w->allocated_height = size;
    } else {
        gl::resize_color_texture(w->tile_paint_texture, size, size);
        gl::resize_color_texture(cache->underlay, size, size);
    }
    CanvasView view = {};
    view.screen_size = {size, size};
    gpu_resize(w, &view);
    return true;
}

static bool
tile_time_available(CanvasTileCache* cache)
{
#if defined(TESTING)
    if ( cache->job_limit && cache->jobs >= cache->job_limit ) { return false; }
#endif
    if ( !cache->can_build ) { return false; }
    if ( cache->cancel_generation
         && cache->request_generation < SDL_AtomicGet(cache->cancel_generation) ) {
        cache->can_build = false;
        return false;
    }
    if ( cache->deadline && !cache->background_context ) {
        SDL_PumpEvents();
        if ( SDL_HasEvents(SDL_KEYDOWN, SDL_TEXTINPUT)
             || SDL_HasEvents(SDL_MOUSEBUTTONDOWN, SDL_MOUSEWHEEL)
             || (SDL_GetMouseState(NULL, NULL) && SDL_HasEvent(SDL_MOUSEMOTION))
             || SDL_HasEvent(SDL_QUIT) ) {
            cache->can_build = false;
            return false;
        }
    }
    return !cache->jobs || !cache->deadline || perf_counter() < cache->deadline;
}

static i64
tile_find(RenderBackend* r, Layer* layer, i64 x, i64 y, i64 scale)
{
    CanvasTileCache* cache = r->tiles;
    for ( i64 i = 0; i < cache->entries.count; ++i ) {
        CanvasTile& tile = cache->entries[i];
        if ( tile.layer == layer && tile.layer_id == layer->id
             && tile.x == x && tile.y == y && tile.scale == scale ) { return i; }
    }
    if ( !cache->can_build ) { return -1; }
    i64 slot = -1;
    if ( (u64)(cache->entries.count + 1) * CANVAS_TILE_BYTES <= CANVAS_TILE_BUDGET ) {
        slot = cache->entries.count;
        push(&cache->entries, CanvasTile{});
    } else {
        for ( i64 i = 0; i < cache->entries.count; ++i ) {
            if ( cache->entries[i].touched != cache->frame
                 && (slot < 0 || cache->entries[i].touched < cache->entries[slot].touched) ) { slot = i; }
        }
        if ( slot < 0 ) { cache->failed = true; return -1; }
        tile_entry_release(&cache->entries[slot]);
    }
    CanvasTile& tile = cache->entries[slot];
    tile.layer = layer;
    tile.layer_id = layer->id;
    tile.x = x; tile.y = y; tile.scale = scale;
    return slot;
}

static i64 tile_request(RenderBackend* r, i64 layer_index, i64 x, i64 y, i64 scale);

static void
tile_render_paint(RenderBackend* r, i64 index, i64 layer_index, int padding, bool lower_paint,
                  const DArray<TileStrokeKey>& keys, const DArray<TileDependency>& dependencies)
{
    CanvasTileCache* cache = r->tiles;
    CanvasTile& tile = cache->entries[index];
    RenderBackend* w = &cache->worker;
    int size = CANVAS_TILE_SIZE + 2 * padding;
    i64 origin_x = tile.x * CANVAS_TILE_SIZE - padding;
    i64 origin_y = tile.y * CANVAS_TILE_SIZE - padding;
    w->render_center = r->render_center;
    w->background_color = r->background_color;
    CanvasView view = {};
    view.screen_size = {size, size};
    view.scale = tile.scale;
    view.pan_center = {origin_x * tile.scale, origin_y * tile.scale};
    gpu_update_view(w, &view);
    glBindFramebufferEXT(GL_FRAMEBUFFER, cache->fbo);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, cache->underlay, 0);
    glViewport(0, 0, size, size);
    glScissor(0, 0, size, size);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glClearColor(r->background_color.r, r->background_color.g, r->background_color.b, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if ( lower_paint ) {
        for ( const TileDependency& dependency : dependencies ) {
            const CanvasTile& lower = cache->entries[dependency.index];
            float left = 2.0f * (lower.x * CANVAS_TILE_SIZE - origin_x) / size - 1;
            float top = 1 - 2.0f * (lower.y * CANVAS_TILE_SIZE - origin_y) / size;
            float step = 2.0f * CANVAS_TILE_SIZE / size;
            float a = 1.0f / CANVAS_TILE_TEXTURE_SIZE, b = 1 - a;
            tile_quad(r, lower.texture, left, top, left + step, top - step, a, b, b, a);
        }
    }
    w->tile_underlay_texture = lower_paint ? cache->underlay : 0;
    reset(&w->clip_array);
    for ( const TileStrokeKey& key : keys ) {
        gpu_cook_stroke(r->tile_arena, w, key.stroke,
                        key.stroke == r->tile_working ? CookStroke_UPDATE_WORKING_STROKE : CookStroke_NEW);
        RenderElement element = *get_render_element(key.stroke->render_handle);
        element.raster_bounds = canvas_to_raster_bounding_rect(&view, key.stroke->bounding_rect);
        push(&w->clip_array, element);
    }
    RenderElement marker = {};
    marker.flags = RenderElementFlags_LAYER;
    marker.layer_alpha = 1;
    marker.effects = cache->layers[layer_index].layer->effects;
    push(&w->clip_array, marker);
    gpu_render_canvas(w, 0, 0, size, size);
    glBindFramebufferEXT(GL_FRAMEBUFFER, cache->fbo);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tile.paint, 0);
    glViewport(0, 0, CANVAS_TILE_TEXTURE_SIZE, CANVAS_TILE_TEXTURE_SIZE);
    glScissor(0, 0, CANVAS_TILE_TEXTURE_SIZE, CANVAS_TILE_TEXTURE_SIZE);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    float a = (float)(padding - 1) / size;
    float b = (float)(padding + CANVAS_TILE_SIZE + 1) / size;
    tile_quad(r, w->tile_paint_texture, -1, 1, 1, -1, a, 1 - a, b, 1 - b);
    tile.paint_valid = true;
#if defined(TESTING)
    ++r->tile_builds;
#endif
}

static i64
tile_request(RenderBackend* r, i64 layer_index, i64 x, i64 y, i64 scale)
{
    CanvasTileCache* cache = r->tiles;
    TileLayer& scene = cache->layers[layer_index];
    i64 index = tile_find(r, scene.layer, x, y, scale);
    if ( index < 0 ) { return -1; }
    cache->entries[index].touched = cache->frame;
    if ( cache->entries[index].checked == cache->frame ) {
        return cache->entries[index].ready ? index : -1;
    }
    cache->entries[index].checked = cache->frame;
    cache->entries[index].ready = false;
    TileBounds bounds = {(x * CANVAS_TILE_SIZE - 1) * scale, (y * CANVAS_TILE_SIZE - 1) * scale,
                         ((x + 1) * CANVAS_TILE_SIZE + 1) * scale, ((y + 1) * CANVAS_TILE_SIZE + 1) * scale};
    i64 expansion = 0;
    for ( const ReplayCacheEffect& effect : scene.effects ) {
        if ( effect.enabled && effect.type == LayerEffectType_BLUR ) {
            expansion += 3 * ((i64)effect.kernel * effect.scale / scale + 1);
        }
    }
    bounds.left -= expansion * scale; bounds.right += expansion * scale;
    bounds.top -= expansion * scale; bounds.bottom += expansion * scale;
    DArray<TileStrokeKey> keys = {};
    bool lower_paint = false;
    // Reverse chronological expansion includes the input footprint of every dependent blur.
    for ( i64 i = scene.strokes.count; i-- > 0; ) {
        const TileStrokeKey& key = scene.strokes[i];
        if ( !tile_intersects(bounds, key.bounds) ) { continue; }
        push(&keys, key);
        lower_paint |= (key.flags & StrokeFlag_ERASER) != 0;
        if ( key.flags & StrokeFlag_BLUR ) {
            i64 halo = (i64)ceil(0.6 * key.brush.radius / scale) + 3;
            expansion += halo;
            bounds.left -= halo * scale; bounds.right += halo * scale;
            bounds.top -= halo * scale; bounds.bottom += halo * scale;
        }
    }
    for ( i64 a = 0, b = keys.count - 1; a < b; ++a, --b ) { swap(keys[a], keys[b]); }
    bool paint_matches = cache->entries[index].paint_valid;
    CanvasTile& previous = cache->entries[index];
    i64 common = min(keys.count, previous.strokes.count);
    i64 changed = 0;
    while ( changed < common && tile_key_equal(keys[changed], previous.strokes[changed]) ) { ++changed; }
    paint_matches &= changed == common && keys.count == previous.strokes.count;
    previous.first_changed = changed < keys.count ? keys[changed].position : scene.strokes.count;
    paint_matches &= previous.effects.count == scene.effects.count;
    for ( i64 i = 0; paint_matches && i < scene.effects.count; ++i ) {
        paint_matches = memcmp(&previous.effects[i], &scene.effects[i], sizeof(ReplayCacheEffect)) == 0;
    }
    bool background_matches = memcmp(&previous.background, &r->background_color, sizeof(v3f)) == 0;
    DArray<TileDependency> dependencies = {};
    bool dependencies_ready = true;
    if ( layer_index > 0 ) {
        i64 pad = lower_paint && !paint_matches ? expansion + 2 : 0;
        i64 first_x = tile_floor(x * CANVAS_TILE_SIZE - pad, CANVAS_TILE_SIZE);
        i64 last_x = tile_floor((x + 1) * CANVAS_TILE_SIZE - 1 + pad, CANVAS_TILE_SIZE);
        i64 first_y = tile_floor(y * CANVAS_TILE_SIZE - pad, CANVAS_TILE_SIZE);
        i64 last_y = tile_floor((y + 1) * CANVAS_TILE_SIZE - 1 + pad, CANVAS_TILE_SIZE);
        // Retain full lower-layer dependencies even on a warm validation.
        if ( lower_paint ) {
            first_x = tile_floor(x * CANVAS_TILE_SIZE - expansion - 2, CANVAS_TILE_SIZE);
            last_x = tile_floor((x + 1) * CANVAS_TILE_SIZE + expansion + 1, CANVAS_TILE_SIZE);
            first_y = tile_floor(y * CANVAS_TILE_SIZE - expansion - 2, CANVAS_TILE_SIZE);
            last_y = tile_floor((y + 1) * CANVAS_TILE_SIZE + expansion + 1, CANVAS_TILE_SIZE);
        }
        if ( (last_x - first_x + 1) * (last_y - first_y + 1) > 128 ) { cache->failed = true; }
        for ( i64 yy = first_y; yy <= last_y && !cache->failed; ++yy ) {
            for ( i64 xx = first_x; xx <= last_x && !cache->failed; ++xx ) {
                i64 lower = tile_request(r, layer_index - 1, xx, yy, scale);
                if ( lower < 0 ) { dependencies_ready = false; }
                else { push(&dependencies, TileDependency{lower, cache->entries[lower].serial}); }
            }
        }
    }
    CanvasTile& tile = cache->entries[index];
    bool lower_matches = dependencies_ready && dependencies.count == tile.dependencies.count;
    for ( i64 i = 0; lower_matches && i < dependencies.count; ++i ) {
        lower_matches = dependencies[i].index == tile.dependencies[i].index
                        && dependencies[i].serial == tile.dependencies[i].serial;
    }
    if ( lower_paint && (!lower_matches || (layer_index == 0 && !background_matches)) ) { paint_matches = false; }
    bool complete = paint_matches && lower_matches && background_matches && tile.alpha == scene.layer->alpha && tile.serial;
    if ( complete ) {
        tile.ready = true;
#if defined(TESTING)
        ++r->tile_hits;
#endif
    } else if ( dependencies_ready && !cache->failed && tile_time_available(cache) ) {
        int padding = (int)min(expansion + 2, (i64)1000000);
        if ( tile_workspace(r, CANVAS_TILE_SIZE + 2 * padding) ) {
            if ( !tile.texture ) {
                tile.texture = gl::new_color_texture(CANVAS_TILE_TEXTURE_SIZE, CANVAS_TILE_TEXTURE_SIZE);
                tile.paint = gl::new_color_texture(CANVAS_TILE_TEXTURE_SIZE, CANVAS_TILE_TEXTURE_SIZE);
            }
            if ( !paint_matches ) {
                padding = (cache->worker.width - CANVAS_TILE_SIZE) / 2;
                tile_render_paint(r, index, layer_index, padding, lower_paint, keys, dependencies);
            }
            glBindFramebufferEXT(GL_FRAMEBUFFER, cache->fbo);
            glFramebufferTexture2DEXT(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tile.texture, 0);
            glViewport(0, 0, CANVAS_TILE_TEXTURE_SIZE, CANVAS_TILE_TEXTURE_SIZE);
            glScissor(0, 0, CANVAS_TILE_TEXTURE_SIZE, CANVAS_TILE_TEXTURE_SIZE);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_BLEND);
            glClearColor(r->background_color.r, r->background_color.g, r->background_color.b, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            if ( layer_index > 0 ) {
                for ( const TileDependency& dependency : dependencies ) {
                    CanvasTile& lower = cache->entries[dependency.index];
                    if ( lower.x == x && lower.y == y ) { tile_quad(r, lower.texture, -1, 1, 1, -1); break; }
                }
            }
            gl::set_uniform_vec2(r->texture_fill_program, "u_screen_size", 1,
                                v2f{(float)CANVAS_TILE_TEXTURE_SIZE, (float)CANVAS_TILE_TEXTURE_SIZE}.d);
            glBindTexture(GL_TEXTURE_2D, tile.paint);
            glEnable(GL_BLEND);
            glBlendEquation(GL_FUNC_ADD);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            gpu_fill_with_texture(r, scene.layer->alpha);
            glDisable(GL_BLEND);
            u8 pixel[4];
            glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
            ++cache->jobs;
            tile.serial = ++cache->serial;
            tile.alpha = scene.layer->alpha;
            tile.background = r->background_color;
            tile.ready = true;
            reset(&tile.strokes);
            for ( const TileStrokeKey& key : keys ) {
                TileStrokeKey cooked = tile_stroke_key(key.stroke, key.position);
                if ( key.stroke == r->tile_working ) { cooked.revision = key.revision; }
                push(&tile.strokes, cooked);
            }
            tile_assign(&tile.effects, scene.effects);
            tile_assign(&tile.dependencies, dependencies);
#if defined(TESTING)
            ++r->tile_composites;
#endif
        }
    }
    bool ready = tile.ready;
    release(&keys);
    release(&dependencies);
    return ready ? index : -1;
}

static void
tile_draw_view(RenderBackend* r, const CanvasTile& tile, i64 x, i64 y, i64 scale)
{
    CanvasView& view = r->tile_view;
    float points[16];
    const i64 xx[] = {x, x, x + 1, x + 1};
    const i64 yy[] = {y, y + 1, y + 1, y};
    float c = cosf(view.angle), s = sinf(view.angle), flip = view.flipped ? -1.0f : 1.0f;
    for ( int i = 0; i < 4; ++i ) {
        double dx = (double)(xx[i] * CANVAS_TILE_SIZE * scale - view.pan_center.x) / view.scale;
        double dy = (double)(yy[i] * CANVAS_TILE_SIZE * scale - view.pan_center.y) / view.scale;
        double px = flip * (c * dx + s * dy) + view.zoom_center.x;
        double py = -s * dx + c * dy + view.zoom_center.y;
        points[4 * i] = (float)(2 * px / r->width - 1);
        points[4 * i + 1] = (float)(1 - 2 * py / r->height);
        double world_x = (double)xx[i] * CANVAS_TILE_SIZE * scale / tile.scale - tile.x * CANVAS_TILE_SIZE;
        double world_y = (double)yy[i] * CANVAS_TILE_SIZE * scale / tile.scale - tile.y * CANVAS_TILE_SIZE;
        points[4 * i + 2] = (float)((world_x + 1) / CANVAS_TILE_TEXTURE_SIZE);
        points[4 * i + 3] = (float)(1 - (world_y + 1) / CANVAS_TILE_TEXTURE_SIZE);
    }
    gl::use_program(r->quad_program);
    gl::set_uniform_i(r->quad_program, "u_canvas", 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tile.texture);
    glBindBuffer(GL_ARRAY_BUFFER, r->tiles->quad);
    glBufferData(GL_ARRAY_BUFFER, sizeof(points), points, GL_DYNAMIC_DRAW);
    GLint point = glGetAttribLocation(r->quad_program, "a_point"), uv = glGetAttribLocation(r->quad_program, "a_uv");
    glEnableVertexAttribArray(point); glEnableVertexAttribArray(uv);
    glVertexAttribPointer(point, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), 0);
    glVertexAttribPointer(uv, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glDisableVertexAttribArray(point); glDisableVertexAttribArray(uv);
}

static bool
gpu_render_tile_cache(RenderBackend* r, i32, i32, i32, i32)
{
#if defined(TESTING)
    r->tile_builds = r->tile_composites = r->tile_hits = 0;
#endif
    if ( r->tile_internal || !r->tile_arena || !r->tile_working ) {
        if ( r->tiles ) { r->tiles->pending = false; }
        return false;
    }
    if ( !r->tiles ) {
        r->tiles = (CanvasTileCache*)mlt_calloc(1, sizeof(CanvasTileCache), "Canvas tiles");
        CanvasTileCache* cache = r->tiles;
        cache->worker = *r;
        RenderBackend* w = &cache->worker;
        w->tiles = NULL;
        w->sampling_pixels = NULL;
        w->tile_internal = true;
        w->canvas_texture = w->eraser_texture = w->helper_texture = w->stroke_info_texture = w->stencil_texture = 0;
        w->opacity_history_texture = w->opacity_stroke_texture = 0;
        w->replay_layer_texture = w->replay_canvas_texture = w->replay_history_texture = 0;
        w->fbo = w->tile_paint_texture = w->tile_underlay_texture = 0;
        w->replay_valid = false;
        w->clip_array = {};
        w->replay_elements = {}; w->replay_effects = {};
        for ( GLuint& texture : w->opacity_history_cache_textures ) { texture = 0; }
        for ( GLuint& texture : w->blur_brush_textures ) { texture = 0; }
        w->width = w->height = 0;
        w->allocated_width = w->allocated_height = 0;
        w->cook_deadline = 0;
        glGenFramebuffersEXT(1, &cache->fbo);
        glGenBuffers(1, &cache->quad);
    }
    CanvasTileCache* cache = r->tiles;
    tile_scene_release(cache);
    ++cache->frame;
    for ( Layer* layer = r->tile_root; layer; layer = layer->next ) {
        if ( !(layer->flags & LayerFlags_VISIBLE) ) { continue; }
        TileLayer scene = {};
        scene.layer = layer;
        StrokeIterator iterator = {};
        i64 position = 0;
        for ( Stroke* stroke = stroke_iter_init(&layer->strokes, &iterator); stroke;
              stroke = stroke_iter_next(&iterator), ++position ) {
            push(&scene.strokes, tile_stroke_key(stroke, position));
        }
        if ( r->tile_working->layer_id == layer->id && r->tile_working->num_points > 0
             && !(r->tile_working->flags & StrokeFlag_CUT) ) {
            TileStrokeKey key = tile_stroke_key(r->tile_working, position);
            key.revision = cache->frame;
            push(&scene.strokes, key);
        }
        for ( LayerEffect* effect = layer->effects; effect; effect = effect->next ) {
            push(&scene.effects, ReplayCacheEffect{effect->type, effect->enabled, effect->blur.original_scale, effect->blur.kernel_size});
        }
        push(&cache->layers, scene);
    }
    cache->jobs = 0; cache->failed = false; cache->pending = false;
    cache->display_revision = 0;
    cache->background_context = r->background_context;
    cache->cancel_generation = r->cancel_generation;
    cache->request_generation = r->request_generation;
    cache->can_build = !(r->flags & RenderBackendFlags_NAVIGATION_PREVIEW);
    cache->deadline = perf_counter() + (u64)(0.004 / perf_count_to_sec(1000000) * 1000000);
    if ( r->tile_working->num_points ) { cache->deadline = 0; }
#if defined(TESTING)
    cache->job_limit = r->tile_job_limit;
    if ( r->tile_cache_unlimited ) { cache->deadline = 0; }
#endif
    if ( !cache->layers.count ) { return false; }
    i64 scale = 1;
    while ( scale <= r->tile_view.scale / 2 ) { scale *= 2; }
    Rect visible = raster_to_canvas_bounding_rect(&r->tile_view, 0, 0, r->width, r->height, r->tile_view.scale);
    i64 first_x = tile_floor(visible.left, scale * CANVAS_TILE_SIZE);
    i64 last_x = tile_floor(visible.right, scale * CANVAS_TILE_SIZE);
    i64 first_y = tile_floor(visible.top, scale * CANVAS_TILE_SIZE);
    i64 last_y = tile_floor(visible.bottom, scale * CANVAS_TILE_SIZE);
    if ( (last_x - first_x + 1) * (last_y - first_y + 1) > 128 ) { cache->failed = true; }
    DArray<i64> ready = {};
    i64 top_layer = cache->layers.count - 1;
    for ( i64 yy = first_y; yy <= last_y && !cache->failed; ++yy ) {
        for ( i64 xx = first_x; xx <= last_x && !cache->failed; ++xx ) {
            i64 tile = tile_request(r, top_layer, xx, yy, scale);
            push(&ready, tile);
            cache->pending |= tile < 0;
        }
    }
    gpu_update_view(r, &r->tile_view);
    glViewport(0, 0, r->width, r->height);
    glScissor(0, 0, r->width, r->height);
    if ( cache->failed ) {
        if ( !cache->logged_limit ) {
            milton_log("Tile cache dependency/workspace limit reached; using full replay for this view.\n");
            cache->logged_limit = true;
        }
        cache->pending = false;
        release(&ready);
        return false;
    }
    cache->logged_limit = false;
    if ( !r->background_context && (cache->pending || (r->flags & RenderBackendFlags_NAVIGATION_PREVIEW)) ) {
        gpu_set_cook_budget(r, 0.002f);
        gpu_clip_strokes_and_update(r->tile_arena, r, &r->tile_view, r->tile_view.scale, r->tile_root,
                                    r->tile_working, 0, 0, r->width, r->height, ClipFlags_NAVIGATION_PREVIEW);
        gpu_set_cook_budget(r, 0);
        gpu_render_canvas(r, 0, 0, r->width, r->height, 1, true);
        // Keep a clean preview for views without complete cached coverage.
        glBindFramebufferEXT(GL_FRAMEBUFFER, r->fbo);
        glFramebufferTexture2DEXT(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->canvas_texture, 0);
        tile_copy_display_texture(r->eraser_texture, r->width, r->height);
    }
    glBindFramebufferEXT(GL_FRAMEBUFFER, r->fbo);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->canvas_texture, 0);
    glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND);
    if ( !cache->pending || r->background_context ) {
        // Partial worker frames leave unfinished pixels for the UI's retained preview.
        if ( cache->pending ) { glClearColor(0, 0, 0, 0); }
        else { glClearColor(r->background_color.r, r->background_color.g, r->background_color.b, 1); }
        glClear(GL_COLOR_BUFFER_BIT);
    }
    i64 i = 0;
    cache->display_ready = true;
    for ( i64 yy = first_y; yy <= last_y; ++yy ) {
        for ( i64 xx = first_x; xx <= last_x; ++xx, ++i ) {
            i64 tile = ready[i];
            // A coarser resident level can stand in for a missing fine tile during navigation/refinement.
            if ( tile < 0 ) {
                bool old_build = cache->can_build;
                cache->can_build = false;
                for ( i64 coarse = scale * 2; tile < 0 && coarse <= scale * 8; coarse *= 2 ) {
                    i64 cx = tile_floor(xx * scale, coarse), cy = tile_floor(yy * scale, coarse);
                    tile = tile_request(r, top_layer, cx, cy, coarse);
                }
                cache->can_build = old_build;
                gpu_update_view(r, &r->tile_view);
                glBindFramebufferEXT(GL_FRAMEBUFFER, r->fbo);
                glFramebufferTexture2DEXT(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->canvas_texture, 0);
                glViewport(0, 0, r->width, r->height);
                glScissor(0, 0, r->width, r->height);
                glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND);
            }
            if ( tile >= 0 ) {
                tile_draw_view(r, cache->entries[tile], xx, yy, scale);
                cache->display_revision = max(cache->display_revision, cache->entries[tile].serial);
            } else {
                bool old_build = cache->can_build;
                cache->can_build = false;
                bool covered = false;
                for ( i64 fine = scale / 2; fine > 0 && fine >= max((i64)1, scale / 4); fine /= 2 ) {
                    i64 ratio = scale / fine;
                    bool level_covered = true;
                    for ( i64 fy = yy * ratio; fy < (yy + 1) * ratio; ++fy ) {
                        for ( i64 fx = xx * ratio; fx < (xx + 1) * ratio; ++fx ) {
                            i64 resident = tile_request(r, top_layer, fx, fy, fine);
                            if ( resident >= 0 ) {
                                tile_draw_view(r, cache->entries[resident], fx, fy, fine);
                                cache->display_revision = max(cache->display_revision, cache->entries[resident].serial);
                            }
                            else { level_covered = false; }
                        }
                    }
                    covered |= level_covered;
                    if ( fine == 1 ) { break; }
                }
                cache->display_ready &= covered;
                cache->can_build = old_build;
            }
        }
    }
    release(&ready);
    return true;
}
