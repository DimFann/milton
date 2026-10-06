// Lasso selection of whole strokes on the working layer. See selection.h.

#include "selection.h"
#include "milton.h"
#include "gui.h"

enum SelOpKind
{
    SelOp_DELETE,
    SelOp_TRANSFORM,
    SelOp_CUT,
};

struct SelItem
{
    i32     index;          // Index in the layer's stroke list
    i32     num_points;
    v2l*    orig_pts;       // Points as of the last committed state (malloc'd)
    i32     orig_radius;
    f32     orig_ax, orig_ay;
};

struct SelOpItem
{
    i32     index;
    i32     num_points;
    v2l*    old_pts;        // TRANSFORM only (malloc'd)
    v2l*    new_pts;
    i32     old_radius, new_radius;
    f32     old_ax, old_ay, new_ax, new_ay;
    Stroke  stroke;         // DELETE only
};

struct SelOp
{
    i32         kind;
    i32         layer_id;
    i32         n;
    SelOpItem*  items;      // Ascending by index
    i32         n2;         // CUT only: the pieces that replace items
    SelOpItem*  items2;     // Ascending by index in the cut layer
};

// An erased-stroke cleanup tied to the eraser stroke that caused it, so it undoes together with it.
struct AutoOp
{
    SelOp   op;
    i64     hist_pos;
};

enum SelState
{
    SelState_IDLE,
    SelState_LASSO,
    SelState_MOVE,
    SelState_XFORM_DRAG,
};

enum SelDrag
{
    SelDrag_TRANSLATE,
    SelDrag_ROTATE,
    SelDrag_SCALE,
};

struct Selection
{
    SelectionCommand pending;
    b32         armed;
    SelState    state;
    b32         prev_down;
    b32         dirty;

    DArray<v2l> lasso;          // Canvas space
    v2l         last_lasso_raster;

    b32         active;
    i32         layer_id;
    i32         n;
    SelItem*    items;

    // Box: original centre/half extents and the current transform, all in canvas space.
    double      c0x, c0y, h0x, h0y;
    double      cx, cy, theta, sx, sy;
    b32         xform_mode;

    // Drag state
    i32         drag_kind;
    double      drag_mx, drag_my;
    double      s_cx, s_cy, s_theta, s_sx, s_sy;
    i32         hx, hy;
    double      anchor_x, anchor_y;
    double      rot_start;

    DArray<SelOp> undo;
    DArray<SelOp> redo;

    DArray<AutoOp> auto_undo;
    DArray<AutoOp> auto_redo;

    i32         toast_n;
    u32         toast_until;
};

Selection*
selection_create(Arena* arena)
{
    Selection* s = arena_alloc_elem(arena, Selection);
    memset(s, 0, sizeof(*s));
    return s;
}

void
selection_command(Milton* milton, SelectionCommand cmd)
{
    milton->selection->pending = cmd;
}

b32
selection_transform_active(Milton* milton)
{
    Selection* s = milton->selection;
    return s->xform_mode || s->armed;
}

b32
selection_lasso_armed(Milton* milton)
{
    return milton->selection->armed;
}

static b32
sel_point_in_box(Milton* milton, v2l raster)
{
    Selection* s = milton->selection;
    v2l m = raster_to_canvas(milton->view, raster);
    double cs = cos(s->theta), sn = sin(s->theta);
    double rx = (double)m.x - s->cx, ry = (double)m.y - s->cy;
    double lx = rx * cs + ry * sn;
    double ly = -rx * sn + ry * cs;
    return fabs(lx) <= s->h0x * s->sx && fabs(ly) <= s->h0y * s->sy;
}

void
selection_disarm_lasso(Milton* milton)
{
    Selection* s = milton->selection;
    if ( s->armed && s->state == SelState_IDLE ) {
        s->armed = false;
    }
}

i32
selection_cursor(Milton* milton)
{
    Selection* s = milton->selection;
    if ( s->state == SelState_LASSO || s->armed ) { return 1; }
    if ( s->state == SelState_MOVE || s->state == SelState_XFORM_DRAG ) { return 2; }
    if ( s->active ) {
        if ( s->xform_mode ) { return 2; }
        if ( !gui_point_hovers(milton->gui, milton->platform->pointer)
             && sel_point_in_box(milton, VEC2L(milton->platform->pointer)) ) {
            return 2;
        }
    }
    return 0;
}

b32
selection_take_dirty(Milton* milton)
{
    b32 d = milton->selection->dirty;
    milton->selection->dirty = false;
    return d;
}

// ---- Stroke list helpers

static Layer*
sel_layer(Milton* milton)
{
    Selection* s = milton->selection;
    return layer::get_by_id(milton->canvas->root_layer, s->layer_id);
}

// Pushing re-computes the bucket bounds, which culling relies on.
static void
list_refresh_bounds(Layer* l)
{
    i64 n = l->strokes.count;
    reset(&l->strokes);
    for ( i64 i = 0; i < n; ++i ) {
        Stroke copy = *get(&l->strokes, i);
        push(&l->strokes, copy);
    }
}

static void
list_remove(Milton* milton, Layer* l, SelOpItem* items, i32 n)
{
    i64 cnt = l->strokes.count;
    Stroke* tmp = (Stroke*)calloc((size_t)(cnt > 0 ? cnt : 1), sizeof(Stroke));
    i64 k = 0;
    i32 j = 0;
    for ( i64 i = 0; i < cnt; ++i ) {
        Stroke* st = get(&l->strokes, i);
        if ( j < n && items[j].index == i ) {
            items[j].stroke = *st;
            gpu_free_strokes(st, 1, milton->renderer);
            ++j;
            continue;
        }
        tmp[k++] = *st;
    }
    reset(&l->strokes);
    for ( i64 i = 0; i < k; ++i ) {
        push(&l->strokes, tmp[i]);
    }
    free(tmp);
}

static void
list_insert(Layer* l, SelOpItem* items, i32 n)
{
    i64 cnt = l->strokes.count;
    i64 total = cnt + n;
    Stroke* tmp = (Stroke*)calloc((size_t)(total > 0 ? total : 1), sizeof(Stroke));
    i64 old_i = 0;
    i32 j = 0;
    for ( i64 k = 0; k < total; ++k ) {
        if ( j < n && items[j].index == k ) {
            tmp[k] = items[j].stroke;
            ++j;
        } else {
            tmp[k] = *get(&l->strokes, old_i++);
        }
    }
    reset(&l->strokes);
    for ( i64 k = 0; k < total; ++k ) {
        push(&l->strokes, tmp[k]);
    }
    free(tmp);
}

static void
sel_op_free(SelOp* op)
{
    for ( i32 i = 0; i < op->n; ++i ) {
        free(op->items[i].old_pts);
        free(op->items[i].new_pts);
    }
    free(op->items);
    free(op->items2);
    *op = {};
}

static void
sel_free_items(Selection* s)
{
    for ( i32 i = 0; i < s->n; ++i ) {
        free(s->items[i].orig_pts);
    }
    free(s->items);
    s->items = NULL;
    s->n = 0;
    s->active = false;
    s->xform_mode = false;
}

static void
sel_clear_stack(DArray<SelOp>* stack)
{
    while ( stack->count > 0 ) {
        SelOp op = pop(stack);
        sel_op_free(&op);
    }
}

static void
sel_push_op(Milton* milton, SelOp op)
{
    Selection* s = milton->selection;
    push(&s->undo, op);
    HistoryElement h = { HistoryElement_SELECTION_OP, -1 };
    push(&milton->canvas->history, h);
    clear_stroke_redo(milton);
    s->dirty = true;
}

// ---- Snapshot and transform

// Take the current strokes as the new original and reset the box around them.
static void
sel_snapshot(Milton* milton)
{
    Selection* s = milton->selection;
    Layer* l = sel_layer(milton);
    if ( !l ) { return; }

    double minx = 1e300, miny = 1e300, maxx = -1e300, maxy = -1e300;
    for ( i32 i = 0; i < s->n; ++i ) {
        SelItem* it = &s->items[i];
        Stroke* st = get(&l->strokes, it->index);
        free(it->orig_pts);
        it->num_points = st->num_points;
        it->orig_pts = (v2l*)malloc(sizeof(v2l) * (size_t)(st->num_points > 0 ? st->num_points : 1));
        memcpy(it->orig_pts, st->points, sizeof(v2l) * (size_t)st->num_points);
        it->orig_radius = st->brush.radius;
        it->orig_ax = st->brush.shape_axis_x;
        it->orig_ay = st->brush.shape_axis_y;

        minx = min(minx, (double)st->bounding_rect.left);
        miny = min(miny, (double)st->bounding_rect.top);
        maxx = max(maxx, (double)st->bounding_rect.right);
        maxy = max(maxy, (double)st->bounding_rect.bottom);
    }
    s->c0x = (minx + maxx) * 0.5;
    s->c0y = (miny + maxy) * 0.5;
    s->h0x = max((maxx - minx) * 0.5, 1.0);
    s->h0y = max((maxy - miny) * 0.5, 1.0);
    s->cx = s->c0x; s->cy = s->c0y;
    s->theta = 0; s->sx = 1; s->sy = 1;
}

static b32
sel_is_dirty(Selection* s)
{
    return s->cx != s->c0x || s->cy != s->c0y || s->theta != 0 || s->sx != 1 || s->sy != 1;
}

static void
sel_apply(Milton* milton)
{
    Selection* s = milton->selection;
    Layer* l = sel_layer(milton);
    if ( !l ) { return; }

    double cs = cos(s->theta), sn = sin(s->theta);
    double rs = sqrt(fabs(s->sx * s->sy));

    for ( i32 i = 0; i < s->n; ++i ) {
        SelItem* it = &s->items[i];
        Stroke* st = get(&l->strokes, it->index);
        for ( i32 p = 0; p < it->num_points; ++p ) {
            double dx = ((double)it->orig_pts[p].x - s->c0x) * s->sx;
            double dy = ((double)it->orig_pts[p].y - s->c0y) * s->sy;
            double px = s->cx + dx * cs - dy * sn;
            double py = s->cy + dx * sn + dy * cs;
            st->points[p] = v2l{ (i64)floor(px + 0.5), (i64)floor(py + 0.5) };
        }
        i32 r = (i32)floor((double)it->orig_radius * rs + 0.5);
        st->brush.radius = r < 1 ? 1 : r;

        double ax = it->orig_ax * s->sx;
        double ay = it->orig_ay * s->sy;
        double rx = ax * cs - ay * sn;
        double ry = ax * sn + ay * cs;
        double len = sqrt(rx*rx + ry*ry);
        if ( len > 1e-9 ) {
            st->brush.shape_axis_x = (f32)(rx / len);
            st->brush.shape_axis_y = (f32)(ry / len);
        }
        st->bounding_rect = bounding_box_for_stroke(st);
        gpu_free_strokes(st, 1, milton->renderer);
    }
    list_refresh_bounds(l);
    milton->render_settings.do_full_redraw = true;
}

static void
sel_commit_xform(Milton* milton)
{
    Selection* s = milton->selection;
    if ( !s->active || !sel_is_dirty(s) ) { return; }
    Layer* l = sel_layer(milton);
    if ( !l ) { return; }

    SelOp op = {};
    op.kind = SelOp_TRANSFORM;
    op.layer_id = s->layer_id;
    op.n = s->n;
    op.items = (SelOpItem*)calloc((size_t)s->n, sizeof(SelOpItem));
    for ( i32 i = 0; i < s->n; ++i ) {
        SelItem* it = &s->items[i];
        SelOpItem* oi = &op.items[i];
        Stroke* st = get(&l->strokes, it->index);
        oi->index = it->index;
        oi->num_points = it->num_points;
        oi->old_pts = it->orig_pts;  // Ownership moves to the op
        it->orig_pts = NULL;
        oi->new_pts = (v2l*)malloc(sizeof(v2l) * (size_t)(it->num_points > 0 ? it->num_points : 1));
        memcpy(oi->new_pts, st->points, sizeof(v2l) * (size_t)it->num_points);
        oi->old_radius = it->orig_radius;
        oi->new_radius = st->brush.radius;
        oi->old_ax = it->orig_ax; oi->old_ay = it->orig_ay;
        oi->new_ax = st->brush.shape_axis_x; oi->new_ay = st->brush.shape_axis_y;
    }
    sel_push_op(milton, op);
    sel_snapshot(milton);
}

static void
sel_cancel_xform(Milton* milton)
{
    Selection* s = milton->selection;
    if ( !s->active ) { return; }
    s->cx = s->c0x; s->cy = s->c0y; s->theta = 0; s->sx = 1; s->sy = 1;
    sel_apply(milton);
}

static void
sel_deselect(Milton* milton)
{
    sel_commit_xform(milton);
    sel_free_items(milton->selection);
}

static void
sel_do_delete(Milton* milton)
{
    Selection* s = milton->selection;
    sel_commit_xform(milton);
    Layer* l = sel_layer(milton);
    if ( !l || s->n == 0 ) { sel_free_items(s); return; }

    SelOp op = {};
    op.kind = SelOp_DELETE;
    op.layer_id = s->layer_id;
    op.n = s->n;
    op.items = (SelOpItem*)calloc((size_t)s->n, sizeof(SelOpItem));
    for ( i32 i = 0; i < s->n; ++i ) {
        op.items[i].index = s->items[i].index;
    }
    list_remove(milton, l, op.items, op.n);
    sel_push_op(milton, op);
    sel_free_items(s);
    milton->render_settings.do_full_redraw = true;
}

// ---- Erased-stroke cleanup

// Radius of a stroke at point i, in canvas units. `reach` grows it to cover a rectangle tip's corners.
static double
sel_radius_at(Stroke* st, i32 i, b32 reach)
{
    const Brush& b = st->brush;
    double size = 1.0;
    if ( b.pressure_size ) {
        double smin = (double)b.pressure_size_min;
        smin = smin < 0.0 ? 0.0 : (smin > 1.0 ? 1.0 : smin);
        size = smin + (1.0 - smin) * (double)st->pressures[i];
    }
    double r = (double)b.radius * size;
    if ( reach && b.shape == BrushShape_RECTANGLE ) {
        r *= sqrt(1.0 + (double)b.shape_aspect * (double)b.shape_aspect);
    }
    return r;
}

