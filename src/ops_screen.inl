//===----------------------------------------------------------------------===//
//                                                                            //
//    ______    _                                                             //
//   /_  __/___(_)_  __                                                       //
//    / / / __/ /\ \/ /       Stack-Based Interpreter & VM                    //
//   / / / / / /  > · <      C++23 · Single-Header Library                    //
//  /_/ /_/ /_/  /_/\_\     Copyright 2026 Mark Guidarelli                    //
//                                                                            //
// Licensed under the Apache License, Version 2.0 (the "License");            //
// you may not use this file except in compliance with the License.           //
// You may obtain a copy of the License at                                    //
//                                                                            //
//     https://www.apache.org/licenses/LICENSE-2.0                            //
//                                                                            //
// Unless required by applicable law or agreed to in writing, software        //
// distributed under the License is distributed on an "AS IS" BASIS,          //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.   //
// See the License for the specific language governing permissions and        //
// limitations under the License.                                             //
//                                                                            //
//===----------------------------------------------------------------------===//

//===--- Screen: Curses-equivalent terminal output surface ---===//
//
// Implements the Screen kind of Object::Type::OpaqueHandle.  A Screen is a
// virtual cell buffer that the user mutates via screen-put-cell / screen-
// put-string / screen-fill-rect and renders to a terminal (or any writable
// stream) via screen-render / screen-render-to.  The render diff against a
// per-screen prev-buffer emits only the cells that changed since last
// render, which is the value-add over raw ANSI emission.
//
// State lives entirely in the VM heap:
//   ScreenState struct (cols, rows, cells_offset, prev_offset, ...)
//   cells   = cols*rows * sizeof(Cell)  bytes
//   prev    = cols*rows * sizeof(Cell)  bytes  (last-rendered snapshot)
//
// All three allocations are made at make-screen time on the VM heap.  No
// pool, no SID -- save/restore rolls back any allocations made above the
// save mark; the Object's m_object_save_level is the only restore-time
// staleness gate needed.
//
// JOURNALING POLICY -- SCREEN OPS BYPASS THE JOURNAL BY DESIGN
//   Cell writes (screen_put_cell_op, screen_put_string_op,
//   screen_put_utf8_string_op, screen_fill_rect_op, screen_clear_op,
//   screen_blit_op) are direct C++ assignments to the cells buffer with
//   no Save::save_object call.  Display output is
//   committed at write time, not at restore time -- a render surface
//   that un-painted itself when an error rolled back state would be a
//   UX bug, not a feature.  Screen ops are therefore deliberately NOT
//   members of the -persist family; see docs/trix-reference.md s7.7.1
//   "Deliberate omissions".  The one screen op that does honor the
//   save barrier is screen-resize, and only because it allocates new
//   cell buffers (which would dangle if the screen state struct lives
//   below the barrier) -- see screen_resize_op's comment block.
//
// Cell layout (8 bytes, 256-color):
//   uint32_t ch     codepoint (UCS-4)
//   uint8_t  fg     0..255 (256-palette)
//   uint8_t  bg     0..255 (256-palette)
//   uint8_t  attrs  bit 0 bold, 1 dim, 2 italic, 3 underline,
//                   4 blink, 5 reverse, 6 strike, 7 reserved
//   uint8_t  pad
//
// Defaults colors at the cell level are not supported in v1 (preserves the
// 8-byte cell budget).  Use ansi.trx's default-fg / default-bg between
// renders if a default-colored region is needed.

struct ScreenCell {
    uint32_t m_ch;    // codepoint (default ' ' = 0x20)
    uint8_t m_fg;     // 0..255 SGR palette
    uint8_t m_bg;     // 0..255 SGR palette
    uint8_t m_attrs;  // SGR attribute bits (see header comment)
    uint8_t m_pad;    // unused
};
static_assert(sizeof(ScreenCell) == 8);

struct ScreenState {
    length_t m_cols;             // 0..MaxLength
    length_t m_rows;             // 0..MaxLength
    vm_offset_t m_cells_offset;  // cols*rows ScreenCells
    vm_offset_t m_prev_offset;   // cols*rows ScreenCells (last-rendered snapshot)
    uint8_t m_last_attrs;        // diff-stateful SGR cache: last emitted attrs
    uint8_t m_last_fg;           // last emitted fg
    uint8_t m_last_bg;           // last emitted bg
    uint8_t m_last_state_valid;  // 0 = no prior render; first render is full repaint
};

// Default cell content for fresh screens / cleared regions.
static constexpr ScreenCell DefaultScreenCell{
        .m_ch = 0x20,  // space
        .m_fg = 7,     // light gray
        .m_bg = 0,     // black
        .m_attrs = 0,
        .m_pad = 0,
};

// Cap cells * sizeof(ScreenCell) at vm_size_t comfortably below 1 GiB.  In
// practice screens are 200x80 (~16K cells, 128K bytes).  This protects
// against vm_alloc_n overflow on absurd sizes.
static constexpr vm_size_t MaxScreenCells{(1ull << 24)};  // 16M cells = 128 MiB cell buffer

// Allocate the three pieces (state + cells + prev) and return the Screen Object.
// Cells are filled with DefaultScreenCell; prev is filled with a sentinel
// that compares unequal to any valid cell so the first render emits everything.
[[nodiscard]] static Object make_screen_helper(Trix *trx, length_t cols, length_t rows) {
    if ((cols == 0) || (rows == 0)) {
        trx->error(Error::RangeCheck, "make-screen: cols={} rows={} must both be positive", cols, rows);
    } else {
        auto cell_count = static_cast<vm_size_t>(cols) * static_cast<vm_size_t>(rows);
        if (cell_count > MaxScreenCells) {
            trx->error(Error::LimitCheck, "make-screen: {}x{}={} cells exceeds maximum {}", cols, rows, cell_count, MaxScreenCells);
        } else {
            auto [state, state_offset] = trx->vm_alloc<ScreenState>();
            auto [cells, cells_offset] = trx->vm_alloc_n<ScreenCell>(cell_count);
            auto [prev, prev_offset] = trx->vm_alloc_n<ScreenCell>(cell_count);

            state->m_cols = cols;
            state->m_rows = rows;
            state->m_cells_offset = cells_offset;
            state->m_prev_offset = prev_offset;
            state->m_last_attrs = 0;
            state->m_last_fg = 0;
            state->m_last_bg = 0;
            state->m_last_state_valid = 0;

            std::fill_n(cells, cell_count, DefaultScreenCell);
            // prev is filled with a sentinel cell guaranteed to differ from any
            // valid cell content -- m_attrs bit 7 is reserved (verify_attrs_arg
            // rejects user values that set it), so any cell with m_attrs == 0x80
            // is distinguishable from every cell a user can construct.  m_ch is
            // also set to 0xFFFFFFFF (out of Unicode range) for defense-in-depth.
            // After the first render, prev is overwritten with a copy of cells.
            auto sentinel = ScreenCell{
                    .m_ch = 0xFFFFFFFFu,
                    .m_fg = 0,
                    .m_bg = 0,
                    .m_attrs = 0x80,
                    .m_pad = 0,
            };
            std::fill_n(prev, cell_count, sentinel);

            return Object::make_handle(state_offset, Object::HandleKind::Screen);
        }
    }
}

