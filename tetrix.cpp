//===----------------------------------------------------------------------===//
//                                                                            //
//     __        ______    _                                                  //
//    / /_ ___  /_  __/___(_)_  __                                            //
//   / __// _ \  / / / __/ /\ \/ /                                            //
//  / /_ /  __/ / / / / / /  > · <                                            //
//  \__/ \___/ /_/ /_/ /_/  /_/\_\                                            //
//                                                                            //
//===----------------------------------------------------------------------===//

// tetrix.cpp -- dedicated binary for examples/tetrix.trx with native AI kernels.
//
// Same Trix runtime as ./trix, but its user_ops[] table registers two C++
// kernels that replace the hottest inner loops of tetrix.trx's AI search:
//
//   field-copy-fast       src dst :- --
//   score-board-fast      fld lines weights :- score
//
// The Trix-level reference implementations stay in examples/tetrix.trx and
// are selected at runtime via --ai-kernel=trix (default --ai-kernel=native
// uses these C ops).  Keeping the kernels out of the default trix binary
// preserves the language surface for non-tetrix users.
//
// Field representation (tetrix invariant; both ops hard-validate):
//   field   : Array of length 20  (rows, top->bottom)
//   row     : Array of length 10  (cols, left->right) of Integer cells
//   cell    : Integer; 0 = empty, nonzero = piece-color id
//
// Save/restore: cell mutations are NON-journaling.  This matches the put-
// persist family contract for game-actor's live field (cells must outlive
// the per-iteration save/restore wrap so accumulated state persists), and
// is harmless for AI search's scratch field (the scratch is allocated
// inside best-placement's save scope and gets reclaimed wholesale by
// restore -- journaled cell entries against above-barrier targets would
// be reclaimed in the same rollback, so the journal calls were always
// wasted work, not load-bearing).  See plan_b1_persist_family.md and
// docs/trix-reference.md s7.7.1.  Cells are Integer Objects with no
// ExtValue, so plain assignment is equivalent to make_clone +
// maybe_free_extvalue.

#include <cstring>
#include <vector>

#include <unistd.h>

#include "trix.h"

// Same alias set the library itself re-exports -- see the trix_details
// block at the tail of trix.h.
using length_t = Trix::length_t;
using save_level_t = Trix::save_level_t;
using integer_t = Trix::integer_t;
using long_t = Trix::long_t;
using Object = Trix::Object;
using Save = Trix::Save;
using Operator = Trix::Operator;
using Error = Trix::Error;
using StartupMode = Trix::StartupMode;
constexpr auto DefaultStreamCount = Trix::DefaultStreamCount;
constexpr auto VerifyArray = Trix::VerifyArray;
constexpr auto VerifyInteger = Trix::VerifyInteger;

static constexpr length_t FIELD_ROWS = 20;
static constexpr length_t FIELD_COLS = 10;

// Validate one row of a field (length 10, plain Array, RW if `require_rw`).
// Returns the row's Object * (10 cells) on success.
static Object *tetrix_validate_row(Trix *trx, Object *row_ptr, length_t r, bool require_rw, const char *op_name) {
    if (!row_ptr->is_array()) {
        trx->error(Error::TypeCheck, "'{}': row {} is not a plain array", op_name, r);
    } else if (row_ptr->arrays_length() != FIELD_COLS) {
        trx->error(Error::RangeCheck, "'{}': row {} length {} != {}", op_name, r, row_ptr->arrays_length(), FIELD_COLS);
    } else if (require_rw && !row_ptr->has_write_access()) {
        trx->error(Error::InvalidAccess, "'{}': dst row {} is not ReadWrite", op_name, r);
    } else {
        return row_ptr->array_objects(trx);
    }
}

