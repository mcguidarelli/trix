//===----------------------------------------------------------------------===//
//                                                                            //
//          __    _       ____                                                //
//    _____/ /_  (_)___  ( __ )                                               //
//   / ___/ __ \/ / __ \/ __  |                                               //
//  / /__/ / / / / /_/ / /_/ /                                                //
//  \___/_/ /_/_/ .___/\____/                                                 //
//             /_/                                                            //
//                                                                            //
//===----------------------------------------------------------------------===//

// chip8.cpp -- dedicated binary for examples/chip8.trx with a native CPU
// kernel, following the tetrix.cpp host-integration pattern.
//
// Same Trix runtime as ./trix, but its user_ops[] table registers ONE C++
// kernel that replaces the fetch/decode/execute hot loop of chip8.trx:
//
//   chip8-step-fast    mem v fb stack state n :- consumed reason
//
// The Trix-level dispatch in examples/chip8.trx stays the REFERENCE
// implementation and is selected at runtime via --cpu-kernel=trix
// (default --cpu-kernel=native in this binary; plain ./trix has no
// native kernel and always runs the reference).  The script's
// --self-test gains a lockstep section under this binary: the same ROMs
// run through both implementations from identical seeds and the
// complete machine state must match.
//
// Operand contract (chip8.trx invariants; all hard-validated):
//   mem    : String length 4096, ReadWrite  (CHIP-8 memory)
//   v      : String length 16,   ReadWrite  (V0..VF)
//   fb     : String length 8192, ReadWrite  (byte-per-pixel framebuffer)
//   stack  : Array  length 16,   ReadWrite  (call stack of Integer PCs)
//   state  : Array  length 9,    ReadWrite  (packed Integer scalars):
//              [0]=PC [1]=I [2]=DT [3]=ST [4]=SP [5]=keys
//              [6]=hi-res? (0/1)  [7]=dirty-out (0/1)  [8]=bell-out (0/1)
//   n      : Integer >= 1, instruction budget
//
// Returns (consumed, reason); reason is an Integer code:
//   0 = ok        budget exhausted
//   1 = defer     the NEXT instruction needs the Trix reference path;
//                 PC is left pointing AT it, `consumed` counts only the
//                 completed instructions.  Deferred set: 00FD EXIT,
//                 00FE/00FF mode switches (they resize the screen),
//                 FX75/FX85 (SCHIP RPL), unknown opcodes (reference
//                 raises/halts with its own diagnostics), CALL with a
//                 full stack / RET with an empty one (reference raises
//                 its themed errors), and any instruction whose memory
//                 access would land out of range (reference raises
//                 /range-check from the same instruction).
//   2 = key-wait  FX0A with no key down; PC has been rewound by 2
//                 (matching the reference), one instruction consumed.
//                 The caller sleeps a tick, exactly like the reference
//                 cadence of one FX0A retry per CPU tick.
//
// Mutation policy mirrors tetrix.cpp: string bytes are raw (string put
// is never journaled by design -- trix-reference s7.7) and the stack /
// state array cells are written WITHOUT journaling; both arrays are
// caller-owned scratch whose pre-call values are never restored to.
//
// Behavioral notes (deliberate, documented):
//   * CLS / scrolls clear and shift the framebuffer IN PLACE where the
//     reference rebinds /FB to a fresh buffer.  Byte content after the
//     operation is identical; only object identity differs, and nothing
//     in chip8.trx depends on FB identity across an instruction.
//   * FX18's terminal bell is reported via state[8] (one-shot per call)
//     rather than printed here; multiple 0->ST transitions inside one
//     burst ring once.  The reference rings per transition per tick --
//     at one instruction per tick the cadences are identical.
//   * Cxnn draws from the SAME engine PCG32 stream as the
//     rand-bounded-uinteger operator (via the rand_bounded_uint32
//     host-kernel API), so seeded runs are bit-identical across
//     kernels -- this is what makes the lockstep self-test possible.