// Pulls a ScreenState* from an Object.  Caller must have already passed
// the operand through verify_operands(VerifyScreen, ...), which guarantees
// type=OpaqueHandle and kind=Screen, so this is a pure offset deref.
[[nodiscard]] static ScreenState *screen_state(Trix *trx, const Object *obj_ptr) {
    return trx->offset_to_ptr<ScreenState>(obj_ptr->handle_offset());
}

// make-screen: cols rows :- screen
// Allocates a virtual screen of the given dimensions.  Both cols and rows
// must be positive.  Cells are initialized to space + light-gray fg + black
// bg + no attrs.  No host effect; allowed under --sandbox.
// throws: limit-check, opstack-underflow, range-check, type-check, vm-full
static void make_screen_op(Trix *trx) {
    trx->verify_operands(VerifyIntegers32 | VerifyNotNegative, VerifyIntegers32 | VerifyNotNegative);
    auto rows_ptr = trx->m_op_ptr;
    auto cols_ptr = (rows_ptr - 1);
    auto [rows_valid, rows_val] = rows_ptr->uinteger_value(trx);
    auto [cols_valid, cols_val] = cols_ptr->uinteger_value(trx);
    if (!rows_valid || !cols_valid || (rows_val > std::numeric_limits<length_t>::max()) ||
        (cols_val > std::numeric_limits<length_t>::max())) {
        trx->error(
                Error::RangeCheck, "make-screen: dimensions must fit in length_t (max {})", std::numeric_limits<length_t>::max());
    }
    auto screen = make_screen_helper(trx, static_cast<length_t>(cols_val), static_cast<length_t>(rows_val));
    *cols_ptr = screen;
    --trx->m_op_ptr;
}

// screen-cols: screen :- cols
// Returns the screen's column count.
// throws: opstack-underflow, type-check
static void screen_cols_op(Trix *trx) {
    trx->verify_operands(VerifyScreen);
    auto screen_ptr = trx->m_op_ptr;
    auto state = screen_state(trx, screen_ptr);
    *screen_ptr = Object::make_integer(static_cast<integer_t>(state->m_cols));
}

// screen-rows: screen :- rows
// Returns the screen's row count.
// throws: opstack-underflow, type-check
static void screen_rows_op(Trix *trx) {
    trx->verify_operands(VerifyScreen);
    auto screen_ptr = trx->m_op_ptr;
    auto state = screen_state(trx, screen_ptr);
    *screen_ptr = Object::make_integer(static_cast<integer_t>(state->m_rows));
}

// screen-clear: screen :- screen
// Resets all cells to the default (space, fg=7, bg=0, no attrs).  Returns
// the same screen handle so chains read left-to-right.  prev is left
// untouched -- the next render will diff cells (now defaults) vs prev (last
// rendered) and emit the changes.
// throws: opstack-underflow, type-check
static void screen_clear_op(Trix *trx) {
    trx->verify_operands(VerifyScreen);
    auto screen_ptr = trx->m_op_ptr;
    auto state = screen_state(trx, screen_ptr);
    auto cell_count = static_cast<vm_size_t>(state->m_cols) * static_cast<vm_size_t>(state->m_rows);
    auto cells = trx->offset_to_ptr<ScreenCell>(state->m_cells_offset);
    std::fill_n(cells, cell_count, DefaultScreenCell);
    // screen-clear leaves the handle on the stack; no change to *screen_ptr.
}

// screen-resize: screen new-cols new-rows :- screen
// Resizes the cell buffer.  Region preserved is the intersection of old and
// new dimensions (top-left); growth is filled with default cells.  Re-
// allocates the cells/prev buffers; the old ones are orphaned -- reclaimed
// on the next restore IF the resize happened above an active save barrier,
// otherwise leaked until the next save/restore pair surrounds the resize.
// A no-op resize (new dims == old dims) early-returns without allocating
// to avoid that leak when callers proactively re-assert dimensions.
// Forces a full repaint on next render when buffers are re-allocated.
//
// New cells/prev buffers are allocated at the current save level.  If the
// screen handle was created at a lower save level (i.e. it survives a
// future restore), its state pointers would be left holding offsets into
// buffers that get reclaimed -- a dangling reference.  Reject that
// combination at resize time rather than ship a silent footgun.  Resizing
// a screen at the same save level it was created at is safe: restore rolls
// back state and buffers together.
// throws: above-barrier, limit-check, opstack-underflow, range-check, type-check, vm-full
static void screen_resize_op(Trix *trx) {
    trx->verify_operands(VerifyIntegers32 | VerifyNotNegative, VerifyIntegers32 | VerifyNotNegative, VerifyScreen);
    auto rows_ptr = trx->m_op_ptr;
    auto cols_ptr = (rows_ptr - 1);
    auto screen_ptr = (rows_ptr - 2);
    if (Save::is_active(trx) && (screen_ptr->handle_offset() < trx->m_save_stack[trx->m_curr_save_level])) {
        trx->error(Error::AboveBarrier,
                   "screen-resize: screen was created below the current save barrier -- new buffers would "
                   "dangle on restore (current sl={})",
                   trx->m_curr_save_level);
    }
    auto state = screen_state(trx, screen_ptr);
    auto [rows_valid, rows_val] = rows_ptr->uinteger_value(trx);
    auto [cols_valid, cols_val] = cols_ptr->uinteger_value(trx);
    if (!rows_valid || !cols_valid || (rows_val == 0) || (cols_val == 0) || (rows_val > std::numeric_limits<length_t>::max()) ||
        (cols_val > std::numeric_limits<length_t>::max())) {
        trx->error(Error::RangeCheck, "screen-resize: invalid dimensions");
    }
    auto new_cols = static_cast<length_t>(cols_val);
    auto new_rows = static_cast<length_t>(rows_val);
    auto new_cell_count = static_cast<vm_size_t>(new_cols) * static_cast<vm_size_t>(new_rows);
    if (new_cell_count > MaxScreenCells) {
        trx->error(Error::LimitCheck,
                   "screen-resize: {}x{}={} cells exceeds maximum {}",
                   new_cols,
                   new_rows,
                   new_cell_count,
                   MaxScreenCells);
    }

    auto old_cols = state->m_cols;
    auto old_rows = state->m_rows;
    if ((new_cols == old_cols) && (new_rows == old_rows)) {
        // No-op resize: skip the alloc that would orphan old buffers.
        trx->m_op_ptr -= 2;
    } else {
        auto copy_cols = std::min(old_cols, new_cols);
        auto copy_rows = std::min(old_rows, new_rows);

        auto [new_cells, new_cells_offset] = trx->vm_alloc_n<ScreenCell>(new_cell_count);
        auto [new_prev, new_prev_offset] = trx->vm_alloc_n<ScreenCell>(new_cell_count);
        auto old_cells = trx->offset_to_ptr<ScreenCell>(state->m_cells_offset);

        // Fill new cells with default; copy intersection from old cells.
        std::fill_n(new_cells, new_cell_count, DefaultScreenCell);
        for (length_t r = 0; r < copy_rows; ++r) {
            auto src = old_cells + static_cast<vm_size_t>(r) * old_cols;
            auto dst = new_cells + static_cast<vm_size_t>(r) * new_cols;
            std::copy_n(src, copy_cols, dst);
        }
        // prev gets the sentinel so next render is a full repaint.  See
        // make_screen_helper for the bit-7 reserved-attrs explanation.
        auto sentinel = ScreenCell{
                .m_ch = 0xFFFFFFFFu,
                .m_fg = 0,
                .m_bg = 0,
                .m_attrs = 0x80,
                .m_pad = 0,
        };
        std::fill_n(new_prev, new_cell_count, sentinel);

        state->m_cols = new_cols;
        state->m_rows = new_rows;
        state->m_cells_offset = new_cells_offset;
        state->m_prev_offset = new_prev_offset;
        state->m_last_state_valid = 0;

        trx->m_op_ptr -= 2;
        // screen handle remains on stack
    }
}