static double
sel_dist_to_seg(double px, double py, double ax, double ay, double bx, double by)
{
    double dx = bx - ax, dy = by - ay;
    double l2 = dx * dx + dy * dy;
    double t = l2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / l2 : 0.0;
    t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
    double qx = ax + t * dx - px, qy = ay + t * dy - py;
    return sqrt(qx * qx + qy * qy);
}

// Only a fully opaque, hard, round eraser hides everything beneath it.
static b32
sel_eraser_is_opaque(Stroke* e)
{
    return (e->flags & StrokeFlag_ERASER)
        && !(e->flags & (StrokeFlag_PRESSURE_TO_OPACITY | StrokeFlag_DISTANCE_TO_OPACITY))
        && e->brush.alpha >= 0.999f
        && e->brush.shape == BrushShape_ROUND
        && e->num_points > 0;
}

// Conservative: true only if every segment of `s` (with its thickness) lies inside a single
// capsule of the eraser path. Strokes that merely leave a small visible sliver return false.
static b32
sel_stroke_covered_by(Stroke* s, Stroke* e, double margin)
{
    Rect sb = s->bounding_rect, eb = e->bounding_rect;
    if ( sb.left < eb.left || sb.right > eb.right || sb.top < eb.top || sb.bottom > eb.bottom ) {
        return false;
    }
    i32 m = e->num_points;
    i32 ns = s->num_points;
    if ( ns <= 0 ) { return false; }
    i32 segs = max(m - 1, 1);
    i32 hint = 0;
    i32 pairs = max(ns - 1, 1);
    for ( i32 i = 0; i < pairs; ++i ) {
        i32 j = min(i + 1, ns - 1);
        double pix = (double)s->points[i].x, piy = (double)s->points[i].y;
        double pjx = (double)s->points[j].x, pjy = (double)s->points[j].y;
        double ri = sel_radius_at(s, i, true), rj = sel_radius_at(s, j, true);
        b32 ok = false;
        for ( i32 t = 0; t < segs && !ok; ++t ) {
            i32 k = (hint + t) % segs;
            i32 k2 = min(k + 1, m - 1);
            double R = min(sel_radius_at(e, k, false), sel_radius_at(e, k2, false)) - margin;
            if ( R <= 0 ) { continue; }
            double ax = (double)e->points[k].x, ay = (double)e->points[k].y;
            double bx = (double)e->points[k2].x, by = (double)e->points[k2].y;
            if ( sel_dist_to_seg(pix, piy, ax, ay, bx, by) + ri <= R
                 && sel_dist_to_seg(pjx, pjy, ax, ay, bx, by) + rj <= R ) {
                ok = true;
                hint = k;
            }
        }
        if ( !ok ) { return false; }
    }
    return true;
}

static void
sel_toast(Milton* milton, i32 n)
{
    milton->selection->toast_n = n;
    milton->selection->toast_until = SDL_GetTicks() + 3000;
}

// Removes strokes hidden by a later opaque eraser. Recorded as a normal undo step.
static void
sel_cleanup_layer(Milton* milton, Layer* l)
{
    if ( !l || l->strokes.count < 2 ) { sel_toast(milton, 0); return; }
    i64 count = l->strokes.count;
    u8* covered = (u8*)calloc((size_t)count, 1);
    double margin = (double)milton->view->scale * 2.0;
    i32 n = 0;
    for ( i64 j = 1; j < count; ++j ) {
        Stroke* e = get(&l->strokes, j);
        if ( !sel_eraser_is_opaque(e) ) { continue; }
        for ( i64 i = 0; i < j; ++i ) {
            if ( covered[i] ) { continue; }
            if ( sel_stroke_covered_by(get(&l->strokes, i), e, margin) ) {
                covered[i] = 1;
                ++n;
            }
        }
    }
    if ( n > 0 ) {
        SelOp op = {};
        op.kind = SelOp_DELETE;
        op.layer_id = l->id;
        op.n = n;
        op.items = (SelOpItem*)calloc((size_t)n, sizeof(SelOpItem));
        i32 k = 0;
        for ( i64 i = 0; i < count; ++i ) {
            if ( covered[i] ) { op.items[k++].index = (i32)i; }
        }
        list_remove(milton, l, op.items, op.n);
        sel_push_op(milton, op);
        milton->render_settings.do_full_redraw = true;
    }
    free(covered);
    sel_toast(milton, n);
}

void
selection_auto_cleanup(Milton* milton, Layer* l)
{
    Selection* s = milton->selection;
    if ( !milton->settings->auto_cleanup_erased || !l || l->strokes.count < 2 ) { return; }
    // Selected strokes are tracked by index, which a removal would shift.
    if ( s->active && s->layer_id == l->id ) { return; }
    i64 ei = l->strokes.count - 1;
    Stroke* e = get(&l->strokes, ei);
    if ( !sel_eraser_is_opaque(e) ) { return; }

    double margin = (double)milton->view->scale * 2.0;
    i32 n = 0, cap = 0;
    SelOpItem* items = NULL;
    for ( i64 i = 0; i < ei; ++i ) {
        if ( !sel_stroke_covered_by(get(&l->strokes, i), e, margin) ) { continue; }
        if ( n == cap ) {
            cap = cap ? cap * 2 : 32;
            items = (SelOpItem*)realloc(items, sizeof(SelOpItem) * (size_t)cap);
        }
        items[n] = {};
        items[n].index = (i32)i;
        ++n;
    }
    if ( n == 0 ) { return; }

    SelOp op = {};
    op.kind = SelOp_DELETE;
    op.layer_id = l->id;
    op.n = n;
    op.items = items;
    list_remove(milton, l, op.items, op.n);

    AutoOp ao = {};
    ao.op = op;
    ao.hist_pos = (i64)milton->canvas->history.count;
    push(&s->auto_undo, ao);
    s->dirty = true;
    milton->render_settings.do_full_redraw = true;
}

void
selection_auto_undo(Milton* milton, i64 hist_pos)
{
    Selection* s = milton->selection;
    // Drop anything newer than the entry being undone.
    while ( s->auto_undo.count > 0 && peek(&s->auto_undo)->hist_pos > hist_pos ) {
        AutoOp stale = pop(&s->auto_undo);
        sel_op_free(&stale.op);
    }
    if ( s->auto_undo.count == 0 || peek(&s->auto_undo)->hist_pos != hist_pos ) { return; }
    AutoOp ao = pop(&s->auto_undo);
    Layer* l = layer::get_by_id(milton->canvas->root_layer, ao.op.layer_id);
    if ( l ) { list_insert(l, ao.op.items, ao.op.n); }
    push(&s->auto_redo, ao);
    milton->render_settings.do_full_redraw = true;
}

void
selection_auto_redo(Milton* milton, i64 hist_pos)
{
    Selection* s = milton->selection;
    if ( s->auto_redo.count == 0 || peek(&s->auto_redo)->hist_pos != hist_pos ) { return; }
    AutoOp ao = pop(&s->auto_redo);
    Layer* l = layer::get_by_id(milton->canvas->root_layer, ao.op.layer_id);
    if ( l ) { list_remove(milton, l, ao.op.items, ao.op.n); }
    push(&s->auto_undo, ao);
    milton->render_settings.do_full_redraw = true;
}

// ---- Merge down

static void sel_cut_impl(Milton* milton, Layer* l, Stroke* cutter, i64 limit, b32 push_undo, b32 cut_erasers = false);

static i64
sel_find_stroke_by_id(Layer* l, i32 id)
{
    for ( i64 i = 0; i < l->strokes.count; ++i ) {
        if ( get(&l->strokes, i)->id == id ) { return i; }
    }
    return -1;
}

// Erasers only affect their own layer, but after a merge they would also hit the lower layer.
// Opaque round erasers are baked into the strokes beneath them by cutting those apart, then removed.
// Erasers that touch nothing are dropped. Soft erasers cannot be baked and are kept.
static void
sel_bake_erasers(Milton* milton, Layer* l, b32 harden)
{
    i64 n = l->strokes.count;
    i32* ids = (i32*)malloc(sizeof(i32) * (size_t)(n > 0 ? n : 1));
    i64 m = 0;
    for ( i64 i = 0; i < n; ++i ) {
        Stroke* e = get(&l->strokes, i);
        if ( (e->flags & StrokeFlag_ERASER) ) { ids[m++] = e->id; }
    }
    for ( i64 k = 0; k < m; ++k ) {
        i64 idx = sel_find_stroke_by_id(l, ids[k]);
        if ( idx < 0 ) { continue; }
        Stroke e = *get(&l->strokes, idx);
        if ( harden && (e.flags & StrokeFlag_ERASER) ) {
            e.flags &= ~(StrokeFlag_PRESSURE_TO_OPACITY | StrokeFlag_DISTANCE_TO_OPACITY);
            e.brush.alpha = 1.0f;
        }
        b32 drop = false;
        if ( (e.flags & StrokeFlag_ERASER) && e.num_points > 0 && e.brush.alpha >= 0.999f
             && !(e.flags & (StrokeFlag_PRESSURE_TO_OPACITY | StrokeFlag_DISTANCE_TO_OPACITY)) ) {
            sel_cut_impl(milton, l, &e, idx, false);
            drop = true;
        } else {
            Rect er = e.bounding_rect;
            b32 touches = false;
            for ( i64 i = 0; i < idx && !touches; ++i ) {
                Stroke* s = get(&l->strokes, i);
                if ( (s->flags & StrokeFlag_ERASER) ) { continue; }
                Rect r = s->bounding_rect;
                touches = !(r.right < er.left || r.left > er.right || r.bottom < er.top || r.top > er.bottom);
            }
            drop = !touches;
        }
        if ( drop ) {
            idx = sel_find_stroke_by_id(l, ids[k]);
            if ( idx >= 0 ) {
                SelOpItem item = {};
                item.index = (i32)idx;
                list_remove(milton, l, &item, 1);
            }
        }
    }
    free(ids);
}

// Moves every stroke of the working layer onto the layer below it and removes the working layer.
// Stroke history refers to strokes by position and layer, so it is cleared.
void
layer_merge_down(Milton* milton, b32 harden_erasers)
{
    Layer* up = milton->canvas->working_layer;
    Layer* dn = up ? up->prev : NULL;
    if ( !dn ) { return; }
    selection_finish(milton);
    sel_bake_erasers(milton, up, harden_erasers);
    i64 n = up->strokes.count;
    for ( i64 i = 0; i < n; ++i ) {
        Stroke s = *get(&up->strokes, i);
        s.layer_id = dn->id;
        push(&dn->strokes, s);
    }
    reset(&up->strokes);
    dn->next = up->next;
    if ( up->next ) { up->next->prev = dn; }
    milton_set_working_layer(milton, dn);

    CanvasState* canvas = milton->canvas;
    reset(&canvas->history);
    reset(&canvas->redo_stack);
    reset(&canvas->stroke_graveyard);
    selection_reset(milton);
    milton->selection->dirty = true;
    milton->flags |= MiltonStateFlags_AUTOSAVE_BLOCKED;
    milton->render_settings.do_full_redraw = true;
}

// ---- Optimize layer

struct OptEntry { i32 j, k; };

struct OptState
{
    int   phase;    // 0 idle, 1 confirm, 2 running, 3 done
    int   stage;    // 0 prepare, 1 test strokes, 2 prune erasers, 3 apply
    i32   layer_id;
    char  layer_name[64];
    i64   n;
    u8*   dead;
    i64   cursor;
    i32   gw, gh;
    double gx0, gy0, cell;
    i32*  cell_start;
    OptEntry* entries;
    b32   have_grid;
    i64   removed_strokes, removed_erasers;

    // View mode: only the visible part of the screen is optimised, using the layer opacity measured there.
    b32   view_mode;
    f32   min_pct;          // Layer opacity (percent) below which strokes are cut away.
    i32   vw, vh, vf;       // Opacity grid size and the number of screen pixels per cell.
    float* layer_a;
    float* scratch;
    i32*  dirty;
    i64   dirty_n, dirty_cap;
    struct OptNew { i32 old_index; Stroke stroke; };
    OptNew* news;
    i64   n_new, cap_new;
    i64   cut_strokes;
};
static OptState g_opt;
static b32 g_opt_view_mode = false;
static f32 g_opt_min_pct = 5.0f;

static void
opt_free(OptState* o)
{
    free(o->dead);
    free(o->cell_start);
    free(o->entries);
    free(o->layer_a);
    free(o->scratch);
    free(o->dirty);
    free(o->news);
    *o = {};
}

b32
optimize_active()
{
    return g_opt.phase != 0;
}

void
optimize_request(Milton* milton)
{
    if ( g_opt.phase != 0 || !milton->canvas->working_layer ) { return; }
    selection_finish(milton);
    g_opt.phase = 1;
    g_opt.view_mode = g_opt_view_mode;
    g_opt.min_pct = g_opt_min_pct;
    g_opt.layer_id = milton->canvas->working_layer->id;
    snprintf(g_opt.layer_name, sizeof(g_opt.layer_name), "%s", milton->canvas->working_layer->name);
}

static b32
opt_eraser_usable(Stroke* e)
{
    return (e->flags & StrokeFlag_ERASER) && e->num_points > 0 && e->brush.shape == BrushShape_ROUND;
}