// Validate the outer field array: plain Array (not packed), length 20,
// RW if `require_rw`.  Returns the outer Object * (20 row slots).
static Object *tetrix_validate_field(Trix *trx, Object *fld_ptr, bool require_rw, const char *op_name) {
    if (!fld_ptr->is_array()) {
        trx->error(Error::TypeCheck, "'{}': field is not a plain array", op_name);
    } else if (fld_ptr->arrays_length() != FIELD_ROWS) {
        trx->error(Error::RangeCheck, "'{}': field outer length {} != {}", op_name, fld_ptr->arrays_length(), FIELD_ROWS);
    } else if (require_rw && !fld_ptr->has_write_access()) {
        trx->error(Error::InvalidAccess, "'{}': dst field is not ReadWrite", op_name);
    } else {
        return fld_ptr->array_objects(trx);
    }
}

// Read an Integer cell, error /type-check otherwise.  Hot-path call so we
// keep the message short and avoid format-string overhead on the success
// side (the failure side is one-shot).  Object is 8 bytes -- pass by value.
static integer_t tetrix_cell_int(Trix *trx, Object cell, length_t r, length_t c, const char *op_name) {
    if (!cell.is_integer()) {
        trx->error(Error::TypeCheck, "'{}': cell ({},{}) is not Integer", op_name, r, c);
    } else {
        return cell.integer_value();
    }
}

// Validate a piece shape (4-element Array of [dr dc] pairs) and unpack
// the i-th cell into (dr, dc) Integer pair.  Returns nothing; on
// validation failure raises directly.
static void
tetrix_unpack_shape_cell(Trix *trx, Object *shape_data, length_t i, integer_t *dr_out, integer_t *dc_out, const char *op_name) {
    auto cell_obj = shape_data[i];
    if (!cell_obj.is_array()) {
        trx->error(Error::TypeCheck, "'{}': shape[{}] must be a plain array", op_name, i);
    } else if (cell_obj.arrays_length() != 2) {
        trx->error(Error::RangeCheck, "'{}': shape[{}] must have length 2, got {}", op_name, i, cell_obj.arrays_length());
    } else {
        auto cell_data = cell_obj.array_objects(trx);
        if (!cell_data[0].is_integer() || !cell_data[1].is_integer()) {
            trx->error(Error::TypeCheck, "'{}': shape[{}] coords must be Integer", op_name, i);
        } else {
            *dr_out = cell_data[0].integer_value();
            *dc_out = cell_data[1].integer_value();
        }
    }
}

// Validate a 4-cell shape array (length 4, plain Array).
static Object *tetrix_validate_shape(Trix *trx, Object *shape_ptr, const char *op_name) {
    if (!shape_ptr->is_array()) {
        trx->error(Error::TypeCheck, "'{}': shape must be a plain array", op_name);
    } else if (shape_ptr->arrays_length() != 4) {
        trx->error(Error::RangeCheck, "'{}': shape length {} != 4", op_name, shape_ptr->arrays_length());
    } else {
        return shape_ptr->array_objects(trx);
    }
}

// field-copy-fast: src dst :- --
// Overwrites every cell of dst with the corresponding cell of src.
// Native replacement for examples/tetrix.trx's /field-copy-into.
static void tetrix_field_copy_fast_op(Trix *trx) {
    trx->verify_operands(VerifyArray, VerifyArray);

    auto dst_ptr = trx->m_op_ptr;
    auto src_ptr = (dst_ptr - 1);

    auto src_rows = tetrix_validate_field(trx, src_ptr, /*require_rw=*/false, "field-copy-fast");
    auto dst_rows = tetrix_validate_field(trx, dst_ptr, /*require_rw=*/true, "field-copy-fast");

    auto curr_save_level = Save::current_level(trx);
    for (length_t r = 0; r < FIELD_ROWS; ++r) {
        auto src_cells = tetrix_validate_row(trx, &src_rows[r], r, /*require_rw=*/false, "field-copy-fast");
        auto dst_cells = tetrix_validate_row(trx, &dst_rows[r], r, /*require_rw=*/true, "field-copy-fast");
        for (length_t c = 0; c < FIELD_COLS; ++c) {
            // No journaling -- see header comment.  Plain assignment since
            // cells are Integer.
            dst_cells[c] = src_cells[c];
            dst_cells[c].set_save_level(curr_save_level);
        }
    }

    trx->m_op_ptr -= 2;
}