// Internal: write one cell at (col, row) of the given screen.  Bounds-checks.
static void screen_write_cell(Trix *trx,
                              ScreenState *state,
                              length_t col,
                              length_t row,
                              uint32_t ch,
                              uint8_t fg,
                              uint8_t bg,
                              uint8_t attrs,
                              std::string_view op_name) {
    if ((col >= state->m_cols) || (row >= state->m_rows)) {
        trx->error(
                Error::RangeCheck, "{}: ({}, {}) out of bounds for {}x{} screen", op_name, col, row, state->m_cols, state->m_rows);
    }
    auto cells = trx->offset_to_ptr<ScreenCell>(state->m_cells_offset);
    auto idx = static_cast<vm_size_t>(row) * state->m_cols + col;
    cells[idx] = ScreenCell{ch, fg, bg, attrs, 0};
}

// Pull a 0..255 byte-clamped palette index from any non-negative integer.
[[nodiscard]] static uint8_t verify_color_arg(Trix *trx, const Object *obj_ptr, std::string_view op_name, std::string_view label) {
    auto [valid, val] = obj_ptr->uinteger_value(trx);
    if (!valid || (val > 255)) {
        trx->error(Error::RangeCheck, "{}: {} must be 0..255, got {}", op_name, label, valid ? val : 0u);
    } else {
        return static_cast<uint8_t>(val);
    }
}

// Pull a 7-bit attribute mask (bits 0..6 used; bit 7 reserved) from any
// non-negative integer.  Rejects values that set bit 7 so the renderer
// can use bit 7 as a free sentinel/marker (see make-screen and resize
// prev-buffer initialization).
[[nodiscard]] static uint8_t verify_attrs_arg(Trix *trx, const Object *obj_ptr, std::string_view op_name) {
    auto [valid, val] = obj_ptr->uinteger_value(trx);
    if (!valid || (val > 127)) {
        trx->error(Error::RangeCheck, "{}: attrs must be 0..127 (bit 7 reserved), got {}", op_name, valid ? val : 0u);
    } else {
        return static_cast<uint8_t>(val);
    }
}

// screen-put-cell: screen col row codepoint fg bg attrs :- screen
// Writes one cell at (col, row).  All numeric args must fit in their
// respective fields; out-of-range raises /range-error.
// throws: opstack-underflow, range-check, type-check
static void screen_put_cell_op(Trix *trx) {
    trx->verify_operands(VerifyIntegers32 | VerifyNotNegative,  // attrs
                         VerifyIntegers32 | VerifyNotNegative,  // bg
                         VerifyIntegers32 | VerifyNotNegative,  // fg
                         VerifyIntegers32 | VerifyNotNegative,  // ch
                         VerifyIntegers32 | VerifyNotNegative,  // row
                         VerifyIntegers32 | VerifyNotNegative,  // col
                         VerifyScreen);                         // screen
    auto attrs_ptr = trx->m_op_ptr;
    auto bg_ptr = (attrs_ptr - 1);
    auto fg_ptr = (attrs_ptr - 2);
    auto ch_ptr = (attrs_ptr - 3);
    auto row_ptr = (attrs_ptr - 4);
    auto col_ptr = (attrs_ptr - 5);
    auto screen_ptr = (attrs_ptr - 6);
    auto state = screen_state(trx, screen_ptr);

    auto [col_valid, col_val] = col_ptr->uinteger_value(trx);
    auto [row_valid, row_val] = row_ptr->uinteger_value(trx);
    auto [ch_valid, ch_val] = ch_ptr->uinteger_value(trx);
    if (!col_valid || !row_valid || !ch_valid || (col_val > std::numeric_limits<length_t>::max()) ||
        (row_val > std::numeric_limits<length_t>::max()) || (ch_val > 0x10FFFF) || ((ch_val >= 0xD800) && (ch_val <= 0xDFFF))) {
        trx->error(Error::RangeCheck, "screen-put-cell: col/row/codepoint out of range");
    }
    auto fg = verify_color_arg(trx, fg_ptr, "screen-put-cell", "fg");
    auto bg = verify_color_arg(trx, bg_ptr, "screen-put-cell", "bg");
    auto attrs = verify_attrs_arg(trx, attrs_ptr, "screen-put-cell");

    screen_write_cell(trx,
                      state,
                      static_cast<length_t>(col_val),
                      static_cast<length_t>(row_val),
                      static_cast<uint32_t>(ch_val),
                      fg,
                      bg,
                      attrs,
                      "screen-put-cell");

    trx->m_op_ptr -= 6;
    // screen remains on stack
}

// screen-put-string: screen col row str fg bg attrs :- screen
// Writes successive bytes of `str` as cells starting at (col, row) on the
// same row.  Off-screen bytes are silently truncated at the row's right
// edge.  fg/bg/attrs apply uniformly to every cell.  Bytes are interpreted
// as Latin-1 codepoints (one cell per byte) -- a UTF-8 helper can sit on
// top in Trix code if multi-byte support is needed.
// throws: opstack-underflow, range-check, type-check
static void screen_put_string_op(Trix *trx) {
    trx->verify_operands(VerifyIntegers32 | VerifyNotNegative,  // attrs
                         VerifyIntegers32 | VerifyNotNegative,  // bg
                         VerifyIntegers32 | VerifyNotNegative,  // fg
                         VerifyString,                          // str
                         VerifyIntegers32 | VerifyNotNegative,  // row
                         VerifyIntegers32 | VerifyNotNegative,  // col
                         VerifyScreen);                         // screen
    auto attrs_ptr = trx->m_op_ptr;
    auto bg_ptr = (attrs_ptr - 1);
    auto fg_ptr = (attrs_ptr - 2);
    auto str_ptr = (attrs_ptr - 3);
    auto row_ptr = (attrs_ptr - 4);
    auto col_ptr = (attrs_ptr - 5);
    auto screen_ptr = (attrs_ptr - 6);
    auto state = screen_state(trx, screen_ptr);

    auto [col_valid, col_val] = col_ptr->uinteger_value(trx);
    auto [row_valid, row_val] = row_ptr->uinteger_value(trx);
    if (!col_valid || !row_valid || (col_val > std::numeric_limits<length_t>::max()) ||
        (row_val > std::numeric_limits<length_t>::max())) {
        trx->error(Error::RangeCheck, "screen-put-string: col/row out of range");
    }
    auto col_start = static_cast<length_t>(col_val);
    auto row = static_cast<length_t>(row_val);
    if (row >= state->m_rows) {
        trx->error(Error::RangeCheck, "screen-put-string: row {} >= rows {}", row, state->m_rows);
    } else {
        auto fg = verify_color_arg(trx, fg_ptr, "screen-put-string", "fg");
        auto bg = verify_color_arg(trx, bg_ptr, "screen-put-string", "bg");
        auto attrs = verify_attrs_arg(trx, attrs_ptr, "screen-put-string");

        auto str_data = str_ptr->string_data_ptr(trx);
        auto str_len = str_ptr->m_string_length;
        auto cells = trx->offset_to_ptr<ScreenCell>(state->m_cells_offset);
        auto row_base = static_cast<vm_size_t>(row) * state->m_cols;
        for (length_t i = 0; (i < str_len) && ((col_start + i) < state->m_cols); ++i) {
            cells[row_base + col_start + i] = ScreenCell{static_cast<uint32_t>(str_data[i]), fg, bg, attrs, 0};
        }

        str_ptr->maybe_free_extvalue(trx);
        trx->m_op_ptr -= 6;
        // screen remains on stack
    }
}