#include <bit>
#include <cstring>
#include <vector>

#include <unistd.h>

#include "trix.h"

// Same alias set the library itself re-exports -- see the trix_details
// block at the tail of trix.h.
using length_t = Trix::length_t;
using save_level_t = Trix::save_level_t;
using integer_t = Trix::integer_t;
using uinteger_t = Trix::uinteger_t;
using long_t = Trix::long_t;
using vm_t = Trix::vm_t;
using Object = Trix::Object;
using Save = Trix::Save;
using Operator = Trix::Operator;
using Error = Trix::Error;
using StartupMode = Trix::StartupMode;
constexpr auto DefaultStreamCount = Trix::DefaultStreamCount;
constexpr auto VerifyArray = Trix::VerifyArray;
constexpr auto VerifyInteger = Trix::VerifyInteger;
constexpr auto VerifyString = Trix::VerifyString;

static constexpr length_t MEM_SIZE = 4096;
static constexpr length_t VREG_COUNT = 16;
static constexpr length_t FB_SIZE = 8192;
static constexpr length_t STACK_SLOTS = 16;
static constexpr length_t STATE_SLOTS = 9;

// state[] slot indices
static constexpr length_t ST_PC = 0;
static constexpr length_t ST_I = 1;
static constexpr length_t ST_DT = 2;
static constexpr length_t ST_ST = 3;
static constexpr length_t ST_SP = 4;
static constexpr length_t ST_KEYS = 5;
static constexpr length_t ST_HI = 6;
static constexpr length_t ST_DIRTY = 7;
static constexpr length_t ST_BELL = 8;

// reason codes
static constexpr integer_t REASON_OK = 0;
static constexpr integer_t REASON_DEFER = 1;
static constexpr integer_t REASON_KEYWAIT = 2;

// Validate a ReadWrite String operand of exactly `len` bytes; returns the
// mutable byte pointer.
static vm_t *chip8_validate_string(Trix *trx, Object *str_ptr, length_t len, const char *what, const char *op_name) {
    if (!str_ptr->is_string()) {
        trx->error(Error::TypeCheck, "'{}': {} is not a String", op_name, what);
    } else if (str_ptr->string_length() != len) {
        trx->error(Error::RangeCheck, "'{}': {} length {} != {}", op_name, what, str_ptr->string_length(), len);
    } else if (!str_ptr->has_write_access()) {
        trx->error(Error::InvalidAccess, "'{}': {} is not ReadWrite", op_name, what);
    } else {
        return str_ptr->string_data_ptr(trx);
    }
}

// Validate a ReadWrite Array operand of exactly `len` slots; returns the
// Object pointer.
static Object *chip8_validate_array(Trix *trx, Object *arr_ptr, length_t len, const char *what, const char *op_name) {
    if (!arr_ptr->is_array()) {
        trx->error(Error::TypeCheck, "'{}': {} is not a plain Array", op_name, what);
    } else if (arr_ptr->arrays_length() != len) {
        trx->error(Error::RangeCheck, "'{}': {} length {} != {}", op_name, what, arr_ptr->arrays_length(), len);
    } else if (!arr_ptr->has_write_access()) {
        trx->error(Error::InvalidAccess, "'{}': {} is not ReadWrite", op_name, what);
    } else {
        return arr_ptr->array_objects(trx);
    }
}

// Read an Integer slot from a state/stack array; /type-check otherwise.
static integer_t chip8_slot_int(Trix *trx, Object slot, length_t i, const char *what, const char *op_name) {
    if (!slot.is_integer()) {
        trx->error(Error::TypeCheck, "'{}': {}[{}] is not Integer", op_name, what, i);
    } else {
        return slot.integer_value();
    }
}

