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

//===--- Infix Expression Parser ---===//
// Pratt parser (precedence climbing) for $( expr ) (strict) and $[ expr ]
// (promoted / auto-widening) infix math expression syntax.  Parses the full
// expression, packs postfix tokens into a VM-allocated packed byte buffer,
// and returns the first token.  Subsequent scanner() calls extract buffered
// tokens until exhausted.
//
// Precedence table (higher = tighter binding):
//   13  **                    right-associative
//   12  * / %                 left-associative
//   11  + -                   left-associative
//   10  << >>                 left-associative
//    9  < <= > >=             left-associative
//    8  == !=                 left-associative
//    7  &  &&                 left-associative
//    6  ^                     left-associative
//    5  |  ||                 left-associative
//    4  ? :                   right-associative (ternary)
//
// Unary `-` binds LOOSER than `**`: `-2**2` parses as -(2**2) = -4,
// matching Python / MATLAB / Fortran / Ruby.  See infix_primary for the
// implementation: unary `-` recurses into infix_expr at min_prec = 13 so
// `**` is bound before `neg` is emitted.  The other prefix operators
// (+, !, ~) bind TIGHTER than `**` -- they consume only a primary, so
// `!2**2` parses as `(!2)**2`.
//
//===--- Notes on subtle semantics ---===//
//
// Ternary `cond ? t : f` is right-associative and re-enters infix_expr
// at min_prec = 4 for its false-arm so `a ? b : c ? d : e` parses as
// `a ? b : (c ? d : e)`.  The desugar is `<cond> <t> <f> rot select`,
// which means BOTH `<t>` and `<f>` are evaluated eagerly before
// `select` picks one -- ternary is NOT short-circuit.  For
// short-circuit semantics, write `{ <t> } { <f> } cond ifelse` instead.
//
// `**` right-associativity is implemented by passing its own precedence
// (13) as the inner call's min_prec rather than precedence+1 (which is
// what left-associative ops use), so `a ** b ** c` parses as
// `a ** (b ** c)`.
//
// Comments inside `$(...)` / `$[...]`: only whitespace is honored; neither line
// `%` comments (would conflict with the `%` modulo operator) nor
// `%{...%}` block comments are recognized.  Reach for postfix when
// you need long-form documentation between operators.
//
//===--- Error semantics ---===//
//
// The infix family is throw-only on errors: every error path invokes
// trx->error() (`[[noreturn]]`), unwinding through the InfixScratchGuard
// destructor which reclaims any ExtValue allocations stranded in
// m_infix_scratch_objs[].  Callers cannot expect a "soft" error
// return.  This differs from the rest of the scanner family
// (scanner.inl), which uses `syntaxerror_pair` for some recoverable
// surface cases; the divergence is intentional -- infix has no
// resumable partial state and no caller that wants to keep parsing
// past a malformed expression.

// RAII guard for m_infix_scratch_objs.  Frees any ExtValue payloads still
// referenced by scratch slots [0..trx->m_infix_scratch_count) at destruction
// unless commit() was called.  Used by scan_infix_expression to undo
// partial parses when trx->error() throws mid-expression: Long / ULong /
// Double / Int128 / UInt128 literals each own an ExtValue offset stamped
// at parse time, and without this cleanup the ExtValue would stay
// claimed but unreachable -- a slow leak under fuzzed or repeated-failure
// workloads.
//
// Count tracking: infix_emit writes to trx->m_infix_scratch_count on
// every successful emission, so the destructor always sees the
// up-to-date high-water mark regardless of which parse function was
// active when the throw fired.
//
// Successful parses commit() before pack_infix_tokens absorbs the offsets
// into the packed byte buffer (where the ExtValues are then owned by the
// resulting buffer + extract_next_packed call chain, not by the scratch).
struct InfixScratchGuard {
    Trix *trx;
    bool committed;

    explicit InfixScratchGuard(Trix *t) : trx(t), committed(false) { trx->m_infix_scratch_count = 0; }