// screen-fill-rect: screen x y w h ch fg bg attrs :- screen
// Fills a rectangle [x, x+w) x [y, y+h) with the given cell content.
// Out-of-bounds extents are clipped silently.
// throws: opstack-underflow, range-check, type-check
static void screen_fill_rect_op(Trix *trx) {
    trx->verify_operands(VerifyIntegers32 | VerifyNotNegative,  // attrs
                         VerifyIntegers32 | VerifyNotNegative,  // bg
                         VerifyIntegers32 | VerifyNotNegative,  // fg
                         VerifyIntegers32 | VerifyNotNegative,  // ch
                         VerifyIntegers32 | VerifyNotNegative,  // h
                         VerifyIntegers32 | VerifyNotNegative,  // w
                         VerifyIntegers32 | VerifyNotNegative,  // y
                         VerifyIntegers32 | VerifyNotNegative,  // x
                         VerifyScreen);                         // screen
    auto attrs_ptr = trx->m_op_ptr;
    auto bg_ptr = (attrs_ptr - 1);
    auto fg_ptr = (attrs_ptr - 2);
    auto ch_ptr = (attrs_ptr - 3);
    auto h_ptr = (attrs_ptr - 4);
    auto w_ptr = (attrs_ptr - 5);
    auto y_ptr = (attrs_ptr - 6);
    auto x_ptr = (attrs_ptr - 7);
    auto screen_ptr = (attrs_ptr - 8);
    auto state = screen_state(trx, screen_ptr);

    auto [x_valid, x_val] = x_ptr->uinteger_value(trx);
    auto [y_valid, y_val] = y_ptr->uinteger_value(trx);
    auto [w_valid, w_val] = w_ptr->uinteger_value(trx);
    auto [h_valid, h_val] = h_ptr->uinteger_value(trx);
    auto [ch_valid, ch_val] = ch_ptr->uinteger_value(trx);
    auto LMAX = static_cast<uinteger_t>(std::numeric_limits<length_t>::max());
    if (!x_valid || !y_valid || !w_valid || !h_valid || !ch_valid || (x_val > LMAX) || (y_val > LMAX) || (w_val > LMAX) ||
        (h_val > LMAX) || (ch_val > 0x10FFFF) || ((ch_val >= 0xD800) && (ch_val <= 0xDFFF))) {
        trx->error(Error::RangeCheck, "screen-fill-rect: rect or codepoint args out of range");
    }
    auto fg = verify_color_arg(trx, fg_ptr, "screen-fill-rect", "fg");
    auto bg = verify_color_arg(trx, bg_ptr, "screen-fill-rect", "bg");
    auto attrs = verify_attrs_arg(trx, attrs_ptr, "screen-fill-rect");

    auto x = static_cast<length_t>(x_val);
    auto y = static_cast<length_t>(y_val);
    auto w = static_cast<length_t>(w_val);
    auto h = static_cast<length_t>(h_val);
    // Clamp in vm_size_t: x + w can reach 2*65535, so narrowing the sum to length_t
    // BEFORE the min would wrap and silently drop/short the fill.  The min result is
    // <= m_cols/m_rows <= length_t max, so the final narrowing is safe.
    auto x_end = static_cast<length_t>(std::min(static_cast<vm_size_t>(x) + w, static_cast<vm_size_t>(state->m_cols)));
    auto y_end = static_cast<length_t>(std::min(static_cast<vm_size_t>(y) + h, static_cast<vm_size_t>(state->m_rows)));
    if ((x < state->m_cols) && (y < state->m_rows) && (x_end > x) && (y_end > y)) {
        auto cells = trx->offset_to_ptr<ScreenCell>(state->m_cells_offset);
        auto fill = ScreenCell{static_cast<uint32_t>(ch_val), fg, bg, attrs, 0};
        for (length_t r = y; r < y_end; ++r) {
            auto base = static_cast<vm_size_t>(r) * state->m_cols;
            std::fill_n(cells + base + x, static_cast<vm_size_t>(x_end - x), fill);
        }
    }

    trx->m_op_ptr -= 8;
    // screen remains on stack
}

//===--- Render diff -------------------------------------------------------===//
//
// The render path walks the cell grid in row-major order, comparing each
// cell against the previous render's snapshot.  For each cell that
// differs, we emit:
//   1. CSI cursor-position move if the cursor is not already at (row, col)
//   2. CSI SGR sequence if attrs/fg/bg differ from the last emitted state
//   3. UTF-8 bytes for the codepoint
// After the walk we copy cells -> prev and mark m_last_state_valid = 1.
//
// Two emit targets share the same diff loop via a templated emitter:
//   - screen-render          --> stack buffer flushed to STDOUT_FILENO
//                                via ::write() (no Stream involvement,
//                                bypasses m_stdout for max throughput)
//   - screen-render-to       --> Stream::putn() into a writable stream
//
// The leading sequence (clear-stale-state cursor reset) is emitted only on
// the first render of a Screen lifetime (m_last_state_valid == 0); from
// then on we trust the diff and emit nothing where nothing changed.