// Lower bound of the opacity with which eraser stroke `e` clears canvas point (px, py) through segment k.
static double
opt_eraser_alpha(Stroke* e, i32 k, double px, double py, double scale, double rmul = 1.0, double pad = 0.0)
{
    i32 k2 = min(k + 1, e->num_points - 1);
    double ax = (double)e->points[k].x, ay = (double)e->points[k].y;
    double bx = (double)e->points[k2].x, by = (double)e->points[k2].y;
    double dx = bx - ax, dy = by - ay;
    double l2 = dx * dx + dy * dy;
    double t = l2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / l2 : 0.0;
    t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
    double qx = ax + t * dx - px, qy = ay + t * dy - py;
    double dist = sqrt(qx * qx + qy * qy);
    double pr = (double)e->pressures[k] + ((double)e->pressures[k2] - (double)e->pressures[k]) * t;
    const Brush& b = e->brush;
    double size = 1.0;
    if ( b.pressure_size ) {
        double smin = (double)b.pressure_size_min;
        smin = smin < 0.0 ? 0.0 : (smin > 1.0 ? 1.0 : smin);
        size = smin + (1.0 - smin) * pr;
    }
    double rad = (double)b.radius * size * rmul + pad;
    double cov = (rad - dist) / (0.5 * scale);
    if ( cov <= 0.0 ) { return 0.0; }
    if ( cov > 1.0 ) { cov = 1.0; }
    double a = 1.0;
    if ( e->flags & StrokeFlag_PRESSURE_TO_OPACITY ) {
        a = (1.0 - (double)b.pressure_opacity_min) * pr + (double)b.pressure_opacity_min;
    }
    a *= (double)(b.alpha < 0.0f ? 0.0f : (b.alpha > 1.0f ? 1.0f : b.alpha));
    if ( e->flags & StrokeFlag_DISTANCE_TO_OPACITY ) {
        double h = ((double)b.hardness - 1.0) / 9.0;
        h = h < 0.0 ? 0.0 : (h > 1.0 ? 1.0 : h);
        double core = h * h;
        double x = dist / rad;
        x = x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x);
        double tt = (x - core) / max(1.0 - core, 0.0001);
        tt = tt < 0.0 ? 0.0 : (tt > 1.0 ? 1.0 : tt);
        double g1 = exp(-4.0);
        a *= (exp(-4.0 * tt * tt) - g1) / (1.0 - g1);
    }
    return cov * a;
}

static void
opt_build_grid(Milton* milton, Layer* l)
{
    OptState* o = &g_opt;
    i64 n = l->strokes.count;
    i64 minx = INT64_MAX, miny = INT64_MAX, maxx = INT64_MIN, maxy = INT64_MIN;
    double cell = 0;
    i64 nseg = 0;
    for ( i64 i = 0; i < n; ++i ) {
        Stroke* e = get(&l->strokes, i);
        if ( !opt_eraser_usable(e) ) { continue; }
        Rect r = e->bounding_rect;
        minx = min(minx, r.left); maxx = max(maxx, r.right);
        miny = min(miny, r.top); maxy = max(maxy, r.bottom);
        cell = max(cell, (double)e->brush.radius * 2.0);
        nseg += max(e->num_points - 1, 1);
    }
    if ( nseg == 0 ) { return; }
    if ( cell < 1.0 ) { cell = 1.0; }
    double w = (double)(maxx - minx) + 1, h = (double)(maxy - miny) + 1;
    while ( (w / cell + 1) * (h / cell + 1) > 4.0e6 ) { cell *= 2.0; }
    o->cell = cell;
    o->gx0 = (double)minx;
    o->gy0 = (double)miny;
    o->gw = (i32)(w / cell) + 1;
    o->gh = (i32)(h / cell) + 1;
    i64 cells = (i64)o->gw * o->gh;
    o->cell_start = (i32*)calloc((size_t)cells + 1, sizeof(i32));

    // Two passes: count entries per cell, then fill them. Entries end up ordered by eraser index.
    i32* fillpos = NULL;
    for ( int pass = 0; pass < 2; ++pass ) {
        if ( pass == 1 ) {
            i64 acc = 0;
            for ( i64 c = 0; c < cells; ++c ) { i64 cnt = o->cell_start[c]; o->cell_start[c] = (i32)acc; acc += cnt; }
            o->cell_start[cells] = (i32)acc;
            o->entries = (OptEntry*)malloc(sizeof(OptEntry) * (size_t)(acc > 0 ? acc : 1));
            fillpos = (i32*)malloc(sizeof(i32) * (size_t)(cells + 1));
            memcpy(fillpos, o->cell_start, sizeof(i32) * (size_t)(cells + 1));
        }
        for ( i64 i = 0; i < n; ++i ) {
            Stroke* e = get(&l->strokes, i);
            if ( !opt_eraser_usable(e) ) { continue; }
            double R = (double)e->brush.radius;
            i32 segs = max(e->num_points - 1, 1);
            for ( i32 k = 0; k < segs; ++k ) {
                i32 k2 = min(k + 1, e->num_points - 1);
                double x0 = min((double)e->points[k].x, (double)e->points[k2].x) - R;
                double x1 = max((double)e->points[k].x, (double)e->points[k2].x) + R;
                double y0 = min((double)e->points[k].y, (double)e->points[k2].y) - R;
                double y1 = max((double)e->points[k].y, (double)e->points[k2].y) + R;
                i32 cx0 = max((i32)floor((x0 - o->gx0) / cell), 0), cx1 = min((i32)floor((x1 - o->gx0) / cell), o->gw - 1);
                i32 cy0 = max((i32)floor((y0 - o->gy0) / cell), 0), cy1 = min((i32)floor((y1 - o->gy0) / cell), o->gh - 1);
                for ( i32 cy = cy0; cy <= cy1; ++cy ) {
                    for ( i32 cx = cx0; cx <= cx1; ++cx ) {
                        i64 c = (i64)cy * o->gw + cx;
                        if ( pass == 0 ) { o->cell_start[c]++; }
                        else { o->entries[fillpos[c]++] = { (i32)i, k }; }
                    }
                }
            }
        }
    }
    free(fillpos);
    o->have_grid = true;
}

// Is any part of the layer at canvas point (px, py) still visible after strokes later than `after` erase it?
static b32
opt_visible_at(Layer* l, double px, double py, i64 after, double scale)
{
    OptState* o = &g_opt;
    if ( !o->have_grid ) { return true; }
    i32 cx = (i32)floor((px - o->gx0) / o->cell), cy = (i32)floor((py - o->gy0) / o->cell);
    if ( cx < 0 || cy < 0 || cx >= o->gw || cy >= o->gh ) { return true; }
    i64 c = (i64)cy * o->gw + cx;
    i32 lo = o->cell_start[c], hi = o->cell_start[c + 1];
    // First entry with j > after.
    i32 a = lo, b = hi;
    while ( a < b ) {
        i32 mid = (a + b) / 2;
        if ( o->entries[mid].j > after ) { b = mid; } else { a = mid + 1; }
    }
    double remaining = 1.0;
    while ( a < hi ) {
        i32 j = o->entries[a].j;
        Stroke* e = get(&l->strokes, j);
        double best = 0.0;
        while ( a < hi && o->entries[a].j == j ) {
            double v = opt_eraser_alpha(e, o->entries[a].k, px, py, scale);
            if ( v > best ) { best = v; }
            ++a;
        }
        remaining *= (1.0 - best);
        if ( remaining <= 0.01 ) { return false; }
    }
    return true;
}

// Samples the stroke's footprint (centre line, flanks, and a ring at every vertex). One surviving sample means it is visible.
static b32
opt_stroke_visible(Layer* l, i64 idx, double scale)
{
    Stroke* st = get(&l->strokes, idx);
    i32 ns = st->num_points;
    for ( i32 i = 0; i < ns; ++i ) {
        double ax = (double)st->points[i].x, ay = (double)st->points[i].y;
        double r = sel_radius_at(st, i, true);
        double rr = r > scale ? r - scale : r * 0.5;
        if ( opt_visible_at(l, ax, ay, idx, scale) ) { return true; }
        i32 m = (i32)ceil(2.0 * 3.14159265 * rr / (6.0 * scale));
        m = m < 8 ? 8 : (m > 24 ? 24 : m);
        for ( i32 q = 0; q < m; ++q ) {
            double ang = 6.28318530718 * q / m;
            if ( opt_visible_at(l, ax + cos(ang) * rr, ay + sin(ang) * rr, idx, scale) ) { return true; }
        }
        if ( i + 1 >= ns ) { break; }
        double bx = (double)st->points[i + 1].x, by = (double)st->points[i + 1].y;
        double r2 = sel_radius_at(st, i + 1, true);
        double rr2 = r2 > scale ? r2 - scale : r2 * 0.5;
        double dx = bx - ax, dy = by - ay;
        double len = sqrt(dx * dx + dy * dy);
        if ( len < 1e-9 ) { continue; }
        double nx = -dy / len, ny = dx / len;
        double step = min(0.6 * min(r, r2), 6.0 * scale);
        if ( step < 0.5 * scale ) { step = 0.5 * scale; }
        i32 steps = (i32)ceil(len / step);
        for ( i32 s = 1; s < steps; ++s ) {
            double t = (double)s / steps;
            double cx = ax + dx * t, cy = ay + dy * t;
            double rt = r + (r2 - r) * t, rrt = rr + (rr2 - rr) * t;
            if ( opt_visible_at(l, cx, cy, idx, scale) ) { return true; }
            double offs[3] = { 0.5 * rt, rrt, 0.0 };
            for ( int w = 0; w < 2; ++w ) {
                if ( opt_visible_at(l, cx + nx * offs[w], cy + ny * offs[w], idx, scale) ) { return true; }
                if ( opt_visible_at(l, cx - nx * offs[w], cy - ny * offs[w], idx, scale) ) { return true; }
            }
        }
    }
    return false;
}

struct CutPoints
{
    v2l* p;
    f32* q;
    i32  n, cap;
};

static void
cut_points_push(CutPoints* b, v2l p, f32 q)
{
    if ( b->n == b->cap ) {
        b->cap = b->cap ? b->cap * 2 : 64;
        b->p = (v2l*)realloc(b->p, sizeof(v2l) * (size_t)b->cap);
        b->q = (f32*)realloc(b->q, sizeof(f32) * (size_t)b->cap);
    }
    b->p[b->n] = p;
    b->q[b->n] = q;
    ++b->n;
}

// ---- View mode: measure the layer opacity on screen, then cut away what is fainter than the minimum.

// Layer opacity at a canvas point, or -1 when the point is off screen (those parts are left alone).
static double
opt_view_alpha_at(Milton* milton, double cx, double cy)
{
    OptState* o = &g_opt;
    v2l r = canvas_to_raster(milton->view, v2l{ (i64)llround(cx), (i64)llround(cy) });
    if ( r.x < 0 || r.y < 0 ) { return -1.0; }
    i64 gx = r.x / o->vf, gy = r.y / o->vf;
    if ( gx >= o->vw || gy >= o->vh ) { return -1.0; }
    return (double)o->layer_a[gy * o->vw + gx];
}

// Coverage of canvas point (px, py) by a rectangle eraser swept along segment k, or 0. Pressure takes the smaller
// end of the segment, so this never overestimates.
static double
opt_rect_eraser_alpha(Stroke* e, i32 k, double px, double py, double scale)
{
    const Brush& b = e->brush;
    i32 k2 = min(k + 1, e->num_points - 1);
    double pr = min((double)e->pressures[k], (double)e->pressures[k2]);
    double size = 1.0;
    if ( b.pressure_size ) {
        double smin = (double)b.pressure_size_min;
        smin = smin < 0.0 ? 0.0 : (smin > 1.0 ? 1.0 : smin);
        size = smin + (1.0 - smin) * pr;
    }
    double R = (double)b.radius * size;
    double asp = (double)b.shape_aspect;
    if ( R <= 0.0 || asp <= 0.0 ) { return 0.0; }
    double ax = (double)b.shape_axis_x, ay = (double)b.shape_axis_y;
    double al = sqrt(ax * ax + ay * ay);
    if ( al < 1e-9 ) { return 0.0; }
    ax /= al; ay /= al;
    double x0 = (double)e->points[k].x, y0 = (double)e->points[k].y;
    double dx = (double)e->points[k2].x - x0, dy = (double)e->points[k2].y - y0;
    double vx = px - x0, vy = py - y0;
    // Box-normalised coordinates of the point relative to the segment start, and of the segment itself.
    double u0 = (vx * ax + vy * ay) / R, w0 = (-vx * ay + vy * ax) / (R * asp);
    double du = (dx * ax + dy * ay) / R, dw = (-dx * ay + dy * ax) / (R * asp);
    double lo = 0.0, hi = 1.0;
    auto cheb = [&](double t) { return max(fabs(u0 - t * du), fabs(w0 - t * dw)); };
    for ( int it = 0; it < 28; ++it ) {
        double m1 = lo + (hi - lo) / 3.0, m2 = hi - (hi - lo) / 3.0;
        if ( cheb(m1) < cheb(m2) ) { hi = m2; } else { lo = m1; }
    }
    double c = cheb(0.5 * (lo + hi));
    double cov = (1.0 - c) * R * asp / (0.5 * scale);
    if ( cov <= 0.0 ) { return 0.0; }
    if ( cov > 1.0 ) { cov = 1.0; }
    double a = 1.0;
    if ( e->flags & StrokeFlag_PRESSURE_TO_OPACITY ) {
        a = (1.0 - (double)b.pressure_opacity_min) * pr + (double)b.pressure_opacity_min;
    }
    a *= (double)(b.alpha < 0.0f ? 0.0f : (b.alpha > 1.0f ? 1.0f : b.alpha));
    if ( e->flags & StrokeFlag_DISTANCE_TO_OPACITY ) {
        double h = ((double)b.hardness - 1.0) / 9.0;
        h = h < 0.0 ? 0.0 : (h > 1.0 ? 1.0 : h);
        double core = h * h;
        double x = c < 0.0 ? 0.0 : (c > 1.0 ? 1.0 : c);
        double tt = (x - core) / max(1.0 - core, 0.0001);
        tt = tt < 0.0 ? 0.0 : (tt > 1.0 ? 1.0 : tt);
        double g1 = exp(-4.0);
        a *= (exp(-4.0 * tt * tt) - g1) / (1.0 - g1);
    }
    return cov * a;
}