    ~InfixScratchGuard() {
        if (!committed) {
            auto count = trx->m_infix_scratch_count;
            for (length_t i = 0; i < count; ++i) {
                trx->m_infix_scratch_objs[i].maybe_free_extvalue(trx);
            }
        }
        trx->m_infix_scratch_count = 0;
    }

    void commit() { committed = true; }
};

// Emit one postfix token into the output buffer.  The scratch slot
// count is tracked via trx->m_infix_scratch_count -- a Trix member
// rather than a threaded local so the InfixScratchGuard's destructor
// reads the up-to-date count on any error throw and reclaims
// ExtValues from slots [0..m_infix_scratch_count) on unwind.
[[nodiscard]] static length_t infix_emit(Trix *trx, Object *tokens_ptr, length_t count, Object token_obj) {
    if (count < MaxInfixTokens) {
        tokens_ptr[count++] = token_obj;
        trx->m_infix_scratch_count = count;
        return count;
    } else {
        trx->error(Error::LimitCheck, "infix: expression exceeds {} tokens", MaxInfixTokens);
    }
}

// Emit a well-known operator name (Neg, Not, Rot, Select, Promote, plus
// the binary-op emission WellKnownNames in infix_binary_op's table).
// Saves the ~30-char repetition of trx->wellknown_name(..., ExecutableAttrib)
// at every emit site.
[[nodiscard]] static length_t infix_emit_wk(Trix *trx, Object *tokens_ptr, length_t count, WellKnownName wk) {
    return infix_emit(trx, tokens_ptr, count, trx->wellknown_name(wk, Object::ExecutableAttrib));
}

// Peek at the byte one position past the current read pointer.
// Fast path: if the next byte is within the current buffer, return it directly.
// Slow path: at a buffer boundary, advance past the current byte to trigger fill(),
// peek at the newly loaded data, then unget the saved byte so it remains
// available for the next read.
[[nodiscard]] int infix_peek_next(Trix *trx) {
    auto next = (m_rptr_offset + 1);
    if (next < m_rlimit_offset) {
        return trx->offset_to_value<vm_t>(next);
    } else if (m_rptr_offset < m_rlimit_offset) {
        // Buffer boundary: consume current byte, peek next (may fill), then unget.
        auto saved = trx->offset_to_value<vm_t>(m_rptr_offset);
        ++m_rptr_offset;            // advance without line tracking
        auto next_ch = peekc(trx);  // may trigger fill()
        if (next_ch == EOFc) {
            --m_rptr_offset;  // fill failed; restore read position
        } else {
            ungetc(trx, saved);  // restore current byte into unget slot
        }
        return next_ch;
    } else {
        return EOFc;
    }
}

// Binary operator descriptor returned by infix_binary_op().
// When precedence == 0 there is no binary operator at the current position;
// `emission` is unused in that case.
struct InfixOp {
    int precedence{0};
    bool right_assoc{false};
    WellKnownName emission{WellKnownName::Default};
    int char_count{0};  // number of characters to consume
};

// Identify the binary operator starting at the current peek position.
// Does not consume any characters.  Returns precedence 0 if the character
// does not start a binary operator.
[[nodiscard]] InfixOp infix_binary_op(Trix *trx) {
    auto ch = peekc(trx);

    switch (ch) {
    case '|':
        return {5, false, WellKnownName::Or, (infix_peek_next(trx) == '|') ? 2 : 1};

    case '^':
        return {6, false, WellKnownName::Xor, 1};

    case '&':
        return {7, false, WellKnownName::And, (infix_peek_next(trx) == '&') ? 2 : 1};

    case '=':
        if (infix_peek_next(trx) == '=') {
            return {8, false, WellKnownName::Eq, 2};
        } else {
            return {0, false, WellKnownName::Default, 0};
        }

    case '!':
        if (infix_peek_next(trx) == '=') {
            return {8, false, WellKnownName::Ne, 2};
        } else {
            return {0, false, WellKnownName::Default, 0};
        }

    case '<': {
        auto ch2 = infix_peek_next(trx);
        if (ch2 == '=') {
            return {9, false, WellKnownName::Le, 2};
        } else if (ch2 == '<') {
            return {10, false, WellKnownName::ShiftLeft, 2};
        } else {
            return {9, false, WellKnownName::Lt, 1};
        }
    }

    case '>': {
        auto ch2 = infix_peek_next(trx);
        if (ch2 == '=') {
            return {9, false, WellKnownName::Ge, 2};
        } else if (ch2 == '>') {
            return {10, false, WellKnownName::ShiftRight, 2};
        } else {
            return {9, false, WellKnownName::Gt, 1};
        }
    }

    case '+':
        return {11, false, WellKnownName::Add, 1};

    case '-':
        return {11, false, WellKnownName::Sub, 1};

    case '*':
        if (infix_peek_next(trx) == '*') {
            return {13, true, WellKnownName::Pow, 2};
        } else {
            return {12, false, WellKnownName::Mul, 1};
        }

    case '/':
        return {12, false, WellKnownName::Div, 1};

    case '%':
        return {12, false, WellKnownName::Mod, 1};

    default:
        return {0, false, WellKnownName::Default, 0};
    }
}