// score-board-fast: fld lines weights :- score
// Computes the AI heuristic over a 20x10 field in pure C, mirroring the
// /score-board-into reference implementation in examples/tetrix.trx.
//
//   weights : Array of length 5 of Integer
//             [w-lines, w-height, w-holes, w-bumpy, w-well]
//   lines   : Integer (lines just cleared on this placement; 0 at search leaf)
//   fld     : 20x10 field (Integer cells)
//
//   score = lines*w-lines + h-sum*w-height + holes*w-holes
//                         + bumpy*w-bumpy  + wells*w-well
//
// Five sub-passes match score-board-into's call graph 1:1:
//   A. heights[c]  = 20 - row-of-topmost-block(c), or 0 if column empty
//   B. h-sum       = sum(heights)
//   C. holes       = count of empty cells in rows below each column's top
//   D. bumpy       = sum |heights[i] - heights[i+1]| over adjacent cols
//   E. wells       = sum of (min(left,right)-h) per col when >= 2; edges
//                    use 20 as virtual neighbor.
static void tetrix_score_board_fast_op(Trix *trx) {
    trx->verify_operands(VerifyArray, VerifyInteger, VerifyArray);

    auto weights_ptr = trx->m_op_ptr;
    auto lines_ptr = (weights_ptr - 1);
    auto fld_ptr = (weights_ptr - 2);

    // Field validation reuses the field-copy-fast helpers.
    auto fld_rows = tetrix_validate_field(trx, fld_ptr, /*require_rw=*/false, "score-board-fast");

    // Weights validation: plain Array of length 5 of Integer.
    if (!weights_ptr->is_array()) {
        trx->error(Error::TypeCheck, "'score-board-fast': weights must be a plain Array");
    } else if (weights_ptr->arrays_length() != 5) {
        trx->error(Error::RangeCheck, "'score-board-fast': weights length {} != 5", weights_ptr->arrays_length());
    } else {
        auto weights_data = weights_ptr->array_objects(trx);
        integer_t w[5];
        for (length_t i = 0; i < 5; ++i) {
            if (!weights_data[i].is_integer()) {
                trx->error(Error::TypeCheck, "'score-board-fast': weights[{}] is not Integer", i);
            } else {
                w[i] = weights_data[i].integer_value();
            }
        }

        auto lines = lines_ptr->integer_value();

        // Read field-cells into a 20x10 int matrix.  One pass + column-major
        // access in the heights/holes passes.  The cost saved (vs reaching back
        // through Object* on every read) is small but eliminates the per-cell
        // type check from the inner passes.
        integer_t cells[FIELD_ROWS][FIELD_COLS];
        for (length_t r = 0; r < FIELD_ROWS; ++r) {
            auto row_data = tetrix_validate_row(trx, &fld_rows[r], r, /*require_rw=*/false, "score-board-fast");
            for (length_t c = 0; c < FIELD_COLS; ++c) {
                cells[r][c] = tetrix_cell_int(trx, row_data[c], r, c, "score-board-fast");
            }
        }

        // Pass A: column heights.  heights[c] = 20 - row-of-topmost-block, or 0 if empty.
        integer_t heights[FIELD_COLS];
        for (length_t c = 0; c < FIELD_COLS; ++c) {
            integer_t h = 0;
            for (length_t r = 0; r < FIELD_ROWS; ++r) {
                if (cells[r][c] != 0) {
                    h = static_cast<integer_t>(FIELD_ROWS - r);
                    break;
                }
            }
            heights[c] = h;
        }

        // Pass B: aggregate height.
        integer_t h_sum = 0;
        for (length_t c = 0; c < FIELD_COLS; ++c) {
            h_sum += heights[c];
        }

        // Pass C: count holes.  For each column with h > 0, iterate from
        // (FIELD_ROWS - h + 1) up to (FIELD_ROWS - 1) inclusive (the rows
        // strictly below the topmost block) and count zero cells.
        integer_t holes = 0;
        for (length_t c = 0; c < FIELD_COLS; ++c) {
            auto h = heights[c];
            if (h > 0) {
                auto first_below = static_cast<length_t>(FIELD_ROWS - h + 1);
                for (auto r = first_below; r < FIELD_ROWS; ++r) {
                    if (cells[r][c] == 0) {
                        ++holes;
                    }
                }
            }
        }

        // Pass D: bumpiness.  Sum |heights[i] - heights[i+1]| over adjacent cols.
        integer_t bumpy = 0;
        for (length_t i = 0; i < (FIELD_COLS - 1); ++i) {
            auto d = static_cast<integer_t>(heights[i] - heights[i + 1]);
            if (d < 0) {
                d = -d;
            }
            bumpy += d;
        }

        // Pass E: well-depth.  For each col, depth = min(left,right) - h; edges
        // use FIELD_ROWS as virtual neighbor.  If depth >= 2, add depth to total.
        integer_t wells = 0;
        for (length_t i = 0; i < FIELD_COLS; ++i) {
            auto h = heights[i];
            auto left = (i == 0) ? static_cast<integer_t>(FIELD_ROWS) : heights[i - 1];
            auto right = (i == (FIELD_COLS - 1)) ? static_cast<integer_t>(FIELD_ROWS) : heights[i + 1];
            auto min_side = (left < right) ? left : right;
            auto depth = static_cast<integer_t>(min_side - h);
            if (depth >= 2) {
                wells += depth;
            }
        }

        // Combine: score = lines*w0 + h_sum*w1 + holes*w2 + bumpy*w3 + wells*w4.
        // Each component fits comfortably in integer_t (e.g. h_sum max = 200,
        // weights bounded by ~1000).  Use overflow-checked builtins anyway --
        // a future tunable could push values out of range and we want a clean
        // error rather than wraparound that silently corrupts the search.
        integer_t score;
        integer_t tmp;
        bool overflow = false;
        overflow |= __builtin_mul_overflow(lines, w[0], &score);
        overflow |= __builtin_mul_overflow(h_sum, w[1], &tmp);
        overflow |= __builtin_add_overflow(score, tmp, &score);
        overflow |= __builtin_mul_overflow(holes, w[2], &tmp);
        overflow |= __builtin_add_overflow(score, tmp, &score);
        overflow |= __builtin_mul_overflow(bumpy, w[3], &tmp);
        overflow |= __builtin_add_overflow(score, tmp, &score);
        overflow |= __builtin_mul_overflow(wells, w[4], &tmp);
        overflow |= __builtin_add_overflow(score, tmp, &score);
        if (overflow) {
            trx->error(Error::NumericalOverflow, "'score-board-fast': score combination overflows Integer");
        } else {
            // Pop weights, lines; replace fld with score.
            trx->m_op_ptr -= 2;
            *trx->m_op_ptr = Object::make_integer(score);
        }
    }
}

