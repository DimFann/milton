// Lasso selection of whole strokes on the working layer. See selection.h.

#include "selection.h"
#include "milton.h"
#include "gui.h"

enum SelOpKind
{
    SelOp_DELETE,
    SelOp_TRANSFORM,
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
        } else {
            sel_op_apply_transform(milton, &op, true);
        }
    }
    push(&s->undo, op);
    milton->render_settings.do_full_redraw = true;
    return true;
}

void
selection_clear_redo(Milton* milton)
{
    if ( milton->selection ) {
        sel_clear_stack(&milton->selection->redo);
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
    i32* picked = (i32*)calloc((size_t)(cnt > 0 ? cnt : 1), sizeof(i32));
    i32 npicked = 0;
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
        for ( i64 i = 0; i < s->lasso.count; ++i ) {
            pts[i] = to_im((double)s->lasso.data[i].x, (double)s->lasso.data[i].y);
        }
        dl->AddPolyline(pts, (int)s->lasso.count, dark, true, ui * 3);
        dl->AddPolyline(pts, (int)s->lasso.count, accent, true, ui * 1.5f);
    }

    if ( !s->active ) { return; }
    Layer* l = sel_layer(milton);
    if ( !l ) { return; }

    // Highlight the selected strokes.
    if ( s->n <= 4000 ) {
        static ImVec2* pts = NULL;
        static i32 cap = 0;
        for ( i32 i = 0; i < s->n; ++i ) {
            Stroke* st = get(&l->strokes, s->items[i].index);
            i32 stride = max(1, st->num_points / 1500);
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