// Scan a number literal inside an infix expression and emit it.
// The current peek character is a digit or leading dot.
[[nodiscard]] length_t infix_number(Trix *trx, Object *tokens_ptr, length_t count) {
    auto token_base = trx->m_token_base;
    auto token_limit = trx->m_token_limit;
    auto token_ptr = token_base;
    auto token_length = length_t{0};

    // Accumulate characters that can be part of a numeric literal:
    // digits, letters (hex, suffix, exponent), dot, hash (radix), underscore.
    while (true) {
        auto ch = peekc(trx);
        if (ch == EOFc) {
            break;
        } else if (Trix::is_digit_or_alpha_or_underscore(ch) || (ch == '.') || (ch == '#')) {
            if (token_ptr < token_limit) {
                *token_ptr++ = static_cast<vm_t>(ch);
            }
            ++token_length;
            consume(trx);

            // Exponent sign: e+/e-/E+/E- (decimal) or p+/p-/P+/P- (hex float)
            if ((ch == 'e') || (ch == 'E') || (ch == 'p') || (ch == 'P')) {
                auto sign = peekc(trx);
                if ((sign == '+') || (sign == '-')) {
                    if (token_ptr < token_limit) {
                        *token_ptr++ = static_cast<vm_t>(sign);
                    }
                    ++token_length;
                    consume(trx);
                }
            }
        } else {
            break;
        }
    }

    if (token_length == 0) {
        trx->error(Error::SyntaxError, "infix: expected number");
    } else if (token_length > MaxNameLength) {
        trx->error(Error::LimitCheck, "infix: number length {} exceeds maximum {}", token_length, MaxNameLength);
    } else {
        auto token_sv = make_sv(token_base, token_length);
        auto number_obj = try_parse_number(trx, token_sv);
        if (number_obj.is_null()) {
            // ExtValue ownership: number_obj may have allocated an ExtValue
            // before try_parse_number's final validation rejected it.
            // Free it before the throw so the scratch guard's invariant
            // ("scratch[0..count) own their ExtValues") is preserved.
            number_obj.maybe_free_extvalue(trx);
            trx->error(Error::SyntaxError, "infix: invalid number '{}'", token_sv);
        } else {
            return infix_emit(trx, tokens_ptr, count, number_obj);
        }
    }
}