// piece-collides-fast: field shape py px :- bool
// True if any of the 4 cells in `shape` (each a [dr dc] pair) collides
// with an occupied field cell after the (py,px) offset, OR falls out of
// bounds.  Native replacement for examples/tetrix.trx's /piece-collides?,
// which dispatches piece-shape lookup to the caller (the rebound wrapper).
static void tetrix_piece_collides_fast_op(Trix *trx) {
    trx->verify_operands(VerifyInteger, VerifyInteger, VerifyArray, VerifyArray);

    auto px_ptr = trx->m_op_ptr;
    auto py_ptr = (px_ptr - 1);
    auto shape_ptr = (px_ptr - 2);
    auto fld_ptr = (px_ptr - 3);

    auto px = px_ptr->integer_value();
    auto py = py_ptr->integer_value();

    auto fld_rows = tetrix_validate_field(trx, fld_ptr, /*require_rw=*/false, "piece-collides-fast");
    auto shape_data = tetrix_validate_shape(trx, shape_ptr, "piece-collides-fast");

    bool hit = false;
    for (length_t i = 0; (i < 4) && !hit; ++i) {
        integer_t dr;
        integer_t dc;
        tetrix_unpack_shape_cell(trx, shape_data, i, &dr, &dc, "piece-collides-fast");
        // py/px and the shape offsets are unbounded script-supplied integer_t
        // (int32); widen to long_t so the add cannot overflow before the
        // bounds check narrows back to length_t.
        auto r = static_cast<long_t>(py) + dr;
        auto c = static_cast<long_t>(px) + dc;
        if ((r < 0) || (r >= FIELD_ROWS) || (c < 0) || (c >= FIELD_COLS)) {
            hit = true;
        } else {
            auto row_data =
                    tetrix_validate_row(trx, &fld_rows[r], static_cast<length_t>(r), /*require_rw=*/false, "piece-collides-fast");
            auto cell_val =
                    tetrix_cell_int(trx, row_data[c], static_cast<length_t>(r), static_cast<length_t>(c), "piece-collides-fast");
            hit = (cell_val != 0);
        }
    }

    trx->m_op_ptr -= 3;
    *trx->m_op_ptr = Object::make_boolean(hit);
}