static void
opt_paint_stroke(Milton* milton, Stroke* st, double scale)
{
    OptState* o = &g_opt;
    CanvasView* view = milton->view;
    const b32 eraser = (st->flags & StrokeFlag_ERASER) != 0;
    const b32 rect_eraser = eraser && st->brush.shape == BrushShape_RECTANGLE;
    if ( eraser && !rect_eraser && st->brush.shape != BrushShape_ROUND ) { return; }
    double rmul = 1.0;
    if ( st->brush.shape == BrushShape_RECTANGLE ) {
        rmul = sqrt(1.0 + (double)st->brush.shape_aspect * (double)st->brush.shape_aspect);
    }
    // Strokes thinner than a cell would fall between cell centres, so grow them (never for erasers).
    double pad = eraser ? 0.0 : 0.75 * (double)o->vf * scale;
    i32 segs = max(st->num_points - 1, 1);
    o->dirty_n = 0;
    for ( i32 k = 0; k < segs; ++k ) {
        i32 k2 = min(k + 1, st->num_points - 1);
        double ax = (double)st->points[k].x, ay = (double)st->points[k].y;
        double bx = (double)st->points[k2].x, by = (double)st->points[k2].y;
        double R = max(sel_radius_at(st, k, false), sel_radius_at(st, k2, false)) * rmul + pad + scale;
        double x0 = min(ax, bx) - R, x1 = max(ax, bx) + R, y0 = min(ay, by) - R, y1 = max(ay, by) + R;
        v2l c[4] = {
            canvas_to_raster(view, v2l{ (i64)x0, (i64)y0 }), canvas_to_raster(view, v2l{ (i64)x1, (i64)y0 }),
            canvas_to_raster(view, v2l{ (i64)x0, (i64)y1 }), canvas_to_raster(view, v2l{ (i64)x1, (i64)y1 }),
        };
        i64 rx0 = min(min(c[0].x, c[1].x), min(c[2].x, c[3].x)), rx1 = max(max(c[0].x, c[1].x), max(c[2].x, c[3].x));
        i64 ry0 = min(min(c[0].y, c[1].y), min(c[2].y, c[3].y)), ry1 = max(max(c[0].y, c[1].y), max(c[2].y, c[3].y));
        if ( rx1 < 0 || ry1 < 0 ) { continue; }
        i64 gx0 = max(rx0 / o->vf - 1, (i64)0), gx1 = min(rx1 / o->vf + 1, (i64)o->vw - 1);
        i64 gy0 = max(ry0 / o->vf - 1, (i64)0), gy1 = min(ry1 / o->vf + 1, (i64)o->vh - 1);
        for ( i64 gy = gy0; gy <= gy1; ++gy ) {
            for ( i64 gx = gx0; gx <= gx1; ++gx ) {
                v2l p = raster_to_canvas(view, v2l{ gx * o->vf + o->vf / 2, gy * o->vf + o->vf / 2 });
                double a = rect_eraser ? opt_rect_eraser_alpha(st, k, (double)p.x, (double)p.y, scale)
                                       : opt_eraser_alpha(st, k, (double)p.x, (double)p.y, scale, rmul, pad);
                if ( a <= 0.0 ) { continue; }
                i64 ci = gy * o->vw + gx;
                if ( o->scratch[ci] == 0.0f ) {
                    if ( o->dirty_n == o->dirty_cap ) {
                        o->dirty_cap = o->dirty_cap ? o->dirty_cap * 2 : 1024;
                        o->dirty = (i32*)realloc(o->dirty, sizeof(i32) * (size_t)o->dirty_cap);
                    }
                    o->dirty[o->dirty_n++] = (i32)ci;
                }
                if ( (float)a > o->scratch[ci] ) { o->scratch[ci] = (float)a; }
            }
        }
    }
    for ( i64 d = 0; d < o->dirty_n; ++d ) {
        i32 ci = o->dirty[d];
        float sa = o->scratch[ci];
        o->scratch[ci] = 0.0f;
        if ( eraser ) { o->layer_a[ci] *= (1.0f - sa); }
        else { o->layer_a[ci] += (1.0f - o->layer_a[ci]) * sa; }
    }
}

struct OptSample { v2l p; f32 q; double r, dx, dy; b32 vertex, keep; };

// Samples the stroke along its path, drops the samples whose surroundings are fainter than the minimum, and
// rebuilds the stroke from the runs that remain.
static void
opt_eval_stroke(Milton* milton, Layer* l, i64 idx, double scale)
{
    OptState* o = &g_opt;
    Stroke* st = get(&l->strokes, idx);
    i32 ns = st->num_points;
    const double thr = (double)o->min_pct / 100.0;
    double step = 2.0 * (double)o->vf * scale;
    static OptSample* smp = NULL;
    static i64 smp_cap = 0;
    i64 n = 0;
    auto add = [&](v2l p, f32 q, double r, double dx, double dy, b32 vertex) {
        if ( n == smp_cap ) {
            smp_cap = smp_cap ? smp_cap * 2 : 256;
            smp = (OptSample*)realloc(smp, sizeof(OptSample) * (size_t)smp_cap);
        }
        smp[n].p = p; smp[n].q = q; smp[n].r = r; smp[n].dx = dx; smp[n].dy = dy;
        smp[n].vertex = vertex; smp[n].keep = false;
        ++n;
    };
    for ( i32 i = 0; i < ns; ++i ) {
        double ax = (double)st->points[i].x, ay = (double)st->points[i].y;
        double r = sel_radius_at(st, i, true);
        double dx = 0, dy = 0;
        if ( i + 1 < ns ) { dx = (double)st->points[i + 1].x - ax; dy = (double)st->points[i + 1].y - ay; }
        else if ( i > 0 ) { dx = ax - (double)st->points[i - 1].x; dy = ay - (double)st->points[i - 1].y; }
        add(st->points[i], st->pressures[i], r, dx, dy, true);
        if ( i + 1 >= ns ) { break; }
        double len = sqrt(dx * dx + dy * dy);
        i32 m = (i32)ceil(len / step);
        double r2 = sel_radius_at(st, i + 1, true);
        for ( i32 s = 1; s < m; ++s ) {
            double t = (double)s / m;
            v2l p = { (i64)llround(ax + dx * t), (i64)llround(ay + dy * t) };
            f32 q = st->pressures[i] + (st->pressures[i + 1] - st->pressures[i]) * (f32)t;
            add(p, q, r + (r2 - r) * t, dx, dy, false);
        }
    }

    i64 kept = 0;
    for ( i64 j = 0; j < n; ++j ) {
        OptSample* sp = &smp[j];
        double len = sqrt(sp->dx * sp->dx + sp->dy * sp->dy);
        double nx = len > 1e-9 ? -sp->dy / len : 1.0, ny = len > 1e-9 ? sp->dx / len : 0.0;
        const double offs[3] = { 0.0, 0.4, -0.4 };
        for ( int w = 0; w < 3 && !sp->keep; ++w ) {
            double a = opt_view_alpha_at(milton, (double)sp->p.x + nx * offs[w] * sp->r, (double)sp->p.y + ny * offs[w] * sp->r);
            if ( a < 0.0 || a >= thr ) { sp->keep = true; }
        }
        if ( sp->keep ) { ++kept; }
    }
    if ( kept == n ) { return; }
    if ( kept == 0 ) {
        o->dead[idx] = 1;
        ++o->removed_strokes;
        return;
    }

    CutPoints cur = {};
    for ( i64 j = 0; j < n; ++j ) {
        if ( !smp[j].keep ) { continue; }
        b32 prev_keep = j > 0 && smp[j - 1].keep;
        b32 next_keep = j + 1 < n && smp[j + 1].keep;
        if ( !prev_keep || !next_keep || smp[j].vertex ) { cut_points_push(&cur, smp[j].p, smp[j].q); }
        if ( next_keep ) { continue; }
        Stroke piece = *st;
        piece.num_points = cur.n;
        piece.points = arena_alloc_array(&milton->canvas->arena, cur.n, v2l);
        piece.pressures = arena_alloc_array(&milton->canvas->arena, cur.n, f32);
        memcpy(piece.points, cur.p, sizeof(v2l) * (size_t)cur.n);
        memcpy(piece.pressures, cur.q, sizeof(f32) * (size_t)cur.n);
        piece.render_handle = 0;
        piece.id = milton->canvas->stroke_id_count++;
        piece.bounding_rect = bounding_box_for_stroke(&piece);
        if ( o->n_new == o->cap_new ) {
            o->cap_new = o->cap_new ? o->cap_new * 2 : 64;
            o->news = (OptState::OptNew*)realloc(o->news, sizeof(OptState::OptNew) * (size_t)o->cap_new);
        }
        o->news[o->n_new].old_index = (i32)idx;
        o->news[o->n_new].stroke = piece;
        ++o->n_new;
        cur.n = 0;
    }
    free(cur.p);
    free(cur.q);
    o->dead[idx] = 2;
    ++o->cut_strokes;
}

// True when some part of stroke `s` lies within reach of eraser `e`. Coarse by design: it only ever errs toward "yes".
static b32
opt_near_eraser(Stroke* s, Stroke* e, double scale)
{
    Rect eb = e->bounding_rect;
    double er = 0;
    for ( i32 j = 0; j < e->num_points; ++j ) { er = max(er, sel_radius_at(e, j, true)); }
    double lim = er + scale;
    for ( i32 i = 0; i < s->num_points; ++i ) {
        double sr = sel_radius_at(s, i, true);
        double ax = (double)s->points[i].x, ay = (double)s->points[i].y;
        i32 i2 = min(i + 1, s->num_points - 1);
        double dx = (double)s->points[i2].x - ax, dy = (double)s->points[i2].y - ay;
        double len = sqrt(dx * dx + dy * dy);
        double step = max(sr * 0.5, scale);
        i32 m = max((i32)ceil(len / step), 1);
        for ( i32 q = 0; q < m; ++q ) {
            double t = (double)q / m;
            double px = ax + dx * t, py = ay + dy * t;
            double reach = lim + sr;
            if ( px < (double)eb.left - reach || px > (double)eb.right + reach ||
                 py < (double)eb.top - reach || py > (double)eb.bottom + reach ) { continue; }
            for ( i32 j = 0; j < e->num_points; ++j ) {
                i32 j2 = min(j + 1, e->num_points - 1);
                double d = sel_dist_to_seg(px, py, (double)e->points[j].x, (double)e->points[j].y,
                                           (double)e->points[j2].x, (double)e->points[j2].y);
                if ( d <= reach ) { return true; }
            }
        }
    }
    return false;
}

static void
opt_apply(Milton* milton, Layer* l)
{
    OptState* o = &g_opt;
    i32 n = 0;
    for ( i64 i = 0; i < o->n; ++i ) { if ( o->dead[i] ) { ++n; } }
    if ( n > 0 ) {
        SelOpItem* items = (SelOpItem*)calloc((size_t)n, sizeof(SelOpItem));
        i32 k = 0;
        for ( i64 i = 0; i < o->n; ++i ) {
            if ( o->dead[i] ) { items[k++].index = (i32)i; }
        }
        list_remove(milton, l, items, n);
        free(items);
    }
    if ( o->n_new > 0 ) {
        // A cut stroke is replaced in place by its pieces.
        SelOpItem* items = (SelOpItem*)calloc((size_t)o->n_new, sizeof(SelOpItem));
        i64 new_index = 0, ni = 0;
        for ( i64 si = 0; si < o->n; ++si ) {
            if ( o->dead[si] == 0 ) { ++new_index; }
            else if ( o->dead[si] == 2 ) {
                while ( ni < o->n_new && o->news[ni].old_index == si ) {
                    items[ni].index = (i32)new_index++;
                    items[ni].stroke = o->news[ni].stroke;
                    ++ni;
                }
            }
        }
        list_insert(l, items, (i32)o->n_new);
        free(items);
    }

    // The undo history refers to strokes by position, so it cannot survive.
    CanvasState* canvas = milton->canvas;
    reset(&canvas->history);
    reset(&canvas->redo_stack);
    reset(&canvas->stroke_graveyard);
    selection_reset(milton);
    milton->selection->dirty = true;
    milton->flags |= MiltonStateFlags_AUTOSAVE_BLOCKED;
    milton->render_settings.do_full_redraw = true;
}

static void
opt_step(Milton* milton)
{
    OptState* o = &g_opt;
    Layer* l = layer::get_by_id(milton->canvas->root_layer, o->layer_id);
    if ( !l ) { opt_free(o); return; }
    double scale = (double)milton->view->scale;
    u32 t0 = SDL_GetTicks();
    auto over_budget = [&]() { return SDL_GetTicks() - t0 > 25; };

    if ( o->stage == 0 ) {
        o->n = l->strokes.count;
        o->dead = (u8*)calloc((size_t)(o->n > 0 ? o->n : 1), 1);
        o->cursor = 0;
        if ( o->view_mode ) {
            i32 w = milton->view->screen_size.w, h = milton->view->screen_size.h;
            o->vf = 1;
            while ( (i64)(w / o->vf + 1) * (h / o->vf + 1) > 4000000 ) { ++o->vf; }
            o->vw = w / o->vf + 1;
            o->vh = h / o->vf + 1;
            o->layer_a = (float*)calloc((size_t)o->vw * o->vh, sizeof(float));
            o->scratch = (float*)calloc((size_t)o->vw * o->vh, sizeof(float));
            o->stage = 10;
            return;
        }
        opt_build_grid(milton, l);
        o->stage = 1;
        return;
    }
    if ( o->stage == 10 ) {
        while ( o->cursor < o->n && !over_budget() ) {
            Stroke* st = get(&l->strokes, o->cursor);
            if ( st->num_points > 0 ) { opt_paint_stroke(milton, st, scale); }
            ++o->cursor;
        }
        if ( o->cursor >= o->n ) { o->cursor = 0; o->stage = 11; }
        return;
    }
    if ( o->stage == 11 ) {
        while ( o->cursor < o->n && !over_budget() ) {
            Stroke* st = get(&l->strokes, o->cursor);
            if ( !(st->flags & StrokeFlag_ERASER) && st->num_points > 0 ) { opt_eval_stroke(milton, l, o->cursor, scale); }
            ++o->cursor;
        }
        if ( o->cursor >= o->n ) { o->cursor = 0; o->stage = 2; }
        return;
    }
    if ( o->stage == 1 ) {
        while ( o->cursor < o->n && !over_budget() ) {
            Stroke* st = get(&l->strokes, o->cursor);
            if ( !(st->flags & StrokeFlag_ERASER) && st->num_points > 0 ) {
                if ( !opt_stroke_visible(l, o->cursor, scale) ) {
                    o->dead[o->cursor] = 1;
                    ++o->removed_strokes;
                }
            }
            ++o->cursor;
        }
        if ( o->cursor >= o->n ) { o->cursor = 0; o->stage = 2; }
        return;
    }
    if ( o->stage == 2 ) {
        // An eraser that no surviving earlier stroke touches does nothing.
        while ( o->cursor < o->n && !over_budget() ) {
            i64 j = o->cursor;
            Stroke* e = get(&l->strokes, j);
            if ( (e->flags & StrokeFlag_ERASER) ) {
                Rect er = e->bounding_rect;
                b32 touches = false;
                for ( i64 i = 0; i < j && !touches; ++i ) {
                    if ( o->dead[i] == 1 ) { continue; }
                    Stroke* s = get(&l->strokes, i);
                    if ( (s->flags & StrokeFlag_ERASER) ) { continue; }
                    Rect r = s->bounding_rect;
                    if ( r.right < er.left || r.left > er.right || r.bottom < er.top || r.top > er.bottom ) { continue; }
                    if ( o->dead[i] == 2 ) {
                        // Judge by the pieces that survive, not by the stroke they were cut from.
                        for ( i64 q = 0; q < o->n_new && !touches; ++q ) {
                            if ( o->news[q].old_index == i ) { touches = opt_near_eraser(&o->news[q].stroke, e, scale); }
                        }
                    } else {
                        touches = opt_near_eraser(s, e, scale);
                    }
                }
                if ( !touches ) {
                    o->dead[j] = 1;
                    ++o->removed_erasers;
                }
            }
            ++o->cursor;
        }
        if ( o->cursor >= o->n ) { o->stage = 3; }
        return;
    }
    opt_apply(milton, l);
    free(o->cell_start); o->cell_start = NULL;
    free(o->entries); o->entries = NULL;
    o->phase = 3;
}