// Scan a name inside an infix expression.  Returns the name as a string_view
// into the token buffer.  Hyphens between alphabetic characters are included
// (e.g. "kahan-sum").  Use whitespace to disambiguate: "a - b" is subtraction,
// "kahan-sum" is a single name.
[[nodiscard]] std::string_view infix_scan_name(Trix *trx) {
    auto token_base = trx->m_token_base;
    auto token_limit = trx->m_token_limit;
    auto token_ptr = token_base;
    auto token_length = length_t{0};

    while (true) {
        auto ch = peekc(trx);
        if (ch == EOFc) {
            break;
        } else if (Trix::is_digit_or_alpha_or_underscore(ch)) {
            if (token_ptr < token_limit) {
                *token_ptr++ = static_cast<vm_t>(ch);
            }
            ++token_length;
            consume(trx);
        } else if ((ch == '-') && (token_ptr != token_base) && Trix::is_alpha(token_ptr[-1])) {
            // Hyphen between alphabetic characters: include if next char is alpha.
            // Uses token_ptr (clamped at token_limit) rather than token_base[token_length-1],
            // which would OOB-read once token_length exceeds MaxTokenLength.
            auto next = infix_peek_next(trx);
            if (Trix::is_alpha(next)) {
                if (token_ptr < token_limit) {
                    *token_ptr++ = static_cast<vm_t>(ch);
                }
                ++token_length;
                consume(trx);
            } else {
                break;
            }
        } else {
            break;
        }
    }

    if (token_length == 0) {
        trx->error(Error::SyntaxError, "infix: expected name");
    } else if (token_length > MaxNameLength) {
        trx->error(Error::LimitCheck, "infix: name length {} exceeds maximum {}", token_length, MaxNameLength);
    } else {
        return make_sv(token_base, token_length);
    }
}

// Parse a name token, and if '(' follows, parse it as a function call with comma-separated arguments.
[[nodiscard]] length_t infix_name_or_call(Trix *trx, Object *tokens_ptr, length_t count, bool auto_promote, int depth) {
    // Intern the name NOW: argument parsing will overwrite the token buffer
    auto name_sv = infix_scan_name(trx);
    auto name_obj = Name::make(trx, name_sv, Object::ExecutableAttrib);
    skip_whitespace(trx);

    if (peekc(trx) == '(') {
        // Function call: name(arg1, arg2, ...)
        consume(trx);
        skip_whitespace(trx);

        if (peekc(trx) != ')') {
            // Parse first argument
            count = infix_expr(trx, tokens_ptr, count, auto_promote, 0, depth + 1);
            skip_whitespace(trx);

            // Parse remaining arguments separated by commas
            while (peekc(trx) == ',') {
                consume(trx);
                count = infix_expr(trx, tokens_ptr, count, auto_promote, 0, depth + 1);
                skip_whitespace(trx);
            }
        }

        if (peekc(trx) != ')') {
            trx->error(Error::SyntaxError, "infix: expected ')' after function arguments");
        } else {
            consume(trx);
            // Emit function name (arguments are already on stack)
            count = infix_emit(trx, tokens_ptr, count, name_obj);
        }
    } else {
        // Bare name: emit as executable name (looked up at runtime)
        count = infix_emit(trx, tokens_ptr, count, name_obj);
    }
    return count;
}