// Write an Integer slot without journaling (caller-owned scratch; see the
// header comment and the matching rationale in tetrix.cpp).
static void chip8_set_slot(Object *target, integer_t value, save_level_t curr_save_level) {
    *target = Object::make_integer(value);
    target->set_save_level(curr_save_level);
}

// chip8-step-fast: mem v fb stack state n :- consumed reason
static void chip8_step_fast_op(Trix *trx) {
    trx->verify_operands(VerifyInteger, VerifyArray, VerifyArray, VerifyString, VerifyString, VerifyString);

    auto n_ptr = trx->m_op_ptr;
    auto state_ptr = (n_ptr - 1);
    auto stack_ptr = (n_ptr - 2);
    auto fb_ptr = (n_ptr - 3);
    auto v_ptr = (n_ptr - 4);
    auto mem_ptr = (n_ptr - 5);

    auto budget = n_ptr->integer_value();
    if (budget < 1) {
        trx->error(Error::RangeCheck, "'chip8-step-fast': budget {} < 1", budget);
    } else {
        auto mem = chip8_validate_string(trx, mem_ptr, MEM_SIZE, "mem", "chip8-step-fast");
        auto v = chip8_validate_string(trx, v_ptr, VREG_COUNT, "v", "chip8-step-fast");
        auto fb = chip8_validate_string(trx, fb_ptr, FB_SIZE, "fb", "chip8-step-fast");
        auto stk = chip8_validate_array(trx, stack_ptr, STACK_SLOTS, "stack", "chip8-step-fast");
        auto st = chip8_validate_array(trx, state_ptr, STATE_SLOTS, "state", "chip8-step-fast");

        auto pc = chip8_slot_int(trx, st[ST_PC], ST_PC, "state", "chip8-step-fast");
        auto i_reg = chip8_slot_int(trx, st[ST_I], ST_I, "state", "chip8-step-fast");
        auto dt = chip8_slot_int(trx, st[ST_DT], ST_DT, "state", "chip8-step-fast");
        auto sound = chip8_slot_int(trx, st[ST_ST], ST_ST, "state", "chip8-step-fast");
        auto sp = chip8_slot_int(trx, st[ST_SP], ST_SP, "state", "chip8-step-fast");
        auto keys = chip8_slot_int(trx, st[ST_KEYS], ST_KEYS, "state", "chip8-step-fast");
        auto hi_res = chip8_slot_int(trx, st[ST_HI], ST_HI, "state", "chip8-step-fast");
        integer_t dirty = 0;
        integer_t bell = 0;

        auto fb_cols = (hi_res != 0) ? static_cast<integer_t>(128) : static_cast<integer_t>(64);
        auto fb_rows = (hi_res != 0) ? static_cast<integer_t>(64) : static_cast<integer_t>(32);

        auto curr_save_level = Save::current_level(trx);

        integer_t consumed = 0;
        integer_t reason = REASON_OK;

        while (consumed < budget) {
            // Fetch bounds: the reference read-word raises /range-check past
            // 4095; defer so it raises from the same PC.
            if ((pc < 0) || ((pc + 1) >= static_cast<integer_t>(MEM_SIZE))) {
                reason = REASON_DEFER;
                break;
            } else {
                auto op = static_cast<uinteger_t>((static_cast<uinteger_t>(mem[pc]) << 8) | mem[pc + 1]);
                auto n0 = static_cast<integer_t>((op >> 12) & 0xF);
                auto x = static_cast<integer_t>((op >> 8) & 0xF);
                auto y = static_cast<integer_t>((op >> 4) & 0xF);
                auto nyb = static_cast<integer_t>(op & 0xF);
                auto nn = static_cast<integer_t>(op & 0xFF);
                auto nnn = static_cast<integer_t>(op & 0xFFF);

                // --- defer screening (no side effects before this block clears) ---
                bool defer = false;
                switch (n0) {
                case 0x0:
                    if ((op == 0x00FD) || (op == 0x00FE) || (op == 0x00FF)) {
                        defer = true;  // EXIT / LOW / HIGH (halt + screen resize)
                    } else if ((op == 0x00EE) && (sp <= 0)) {
                        defer = true;  // RET underflow: themed error in reference
                    }
                    break;

                case 0x2:
                    if (sp >= static_cast<integer_t>(STACK_SLOTS)) {
                        defer = true;  // CALL overflow: themed error in reference
                    }
                    break;

                case 0x8:
                    if ((nyb > 7) && (nyb != 0xE)) {
                        defer = true;  // unknown ALU op -> reference halts
                    }
                    break;

                case 0xD: {
                    auto rows = ((nyb == 0) && (hi_res != 0)) ? static_cast<integer_t>(32) : nyb;
                    auto bytes = ((nyb == 0) && (hi_res != 0)) ? static_cast<integer_t>(32) : nyb;
                    (void)rows;
                    if ((i_reg < 0) || ((i_reg + bytes) > static_cast<integer_t>(MEM_SIZE))) {
                        defer = true;  // sprite read out of range -> /range-check
                    }
                    break;
                }

                case 0xE:
                    if (((nn != 0x9E) && (nn != 0xA1)) || (static_cast<integer_t>(v[x]) > 15)) {
                        defer = true;  // unknown E-op, or shift-by->15 semantics
                    }
                    break;

                case 0xF:
                    switch (nn) {
                    case 0x07:
                    case 0x0A:
                    case 0x15:
                    case 0x18:
                    case 0x1E:
                    case 0x29:
                    case 0x30:
                        break;

                    case 0x33:
                        if ((i_reg < 0) || ((i_reg + 3) > static_cast<integer_t>(MEM_SIZE))) {
                            defer = true;
                        }
                        break;

                    case 0x55:
                    case 0x65:
                        if ((i_reg < 0) || ((i_reg + x + 1) > static_cast<integer_t>(MEM_SIZE))) {
                            defer = true;
                        }
                        break;

                    default:
                        defer = true;  // FX75/FX85 (RPL) and unknown F-ops
                        break;
                    }
                    break;

                default:
                    break;
                }
                if (defer) {
                    reason = REASON_DEFER;
                    break;
                } else {
                    // --- execute (mirrors examples/chip8.trx exec-* dispatch) ---
                    pc += 2;
                    switch (n0) {
                    case 0x0:
                        if (op == 0x00E0) {  // CLS
                            std::memset(fb, 0, FB_SIZE);
                            dirty = 1;
                        } else if (op == 0x00EE) {  // RET
                            --sp;
                            pc = chip8_slot_int(trx, stk[sp], static_cast<length_t>(sp), "stack", "chip8-step-fast");
                        } else if (op == 0x00FB) {  // SCR: right by 4
                            for (integer_t row = 0; row < fb_rows; ++row) {
                                auto base = fb + (row * fb_cols);
                                std::memmove(base + 4, base, static_cast<size_t>(fb_cols - 4));
                                std::memset(base, 0, 4);
                            }
                            dirty = 1;
                        } else if (op == 0x00FC) {  // SCL: left by 4
                            for (integer_t row = 0; row < fb_rows; ++row) {
                                auto base = fb + (row * fb_cols);
                                std::memmove(base, base + 4, static_cast<size_t>(fb_cols - 4));
                                std::memset(base + (fb_cols - 4), 0, 4);
                            }
                            dirty = 1;
                        } else if ((op & 0xFFF0u) == 0x00C0u) {  // SCD N
                            if (nyb >= fb_rows) {
                                // reference copies nothing into the fresh buffer
                                std::memset(fb, 0, FB_SIZE);
                            } else if (nyb > 0) {
                                std::memmove(fb + (nyb * fb_cols), fb, static_cast<size_t>((fb_rows - nyb) * fb_cols));
                                std::memset(fb, 0, static_cast<size_t>(nyb * fb_cols));
                            }
                            dirty = 1;
                        }
                        // anything else in 0x0NNN: SYS -- no-op (matches reference)
                        break;

                    case 0x1:  // JP nnn
                        pc = nnn;
                        break;

                    case 0x2:  // CALL nnn
                        chip8_set_slot(&stk[sp], pc, curr_save_level);
                        ++sp;
                        pc = nnn;
                        break;

                    case 0x3:  // SE Vx, nn
                        if (static_cast<integer_t>(v[x]) == nn) {
                            pc += 2;
                        }
                        break;

                    case 0x4:  // SNE Vx, nn
                        if (static_cast<integer_t>(v[x]) != nn) {
                            pc += 2;
                        }
                        break;

                    case 0x5:  // SE Vx, Vy (only when low nibble 0 -- else no-op)
                        if ((nyb == 0) && (v[x] == v[y])) {
                            pc += 2;
                        }
                        break;

                    case 0x6:  // LD Vx, nn
                        v[x] = static_cast<vm_t>(nn);
                        break;

                    case 0x7:  // ADD Vx, nn (no carry)
                        v[x] = static_cast<vm_t>((static_cast<integer_t>(v[x]) + nn) & 0xFF);
                        break;

                    case 0x8: {
                        auto vx = static_cast<integer_t>(v[x]);
                        auto vy = static_cast<integer_t>(v[y]);
                        switch (nyb) {
                        case 0x0:
                            v[x] = static_cast<vm_t>(vy);
                            break;

                        case 0x1:
                            v[x] = static_cast<vm_t>(vx | vy);
                            break;

                        case 0x2:
                            v[x] = static_cast<vm_t>(vx & vy);
                            break;

                        case 0x3:
                            v[x] = static_cast<vm_t>(vx ^ vy);
                            break;

                        case 0x4: {  // ADD with carry; flag write AFTER result (ref order)
                            auto sum = vx + vy;
                            v[x] = static_cast<vm_t>(sum & 0xFF);
                            v[15] = (sum >= 0x100) ? 1 : 0;
                            break;
                        }

                        case 0x5:  // SUB; VF = NOT borrow
                            v[x] = static_cast<vm_t>((vx - vy) & 0xFF);
                            v[15] = (vx >= vy) ? 1 : 0;
                            break;

                        case 0x6:  // SHR Vx; VF = LSB pre-shift
                            v[x] = static_cast<vm_t>((vx >> 1) & 0xFF);
                            v[15] = ((vx & 1) != 0) ? 1 : 0;
                            break;

                        case 0x7:  // SUBN; Vx = Vy - Vx
                            v[x] = static_cast<vm_t>((vy - vx) & 0xFF);
                            v[15] = (vy >= vx) ? 1 : 0;
                            break;

                        case 0xE:  // SHL Vx; VF = MSB pre-shift
                            v[x] = static_cast<vm_t>((vx << 1) & 0xFF);
                            v[15] = ((vx & 0x80) != 0) ? 1 : 0;
                            break;

                        default:
                            break;  // unreachable: defer-screened
                        }
                        break;
                    }

                    case 0x9:  // SNE Vx, Vy (only when low nibble 0)
                        if ((nyb == 0) && (v[x] != v[y])) {
                            pc += 2;
                        }
                        break;

                    case 0xA:  // LD I, nnn
                        i_reg = nnn;
                        break;

                    case 0xB:  // JP V0, nnn
                        pc = (nnn + static_cast<integer_t>(v[0])) % 0x1000;
                        break;

                    case 0xC: {  // RND Vx, nn -- the engine PCG32, for lockstep
                        auto r = static_cast<integer_t>(trx->rand_bounded_uint32(256));
                        v[x] = static_cast<vm_t>(r & nn);
                        break;
                    }

                    case 0xD: {  // DRW Vx, Vy, n  (and SCHIP DXY0 16x16 in hi-res)
                        auto vx0 = static_cast<integer_t>(v[x]) % fb_cols;
                        auto vy0 = static_cast<integer_t>(v[y]) % fb_rows;
                        integer_t collision = 0;
                        if ((nyb == 0) && (hi_res != 0)) {
                            for (integer_t row = 0; row < 16; ++row) {
                                auto hi_byte = static_cast<integer_t>(mem[i_reg + (row * 2)]);
                                auto lo_byte = static_cast<integer_t>(mem[i_reg + (row * 2) + 1]);
                                for (integer_t col = 0; col < 8; ++col) {
                                    if (((hi_byte >> (7 - col)) & 1) != 0) {
                                        auto px = (vx0 + col) % fb_cols;
                                        auto py = (vy0 + row) % fb_rows;
                                        auto cell = fb + ((py * fb_cols) + px);
                                        collision |= (*cell == 1) ? 1 : 0;
                                        *cell ^= 1;
                                    }
                                    if (((lo_byte >> (7 - col)) & 1) != 0) {
                                        auto px = (vx0 + col + 8) % fb_cols;
                                        auto py = (vy0 + row) % fb_rows;
                                        auto cell = fb + ((py * fb_cols) + px);
                                        collision |= (*cell == 1) ? 1 : 0;
                                        *cell ^= 1;
                                    }
                                }
                            }
                        } else {
                            for (integer_t row = 0; row < nyb; ++row) {
                                auto sprite = static_cast<integer_t>(mem[i_reg + row]);
                                for (integer_t col = 0; col < 8; ++col) {
                                    if (((sprite >> (7 - col)) & 1) != 0) {
                                        auto px = (vx0 + col) % fb_cols;
                                        auto py = (vy0 + row) % fb_rows;
                                        auto cell = fb + ((py * fb_cols) + px);
                                        collision |= (*cell == 1) ? 1 : 0;
                                        *cell ^= 1;
                                    }
                                }
                            }
                        }
                        v[15] = static_cast<vm_t>(collision);
                        dirty = 1;
                        break;
                    }

                    case 0xE: {
                        auto vx = static_cast<integer_t>(v[x]);  // <= 15, defer-screened
                        auto down = ((keys >> vx) & 1) != 0;
                        if (((nn == 0x9E) && down) || ((nn == 0xA1) && !down)) {
                            pc += 2;
                        }
                        break;
                    }

                    case 0xF:
                        switch (nn) {
                        case 0x07:
                            v[x] = static_cast<vm_t>(dt & 0xFF);
                            break;

                        case 0x0A:  // LD Vx, K
                            if (keys == 0) {
                                pc -= 2;  // rewind; retry next tick (reference cadence)
                                ++consumed;
                                reason = REASON_KEYWAIT;
                            } else {
                                v[x] = static_cast<vm_t>(std::countr_zero(static_cast<unsigned>(keys)) & 0xFF);
                            }
                            break;

                        case 0x15:
                            dt = static_cast<integer_t>(v[x]);
                            break;

                        case 0x18: {
                            auto old_st = sound;
                            sound = static_cast<integer_t>(v[x]);
                            if ((old_st == 0) && (sound > 0)) {
                                bell = 1;
                            }
                            break;
                        }

                        case 0x1E:
                            i_reg = (i_reg + static_cast<integer_t>(v[x])) % 0x10000;
                            break;

                        case 0x29:
                            i_reg = static_cast<integer_t>(v[x]) * 5;
                            break;

                        case 0x30:
                            i_reg = (static_cast<integer_t>(v[x]) * 10) + 0x50;
                            break;

                        case 0x33: {
                            auto vx = static_cast<integer_t>(v[x]);
                            mem[i_reg] = static_cast<vm_t>((vx / 100) % 10);
                            mem[i_reg + 1] = static_cast<vm_t>((vx / 10) % 10);
                            mem[i_reg + 2] = static_cast<vm_t>(vx % 10);
                            break;
                        }

                        case 0x55:
                            for (integer_t i = 0; i <= x; ++i) {
                                mem[i_reg + i] = v[i];
                            }
                            break;

                        case 0x65:
                            for (integer_t i = 0; i <= x; ++i) {
                                v[i] = mem[i_reg + i];
                            }
                            break;

                        default:
                            break;  // unreachable: defer-screened
                        }
                        break;

                    default:
                        break;
                    }

                    if (reason == REASON_KEYWAIT) {
                        break;
                    } else {
                        ++consumed;
                    }
                }
            }
        }

        // Write the scalars back (non-journaled scratch; see header).
        chip8_set_slot(&st[ST_PC], pc, curr_save_level);
        chip8_set_slot(&st[ST_I], i_reg, curr_save_level);
        chip8_set_slot(&st[ST_DT], dt, curr_save_level);
        chip8_set_slot(&st[ST_ST], sound, curr_save_level);
        chip8_set_slot(&st[ST_SP], sp, curr_save_level);
        chip8_set_slot(&st[ST_DIRTY], dirty, curr_save_level);
        chip8_set_slot(&st[ST_BELL], bell, curr_save_level);

        // Pop the six operands, push (consumed, reason).
        trx->m_op_ptr -= 5;
        *trx->m_op_ptr = Object::make_integer(consumed);
        ++trx->m_op_ptr;
        *trx->m_op_ptr = Object::make_integer(reason);
    }
}