void
optimize_draw(Milton* milton)
{
    OptState* o = &g_opt;
    if ( o->phase == 0 ) { return; }
    f32 ui = milton->gui->scale;
    const char* title = "Optimize Layer";
    if ( !ImGui::IsPopupOpen(title) ) { ImGui::OpenPopup(title); }
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if ( !ImGui::BeginPopupModal(title, NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove) ) {
        return;
    }
    const f32 wrap = ui * 420;
    if ( o->phase == 1 ) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
        ImGui::Text("Layer: %s", o->layer_name);
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.3f, 1.0f));
        ImGui::TextWrapped("WARNING: THIS CANNOT BE UNDONE.");
        ImGui::TextWrapped("ALL undo and redo history for the entire drawing will be permanently erased, "
                           "and you will not be able to get it back.");
        ImGui::TextWrapped("Autosave will also be PAUSED afterwards: nothing is written to disk until you save yourself "
                           "(Ctrl+S). Closing without saving discards the optimization and keeps the old file.");
        ImGui::PopStyleColor();
        ImGui::Spacing();
        if ( o->view_mode ) {
            ImGui::TextWrapped("Only what is visible in the current view is changed. The layer opacity on screen is measured, "
                               "and every stroke part sitting where the layer is fainter than the minimum below is cut away and "
                               "permanently deleted. Parts off screen are left alone.");
        } else {
            ImGui::TextWrapped("Strokes on this layer that are completely hidden by erasing (including soft erasers) "
                               "and erasers that no longer affect anything will be permanently deleted. "
                               "This can take a while on large layers.");
        }
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if ( ImGui::Checkbox("Current view only, remove faint areas", (bool*)&o->view_mode) ) { g_opt_view_mode = o->view_mode; }
        if ( o->view_mode ) {
            ImGui::PushItemWidth(ui * 200);
            if ( ImGui::SliderFloat("Minimum opacity to keep", &o->min_pct, 1.0f, 50.0f, "%.0f%%") ) { g_opt_min_pct = o->min_pct; }
            ImGui::PopItemWidth();
        }
        ImGui::Spacing();
        if ( ImGui::Button("Optimize and erase undo history", ImVec2(ui * 240, 0)) ) {
            o->phase = 2;
            o->stage = 0;
        }
        ImGui::SameLine();
        if ( ImGui::Button("Cancel", ImVec2(ui * 100, 0)) ) {
            ImGui::CloseCurrentPopup();
            opt_free(o);
        }
    } else if ( o->phase == 2 ) {
        f32 frac = 0.0f;
        const char* what = "Preparing...";
        if ( o->n > 0 ) {
            if ( o->stage == 10 ) { frac = 0.45f * (f32)((double)o->cursor / (double)o->n); what = "Measuring layer opacity in view..."; }
            else if ( o->stage == 11 ) { frac = 0.45f + 0.45f * (f32)((double)o->cursor / (double)o->n); what = "Cutting away faint areas..."; }
            else if ( o->stage == 1 ) { frac = 0.9f * (f32)((double)o->cursor / (double)o->n); what = "Checking which strokes are visible..."; }
            else if ( o->stage == 2 ) { frac = 0.9f + 0.09f * (f32)((double)o->cursor / (double)o->n); what = "Removing unused erasers..."; }
            else if ( o->stage == 3 ) { frac = 0.99f; what = "Applying..."; }
        }
        ImGui::Text("Optimizing layer: %s", o->layer_name);
        ImGui::Text("%s", what);
        ImGui::ProgressBar(frac, ImVec2(ui * 360, ui * 22));
        if ( ImGui::Button("Cancel", ImVec2(ui * 100, 0)) ) {
            // Nothing has been modified before the final apply step.
            ImGui::CloseCurrentPopup();
            opt_free(o);
            ImGui::EndPopup();
            return;
        }
        opt_step(milton);
        if ( milton->platform ) { milton->platform->force_next_frame = true; }
    } else {
        ImGui::Text("Done. Removed %lld hidden strokes and %lld unused erasers.",
                    (long long)o->removed_strokes, (long long)o->removed_erasers);
        if ( o->view_mode ) { ImGui::Text("Trimmed %lld strokes at the faint areas.", (long long)o->cut_strokes); }
        ImGui::Text("Undo history has been cleared.");
        if ( ImGui::Button("OK", ImVec2(ui * 100, 0)) ) {
            ImGui::CloseCurrentPopup();
            opt_free(o);
        }
    }
    ImGui::EndPopup();
}
// ---- Cutting eraser

struct CutCap
{
    double ax, ay, bx, by, R;
    double ux, uy, L;
    double minx, miny, maxx, maxy;  // Of the centre line
    // Rectangle brushes: convex hull (CCW) of the rectangle swept along the segment.
    b32 is_rect;
    i32 np;
    double hx[8], hy[8];
};

static void
sel_cap_build_rect_hull(CutCap* c, Stroke* cutter, i32 k, i32 k2)
{
    double axx = (double)cutter->brush.shape_axis_x, axy = (double)cutter->brush.shape_axis_y;
    double al = sqrt(axx * axx + axy * axy);
    if ( al < 1e-9 ) { axx = 1; axy = 0; al = 1; }
    axx /= al; axy /= al;
    double pxx = -axy, pxy = axx;
    double aspect = (double)cutter->brush.shape_aspect;
    double ends[2] = { sel_radius_at(cutter, k, false), sel_radius_at(cutter, k2, false) };
    double r = max(ends[0], ends[1]);
    double hl = r, hw = r * aspect;  // Half extents along the long axis and the short axis.
    double pts[16][2];
    i32 n = 0;
    for ( int e = 0; e < 2; ++e ) {
        double cx = e ? c->bx : c->ax, cy = e ? c->by : c->ay;
        for ( int sx = -1; sx <= 1; sx += 2 ) {
            for ( int sy = -1; sy <= 1; sy += 2 ) {
                pts[n][0] = cx + axx * hl * sx + pxx * hw * sy;
                pts[n][1] = cy + axy * hl * sx + pxy * hw * sy;
                ++n;
            }
        }
    }
    // Monotone chain.
    i32 idx[8];
    for ( i32 i = 0; i < n; ++i ) { idx[i] = i; }
    for ( i32 i = 1; i < n; ++i ) {
        i32 v = idx[i]; i32 j = i - 1;
        while ( j >= 0 && (pts[idx[j]][0] > pts[v][0] || (pts[idx[j]][0] == pts[v][0] && pts[idx[j]][1] > pts[v][1])) ) { idx[j + 1] = idx[j]; --j; }
        idx[j + 1] = v;
    }
    auto cross = [&](i32 o, i32 a, i32 b) {
        return (pts[a][0] - pts[o][0]) * (pts[b][1] - pts[o][1]) - (pts[a][1] - pts[o][1]) * (pts[b][0] - pts[o][0]);
    };
    i32 hull[16]; i32 h = 0;
    for ( i32 i = 0; i < n; ++i ) {
        while ( h >= 2 && cross(hull[h - 2], hull[h - 1], idx[i]) <= 1e-9 ) { --h; }
        hull[h++] = idx[i];
    }
    i32 lower = h + 1;
    for ( i32 i = n - 2; i >= 0; --i ) {
        while ( h >= lower && cross(hull[h - 2], hull[h - 1], idx[i]) <= 1e-9 ) { --h; }
        hull[h++] = idx[i];
    }
    h -= 1;
    c->np = h > 8 ? 8 : h;
    for ( i32 i = 0; i < c->np; ++i ) { c->hx[i] = pts[hull[i]][0]; c->hy[i] = pts[hull[i]][1]; }
    c->is_rect = c->np >= 3;
}

// Parameter range [lo, hi] of segment A + tD (t in 0..1) that lies within `Rs` of the capsule.
// The capsule is convex, so the range is a single interval.
static b32
sel_cap_interval(const CutCap* c, double Rs, double Ax, double Ay, double Dx, double Dy, double* out_lo, double* out_hi)
{
    if ( c->is_rect ) {
        // Cyrus-Beck against the swept rectangle, with every edge pushed out by Rs.
        double t0 = 0.0, t1 = 1.0;
        for ( i32 i = 0; i < c->np; ++i ) {
            i32 j = (i + 1) % c->np;
            double ex = c->hx[j] - c->hx[i], ey = c->hy[j] - c->hy[i];
            double el = sqrt(ex * ex + ey * ey);
            if ( el < 1e-9 ) { continue; }
            double nx = ey / el, ny = -ex / el;  // Outward for CCW.
            double num = Rs - ((Ax - c->hx[i]) * nx + (Ay - c->hy[i]) * ny);  // Allowed: n.(P - v) <= Rs
            double den = Dx * nx + Dy * ny;
            if ( fabs(den) < 1e-12 ) {
                if ( num < 0.0 ) { return false; }
                continue;
            }
            double t = num / den;
            if ( den > 0 ) { if ( t < t1 ) { t1 = t; } }
            else { if ( t > t0 ) { t0 = t; } }
            if ( t0 > t1 ) { return false; }
        }
        *out_lo = t0;
        *out_hi = t1;
        return t0 <= t1;
    }
    double R = c->R + Rs;
    double lo = 2.0, hi = -1.0;
    auto add = [&](double a, double b) {
        if ( a <= b ) { lo = a < lo ? a : lo; hi = b > hi ? b : hi; }
    };
    auto disc = [&](double cx, double cy) {
        double fx = Ax - cx, fy = Ay - cy;
        double a = Dx * Dx + Dy * Dy;
        double b = 2.0 * (Dx * fx + Dy * fy);
        double cc = fx * fx + fy * fy - R * R;
        if ( a < 1e-12 ) {
            if ( cc <= 0 ) { add(0.0, 1.0); }
            return;
        }
        double dd = b * b - 4.0 * a * cc;
        if ( dd < 0 ) { return; }
        double sq = sqrt(dd);
        double t0 = (-b - sq) / (2.0 * a), t1 = (-b + sq) / (2.0 * a);
        add(t0 < 0.0 ? 0.0 : t0, t1 > 1.0 ? 1.0 : t1);
    };
    disc(c->ax, c->ay);
    disc(c->bx, c->by);
    if ( c->L > 0 ) {
        double fx = Ax - c->ax, fy = Ay - c->ay;
        double t0 = 0.0, t1 = 1.0;
        auto clip = [&](double al, double be, double l, double h) {
            if ( fabs(be) < 1e-12 ) {
                if ( al < l || al > h ) { t1 = -1.0; }
                return;
            }
            double ta = (l - al) / be, tb = (h - al) / be;
            if ( ta > tb ) { double tmp = ta; ta = tb; tb = tmp; }
            if ( ta > t0 ) { t0 = ta; }
            if ( tb < t1 ) { t1 = tb; }
        };
        clip(fx * c->ux + fy * c->uy, Dx * c->ux + Dy * c->uy, 0.0, c->L);
        clip(-fx * c->uy + fy * c->ux, -Dx * c->uy + Dy * c->ux, -R, R);
        if ( t0 <= t1 ) { add(t0, t1); }
    }
    *out_lo = lo;
    *out_hi = hi;
    return lo <= hi;
}

struct CutPiece
{
    v2l* p;
    f32* q;
    i32  n;
};

