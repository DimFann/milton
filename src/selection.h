// Lasso selection of whole strokes on the working layer, with delete, translate and free transform.
// All edits are recorded as history elements of type HistoryElement_SELECTION_OP.

#pragma once

struct Milton;
struct MiltonInput;

enum SelectionCommand
{
    SelCmd_NONE,
    SelCmd_ARM_LASSO,       // Toggle lasso mode
    SelCmd_DELETE,
    SelCmd_DESELECT,
    SelCmd_TRANSFORM,       // Toggle the free-transform box
    SelCmd_COMMIT,          // Enter: apply the free transform
    SelCmd_CANCEL,          // Esc: revert the free transform
    SelCmd_CLEANUP,         // Remove strokes on the current layer that opaque erasers fully cover
};

struct Selection;

Selection*  selection_create(Arena* arena);

// Queue a command. It runs on the next update.
void        selection_command(Milton* milton, SelectionCommand cmd);
b32         selection_transform_active(Milton* milton);  // Free transform or lasso armed: Esc should cancel it

// Returns true if the selection consumed this frame's pointer input.
b32         selection_tick(Milton* milton, MiltonInput const* input);
b32         selection_lasso_armed(Milton* milton);
void        selection_disarm_lasso(Milton* milton);  // Called when another tool is activated
// Cursor the selection wants at the pointer: 0 = none (normal tool cursor), 1 = lasso, 2 = regular arrow.
i32         selection_cursor(Milton* milton);
b32         selection_take_dirty(Milton* milton);  // True once after an edit that should trigger a save

// Undo/redo for HistoryElement_SELECTION_OP. Return false if there was nothing to do.
b32         selection_undo_op(Milton* milton);
b32         selection_redo_op(Milton* milton);
void        selection_clear_redo(Milton* milton);

// Drop all selection state (and op stacks). Used when a canvas is reset or loaded.
void        selection_reset(Milton* milton);
// Apply any pending free transform and drop the selection (before undo/redo).
void        selection_finish(Milton* milton);

void        selection_draw_overlay(Milton* milton);
void        optimize_request(Milton* milton);
void        layer_merge_down(Milton* milton, b32 harden_erasers);
i32         layer_count_unbakeable_erasers(Layer* l, b32 harden);
b32         optimize_active();
void        optimize_draw(Milton* milton);

// Erased-stroke cleanup. Called after a stroke has been added to layer.
void        selection_auto_cleanup(Milton* milton, Layer* layer);
// Keep the automatic cleanup in step with stroke undo/redo. hist_pos is the history count that
// includes the eraser stroke's entry.
void        selection_auto_undo(Milton* milton, i64 hist_pos);
void        selection_auto_redo(Milton* milton, i64 hist_pos);

// Cutting eraser: split every stroke on the working layer along the path of cutter.
void        selection_cut(Milton* milton, Stroke* cutter);