// piece-merge-fast: field shape color py px :- field
// Writes `color` to every in-bounds cell of `shape` after offset (py,px).
// Out-of-bounds cells are silently dropped (matches /piece-merge).  Field
// is mutated in place; the same field is left on the operand stack so the
// signature matches the original /piece-merge return shape.
static void tetrix_piece_merge_fast_op(Trix *trx) {
    trx->verify_operands(VerifyInteger, VerifyInteger, VerifyInteger, VerifyArray, VerifyArray);

    auto px_ptr = trx->m_op_ptr;
    auto py_ptr = (px_ptr - 1);
    auto color_ptr = (px_ptr - 2);
    auto shape_ptr = (px_ptr - 3);
    auto fld_ptr = (px_ptr - 4);

    auto px = px_ptr->integer_value();
    auto py = py_ptr->integer_value();
    auto color = color_ptr->integer_value();

    auto fld_rows = tetrix_validate_field(trx, fld_ptr, /*require_rw=*/true, "piece-merge-fast");
    auto shape_data = tetrix_validate_shape(trx, shape_ptr, "piece-merge-fast");

    auto curr_save_level = Save::current_level(trx);

    for (length_t i = 0; i < 4; ++i) {
        integer_t dr;
        integer_t dc;
        tetrix_unpack_shape_cell(trx, shape_data, i, &dr, &dc, "piece-merge-fast");
        // Widen to long_t before the add (see piece-collides-fast).
        auto r = static_cast<long_t>(py) + dr;
        auto c = static_cast<long_t>(px) + dc;
        if ((r >= 0) && (r < FIELD_ROWS) && (c >= 0) && (c < FIELD_COLS)) {
            auto row_data =
                    tetrix_validate_row(trx, &fld_rows[r], static_cast<length_t>(r), /*require_rw=*/true, "piece-merge-fast");
            auto target = &row_data[c];
            // No journaling -- see header comment.
            *target = Object::make_integer(color);
            target->set_save_level(curr_save_level);
        }
    }

    // Pop px, py, color, shape; leave fld on top of stack.
    trx->m_op_ptr -= 4;
}