static void
sel_cut_impl(Milton* milton, Layer* l, Stroke* cutter, i64 limit, b32 push_undo, b32 cut_erasers)
{
    if ( !l || cutter->num_points <= 0 ) { return; }
    Selection* s = milton->selection;
    sel_deselect(milton);

    i32 m = cutter->num_points;
    i32 ncaps = max(m - 1, 1);
    CutCap* caps = (CutCap*)malloc(sizeof(CutCap) * (size_t)ncaps);
    for ( i32 k = 0; k < ncaps; ++k ) {
        i32 k2 = min(k + 1, m - 1);
        CutCap* c = &caps[k];
        c->ax = (double)cutter->points[k].x;  c->ay = (double)cutter->points[k].y;
        c->bx = (double)cutter->points[k2].x; c->by = (double)cutter->points[k2].y;
        c->is_rect = false;
        c->R = max(sel_radius_at(cutter, k, false), sel_radius_at(cutter, k2, false));
        double dx = c->bx - c->ax, dy = c->by - c->ay;
        c->L = sqrt(dx * dx + dy * dy);
        c->ux = c->L > 0 ? dx / c->L : 1.0;
        c->uy = c->L > 0 ? dy / c->L : 0.0;
        c->minx = min(c->ax, c->bx); c->maxx = max(c->ax, c->bx);
        c->miny = min(c->ay, c->by); c->maxy = max(c->ay, c->by);
        if ( cutter->brush.shape == BrushShape_RECTANGLE ) {
            c->R = max(sel_radius_at(cutter, k, true), sel_radius_at(cutter, k2, true));
            sel_cap_build_rect_hull(c, cutter, k, k2);
        }
    }
    Rect cb = bounding_box_for_stroke(cutter);
    if ( cutter->brush.shape == BrushShape_RECTANGLE ) { cb = rect_enlarge(cb, (i64)ceil((double)cutter->brush.radius * 0.5)); }

    SelOpItem* old_items = NULL; i32 n_old = 0, cap_old = 0;
    SelOpItem* new_items = NULL; i32 n_new = 0, cap_new = 0;
    double* iv = NULL; i32 iv_cap = 0;
    CutPoints cur = {};
    CutPiece* pieces = NULL; i32 n_pieces = 0, cap_pieces = 0;

    i64 count = l->strokes.count;
    i64 new_index = 0;
    for ( i64 si = 0; si < count; ++si ) {
        Stroke* st = get(&l->strokes, si);
        b32 touched = false;
        n_pieces = 0;
        cur.n = 0;

        Rect sb = st->bounding_rect;
        if ( st->brush.shape == BrushShape_RECTANGLE ) { sb = rect_enlarge(sb, (i64)ceil((double)st->brush.radius * 0.5)); }
        b32 overlaps = !(sb.right < cb.left || sb.left > cb.right || sb.bottom < cb.top || sb.top > cb.bottom);
        if ( si < limit && overlaps && (cut_erasers || !(st->flags & StrokeFlag_ERASER)) && st->num_points > 0 ) {
            auto break_piece = [&]() {
                if ( cur.n == 0 ) { return; }
                if ( n_pieces == cap_pieces ) {
                    cap_pieces = cap_pieces ? cap_pieces * 2 : 8;
                    pieces = (CutPiece*)realloc(pieces, sizeof(CutPiece) * (size_t)cap_pieces);
                }
                CutPiece pc;
                pc.n = cur.n;
                pc.p = (v2l*)malloc(sizeof(v2l) * (size_t)cur.n);
                pc.q = (f32*)malloc(sizeof(f32) * (size_t)cur.n);
                memcpy(pc.p, cur.p, sizeof(v2l) * (size_t)cur.n);
                memcpy(pc.q, cur.q, sizeof(f32) * (size_t)cur.n);
                pieces[n_pieces++] = pc;
                cur.n = 0;
            };

            i32 ns = st->num_points;
            if ( ns == 1 ) {
                double Ax = (double)st->points[0].x, Ay = (double)st->points[0].y;
                double rs = sel_radius_at(st, 0, true);
                for ( i32 k = 0; k < ncaps; ++k ) {
                    double lo, hi;
                    if ( sel_cap_interval(&caps[k], rs, Ax, Ay, 0.0, 0.0, &lo, &hi) ) { touched = true; break; }
                }
            } else {
                for ( i32 i = 0; i < ns - 1; ++i ) {
                    double Ax = (double)st->points[i].x, Ay = (double)st->points[i].y;
                    double Bx = (double)st->points[i + 1].x, By = (double)st->points[i + 1].y;
                    double Dx = Bx - Ax, Dy = By - Ay;
                    double Rs = max(sel_radius_at(st, i, true), sel_radius_at(st, i + 1, true));
                    double sminx = min(Ax, Bx) - Rs, smaxx = max(Ax, Bx) + Rs;
                    double sminy = min(Ay, By) - Rs, smaxy = max(Ay, By) + Rs;
                    f32 qa = st->pressures[i], qb = st->pressures[i + 1];

                    i32 niv = 0;
                    for ( i32 k = 0; k < ncaps; ++k ) {
                        const CutCap* c = &caps[k];
                        if ( sminx > c->maxx + c->R || smaxx < c->minx - c->R
                             || sminy > c->maxy + c->R || smaxy < c->miny - c->R ) { continue; }
                        double lo, hi;
                        if ( sel_cap_interval(c, Rs, Ax, Ay, Dx, Dy, &lo, &hi) ) {
                            if ( (niv + 1) * 2 > iv_cap ) {
                                iv_cap = iv_cap ? iv_cap * 2 : 64;
                                iv = (double*)realloc(iv, sizeof(double) * (size_t)iv_cap);
                            }
                            iv[niv * 2] = lo;
                            iv[niv * 2 + 1] = hi;
                            ++niv;
                        }
                    }

                    auto point_at = [&](double t, v2l* p, f32* q) {
                        p->x = (i64)llround(Ax + Dx * t);
                        p->y = (i64)llround(Ay + Dy * t);
                        *q = qa + (qb - qa) * (f32)t;
                    };

                    if ( niv == 0 ) {
                        // Untouched segment: carry on the current piece.
                        if ( cur.n == 0 ) { cut_points_push(&cur, st->points[i], qa); }
                        cut_points_push(&cur, st->points[i + 1], qb);
                        continue;
                    }
                    touched = true;

                    // Sort the removal intervals by start and walk the gaps between them.
                    for ( i32 a = 1; a < niv; ++a ) {
                        double lo = iv[a * 2], hi = iv[a * 2 + 1];
                        i32 b = a - 1;
                        while ( b >= 0 && iv[b * 2] > lo ) {
                            iv[(b + 1) * 2] = iv[b * 2];
                            iv[(b + 1) * 2 + 1] = iv[b * 2 + 1];
                            --b;
                        }
                        iv[(b + 1) * 2] = lo;
                        iv[(b + 1) * 2 + 1] = hi;
                    }
                    double kept_from = 0.0;
                    b32 first = true;
                    for ( i32 a = 0; a <= niv; ++a ) {
                        double rm_lo = a < niv ? iv[a * 2] : 2.0;
                        double rm_hi = a < niv ? iv[a * 2 + 1] : 2.0;
                        if ( a < niv && rm_lo <= kept_from ) {
                            kept_from = max(kept_from, rm_hi);
                            if ( first ) { break_piece(); }
                            first = false;
                            continue;
                        }
                        double k0 = kept_from;
                        double k1 = a < niv ? rm_lo : 1.0;
                        if ( k1 - k0 > 1e-9 ) {
                            v2l p; f32 q;
                            if ( k0 <= 0.0 ) {
                                if ( cur.n == 0 ) { cut_points_push(&cur, st->points[i], qa); }
                            } else {
                                break_piece();
                                point_at(k0, &p, &q);
                                cut_points_push(&cur, p, q);
                            }
                            if ( k1 >= 1.0 ) {
                                cut_points_push(&cur, st->points[i + 1], qb);
                            } else {
                                point_at(k1, &p, &q);
                                cut_points_push(&cur, p, q);
                                break_piece();
                            }
                        } else if ( first && k0 <= 0.0 ) {
                            break_piece();
                        }
                        first = false;
                        if ( a < niv ) { kept_from = max(kept_from, rm_hi); }
                    }
                    if ( kept_from >= 1.0 ) { break_piece(); }
                }
                break_piece();
            }
        }

        if ( !touched ) {
            ++new_index;
            continue;
        }

        if ( n_old == cap_old ) {
            cap_old = cap_old ? cap_old * 2 : 16;
            old_items = (SelOpItem*)realloc(old_items, sizeof(SelOpItem) * (size_t)cap_old);
        }
        old_items[n_old] = {};
        old_items[n_old].index = (i32)si;
        ++n_old;

        for ( i32 pi = 0; pi < n_pieces; ++pi ) {
            Stroke ns_ = *st;
            ns_.num_points = pieces[pi].n;
            ns_.points = arena_alloc_array(&milton->canvas->arena, pieces[pi].n, v2l);
            ns_.pressures = arena_alloc_array(&milton->canvas->arena, pieces[pi].n, f32);
            memcpy(ns_.points, pieces[pi].p, sizeof(v2l) * (size_t)pieces[pi].n);
            memcpy(ns_.pressures, pieces[pi].q, sizeof(f32) * (size_t)pieces[pi].n);
            ns_.render_handle = 0;
            ns_.id = milton->canvas->stroke_id_count++;
            ns_.bounding_rect = bounding_box_for_stroke(&ns_);
            free(pieces[pi].p);
            free(pieces[pi].q);

            if ( n_new == cap_new ) {
                cap_new = cap_new ? cap_new * 2 : 16;
                new_items = (SelOpItem*)realloc(new_items, sizeof(SelOpItem) * (size_t)cap_new);
            }
            new_items[n_new] = {};
            new_items[n_new].index = (i32)new_index;
            new_items[n_new].stroke = ns_;
            ++n_new;
            ++new_index;
        }
    }

    free(caps);
    free(iv);
    free(cur.p);
    free(cur.q);
    free(pieces);

    if ( n_old == 0 ) {
        free(old_items);
        free(new_items);
        return;
    }

    SelOp op = {};
    op.kind = SelOp_CUT;
    op.layer_id = l->id;
    op.n = n_old;
    op.items = old_items;
    op.n2 = n_new;
    op.items2 = new_items;
    list_remove(milton, l, op.items, op.n);
    list_insert(l, op.items2, op.n2);
    if ( push_undo ) {
        sel_push_op(milton, op);
    } else {
        sel_op_free(&op);
    }
    milton->render_settings.do_full_redraw = true;
}

void
selection_cut(Milton* milton, Stroke* cutter)
{
    Layer* l = milton->canvas->working_layer;
    if ( !l || !(l->flags & LayerFlags_VISIBLE) ) { return; }
    sel_cut_impl(milton, l, cutter, INT64_MAX, true, true);
}

// Eraser strokes of the layer that cannot be turned into cuts (soft, pressure-based or rectangular).
i32
layer_count_unbakeable_erasers(Layer* l, b32 harden)
{
    i32 n = 0;
    for ( i64 i = 0; i < l->strokes.count; ++i ) {
        Stroke* e = get(&l->strokes, i);
        if ( !(e->flags & StrokeFlag_ERASER) ) { continue; }
        if ( !harden && (e->brush.alpha < 0.999f
             || (e->flags & (StrokeFlag_PRESSURE_TO_OPACITY | StrokeFlag_DISTANCE_TO_OPACITY))) ) { ++n; }
    }
    return n;
}
// ---- Undo / redo

static void
sel_op_apply_transform(Milton* milton, SelOp* op, b32 use_new)
{
    Layer* l = layer::get_by_id(milton->canvas->root_layer, op->layer_id);
    if ( !l ) { return; }
    for ( i32 i = 0; i < op->n; ++i ) {
        SelOpItem* oi = &op->items[i];
        if ( oi->index >= l->strokes.count ) { continue; }
        Stroke* st = get(&l->strokes, oi->index);
        v2l* pts = use_new ? oi->new_pts : oi->old_pts;
        memcpy(st->points, pts, sizeof(v2l) * (size_t)oi->num_points);
        st->brush.radius = use_new ? oi->new_radius : oi->old_radius;
        st->brush.shape_axis_x = use_new ? oi->new_ax : oi->old_ax;
        st->brush.shape_axis_y = use_new ? oi->new_ay : oi->old_ay;
        st->bounding_rect = bounding_box_for_stroke(st);
        gpu_free_strokes(st, 1, milton->renderer);
    }
    list_refresh_bounds(l);
}

b32
selection_undo_op(Milton* milton)
{
    Selection* s = milton->selection;
    if ( s->undo.count == 0 ) { return false; }
    SelOp op = pop(&s->undo);
    Layer* l = layer::get_by_id(milton->canvas->root_layer, op.layer_id);
    if ( l ) {
        if ( op.kind == SelOp_DELETE ) {
            list_insert(l, op.items, op.n);
        } else if ( op.kind == SelOp_CUT ) {
            list_remove(milton, l, op.items2, op.n2);
            list_insert(l, op.items, op.n);
        } else {
            sel_op_apply_transform(milton, &op, false);
        }
    }
    push(&s->redo, op);
    milton->render_settings.do_full_redraw = true;
    return true;
}

b32
selection_redo_op(Milton* milton)
{
    Selection* s = milton->selection;
    if ( s->redo.count == 0 ) { return false; }
    SelOp op = pop(&s->redo);
    Layer* l = layer::get_by_id(milton->canvas->root_layer, op.layer_id);
    if ( l ) {
        if ( op.kind == SelOp_DELETE ) {
            list_remove(milton, l, op.items, op.n);
        } else if ( op.kind == SelOp_CUT ) {
            list_remove(milton, l, op.items, op.n);
            list_insert(l, op.items2, op.n2);
        } else {
            sel_op_apply_transform(milton, &op, true);
        }
    }
    push(&s->undo, op);
    milton->render_settings.do_full_redraw = true;
    return true;
}

static void
sel_clear_auto(DArray<AutoOp>* stack)
{
    while ( stack->count > 0 ) {
        AutoOp a = pop(stack);
        sel_op_free(&a.op);
    }
}

void
selection_clear_redo(Milton* milton)
{
    if ( milton->selection ) {
        sel_clear_stack(&milton->selection->redo);
        sel_clear_auto(&milton->selection->auto_redo);
    }
}

void
selection_reset(Milton* milton)
{
    Selection* s = milton->selection;
    if ( !s ) { return; }
    sel_free_items(s);
    sel_clear_stack(&s->undo);
    sel_clear_stack(&s->redo);
    sel_clear_auto(&s->auto_undo);
    sel_clear_auto(&s->auto_redo);
    s->state = SelState_IDLE;
    s->armed = false;
    s->pending = SelCmd_NONE;
    reset(&s->lasso);
}

void
selection_finish(Milton* milton)
{
    Selection* s = milton->selection;
    if ( s->active ) {
        sel_deselect(milton);
    }
    s->state = SelState_IDLE;
    s->armed = false;
}

// ---- Lasso picking

static b32
point_in_poly(DArray<v2l>* poly, double x, double y)
{
    b32 inside = false;
    i64 n = poly->count;
    for ( i64 i = 0, j = n - 1; i < n; j = i++ ) {
        double xi = (double)poly->data[i].x, yi = (double)poly->data[i].y;
        double xj = (double)poly->data[j].x, yj = (double)poly->data[j].y;
        if ( ((yi > y) != (yj > y)) && (x < (xj - xi) * (y - yi) / (yj - yi) + xi) ) {
            inside = !inside;
        }
    }
    return inside;
}

static b32
segments_cross(double ax, double ay, double bx, double by,
               double cx, double cy, double dx, double dy)
{
    double d1 = (bx-ax)*(cy-ay) - (by-ay)*(cx-ax);
    double d2 = (bx-ax)*(dy-ay) - (by-ay)*(dx-ax);
    double d3 = (dx-cx)*(ay-cy) - (dy-cy)*(ax-cx);
    double d4 = (dx-cx)*(by-cy) - (dy-cy)*(bx-cx);
    return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0));
}