// UTF-8 encode a Unicode codepoint into 1..4 bytes.  Returns the byte count
// written into out (which must have at least 4 bytes of room).
[[nodiscard]] static size_t utf8_encode(uint32_t cp, vm_t *out) {
    if (cp < 0x80) {
        out[0] = static_cast<vm_t>(cp);
        return 1;
    } else if (cp < 0x800) {
        out[0] = static_cast<vm_t>(0xC0 | (cp >> 6));
        out[1] = static_cast<vm_t>(0x80 | (cp & 0x3F));
        return 2;
    } else {
        if (cp < 0x10000) {
            if ((cp >= 0xD800) && (cp <= 0xDFFF)) {
                // Surrogate halves are not encodable codepoints -- emitting the
                // naive 3-byte form would put invalid UTF-8 (CESU-8) on the wire.
                // The put-site validation rejects them; this is defense-in-depth
                // for any other path that lands one in a cell.
                out[0] = 0xEF;
                out[1] = 0xBF;
                out[2] = 0xBD;
                return 3;
            } else {
                out[0] = static_cast<vm_t>(0xE0 | (cp >> 12));
                out[1] = static_cast<vm_t>(0x80 | ((cp >> 6) & 0x3F));
                out[2] = static_cast<vm_t>(0x80 | (cp & 0x3F));
                return 3;
            }
        }
        if (cp < 0x110000) {
            out[0] = static_cast<vm_t>(0xF0 | (cp >> 18));
            out[1] = static_cast<vm_t>(0x80 | ((cp >> 12) & 0x3F));
            out[2] = static_cast<vm_t>(0x80 | ((cp >> 6) & 0x3F));
            out[3] = static_cast<vm_t>(0x80 | (cp & 0x3F));
            return 4;
        } else {
            // Out-of-range codepoint becomes U+FFFD REPLACEMENT CHARACTER.
            out[0] = 0xEF;
            out[1] = 0xBF;
            out[2] = 0xBD;
            return 3;
        }
    }
}

// Emit a non-negative decimal integer (0..65535) into out, returning bytes
// written.  Hand-rolled to avoid printf overhead inside the inner loop.
[[nodiscard]] static size_t emit_dec_uint(uint32_t n, vm_t *out) {
    if (n == 0) {
        out[0] = '0';
        return 1;
    } else {
        vm_t tmp[10];
        size_t len = 0;
        while (n > 0) {
            tmp[len++] = static_cast<vm_t>('0' + (n % 10));
            n /= 10;
        }
        for (size_t i = 0; i < len; ++i) {
            out[i] = tmp[len - 1 - i];
        }
        return len;
    }
}

// Diff-walk the screen and call emit_bytes(ptr, count) for each chunk of
// output bytes.  At end, copies cells -> prev and updates the SGR cache.
//
// SGR strategy: any change in (attrs, fg, bg) since the last emit forces a
// CSI 0 m reset followed by the full set of attrs + fg + bg.  This is
// straightforward and correct; per-attribute toggling could be added later
// for fewer bytes on attribute-heavy diffs.
template<typename Emit>
static void screen_render_emit(Trix *trx, ScreenState *state, Emit emit_bytes) {
    auto cells = trx->offset_to_ptr<ScreenCell>(state->m_cells_offset);
    auto prev = trx->offset_to_ptr<ScreenCell>(state->m_prev_offset);
    auto cols = state->m_cols;
    auto rows = state->m_rows;

    // Cursor tracking is per-render: always start with cursor unknown.
    // SGR state (m_last_attrs/fg/bg) persists across renders.
    bool cursor_known = false;
    length_t cursor_row = 0;
    length_t cursor_col = 0;

    auto last_attrs = state->m_last_attrs;
    auto last_fg = state->m_last_fg;
    auto last_bg = state->m_last_bg;
    auto sgr_known = (state->m_last_state_valid != 0);

    // Stack buffer for one cell's worth of emit bytes -- generous upper
    // bound: cursor move (up to 10 bytes) + SGR (up to ~40 bytes) + UTF-8 (4 bytes).
    std::array<vm_t, 64> buf;

    for (length_t row = 0; row < rows; ++row) {
        auto row_base = static_cast<vm_size_t>(row) * cols;
        for (length_t col = 0; col < cols; ++col) {
            auto idx = row_base + col;
            auto cur = cells[idx];
            auto pre = prev[idx];
            const bool same =
                    ((cur.m_ch == pre.m_ch) && (cur.m_fg == pre.m_fg) && (cur.m_bg == pre.m_bg) && (cur.m_attrs == pre.m_attrs));
            if (!same) {
                // Cursor move if needed.
                if (!cursor_known || (cursor_row != row) || (cursor_col != col)) {
                    size_t pos = 0;
                    buf[pos++] = 0x1B;
                    buf[pos++] = '[';
                    pos += emit_dec_uint(static_cast<uint32_t>(row + 1), buf.data() + pos);
                    buf[pos++] = ';';
                    pos += emit_dec_uint(static_cast<uint32_t>(col + 1), buf.data() + pos);
                    buf[pos++] = 'H';
                    emit_bytes(buf.data(), pos);
                    cursor_known = true;
                    cursor_row = row;
                    cursor_col = col;
                }

                // SGR diff.
                if (!sgr_known || (cur.m_attrs != last_attrs) || (cur.m_fg != last_fg) || (cur.m_bg != last_bg)) {
                    size_t pos = 0;
                    buf[pos++] = 0x1B;
                    buf[pos++] = '[';
                    buf[pos++] = '0';
                    if ((cur.m_attrs & 0x01) != 0) {
                        buf[pos++] = ';';
                        buf[pos++] = '1';
                    }
                    if ((cur.m_attrs & 0x02) != 0) {
                        buf[pos++] = ';';
                        buf[pos++] = '2';
                    }
                    if ((cur.m_attrs & 0x04) != 0) {
                        buf[pos++] = ';';
                        buf[pos++] = '3';
                    }
                    if ((cur.m_attrs & 0x08) != 0) {
                        buf[pos++] = ';';
                        buf[pos++] = '4';
                    }
                    if ((cur.m_attrs & 0x10) != 0) {
                        buf[pos++] = ';';
                        buf[pos++] = '5';
                    }
                    if ((cur.m_attrs & 0x20) != 0) {
                        buf[pos++] = ';';
                        buf[pos++] = '7';
                    }
                    if ((cur.m_attrs & 0x40) != 0) {
                        buf[pos++] = ';';
                        buf[pos++] = '9';
                    }
                    // Always emit fg/bg explicitly so the output is deterministic
                    // and independent of terminal default-color state.
                    buf[pos++] = ';';
                    buf[pos++] = '3';
                    buf[pos++] = '8';
                    buf[pos++] = ';';
                    buf[pos++] = '5';
                    buf[pos++] = ';';
                    pos += emit_dec_uint(cur.m_fg, buf.data() + pos);
                    buf[pos++] = ';';
                    buf[pos++] = '4';
                    buf[pos++] = '8';
                    buf[pos++] = ';';
                    buf[pos++] = '5';
                    buf[pos++] = ';';
                    pos += emit_dec_uint(cur.m_bg, buf.data() + pos);
                    buf[pos++] = 'm';
                    emit_bytes(buf.data(), pos);
                    last_attrs = cur.m_attrs;
                    last_fg = cur.m_fg;
                    last_bg = cur.m_bg;
                    sgr_known = true;
                }

                // Codepoint.
                std::array<vm_t, 4> cpbuf;
                auto n = utf8_encode(cur.m_ch, cpbuf.data());
                emit_bytes(cpbuf.data(), n);
                cursor_col = static_cast<length_t>(col + 1);
            }
        }
    }

    // Persist SGR cache + cells->prev snapshot.
    state->m_last_attrs = last_attrs;
    state->m_last_fg = last_fg;
    state->m_last_bg = last_bg;
    state->m_last_state_valid = 1;
    auto cell_count = static_cast<vm_size_t>(cols) * static_cast<vm_size_t>(rows);
    std::copy_n(cells, cell_count, prev);
}