// piece-ghost-row-fast: field shape py px :- ghost-row
// Drops the piece downward from py until the next row would collide, and
// returns the deepest row at which it still fits.  Native replacement for
// examples/tetrix.trx's /piece-ghost-row, called once per simulate-into
// placement test (~150 calls per AI-peek piece).
//
// The Trix reference loops `piece-collides?` at try_row+1; we inline the
// collision test and pre-unpack the shape's [dr dc] cells once instead of
// re-validating them on every iteration (worst case: 20 iters per call).
static void tetrix_piece_ghost_row_fast_op(Trix *trx) {
    trx->verify_operands(VerifyInteger, VerifyInteger, VerifyArray, VerifyArray);

    auto px_ptr = trx->m_op_ptr;
    auto py_ptr = (px_ptr - 1);
    auto shape_ptr = (px_ptr - 2);
    auto fld_ptr = (px_ptr - 3);

    auto px = px_ptr->integer_value();
    auto py_start = py_ptr->integer_value();

    auto fld_rows = tetrix_validate_field(trx, fld_ptr, /*require_rw=*/false, "piece-ghost-row-fast");
    auto shape_data = tetrix_validate_shape(trx, shape_ptr, "piece-ghost-row-fast");

    integer_t shape_dr[4];
    integer_t shape_dc[4];
    for (length_t i = 0; i < 4; ++i) {
        tetrix_unpack_shape_cell(trx, shape_data, i, &shape_dr[i], &shape_dc[i], "piece-ghost-row-fast");
    }

    // try_row is long_t and the loop is capped at FIELD_ROWS (the piece can
    // never come to rest below the field), so neither try_row + 1 nor the
    // ++try_row increment can overflow int32 on an adversarial py_start.
    auto try_row = static_cast<long_t>(py_start);
    while (try_row < FIELD_ROWS) {
        auto next_row = try_row + 1;
        bool hit = false;
        for (length_t i = 0; (i < 4) && !hit; ++i) {
            auto r = next_row + shape_dr[i];
            auto c = static_cast<long_t>(px) + shape_dc[i];
            if ((r < 0) || (r >= FIELD_ROWS) || (c < 0) || (c >= FIELD_COLS)) {
                hit = true;
            } else {
                auto row_data = tetrix_validate_row(trx,
                                                    &fld_rows[static_cast<length_t>(r)],
                                                    static_cast<length_t>(r),
                                                    /*require_rw=*/false,
                                                    "piece-ghost-row-fast");
                auto cell_val = tetrix_cell_int(trx,
                                                row_data[static_cast<length_t>(c)],
                                                static_cast<length_t>(r),
                                                static_cast<length_t>(c),
                                                "piece-ghost-row-fast");
                hit = (cell_val != 0);
            }
        }
        if (hit) {
            break;
        } else {
            ++try_row;
        }
    }

    // Pop px, py, shape; replace fld with try_row.
    trx->m_op_ptr -= 3;
    *trx->m_op_ptr = Object::make_integer(static_cast<integer_t>(try_row));
}

// Mutate one cell of dst.  No journaling -- see header comment for the
// rationale.  Centralizes the cell-mutation pattern shared by field-copy-
// fast (now inlined) and the row shifts inside field-clear-rows-fast.
static void tetrix_set_cell(Object *target, Object value, save_level_t curr_save_level) {
    *target = value;
    target->set_save_level(curr_save_level);
}

// field-clear-rows-fast: field :- field cleared-count
// Drops every full row, shifts the surviving rows downward, zero-fills the
// freed top rows, and returns (field, count-of-cleared-rows).  Mutates the
// field in place; the returned field is the same Object as the input.
//
// Two-pointer scan matches the Trix-level reference exactly: read and
// write start at row 19; full rows advance only read; surviving rows copy
// read->write cell-by-cell (no journaling, see header) and advance both.  After the
// scan, rows 0..write are zero-filled in place.
static void tetrix_field_clear_rows_fast_op(Trix *trx) {
    trx->verify_operands(VerifyArray);

    auto fld_ptr = trx->m_op_ptr;
    auto fld_rows = tetrix_validate_field(trx, fld_ptr, /*require_rw=*/true, "field-clear-rows-fast");

    auto curr_save_level = Save::current_level(trx);

    // Pre-validate every row once so the inner loop can index without re-checking.
    Object *row_data[FIELD_ROWS];
    for (length_t r = 0; r < FIELD_ROWS; ++r) {
        row_data[r] = tetrix_validate_row(trx, &fld_rows[r], r, /*require_rw=*/true, "field-clear-rows-fast");
    }

    // Two-pointer scan, signed so the loop can terminate at -1.
    auto write = static_cast<integer_t>(FIELD_ROWS) - 1;
    auto read = static_cast<integer_t>(FIELD_ROWS) - 1;
    while (read >= 0) {
        auto src = row_data[read];
        bool full = true;
        for (length_t c = 0; c < FIELD_COLS; ++c) {
            if (!src[c].is_integer()) {
                trx->error(Error::TypeCheck, "'field-clear-rows-fast': cell ({},{}) is not Integer", read, c);
            } else if (src[c].integer_value() == 0) {
                full = false;
                break;
            }
        }
        if (full) {
            --read;
        } else {
            if (read != write) {
                auto dst = row_data[write];
                for (length_t c = 0; c < FIELD_COLS; ++c) {
                    tetrix_set_cell(&dst[c], src[c], curr_save_level);
                }
            }
            --read;
            --write;
        }
    }

    auto cleared = write + 1;
    auto zero_obj = Object::make_integer(0);
    for (integer_t i = 0; i < cleared; ++i) {
        auto dst = row_data[i];
        for (length_t c = 0; c < FIELD_COLS; ++c) {
            tetrix_set_cell(&dst[c], zero_obj, curr_save_level);
        }
    }

    // Field stays on m_op_ptr; push cleared count above it.  verify_operands
    // guarantees a minimum operand count, never headroom above the top, so the
    // net +1 push must reserve a slot or it can overrun the operand stack.
    trx->require_op_capacity(1);
    ++trx->m_op_ptr;
    *trx->m_op_ptr = Object::make_integer(cleared);
}