// Splits every stroke of the layer at the lasso boundary and returns the new indices of the pieces inside.
// The split is recorded as one undoable CUT op. Caller frees the result.
static i32*
sel_lasso_split(Milton* milton, Layer* l, i32* out_n, double lminx, double lminy, double lmaxx, double lmaxy)
{
    Selection* s = milton->selection;
    DArray<v2l>* poly = &s->lasso;
    i64 count = l->strokes.count;
    i32* picked = (i32*)malloc(sizeof(i32) * (size_t)(count > 0 ? count : 1));
    i32 npicked = 0, cap_picked = (i32)(count > 0 ? count : 1);

    SelOpItem* old_items = NULL; i32 n_old = 0, cap_old = 0;
    SelOpItem* new_items = NULL; i32 n_new = 0, cap_new = 0;
    CutPoints cur = {};
    CutPiece* pieces = NULL; b32* piece_in = NULL; i32 n_pieces = 0, cap_pieces = 0;
    double* ts = NULL; i32 ts_cap = 0;
    i64 new_index = 0;

    for ( i64 si = 0; si < count; ++si ) {
        Stroke* st = get(&l->strokes, si);
        n_pieces = 0;
        cur.n = 0;
        b32 in_bounds = st->num_points > 0 &&
            !((double)st->bounding_rect.right < lminx || (double)st->bounding_rect.left > lmaxx ||
              (double)st->bounding_rect.bottom < lminy || (double)st->bounding_rect.top > lmaxy);
        if ( !in_bounds ) { ++new_index; continue; }

        b32 cur_in = false;
        b32 touched = false;
        b32 any_in = false;
        auto break_piece = [&]() {
            if ( cur.n == 0 ) { return; }
            if ( cur.n >= 2 || st->num_points == 1 ) {
                if ( n_pieces == cap_pieces ) {
                    cap_pieces = cap_pieces ? cap_pieces * 2 : 8;
                    pieces = (CutPiece*)realloc(pieces, sizeof(CutPiece) * (size_t)cap_pieces);
                    piece_in = (b32*)realloc(piece_in, sizeof(b32) * (size_t)cap_pieces);
                }
                CutPiece pc;
                pc.n = cur.n;
                pc.p = (v2l*)malloc(sizeof(v2l) * (size_t)cur.n);
                pc.q = (f32*)malloc(sizeof(f32) * (size_t)cur.n);
                memcpy(pc.p, cur.p, sizeof(v2l) * (size_t)cur.n);
                memcpy(pc.q, cur.q, sizeof(f32) * (size_t)cur.n);
                piece_in[n_pieces] = cur_in;
                pieces[n_pieces++] = pc;
            }
            cur.n = 0;
        };

        if ( st->num_points == 1 ) {
            b32 in = point_in_poly(poly, (double)st->points[0].x, (double)st->points[0].y);
            if ( in ) {
                if ( npicked == cap_picked ) { cap_picked *= 2; picked = (i32*)realloc(picked, sizeof(i32) * (size_t)cap_picked); }
                picked[npicked++] = (i32)new_index;
            }
            ++new_index;
            continue;
        }

        for ( i32 i = 0; i + 1 < st->num_points; ++i ) {
            double Ax = (double)st->points[i].x, Ay = (double)st->points[i].y;
            double Bx = (double)st->points[i + 1].x, By = (double)st->points[i + 1].y;
            double Dx = Bx - Ax, Dy = By - Ay;
            f32 qa = st->pressures[i], qb = st->pressures[i + 1];

            i32 nts = 0;
            b32 seg_near = !(max(Ax, Bx) < lminx || min(Ax, Bx) > lmaxx || max(Ay, By) < lminy || min(Ay, By) > lmaxy);
            if ( (nts + 2) > ts_cap ) { ts_cap = ts_cap ? ts_cap * 2 : 64; ts = (double*)realloc(ts, sizeof(double) * (size_t)ts_cap); }
            ts[nts++] = 0.0;
            if ( seg_near ) {
                for ( i64 e = 0; e < poly->count; ++e ) {
                    v2l c = poly->data[e];
                    v2l dd = poly->data[(e + 1) % poly->count];
                    double Cx = (double)c.x, Cy = (double)c.y;
                    double Sx = (double)dd.x - Cx, Sy = (double)dd.y - Cy;
                    double denom = Dx * Sy - Dy * Sx;
                    if ( fabs(denom) < 1e-12 ) { continue; }
                    double t = ((Cx - Ax) * Sy - (Cy - Ay) * Sx) / denom;
                    double u = ((Cx - Ax) * Dy - (Cy - Ay) * Dx) / denom;
                    if ( t > 1e-9 && t < 1.0 - 1e-9 && u >= 0.0 && u <= 1.0 ) {
                        if ( (nts + 2) > ts_cap ) { ts_cap *= 2; ts = (double*)realloc(ts, sizeof(double) * (size_t)ts_cap); }
                        ts[nts++] = t;
                    }
                }
            }
            ts[nts++] = 1.0;
            for ( i32 a = 1; a < nts; ++a ) {  // Insertion sort.
                double v = ts[a]; i32 b = a - 1;
                while ( b >= 0 && ts[b] > v ) { ts[b + 1] = ts[b]; --b; }
                ts[b + 1] = v;
            }

            for ( i32 a = 0; a + 1 < nts; ++a ) {
                double t0 = ts[a], t1 = ts[a + 1];
                if ( t1 - t0 < 1e-9 ) { continue; }
                b32 in = false;
                if ( seg_near ) {
                    double tm = (t0 + t1) * 0.5;
                    in = point_in_poly(poly, Ax + Dx * tm, Ay + Dy * tm);
                }
                v2l p0 = (t0 <= 0.0) ? st->points[i] : v2l{ (i64)llround(Ax + Dx * t0), (i64)llround(Ay + Dy * t0) };
                v2l p1 = (t1 >= 1.0) ? st->points[i + 1] : v2l{ (i64)llround(Ax + Dx * t1), (i64)llround(Ay + Dy * t1) };
                f32 q0 = qa + (qb - qa) * (f32)t0, q1 = qa + (qb - qa) * (f32)t1;
                if ( cur.n > 0 && in != cur_in ) { break_piece(); touched = true; }
                if ( cur.n == 0 ) { cur_in = in; cut_points_push(&cur, p0, q0); }
                cut_points_push(&cur, p1, q1);
                if ( in ) { any_in = true; }
            }
        }
        break_piece();

        if ( n_pieces <= 1 && !touched ) {
            if ( any_in ) {
                if ( npicked == cap_picked ) { cap_picked *= 2; picked = (i32*)realloc(picked, sizeof(i32) * (size_t)cap_picked); }
                picked[npicked++] = (i32)new_index;
            }
            for ( i32 pi = 0; pi < n_pieces; ++pi ) { free(pieces[pi].p); free(pieces[pi].q); }
            ++new_index;
            continue;
        }

        if ( n_old == cap_old ) {
            cap_old = cap_old ? cap_old * 2 : 16;
            old_items = (SelOpItem*)realloc(old_items, sizeof(SelOpItem) * (size_t)cap_old);
        }
        old_items[n_old] = {};
        old_items[n_old].index = (i32)si;
        ++n_old;

        for ( i32 pi = 0; pi < n_pieces; ++pi ) {
            Stroke ns_ = *st;
            ns_.num_points = pieces[pi].n;
            ns_.points = arena_alloc_array(&milton->canvas->arena, pieces[pi].n, v2l);
            ns_.pressures = arena_alloc_array(&milton->canvas->arena, pieces[pi].n, f32);
            memcpy(ns_.points, pieces[pi].p, sizeof(v2l) * (size_t)pieces[pi].n);
            memcpy(ns_.pressures, pieces[pi].q, sizeof(f32) * (size_t)pieces[pi].n);
            ns_.render_handle = 0;
            ns_.id = milton->canvas->stroke_id_count++;
            ns_.bounding_rect = bounding_box_for_stroke(&ns_);
            free(pieces[pi].p);
            free(pieces[pi].q);

            if ( n_new == cap_new ) {
                cap_new = cap_new ? cap_new * 2 : 16;
                new_items = (SelOpItem*)realloc(new_items, sizeof(SelOpItem) * (size_t)cap_new);
            }
            new_items[n_new] = {};
            new_items[n_new].index = (i32)new_index;
            new_items[n_new].stroke = ns_;
            ++n_new;
            if ( piece_in[pi] ) {
                if ( npicked == cap_picked ) { cap_picked *= 2; picked = (i32*)realloc(picked, sizeof(i32) * (size_t)cap_picked); }
                picked[npicked++] = (i32)new_index;
            }
            ++new_index;
        }
    }

    free(cur.p);
    free(cur.q);
    free(pieces);
    free(piece_in);
    free(ts);

    if ( n_old == 0 ) {
        free(old_items);
        free(new_items);
    } else {
        SelOp op = {};
        op.kind = SelOp_CUT;
        op.layer_id = l->id;
        op.n = n_old;
        op.items = old_items;
        op.n2 = n_new;
        op.items2 = new_items;
        list_remove(milton, l, op.items, op.n);
        list_insert(l, op.items2, op.n2);
        sel_push_op(milton, op);
        milton->render_settings.do_full_redraw = true;
    }
    *out_n = npicked;
    return picked;
}

static void
sel_pick_from_lasso(Milton* milton)
{
    Selection* s = milton->selection;
    sel_deselect(milton);

    Layer* l = milton->canvas->working_layer;
    if ( !l || !(l->flags & LayerFlags_VISIBLE) || s->lasso.count < 3 ) { return; }

    double lminx = 1e300, lminy = 1e300, lmaxx = -1e300, lmaxy = -1e300;
    for ( i64 i = 0; i < s->lasso.count; ++i ) {
        lminx = min(lminx, (double)s->lasso.data[i].x);
        lminy = min(lminy, (double)s->lasso.data[i].y);
        lmaxx = max(lmaxx, (double)s->lasso.data[i].x);
        lmaxy = max(lmaxy, (double)s->lasso.data[i].y);
    }

    i64 cnt = l->strokes.count;
    i32* picked = NULL;
    i32 npicked = 0;
    if ( !milton->settings->lasso_whole ) {
        picked = sel_lasso_split(milton, l, &npicked, lminx, lminy, lmaxx, lmaxy);
        cnt = 0;
    } else {
        picked = (i32*)calloc((size_t)(cnt > 0 ? cnt : 1), sizeof(i32));
    }
    for ( i64 si = 0; si < cnt; ++si ) {
        Stroke* st = get(&l->strokes, si);
        if ( st->num_points <= 0 ) { continue; }
        if ( (double)st->bounding_rect.right < lminx || (double)st->bounding_rect.left > lmaxx ||
             (double)st->bounding_rect.bottom < lminy || (double)st->bounding_rect.top > lmaxy ) {
            continue;
        }
        b32 hit = false;
        for ( i32 p = 0; p < st->num_points && !hit; ++p ) {
            double x = (double)st->points[p].x, y = (double)st->points[p].y;
            if ( x < lminx || x > lmaxx || y < lminy || y > lmaxy ) { continue; }
            hit = point_in_poly(&s->lasso, x, y);
        }
        for ( i32 p = 0; p + 1 < st->num_points && !hit; ++p ) {
            double ax = (double)st->points[p].x, ay = (double)st->points[p].y;
            double bx = (double)st->points[p+1].x, by = (double)st->points[p+1].y;
            if ( max(ax, bx) < lminx || min(ax, bx) > lmaxx || max(ay, by) < lminy || min(ay, by) > lmaxy ) {
                continue;
            }
            for ( i64 e = 0; e < s->lasso.count && !hit; ++e ) {
                v2l c = s->lasso.data[e];
                v2l d = s->lasso.data[(e + 1) % s->lasso.count];
                hit = segments_cross(ax, ay, bx, by, (double)c.x, (double)c.y, (double)d.x, (double)d.y);
            }
        }
        if ( hit ) { picked[npicked++] = (i32)si; }
    }

    if ( npicked > 0 ) {
        s->active = true;
        s->layer_id = l->id;
        s->n = npicked;
        s->items = (SelItem*)calloc((size_t)npicked, sizeof(SelItem));
        for ( i32 i = 0; i < npicked; ++i ) {
            s->items[i].index = picked[i];
        }
        sel_snapshot(milton);
    }
    free(picked);
}

// ---- Free transform handles

static void
box_to_canvas(Selection* s, double ux, double uy, double* ox, double* oy)
{
    double cs = cos(s->theta), sn = sin(s->theta);
    double lx = s->h0x * s->sx * ux;
    double ly = s->h0y * s->sy * uy;
    *ox = s->cx + lx * cs - ly * sn;
    *oy = s->cy + lx * sn + ly * cs;
}

static v2l
canvas_d_to_raster(CanvasView* view, double x, double y)
{
    return canvas_to_raster(view, v2l{ (i64)floor(x + 0.5), (i64)floor(y + 0.5) });
}

static const i32 k_handle_u[8][2] = {
    {-1,-1}, {0,-1}, {1,-1}, {1,0}, {1,1}, {0,1}, {-1,1}, {-1,0},
};