// screen-render: screen :- screen
// Renders the screen's diff to STDOUT_FILENO via a dedicated fast path
// (4 KiB stack buffer + ::write()).  Sandbox-gated.  Flushes m_stdout
// first so any pending `print`/`=` bytes still in the stdout Stream
// buffer reach the terminal before the render bytes -- prior to this,
// mixing print + screen-render produced visible byte-order interleaving.
// throws: io-write-error, opstack-underflow, type-check, unsupported (sandbox)
static void screen_render_op(Trix *trx) {
    if (trx->m_sandbox) {
        trx->error(Error::Unsupported, "screen-render: disabled in sandbox mode");
    } else {
        trx->verify_operands(VerifyScreen);
        auto screen_ptr = trx->m_op_ptr;
        auto state = screen_state(trx, screen_ptr);

        // Flush m_stdout so prior print bytes don't arrive AFTER our direct
        // ::write output (the stream buffer's userspace write is independent
        // of STDOUT_FILENO, so without this flush the byte order on the
        // terminal is the order the kernel sees the writes -- not source
        // order).  Cheap when the buffer is empty.
        if (trx->m_stdout != nullptr) {
            trx->m_stdout->flush(trx);
        }

        // 4 KiB stack buffer: typical full repaint (~80x24 = 1920 cells, ~10 KiB
        // bytes) flushes 2-3 times; partial diffs usually fit in one flush.
        struct StdoutSink {
            std::array<vm_t, 4096> buf;
            size_t pos{0};
        } sink;

        // Drain n bytes to STDOUT_FILENO, blocking via poll() on EAGAIN.
        // raw-mode sets STDIN to O_NONBLOCK; on a typical interactive shell
        // STDIN and STDOUT share the same open file description so STDOUT
        // also becomes non-blocking, and a large render burst can fill the
        // kernel tty buffer faster than the terminal drains it.  EAGAIN
        // means "try again later" -- we want to wait, not surface as an
        // error, so poll() until the fd is writable and retry.
        auto drain_stdout = [trx](const vm_t *p, size_t n) {
            size_t off = 0;
            while (off < n) {
                auto written = ::write(STDOUT_FILENO, p + off, n - off);
                if (written > 0) {
                    off += static_cast<size_t>(written);
                } else if (written == 0) {
                    // No bytes accepted and no error -- treat as device full, stop draining.
                    break;
                } else if (errno == EINTR) {
                    // interrupted before any byte was written -- retry
                } else if (errno == EAGAIN) {
                    // fd not yet writable -- block until it is, then retry
                    pollfd pfd{STDOUT_FILENO, POLLOUT, 0};
                    static_cast<void>(::poll(&pfd, 1, -1));  // wait until writable
                } else {
                    trx->error(Error::IOWriteError, "screen-render: write failed: {}", trx->errno_string());
                }
            }
        };

        auto flush_sink = [&sink, &drain_stdout]() {
            if (sink.pos != 0) {
                drain_stdout(sink.buf.data(), sink.pos);
                sink.pos = 0;
            }
        };

        auto emit_bytes = [&sink, &flush_sink, &drain_stdout](const vm_t *p, size_t n) {
            // For chunks larger than the buffer (rare: a single SGR > 4 KiB),
            // flush whatever is staged then write the chunk directly.
            if (n > sink.buf.size()) {
                flush_sink();
                drain_stdout(p, n);
            } else {
                if ((sink.pos + n) > sink.buf.size()) {
                    flush_sink();
                }
                std::copy_n(p, n, sink.buf.data() + sink.pos);
                sink.pos += n;
            }
        };

        screen_render_emit(trx, state, emit_bytes);
        flush_sink();
        // screen handle stays on stack; no edit to *screen_ptr needed.
    }
}

// screen-render-to: screen stream :- screen
// Renders the screen's diff to a writable stream.  The stream must be
// open and writable (raises /type-check otherwise).  Useful for byte-
// level testing via make-string-stream and for redirecting screen
// output to a file or pipe.
// throws: invalid-stream, io-write-error, opstack-underflow, type-check
static void screen_render_to_op(Trix *trx) {
    trx->verify_operands(VerifyStream | VerifyRW, VerifyScreen);
    auto stream_ptr = trx->m_op_ptr;
    auto screen_ptr = (stream_ptr - 1);
    auto state = screen_state(trx, screen_ptr);
    auto [stream, sid] = stream_ptr->stream_value(trx);
    if (!stream->is_open(sid)) {
        trx->error(Error::InvalidStream, "screen-render-to: stream not open");
    } else if (!stream->is_writable(sid)) {
        trx->error(Error::TypeCheck, "screen-render-to: stream not writable");
    } else {
        auto emit_bytes = [stream, trx](const vm_t *p, size_t n) { stream->putn(trx, p, n); };
        screen_render_emit(trx, state, emit_bytes);

        --trx->m_op_ptr;
        // screen handle remains on stack
    }
}

// screen-get-cell: screen col row :- ch fg bg attrs
// Reads one cell at (col, row).  Pops screen+col+row, pushes 4 Integers
// (codepoint, fg, bg, attrs).  Out-of-range col/row raises /range-check.
// Companion to screen-put-cell -- enables blit, region copy, debugging,
// and any composition pattern that needs to read screen state.
// throws: opstack-underflow, range-check, type-check
static void screen_get_cell_op(Trix *trx) {
    trx->verify_operands(VerifyIntegers32 | VerifyNotNegative,  // row
                         VerifyIntegers32 | VerifyNotNegative,  // col
                         VerifyScreen);                         // screen
    auto row_ptr = trx->m_op_ptr;
    auto col_ptr = (row_ptr - 1);
    auto screen_ptr = (row_ptr - 2);
    auto state = screen_state(trx, screen_ptr);

    auto [col_valid, col_val] = col_ptr->uinteger_value(trx);
    auto [row_valid, row_val] = row_ptr->uinteger_value(trx);
    if (!col_valid || !row_valid || (col_val >= state->m_cols) || (row_val >= state->m_rows)) {
        trx->error(Error::RangeCheck,
                   "screen-get-cell: ({}, {}) out of {}x{} screen",
                   col_valid ? static_cast<integer_t>(col_val) : -1,
                   row_valid ? static_cast<integer_t>(row_val) : -1,
                   state->m_cols,
                   state->m_rows);
    }

    auto cells = trx->offset_to_ptr<ScreenCell>(state->m_cells_offset);
    auto cell = cells[static_cast<vm_size_t>(row_val) * state->m_cols + static_cast<vm_size_t>(col_val)];

    trx->require_op_capacity(1);  // pop 3, push 4 -> net +1
    *screen_ptr = Object::make_integer(static_cast<integer_t>(cell.m_ch));
    *col_ptr = Object::make_integer(static_cast<integer_t>(cell.m_fg));
    *row_ptr = Object::make_integer(static_cast<integer_t>(cell.m_bg));
    *++trx->m_op_ptr = Object::make_integer(static_cast<integer_t>(cell.m_attrs));
}

// screen?: any :- bool
// True iff the top operand is an OpaqueHandle of kind Screen.
// throws: opstack-underflow
static void is_screen_op(Trix *trx) {
    trx->require_op_count(1);
    auto top_ptr = trx->m_op_ptr;
    auto result = top_ptr->is_screen();
    top_ptr->maybe_free_extvalue(trx);
    *top_ptr = Object::make_boolean(result);
}