// Parse a primary expression: number, name, function call, unary prefix, or grouping.
[[nodiscard]] length_t infix_primary(Trix *trx, Object *tokens_ptr, length_t count, bool auto_promote, int depth) {
    if (depth > MaxInfixNesting) {
        trx->error(Error::LimitCheck, "infix: expression nesting depth exceeds maximum {}", MaxInfixNesting);
    } else {
        skip_whitespace(trx);
        auto ch = peekc(trx);

        if (ch == EOFc) {
            trx->error(Error::SyntaxError, "infix: unexpected end of input");
        } else {
            // Grouping: ( expr )
            if (ch == '(') {
                consume(trx);
                count = infix_expr(trx, tokens_ptr, count, auto_promote, 0, depth + 1);
                skip_whitespace(trx);
                if (peekc(trx) != ')') {
                    trx->error(Error::SyntaxError, "infix: expected ')' in grouping");
                } else {
                    consume(trx);
                }
            } else if (ch == '-') {
                // Unary minus: -expr -> expr neg
                //
                // Parse the operand at min_prec = 13 (matching `**`'s
                // precedence and its right-associative right-operand
                // precedence) so `-x ** y` parses as -(x ** y), the
                // Python / MATLAB / Fortran / Ruby convention.  Higher-
                // precedence ops (none above ** in Trix's table) would
                // also be absorbed; lower-precedence ops stay outside.
                //
                // Parsing via infix_primary would have grabbed only `x`
                // and emitted `x neg`, leaving `** y` for the outer
                // loop and computing (-x) ** y -- correct sign only for
                // odd y.
                consume(trx);
                count = infix_expr(trx, tokens_ptr, count, auto_promote, 13, depth + 1);
                count = infix_emit_wk(trx, tokens_ptr, count, WellKnownName::Neg);
            } else if (ch == '+') {
                // Unary plus: +expr -> expr (identity, no-op)
                consume(trx);
                count = infix_primary(trx, tokens_ptr, count, auto_promote, depth + 1);
            } else if ((ch == '!') || (ch == '~')) {
                // Unary not: !expr or ~expr -> expr not
                consume(trx);
                count = infix_primary(trx, tokens_ptr, count, auto_promote, depth + 1);
                count = infix_emit_wk(trx, tokens_ptr, count, WellKnownName::Not);
            } else if (Trix::is_digit(ch)) {
                // Number literal
                count = infix_number(trx, tokens_ptr, count);
            } else if ((ch == '.') && Trix::is_digit(infix_peek_next(trx))) {
                // Leading dot: could be a decimal number like .5
                count = infix_number(trx, tokens_ptr, count);
            } else if (Trix::is_alpha_or_underscore(ch)) {
                // Name or function call
                count = infix_name_or_call(trx, tokens_ptr, count, auto_promote, depth);
            } else if (Trix::is_syntax_error(ch) || Trix::is_binary_token(ch)) {
                // Non-printable control char or high-bit byte (whitespace
                // was already consumed by the skip_whitespace above).
                trx->error(Error::SyntaxError, "infix: unexpected character 0x{:02X}", ch);
            } else {
                trx->error(Error::SyntaxError, "infix: unexpected '{:c}' in expression", static_cast<char>(ch));
            }
            return count;
        }
    }
}

// Parse an infix expression with minimum precedence (Pratt parser).
[[nodiscard]] length_t infix_expr(Trix *trx, Object *tokens_ptr, length_t count, bool auto_promote, int min_prec, int depth) {
    if (depth > MaxInfixNesting) {
        trx->error(Error::LimitCheck, "infix: expression nesting depth exceeds maximum {}", MaxInfixNesting);
    } else {
        count = infix_primary(trx, tokens_ptr, count, auto_promote, depth);

        while (true) {
            skip_whitespace(trx);

            // Ternary: cond ? true_expr : false_expr
            // Precedence 4 (below all binary operators), right-associative.
            // Emits: <cond> <true> <false> rot select
            if ((peekc(trx) == '?') && (4 >= min_prec)) {
                consume(trx);
                count = infix_expr(trx, tokens_ptr, count, auto_promote, 0, depth + 1);
                skip_whitespace(trx);
                if (peekc(trx) != ':') {
                    trx->error(Error::SyntaxError, "infix: expected ':' in ternary expression");
                } else {
                    consume(trx);
                    count = infix_expr(trx, tokens_ptr, count, auto_promote, 4, depth + 1);
                    count = infix_emit_wk(trx, tokens_ptr, count, WellKnownName::Rot);
                    count = infix_emit_wk(trx, tokens_ptr, count, WellKnownName::Select);
                }
            } else {
                auto op = infix_binary_op(trx);
                if ((op.precedence == 0) || (op.precedence < min_prec)) {
                    return count;
                } else {
                    // Consume operator characters
                    for (int i = 0; i < op.char_count; ++i) {
                        consume(trx);
                    }

                    // Parse right operand
                    auto next_prec = op.right_assoc ? op.precedence : (op.precedence + 1);
                    count = infix_expr(trx, tokens_ptr, count, auto_promote, next_prec, depth + 1);

                    // Auto-promote: emit promote before binary operator
                    if (auto_promote) {
                        count = infix_emit_wk(trx, tokens_ptr, count, WellKnownName::Promote);
                    }

                    // Emit operator
                    count = infix_emit_wk(trx, tokens_ptr, count, op.emission);
                }
            }
        }
    }
}

