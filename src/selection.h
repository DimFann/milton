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
};

struct Selection;

Selection*  selection_create(Arena* arena);

// Queue a command. It runs on the next update.
void        selection_command(Milton* milton, SelectionCommand cmd);
b32         selection_transform_active(Milton* milton);  // Free transform or lasso armed: Esc should cancel it

// Returns true if the selection consumed this frame's pointer input.
b32         selection_tick(Milton* milton, MiltonInput const* input);
b32         selection_lasso_armed(Milton* milton);
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