// Null-terminated user operator table.
static constexpr Operator user_ops[] = {
        {      tetrix_field_copy_fast_op,       "field-copy-fast"},
        {     tetrix_score_board_fast_op,      "score-board-fast"},
        {  tetrix_piece_collides_fast_op,   "piece-collides-fast"},
        {     tetrix_piece_merge_fast_op,      "piece-merge-fast"},
        {tetrix_field_clear_rows_fast_op, "field-clear-rows-fast"},
        { tetrix_piece_ghost_row_fast_op,  "piece-ghost-row-fast"},
        {                        nullptr,                      {}},
};

// Long options recognized by parse_args (mirror of api.inl's long_options
// table).  Used by tetrix-side argv pre-processing to distinguish trix
// flags (which parse_args consumes) from tetrix-script flags (--ai-peek,
// --seed N, --self-test, ...) which must reach the embedded examples/tetrix.trx
// via command-line-args.
//
// Maintenance: when api.inl gains a new long option, add the same string
// here.  A missing entry degrades to "treated as a script flag": the binary
// still runs, but the flag is never consumed by parse_args and so never takes
// effect as a trix flag -- it falls through to the script's command-line-args
// instead.  There is no escape hatch: spelling out the script path
// (`./tetrix examples/tetrix.trx --new-flag`) does NOT rescue it, because
// getopt_long's leading `+` (see api.inl) stops option scanning at the script
// path, so every token after it goes to the script.
static constexpr const char *kTrixLongOptions[] = {
        "--help",         "--version",       "--debug",         "--stdedit",       "--stdin",
        "--image",        "--quiet",         "--no-banner",     "--sandbox",       "--resident",
        "--about",        "--vm-size",       "--operand-depth", "--exec-depth",    "--dict-depth",
        "--error-depth",  "--save-depth",    "--stream-count",  "--stream-buffer", "--stream-io",
        "--name-buckets", "--userdict-size", "--eq-string",     "--eq-array",      "--eq-proc",
        "--eq-dict",      "--quantum",       "--max-ops",       "--sleep-budget",  "--test-eqgen-preload",
        "--interactive",  "--scratch-depth", "--module-path",   "--inspect",       "--inspect-on-error",
        "--inspect-at",   "--no-color",
};

// True when arg is a long option present in kTrixLongOptions, in either
// `--name` or `--name=value` form.
static bool is_trix_long_option(const char *arg) {
    for (const char *known : kTrixLongOptions) {
        auto klen = std::strlen(known);
        if (std::strncmp(arg, known, klen) == 0) {
            if ((arg[klen] == '\0') || (arg[klen] == '=')) {
                return true;
            }
        }
    }
    return false;
}