static void
sel_begin_drag(Milton* milton, v2l cur_raster)
{
    Selection* s = milton->selection;
    CanvasView* view = milton->view;
    v2l m = raster_to_canvas(view, cur_raster);
    s->drag_mx = (double)m.x; s->drag_my = (double)m.y;
    s->s_cx = s->cx; s->s_cy = s->cy; s->s_theta = s->theta; s->s_sx = s->sx; s->s_sy = s->sy;

    double thresh = 12.0 * milton->gui->scale;
    i32 found = -1;
    double best = thresh * thresh;
    for ( i32 h = 0; h < 8; ++h ) {
        double wx, wy;
        box_to_canvas(s, k_handle_u[h][0], k_handle_u[h][1], &wx, &wy);
        v2l r = canvas_d_to_raster(view, wx, wy);
        double dx = (double)(r.x - cur_raster.x), dy = (double)(r.y - cur_raster.y);
        double d2 = dx*dx + dy*dy;
        if ( d2 < best ) { best = d2; found = h; }
    }

    if ( found >= 0 ) {
        s->drag_kind = SelDrag_SCALE;
        s->hx = k_handle_u[found][0];
        s->hy = k_handle_u[found][1];
        box_to_canvas(s, -s->hx, -s->hy, &s->anchor_x, &s->anchor_y);
        return;
    }

    // Inside the box: translate. Outside: rotate.
    double cs = cos(s->theta), sn = sin(s->theta);
    double rx = s->drag_mx - s->cx, ry = s->drag_my - s->cy;
    double lx = rx * cs + ry * sn;
    double ly = -rx * sn + ry * cs;
    if ( fabs(lx) <= s->h0x * s->sx && fabs(ly) <= s->h0y * s->sy ) {
        s->drag_kind = SelDrag_TRANSLATE;
    } else {
        s->drag_kind = SelDrag_ROTATE;
        s->rot_start = atan2(ry, rx);
    }
}

static void
sel_update_drag(Milton* milton, v2l cur_raster)
{
    Selection* s = milton->selection;
    v2l m = raster_to_canvas(milton->view, cur_raster);
    double mx = (double)m.x, my = (double)m.y;

    if ( s->drag_kind == SelDrag_TRANSLATE ) {
        s->cx = s->s_cx + (mx - s->drag_mx);
        s->cy = s->s_cy + (my - s->drag_my);
    }
    else if ( s->drag_kind == SelDrag_ROTATE ) {
        double a = atan2(my - s->cy, mx - s->cx);
        s->theta = s->s_theta + (a - s->rot_start);
    }
    else {
        double cs = cos(s->theta), sn = sin(s->theta);
        double rx = mx - s->anchor_x, ry = my - s->anchor_y;
        double dx = rx * cs + ry * sn;
        double dy = -rx * sn + ry * cs;
        double nsx = s->s_sx, nsy = s->s_sy;
        if ( s->hx != 0 ) { nsx = max(0.02, s->hx * dx / (2.0 * s->h0x)); }
        if ( s->hy != 0 ) { nsy = max(0.02, s->hy * dy / (2.0 * s->h0y)); }
        if ( s->hx != 0 && s->hy != 0 && (SDL_GetModState() & KMOD_SHIFT) ) {
            double f = max(nsx / s->s_sx, nsy / s->s_sy);
            nsx = s->s_sx * f;
            nsy = s->s_sy * f;
        }
        s->sx = nsx;
        s->sy = nsy;
        // Keep the anchor fixed in the world.
        double alx = -s->hx * s->h0x * nsx;
        double aly = -s->hy * s->h0y * nsy;
        s->cx = s->anchor_x - (alx * cs - aly * sn);
        s->cy = s->anchor_y - (alx * sn + aly * cs);
    }
    sel_apply(milton);
}

// ---- Per-frame tick

b32
selection_tick(Milton* milton, MiltonInput const* input)
{
    Selection* s = milton->selection;
    PlatformState* platform = milton->platform;

    // Commands
    SelectionCommand cmd = s->pending;
    s->pending = SelCmd_NONE;
    switch ( cmd ) {
        case SelCmd_ARM_LASSO: {
            if ( s->xform_mode ) { sel_commit_xform(milton); s->xform_mode = false; }
            s->armed = !s->armed;
            s->state = SelState_IDLE;
        } break;
        case SelCmd_DELETE: {
            if ( s->active ) { sel_do_delete(milton); }
        } break;
        case SelCmd_DESELECT: {
            sel_deselect(milton);
            s->armed = false;
            s->state = SelState_IDLE;
        } break;
        case SelCmd_TRANSFORM: {
            if ( s->active ) {
                if ( s->xform_mode ) { sel_commit_xform(milton); }
                s->xform_mode = !s->xform_mode;
            }
        } break;
        case SelCmd_COMMIT: {
            if ( s->xform_mode ) { sel_commit_xform(milton); s->xform_mode = false; }
        } break;
        case SelCmd_CLEANUP: {
            sel_deselect(milton);
            sel_cleanup_layer(milton, milton->canvas->working_layer);
        } break;
        case SelCmd_CANCEL: {
            if ( s->xform_mode ) { sel_cancel_xform(milton); s->xform_mode = false; }
            s->armed = false;
            s->state = SelState_IDLE;
        } break;
        default: break;
    }

    if ( s->active && !sel_layer(milton) ) {
        sel_free_items(s);
        s->state = SelState_IDLE;
    }

    SelState state_before = s->state;

    b32 down = platform->is_pointer_down;
    b32 pressed = down && !s->prev_down;
    b32 released = !down || (input->flags & MiltonInputFlags_END_STROKE);
    s->prev_down = down;

    v2l cur = VEC2L(platform->pointer);
    if ( input->input_count > 0 ) {
        cur = input->points[input->input_count - 1];
    }

    b32 blocked =    (input->flags & (MiltonInputFlags_PANNING | MiltonInputFlags_IMGUI_GRABBED_INPUT))
                  || platform->is_space_down
                  || platform->is_panning
                  || milton->gui->owns_user_input
                  || gui_point_hovers(milton->gui, platform->pointer)
                  || milton->current_mode == MiltonMode::DRAG_ZOOM
                  || milton->current_mode == MiltonMode::DRAG_BRUSH_SIZE
                  || milton->current_mode == MiltonMode::TRANSFORM
                  || milton->current_mode == MiltonMode::EYEDROPPER
                  || milton->current_mode == MiltonMode::EXPORTING
                  || milton->current_mode == MiltonMode::PEEK_OUT;

    switch ( s->state ) {
        case SelState_IDLE: {
            if ( pressed && !blocked ) {
                if ( s->armed ) {
                    sel_deselect(milton);
                    reset(&s->lasso);
                    v2l c = raster_to_canvas(milton->view, cur);
                    push(&s->lasso, c);
                    s->last_lasso_raster = cur;
                    s->state = SelState_LASSO;
                }
                else if ( s->active && s->xform_mode ) {
                    sel_begin_drag(milton, cur);
                    s->state = SelState_XFORM_DRAG;
                }
                else if ( s->active && sel_point_in_box(milton, cur) ) {
                    sel_begin_drag(milton, cur);
                    s->drag_kind = SelDrag_TRANSLATE;
                    s->state = SelState_MOVE;
                }
            }
        } break;
        case SelState_LASSO: {
            for ( i32 i = 0; i < input->input_count; ++i ) {
                v2l p = input->points[i];
                i64 dx = p.x - s->last_lasso_raster.x, dy = p.y - s->last_lasso_raster.y;
                if ( dx*dx + dy*dy >= 9 ) {
                    push(&s->lasso, raster_to_canvas(milton->view, p));
                    s->last_lasso_raster = p;
                }
            }
            if ( released ) {
                sel_pick_from_lasso(milton);
                reset(&s->lasso);
                s->armed = false;
                s->state = SelState_IDLE;
            }
        } break;
        case SelState_MOVE:
        case SelState_XFORM_DRAG: {
            if ( input->input_count > 0 || released ) {
                sel_update_drag(milton, cur);
            }
            if ( released ) {
                if ( s->state == SelState_MOVE ) { sel_commit_xform(milton); }
                s->state = SelState_IDLE;
            }
        } break;
    }

    return state_before != SelState_IDLE || s->state != SelState_IDLE;
}

// ---- Overlay

void
selection_draw_overlay(Milton* milton)
{
    Selection* s = milton->selection;
    CanvasView* view = milton->view;
    f32 ui = milton->gui->scale;
    ImDrawList* dl = ImGui::GetOverlayDrawList();

    const ImU32 dark = IM_COL32(0, 0, 0, 220);
    const ImU32 light = IM_COL32(255, 255, 255, 230);
    const ImU32 accent = IM_COL32(0, 190, 255, 255);

    auto to_im = [view](double x, double y) {
        v2l r = canvas_d_to_raster(view, x, y);
        return ImVec2((f32)r.x, (f32)r.y);
    };

    {
        Stroke* ws = &milton->working_stroke;
        if ( (ws->flags & StrokeFlag_CUT) && ws->num_points > 0 ) {
            f32 w = (f32)(2.0 * ws->brush.radius / view->scale);
            i32 step = ws->num_points > 1500 ? ws->num_points / 1500 : 1;
            const ImU32 cc = IM_COL32(255, 80, 80, 110);
            ImVec2 prev = to_im((double)ws->points[0].x, (double)ws->points[0].y);
            if ( ws->num_points == 1 ) { dl->AddCircleFilled(prev, w * 0.5f, cc, 32); }
            for ( i32 i = 1; i < ws->num_points; i += step ) {
                ImVec2 cur = to_im((double)ws->points[i].x, (double)ws->points[i].y);
                dl->AddLine(prev, cur, cc, w);
                dl->AddCircleFilled(cur, w * 0.5f, cc, 16);
                prev = cur;
            }
        }
        if ( s->toast_until && (i32)(s->toast_until - SDL_GetTicks()) > 0 ) {
            char msg[64];
            snprintf(msg, sizeof(msg), s->toast_n ? "Removed %d fully erased strokes" : "No fully erased strokes found", s->toast_n);
            ImVec2 ts = ImGui::CalcTextSize(msg);
            dl->AddText(ImVec2((ImGui::GetIO().DisplaySize.x - ts.x) * 0.5f, ui * 8 + ((milton->flags & MiltonStateFlags_AUTOSAVE_BLOCKED) ? ui * 48 : 0.0f)), light, msg);
            if ( milton->platform ) { milton->platform->force_next_frame = true; }
        }
    }

    if ( milton->flags & MiltonStateFlags_AUTOSAVE_BLOCKED ) {
        const char* msg = "Autosave paused - press Ctrl+S to save";
        ImVec2 ts = ImGui::CalcTextSize(msg);
        ImVec2 ds = ImGui::GetIO().DisplaySize;
        f32 top = (milton->gui->menu_visible ? ImGui::GetFrameHeight() : 0.0f) + ui * 6;
        ImVec2 p0 = ImVec2((ds.x - ts.x) * 0.5f - ui * 12, top);
        ImVec2 p1 = ImVec2(p0.x + ts.x + ui * 24, top + ts.y + ui * 10);
        dl->AddRectFilled(p0, p1, IM_COL32(220, 20, 20, 245), ui * 4);
        dl->AddText(ImVec2(p0.x + ui * 12, p0.y + ui * 5), IM_COL32(255, 255, 255, 255), msg);
    }

    if ( s->armed && s->state == SelState_IDLE ) {
        dl->AddText(ImVec2(ui * 12, ui * 8), light, "Lasso select: drag around strokes (Esc to cancel)");
    }

    if ( s->state == SelState_LASSO && s->lasso.count > 1 ) {
        static ImVec2* pts = NULL;
        static i64 cap = 0;
        if ( cap < s->lasso.count ) {
            free(pts);
            cap = s->lasso.count * 2;
            pts = (ImVec2*)malloc(sizeof(ImVec2) * (size_t)cap);
        }
        // Long lassos are thinned for display only; the selection test still uses every point.
        i64 stride = max((i64)1, s->lasso.count / 2000);
        i32 n = 0;
        for ( i64 i = 0; i < s->lasso.count; i += stride ) {
            pts[n++] = to_im((double)s->lasso.data[i].x, (double)s->lasso.data[i].y);
        }
        dl->AddPolyline(pts, n, dark, true, ui * 3);
        dl->AddPolyline(pts, n, accent, true, ui * 1.5f);
    }

    if ( !s->active ) { return; }
    Layer* l = sel_layer(milton);
    if ( !l ) { return; }

    // Highlight the selected strokes.
    {
        static ImVec2* pts = NULL;
        static i32 cap = 0;
        // Keep the total drawn vertex count bounded however large the selection is.
        i64 total_points = 0;
        for ( i32 i = 0; i < s->n; ++i ) {
            total_points += get(&l->strokes, s->items[i].index)->num_points;
        }
        const i64 budget = 60000;
        i32 min_stride = (i32)max((i64)1, total_points / budget);
        for ( i32 i = 0; i < s->n; ++i ) {
            Stroke* st = get(&l->strokes, s->items[i].index);
            i32 stride = max(min_stride, max(1, st->num_points / 1500));
            i32 cnt = 0;
            if ( cap < st->num_points + 1 ) {
                free(pts);
                cap = st->num_points + 1;
                pts = (ImVec2*)malloc(sizeof(ImVec2) * (size_t)cap);
            }
            for ( i32 p = 0; p < st->num_points; p += stride ) {
                pts[cnt++] = to_im((double)st->points[p].x, (double)st->points[p].y);
            }
            if ( cnt == 1 ) {
                dl->AddCircle(pts[0], ui * 4, accent, 12, ui * 1.5f);
            } else if ( cnt > 1 ) {
                dl->AddPolyline(pts, cnt, accent, false, ui * 1.5f);
            }
        }
    }

    // Bounding box
    ImVec2 corners[4];
    const double cu[4][2] = { {-1,-1}, {1,-1}, {1,1}, {-1,1} };
    for ( i32 i = 0; i < 4; ++i ) {
        double wx, wy;
        box_to_canvas(s, cu[i][0], cu[i][1], &wx, &wy);
        corners[i] = to_im(wx, wy);
    }
    dl->AddPolyline(corners, 4, dark, true, ui * 3);
    dl->AddPolyline(corners, 4, light, true, ui * 1.5f);

    if ( s->xform_mode ) {
        f32 hs = ui * 5;
        for ( i32 h = 0; h < 8; ++h ) {
            double wx, wy;
            box_to_canvas(s, k_handle_u[h][0], k_handle_u[h][1], &wx, &wy);
            ImVec2 c = to_im(wx, wy);
            dl->AddRectFilled(ImVec2(c.x - hs, c.y - hs), ImVec2(c.x + hs, c.y + hs), light);
            dl->AddRect(ImVec2(c.x - hs, c.y - hs), ImVec2(c.x + hs, c.y + hs), dark, 0.0f, ImDrawCornerFlags_All, ui * 1.5f);
        }
        dl->AddText(ImVec2(ui * 12, ui * 8), light,
                    "Free transform: drag handles to scale (Shift = uniform), drag outside to rotate, Enter = apply, Esc = cancel");
    }
}