// handle-kind: any :- name | null
// Returns the kind of an OpaqueHandle as a Name (e.g. /screen), or null
// for non-handle types.  Closes the introspection gap that previously
// required `{ scr screen-cols pop } try /no-error eq`-style probes.
// throws: opstack-underflow
static void handle_kind_op(Trix *trx) {
    trx->require_op_count(1);
    auto top_ptr = trx->m_op_ptr;
    if (!top_ptr->is_handle()) {
        top_ptr->maybe_free_extvalue(trx);
        *top_ptr = Object::make_null();
    } else {
        auto kind = top_ptr->handle_kind();
        std::string_view kind_sv;
        switch (kind) {
        case Object::HandleKind::Screen:
            kind_sv = "screen";
            break;
        }
        auto name_offset = Name::add(trx, kind_sv);
        *top_ptr = Object::make_name(name_offset, static_cast<length_t>(kind_sv.size()));
    }
}

// Decode the next UTF-8 codepoint starting at `p` (length `n` total bytes).
// Returns the decoded codepoint and the number of bytes consumed.  An ill-
// formed sequence consumes one byte and returns U+FFFD so the caller can
// continue scanning the rest of the input rather than abort.
struct Utf8Decode {
    uint32_t codepoint;
    size_t consumed;
};
[[nodiscard]] static Utf8Decode utf8_decode(const vm_t *p, size_t n) {
    static constexpr uint32_t REPLACEMENT = 0xFFFDu;
    if (n == 0) {
        return {REPLACEMENT, 0};
    } else {
        auto b0 = static_cast<uint8_t>(p[0]);
        if (b0 < 0x80) {
            return {b0, 1};
        } else {
            if ((b0 & 0xE0) == 0xC0) {
                if ((n < 2) || ((static_cast<uint8_t>(p[1]) & 0xC0) != 0x80)) {
                    return {REPLACEMENT, 1};
                } else {
                    const uint32_t cp = (static_cast<uint32_t>(b0 & 0x1F) << 6) | static_cast<uint32_t>(p[1] & 0x3F);
                    return ((cp < 0x80) ? Utf8Decode{REPLACEMENT, 1} : Utf8Decode{cp, 2});  // overlong
                }
            }
            if ((b0 & 0xF0) == 0xE0) {
                if ((n < 3) || ((static_cast<uint8_t>(p[1]) & 0xC0) != 0x80) || ((static_cast<uint8_t>(p[2]) & 0xC0) != 0x80)) {
                    return {REPLACEMENT, 1};
                } else {
                    const uint32_t cp = (static_cast<uint32_t>(b0 & 0x0F) << 12) | (static_cast<uint32_t>(p[1] & 0x3F) << 6) |
                                        static_cast<uint32_t>(p[2] & 0x3F);
                    if ((cp < 0x800) || ((cp >= 0xD800) && (cp <= 0xDFFF))) {  // overlong or surrogate
                        return {REPLACEMENT, 1};
                    } else {
                        return {cp, 3};
                    }
                }
            }
            if ((b0 & 0xF8) == 0xF0) {
                if ((n < 4) || ((static_cast<uint8_t>(p[1]) & 0xC0) != 0x80) || ((static_cast<uint8_t>(p[2]) & 0xC0) != 0x80) ||
                    ((static_cast<uint8_t>(p[3]) & 0xC0) != 0x80)) {
                    return {REPLACEMENT, 1};
                }
                const uint32_t cp = (static_cast<uint32_t>(b0 & 0x07) << 18) | (static_cast<uint32_t>(p[1] & 0x3F) << 12) |
                                    (static_cast<uint32_t>(p[2] & 0x3F) << 6) | static_cast<uint32_t>(p[3] & 0x3F);
                if ((cp < 0x10000) || (cp > 0x10FFFF)) {  // overlong or out-of-range
                    return {REPLACEMENT, 1};
                } else {
                    return {cp, 4};
                }
            }
            return {REPLACEMENT, 1};
        }
    }
}

// screen-put-utf8-string: screen col row str fg bg attrs :- screen
// Like screen-put-string, but interprets `str` as UTF-8 and writes one
// cell per decoded codepoint (instead of one cell per byte).  Ill-formed
// sequences become U+FFFD and consume one byte.  Off-screen cells are
// silently truncated at the row's right edge.
// throws: opstack-underflow, range-check, type-check
static void screen_put_utf8_string_op(Trix *trx) {
    trx->verify_operands(VerifyIntegers32 | VerifyNotNegative,  // attrs
                         VerifyIntegers32 | VerifyNotNegative,  // bg
                         VerifyIntegers32 | VerifyNotNegative,  // fg
                         VerifyString,                          // str
                         VerifyIntegers32 | VerifyNotNegative,  // row
                         VerifyIntegers32 | VerifyNotNegative,  // col
                         VerifyScreen);                         // screen
    auto attrs_ptr = trx->m_op_ptr;
    auto bg_ptr = (attrs_ptr - 1);
    auto fg_ptr = (attrs_ptr - 2);
    auto str_ptr = (attrs_ptr - 3);
    auto row_ptr = (attrs_ptr - 4);
    auto col_ptr = (attrs_ptr - 5);
    auto screen_ptr = (attrs_ptr - 6);
    auto state = screen_state(trx, screen_ptr);

    auto [col_valid, col_val] = col_ptr->uinteger_value(trx);
    auto [row_valid, row_val] = row_ptr->uinteger_value(trx);
    if (!col_valid || !row_valid || (col_val > std::numeric_limits<length_t>::max()) ||
        (row_val > std::numeric_limits<length_t>::max())) {
        trx->error(Error::RangeCheck, "screen-put-utf8-string: col/row out of range");
    }
    auto col_start = static_cast<length_t>(col_val);
    auto row = static_cast<length_t>(row_val);
    if (row >= state->m_rows) {
        trx->error(Error::RangeCheck, "screen-put-utf8-string: row {} >= rows {}", row, state->m_rows);
    } else {
        auto fg = verify_color_arg(trx, fg_ptr, "screen-put-utf8-string", "fg");
        auto bg = verify_color_arg(trx, bg_ptr, "screen-put-utf8-string", "bg");
        auto attrs = verify_attrs_arg(trx, attrs_ptr, "screen-put-utf8-string");

        auto str_data = str_ptr->string_data_ptr(trx);
        auto str_len = static_cast<size_t>(str_ptr->m_string_length);
        auto cells = trx->offset_to_ptr<ScreenCell>(state->m_cells_offset);
        auto row_base = static_cast<vm_size_t>(row) * state->m_cols;
        auto col = col_start;
        size_t i = 0;
        while ((i < str_len) && (col < state->m_cols)) {
            auto [cp, consumed] = utf8_decode(str_data + i, str_len - i);
            cells[row_base + col] = ScreenCell{cp, fg, bg, attrs, 0};
            i += consumed;
            ++col;
        }

        str_ptr->maybe_free_extvalue(trx);
        trx->m_op_ptr -= 6;
        // screen remains on stack
    }
}