// Null-terminated user operator table.
static constexpr Operator user_ops[] = {
        {chip8_step_fast_op, "chip8-step-fast"},
        {           nullptr,                {}},
};

// Long options recognized by parse_args (mirror of api.inl's long_options
// table; same maintenance contract as the copy in tetrix.cpp -- a missing
// entry degrades to "treated as a script flag").
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
    // Pre-process argv exactly like tetrix.cpp: when invoked without a
    // script filename and the remaining args contain a long option NOT
    // recognized by trix (e.g. --self-test, --ips=N, --cpu-kernel=trix,
    // --disasm), insert examples/chip8.trx so getopt's leading `+` stops
    // there and the unknown flags fall through to command-line-args.
    std::vector<char *> rewritten_argv;
    int effective_argc = argc;
    char **effective_argv = argv;
    char default_path[] = "examples/chip8.trx";

    int insert_pos = -1;
    bool already_has_script = false;
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (a[0] != '-') {
            // Bare argument (diverges from tetrix.cpp, which has no file
            // args): only a .trx path overrides the script; anything else
            // is a ROM path destined for chip8.trx -- insert the default
            // script before it so trix does not scan the ROM as source.
            const char *dot = std::strrchr(a, '.');
            if ((dot != nullptr) && (std::strcmp(dot, ".trx") == 0)) {
                already_has_script = true;
            } else {
                insert_pos = i;
            }
            break;
        } else if ((a[1] == '-') && (a[2] == '\0')) {
            already_has_script = true;
            break;
        } else if ((std::strcmp(a, "--help") == 0) || (std::strcmp(a, "-h") == 0)) {
            // Help belongs to the emulator: insert the default script so
            // the flag falls through to command-line-args and chip8.trx
            // prints its usage (which points at `trix --help` for engine
            // flags).  Without this, trix's own parser consumes --help
            // and only the baseline interpreter help appears.
            insert_pos = i;
            break;
        } else if (a[1] != '-') {
            // Short option: trix-known, keep scanning.
        } else if (is_trix_long_option(a)) {
            const char *eq = std::strchr(a, '=');
            if ((eq == nullptr) && ((i + 1) < argc)) {
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
        if (result.config.m_stream_count == DefaultStreamCount) {
            result.config.m_stream_count = 16;
        }

        // Default to examples/chip8.trx when no script was specified (the
        // bare `./chip8` and `./chip8 --vm-size 4M` cases).
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