int main(int argc, char *argv[]) {
    // Pre-process argv: when invoked without a script filename and the
    // remaining args contain at least one long option NOT recognized by
    // trix (e.g. --ai-peek, --seed, --self-test), insert
    // examples/tetrix.trx at the right position so getopt's leading `+`
    // stops there and the unknown flags fall through to command-line-args.
    //
    // Scan rules: positional arg or `--` separator means the user has
    // already specified a script -- no insertion.  --help/-h belongs to
    // the GAME (script usage), so it triggers insertion too.  Other
    // short options ("-d", etc.) are always trix-known.  Long options
    // not in kTrixLongOptions are the trigger for insertion.
    std::vector<char *> rewritten_argv;
    int effective_argc = argc;
    char **effective_argv = argv;
    char default_path[] = "examples/tetrix.trx";

    int insert_pos = -1;
    bool already_has_script = false;
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if ((a[0] != '-') || ((a[1] == '-') && (a[2] == '\0'))) {
            // Positional arg or `--` separator: user has supplied a script.
            already_has_script = true;
            break;
        } else if ((std::strcmp(a, "--help") == 0) || (std::strcmp(a, "-h") == 0)) {
            // Help belongs to the game: insert the default script so the
            // flag falls through to command-line-args and tetrix.trx
            // prints its usage (which points at `trix --help` for engine
            // flags).  Without this, trix's own parser consumes --help
            // and only the baseline interpreter help appears.
            insert_pos = i;
            break;
        } else if (a[1] != '-') {
            // Short option ("-d", ...) or bare "-": trix-known, keep scanning.
        } else if (is_trix_long_option(a)) {
            // Skip the option's value if it's a separate token.  The trix
            // long_options table marks all value-bearing options as
            // required_argument, so the value is either inline (--foo=BAR)
            // or in the next argv slot (--foo BAR).  Inline form needs no
            // skip; separate form does.
            const char *eq = std::strchr(a, '=');
            if ((eq == nullptr) && ((i + 1) < argc)) {
                // Heuristic: known boolean-like trix flags don't take a
                // value.  Keep them in a small list; everything else with
                // required_argument advances past the value token.
                static constexpr const char *kTrixBooleanLong[] = {
                        "--help",
                        "--version",
                        "--debug",
                        "--stdedit",
                        "--stdin",
                        "--image",
                        "--quiet",
                        "--no-banner",
                        "--sandbox",
                        "--resident",
                        "--about",
                        "--interactive",
                        "--inspect",
                        "--inspect-on-error",
                        "--no-color",
                };
                bool is_boolean = false;
                for (const char *b : kTrixBooleanLong) {
                    if (std::strcmp(a, b) == 0) {
                        is_boolean = true;
                        break;
                    }
                }
                if (!is_boolean) {
                    ++i;  // skip the value token
                }
            }
        } else {
            // Unknown long option -- this is a script flag.  Insert the
            // default script path here so getopt stops at it and the rest
            // of argv falls through to command-line-args.
            insert_pos = i;
            break;
        }
    }

    if (!already_has_script && (insert_pos >= 0) && (access(default_path, F_OK) == 0)) {
        rewritten_argv.reserve(static_cast<size_t>(argc) + 1);
        for (int i = 0; i < insert_pos; ++i) {
            rewritten_argv.push_back(argv[i]);
        }
        rewritten_argv.push_back(default_path);
        for (int i = insert_pos; i < argc; ++i) {
            rewritten_argv.push_back(argv[i]);
        }
        effective_argc = static_cast<int>(rewritten_argv.size());
        effective_argv = rewritten_argv.data();
    }

    auto result = Trix::parse_args(effective_argc, effective_argv);
    if (result.should_exit) {
        return result.exit_code;
    } else {
        result.config.m_useroperators = user_ops;
        // Only override when the user left the library default, so an explicit
        // --stream-count=N is honored (parse_args has already applied it).
        if (result.config.m_stream_count == DefaultStreamCount) {
            result.config.m_stream_count = 16;
        }

        // Default to examples/tetrix.trx when no script was specified.  This
        // covers the bare `./tetrix` and `./tetrix --vm-size 4M` cases (no
        // unknown long option triggered the pre-scan insertion above).
        if ((result.config.m_filename == nullptr) && (result.config.m_mode == StartupMode::Interactive)) {
            if (access(default_path, F_OK) == 0) {
                result.config.m_filename = default_path;
                result.config.m_mode = StartupMode::ScriptFile;
            }
        }

        Trix trx(result.vm_size, result.config);
        return trx.exit_code();
    }
}