// screen-blit: src sx sy w h dst dx dy :- dst
// Copies a rectangular cell region from `src` at (sx, sy) of dimensions
// w x h to `dst` at (dx, dy).  All coords/dims clip to the intersection
// of both screens; out-of-bounds regions are silently dropped (no error).
// `src` and `dst` may be the same screen; in that case the copy direction
// is chosen so overlapping regions don't overwrite themselves.  Both
// handles must be Screen kind; numeric args must fit in length_t.
// throws: opstack-underflow, range-check, type-check
static void screen_blit_op(Trix *trx) {
    trx->verify_operands(VerifyIntegers32 | VerifyNotNegative,  // dy
                         VerifyIntegers32 | VerifyNotNegative,  // dx
                         VerifyScreen,                          // dst
                         VerifyIntegers32 | VerifyNotNegative,  // h
                         VerifyIntegers32 | VerifyNotNegative,  // w
                         VerifyIntegers32 | VerifyNotNegative,  // sy
                         VerifyIntegers32 | VerifyNotNegative,  // sx
                         VerifyScreen);                         // src
    auto dy_ptr = trx->m_op_ptr;
    auto dx_ptr = (dy_ptr - 1);
    auto dst_ptr = (dy_ptr - 2);
    auto h_ptr = (dy_ptr - 3);
    auto w_ptr = (dy_ptr - 4);
    auto sy_ptr = (dy_ptr - 5);
    auto sx_ptr = (dy_ptr - 6);
    auto src_ptr = (dy_ptr - 7);

    auto src_state = screen_state(trx, src_ptr);
    auto dst_state = screen_state(trx, dst_ptr);

    auto extract = [&](const Object *p) -> length_t {
        auto [valid, val] = p->uinteger_value(trx);
        if (!valid || (val > std::numeric_limits<length_t>::max())) {
            trx->error(Error::RangeCheck, "screen-blit: coordinate/dim out of length_t range");
        } else {
            return static_cast<length_t>(val);
        }
    };
    auto sx = extract(sx_ptr);
    auto sy = extract(sy_ptr);
    auto w = extract(w_ptr);
    auto h = extract(h_ptr);
    auto dx = extract(dx_ptr);
    auto dy = extract(dy_ptr);

    // Clip src rect to src dimensions.
    if (sx >= src_state->m_cols) {
        w = 0;
    } else if ((sx + w) > src_state->m_cols) {
        w = static_cast<length_t>(src_state->m_cols - sx);
    }
    if (sy >= src_state->m_rows) {
        h = 0;
    } else if ((sy + h) > src_state->m_rows) {
        h = static_cast<length_t>(src_state->m_rows - sy);
    }
    // Clip dst rect to dst dimensions (further reducing w/h).
    if (dx >= dst_state->m_cols) {
        w = 0;
    } else if ((dx + w) > dst_state->m_cols) {
        w = static_cast<length_t>(dst_state->m_cols - dx);
    }
    if (dy >= dst_state->m_rows) {
        h = 0;
    } else if ((dy + h) > dst_state->m_rows) {
        h = static_cast<length_t>(dst_state->m_rows - dy);
    }

    if ((w > 0) && (h > 0)) {
        auto src_cells = trx->offset_to_ptr<ScreenCell>(src_state->m_cells_offset);
        auto dst_cells = trx->offset_to_ptr<ScreenCell>(dst_state->m_cells_offset);
        // Same-screen overlap: pick row direction to avoid stepping on
        // unread source rows.  Within a row, std::copy_backward handles
        // the column overlap when sx < dx (moving right within same row).
        const bool same = (src_state == dst_state);
        const bool reverse_rows = same && (dy > sy);
        for (length_t row = 0; row < h; ++row) {
            auto r = reverse_rows ? (h - 1 - row) : row;
            auto src_row = src_cells + static_cast<vm_size_t>(sy + r) * src_state->m_cols + sx;
            auto dst_row = dst_cells + static_cast<vm_size_t>(dy + r) * dst_state->m_cols + dx;
            if (same && ((sy + r) == (dy + r)) && (dx > sx)) {
                std::copy_backward(src_row, src_row + w, dst_row + w);
            } else {
                std::copy_n(src_row, w, dst_row);
            }
        }
    }

    // Pop sx, sy, w, h, dx, dy, src; leave dst on top.
    *src_ptr = *dst_ptr;
    trx->m_op_ptr -= 7;
}

// screen-park-cursor: col row :- --
// Emits a CSI cursor-position sequence to STDOUT_FILENO via direct
// ::write, parking the terminal cursor at (col, row) in 0-indexed
// screen coordinates (CSI is 1-indexed; we add 1 internally).  Pairs
// with screen-render to give TUIs a predictable cursor location after
// a render -- otherwise the cursor lands at the rightmost dirty cell
// and produces visible flicker when the program subsequently calls
// `print` or another screen-render.  Sandbox-gated like screen-render.
// throws: io-write-error, opstack-underflow, range-check, type-check, unsupported (sandbox)
static void screen_park_cursor_op(Trix *trx) {
    if (trx->m_sandbox) {
        trx->error(Error::Unsupported, "screen-park-cursor: disabled in sandbox mode");
    } else {
        trx->verify_operands(VerifyIntegers32 | VerifyNotNegative,   // row
                             VerifyIntegers32 | VerifyNotNegative);  // col
        auto row_ptr = trx->m_op_ptr;
        auto col_ptr = (row_ptr - 1);
        auto [col_valid, col_val] = col_ptr->uinteger_value(trx);
        auto [row_valid, row_val] = row_ptr->uinteger_value(trx);
        if (!col_valid || !row_valid || (col_val >= std::numeric_limits<uint32_t>::max()) ||
            (row_val >= std::numeric_limits<uint32_t>::max())) {
            trx->error(Error::RangeCheck, "screen-park-cursor: col/row out of range");
        }

        // Flush m_stdout for the same reason screen-render does -- prior
        // print bytes must reach the terminal before our direct write.
        if (trx->m_stdout != nullptr) {
            trx->m_stdout->flush(trx);
        }

        char buf[32];
        auto [ptr, _] = std::format_to_n(buf, sizeof(buf), "\x1b[{};{}H", row_val + 1, col_val + 1);
        auto len = static_cast<size_t>(ptr - buf);
        size_t off = 0;
        while (off < len) {
            auto written = ::write(STDOUT_FILENO, buf + off, len - off);
            if (written > 0) {
                off += static_cast<size_t>(written);
            } else if (written == 0) {
                // No bytes accepted and no error -- treat as device full, stop.
                break;
            } else if (errno == EINTR) {
                // interrupted before any byte was written -- retry
            } else if (errno == EAGAIN) {
                // raw-mode set STDIN non-blocking, which on shared open file
                // description makes STDOUT non-blocking too -- poll for
                // writability rather than surfacing EAGAIN as an error.
                pollfd pfd{STDOUT_FILENO, POLLOUT, 0};
                static_cast<void>(::poll(&pfd, 1, -1));
            } else {
                trx->error(Error::IOWriteError, "screen-park-cursor: write failed: {}", trx->errno_string());
            }
        }

        trx->m_op_ptr -= 2;
    }
}