// Pack an array of infix postfix tokens into a compact packed byte stream.
// Handles only the types produced by the infix scanner: Byte, Integer, UInteger,
// Long, ULong, Real, Double, Name.  Writes standard packed format so
// extract_next_packed() works unchanged for extraction.
// Returns the number of packed bytes written.
[[nodiscard]] static length_t pack_infix_tokens(Trix *trx, const Object *tokens_ptr, length_t count, packed_data_t *dst) {
    using PackedType = Object::PackedType;
    using Type = Object::Type;

    auto start = dst;
    for (length_t i = 0; i < count; ++i) {
        auto token_obj = tokens_ptr[i];
        auto attrib = token_obj.is_executable() ? PackedExecutableAttrib : PackedLiteralAttrib;
        auto packed_type = Object::packed_type_t{+PackedType::Byte};
        auto value = vm_offset_t{0};
        auto value_size = int{-1};  // -1 = auto-calculate, 1-4 = explicit

        switch (+token_obj.type()) {
        case +Type::Byte:
            packed_type = +PackedType::Byte;
            value = token_obj.byte_value();
            value_size = 1;
            break;

        case +Type::Integer:
            packed_type = +PackedType::Integer;
            value = static_cast<vm_offset_t>(static_cast<uint32_t>(token_obj.integer_value()));
            break;

        case +Type::UInteger:
            packed_type = +PackedType::UInteger;
            value = token_obj.uinteger_value();
            break;

        case +Type::Long:
            packed_type = +PackedType::Long;
            value = token_obj.extvalue_offset();
            break;

        case +Type::ULong:
            packed_type = +PackedType::ULong;
            value = token_obj.extvalue_offset();
            break;

        case +Type::Address:
            packed_type = +PackedType::Address;
            value = token_obj.extvalue_offset();
            break;

        case +Type::Real:
            packed_type = +PackedType::Real;
            value = std::bit_cast<uint32_t>(token_obj.real_value());
            break;

        case +Type::Double:
            packed_type = +PackedType::Double;
            value = token_obj.extvalue_offset();
            break;

        case +Type::Name:
            packed_type = +PackedType::Name;
            value = token_obj.m_name;
            break;

        case +Type::Int128:
        case +Type::UInt128:
            packed_type = +PackedType::PackedExt;
            value = token_obj.m_offset;  // WideValue vm_offset_t
            break;

        default:
            trx->error(Error::TypeCheck, "pack_infix_tokens: unexpected type");
        }

        // Determine minimum byte count for the value (sign-aware for Integer).
        if (value_size == -1) {
            value_size = Object::packed_value_size(packed_type, value);
        }

        // PackedExt: custom encoding [header][subcode][offset: SS+1 bytes].
        // Infix 128-bit tokens are always Literal (X=0); eqref subcodes never
        // appear in infix token streams (only Int128/UInt128 reach here).
        if (packed_type == +PackedType::PackedExt) {
            auto ss = static_cast<packed_data_t>(((value_size - 1) & 3) << 5);
            *dst++ = (ss | +PackedType::PackedExt);
            *dst++ = token_obj.is_int128() ? Object::PackedExtSubcode_Int128 : Object::PackedExtSubcode_UInt128;
            dst = Object::pack_value_bytes(dst, value, value_size);
        } else {
            // Header byte: |X|SS|TTTTT| where SS = value_size - 1
            auto ss = static_cast<packed_data_t>(((value_size - 1) & 3) << 5);
            *dst++ = (attrib | ss | +packed_type);

            // Value bytes (big-endian, 1-4 bytes)
            dst = Object::pack_value_bytes(dst, value, value_size);
        }
    }
    return static_cast<length_t>(dst - start);
}

// Allocate (lazily) the per-stream packed-token buffer if needed and
// pack tokens [tokens_ptr+1..tokens_ptr+count) into it.  Sets
// m_infix_packed_size and m_infix_packed_read so subsequent scanner()
// calls drain the buffered tokens.  Shared by scan_infix_expression
// (scanner_infix.inl) and scan_field_access (scanner.inl).
//
// VMFull throws via trx->error with `error_context` as the message
// prefix ("infix" or "field-access").  Caller is responsible for any
// per-token cleanup on the VMFull throw path -- scan_infix_expression
// uses InfixScratchGuard to reclaim ExtValues from scratch slots;
// scan_field_access has Name-only tokens that don't carry ExtValues.
//
// Precondition: count >= 2 (single-token callers must return
// tokens_ptr[0] directly without buffering).
void pack_and_buffer_infix_tail(Trix *trx, Object *tokens_ptr, length_t count, std::string_view error_context) {
    if (m_infix_offset == nulloffset) {
        auto size = MaxInfixPackedBufferSize;
        if (trx->vm_remaining<packed_data_t>() < size) {
            trx->error(Error::VMFull, "{}: while allocating packed token buffer", error_context);
        } else {
            // Journal the old nulloffset value so restore() resets it when
            // the VM allocation is rolled back past this buffer.
            if (trx->m_curr_save_level > Save::BASE) {
                Save::save_stream_infix_offset(trx, &m_infix_offset);
            }
            auto [alloc_ptr, alloc_offset] = trx->vm_alloc<packed_data_t>(size);
            m_infix_offset = alloc_offset;
        }
    }
    auto buffer = trx->offset_to_ptr<packed_data_t>(m_infix_offset);
    m_infix_packed_size = pack_infix_tokens(trx, tokens_ptr + 1, static_cast<length_t>(count - 1), buffer);
    m_infix_packed_read = 0;
}

// Entry point: parse a $( expr ) or $[ expr ] infix expression.
// The opening sigil ($( or $[) has already been consumed by the caller.
// close_ch is ')' for $(...) (strict) or ']' for $[...] (promoted),
// matching the opening bracket the caller consumed.  Parses the
// expression, expects close_ch, packs postfix tokens into VM buffer,
// returns the first token.  Inner sub-expressions still use ()
// regardless of the outer bracket -- only the top-level close char
// varies.
[[nodiscard]] ScanToken scan_infix_expression(Trix *trx, bool auto_promote, int close_ch) {
    auto tokens_ptr = trx->m_infix_scratch_objs;
    auto count = length_t{0};

    // Reclaim ExtValue payloads from partial scratch on any error throw.
    // commit() runs once we are about to publish the tokens (single-token
    // path returns the slot directly; multi-token path packs the slots
    // into m_infix_offset before commit).
    InfixScratchGuard scratch_guard(trx);

    // Parse the expression
    count = infix_expr(trx, tokens_ptr, count, auto_promote, 0, 0);

    // Expect closing delimiter (')' for $(...) or ']' for $[...])
    skip_whitespace(trx);
    auto ch = peekc(trx);
    if (ch == close_ch) {
        consume(trx);
        if (count == 0) {
            trx->error(Error::SyntaxError, "infix: empty expression");
        } else if (count == 1) {
            // Single token -- return directly, no buffering.  The ExtValue
            // (if any) is owned by the returned Object on the operand
            // stack; commit so the guard does not free it.
            scratch_guard.commit();
            auto lexeme = tokens_ptr[0].is_literal() ? Lexeme::LiteralValue : Lexeme::ExecutableValue;
            return std::pair{lexeme, tokens_ptr[0]};
        } else {
            // Multiple tokens: pack into reusable per-stream VM buffer.
            // On VMFull, scratch_guard's destructor frees partial ExtValues.
            // After pack succeeds, the offsets are owned by the packed
            // buffer + returned Object; commit so the guard releases them.
            pack_and_buffer_infix_tail(trx, tokens_ptr, count, "infix");
            scratch_guard.commit();
            auto lexeme = tokens_ptr[0].is_literal() ? Lexeme::LiteralValue : Lexeme::ExecutableValue;
            return std::pair{lexeme, tokens_ptr[0]};
        }
    } else if (ch == EOFc) {
        trx->error(Error::SyntaxError, "infix: missing closing '{:c}'", static_cast<char>(close_ch));
    } else {
        trx->error(Error::SyntaxError,
                   "infix: unexpected '{:c}', expected '{:c}'",
                   static_cast<char>(ch),
                   static_cast<char>(close_ch));
    }
}
