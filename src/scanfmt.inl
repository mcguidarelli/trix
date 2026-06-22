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

//===--- ScanFmt: format-string input engine ---===//
//
// Implements format-string-driven input parsing -- the inverse of PrintFmt.
// Reads structured data from a string or stream by matching a format pattern
// and extracting values into argument slots.  Based on:
//
//   C scanf: format-string-driven input parsing with type specifiers
//   (d, x, f, s, c) and width limits.  Trix extends this with positional
//   argument IDs and Trix-specific types.
//
//   Python struct.unpack / Rust scan!: the concept of a "scan pattern"
//   that destructures formatted text into typed values.
//
// --- Core concepts for maintainers ---
//
// USAGE
//   ScanFmt is used by two operators:
//     sscan-fmt: src-str fmt-str mark arg* -- arg* int    (scan from string)
//     fscan-fmt: stream  fmt-str mark arg* -- arg* int    (scan from stream)
//   The arguments above the mark are output slots.  ScanFmt reads from
//   the input, matches literals and whitespace in the format string,
//   and writes parsed values into the argument slots.  Returns the number
//   of successfully scanned fields.
//
// TWO INPUT MODES
//   String input: reads from a vm_t[] memory buffer with pointer advancement.
//   Stream input: reads from a Stream via getc/peekc.  Blocks on interactive
//     streams.  A read-only stream auto-closes on EOF per Stream::fill_file.
//
// SCAN PROCESS
//   The format string is walked character by character:
//   - Literal characters must match the input exactly (scan-match-fail
//     on mismatch).
//   - Whitespace in the format string matches zero or more whitespace
//     characters in the input.
//   - Replacement fields {[arg-id][:format-spec]} extract a typed value
//     from the input and store it in the corresponding argument slot.
//
// TYPE DISPATCH
//   Each format type specifier drives a different scan function:
//     d  -- scan a decimal integer
//     x  -- scan a hexadecimal integer (optional 0x/0X prefix)
//     f  -- scan a floating-point number
//     s  -- scan a string (up to width limit or next whitespace)
//     c  -- scan a single character (as byte)
//     ?  -- scan an escape-aware token: a backslash-escaped string into a
//           String/Null target, or a single byte into a Byte target
//     I  -- scan a chrono instant via strftime template; "l" suffix
//           selects local zone.  Stores ms since 1970 UTC into a ULong
//           target.  Both memory and stream input are supported; stream
//           input parses the currently-buffered window (records longer
//           than MaxStrftimeInputLength fail with scan-type-fail).  Raises
//           Unsupported only when "l" (local zone) is requested but the
//           system zone database is unavailable.
//     D  -- scan a packed udate (date-only) via strftime template into
//           a UInteger target.  Both memory and stream input are supported.
//
// ERROR HANDLING
//   Scan failures are reported as specific error codes:
//     scan-match-fail   -- literal character mismatch
//     scan-input-fail   -- unexpected end of input
//     scan-type-fail    -- value doesn't match expected type
//     scan-type-mismatch -- argument slot type incompatible
//     scan-duplicate-arg-id -- same argument written twice
//
// Format-string grammar:
// format-string     ::= (whitespace-char | literal-char | escaped-char | replacement-field)*
// whitespace-char   ::= HT | LF | VT | FF | CR | " "
// literal-char      ::= <all characters except (whitespace-char | "{" | "}")>
// escaped-char      ::= "{{" | "}}"
//
// replacement-field ::= "{" [arg-id] [":" format-spec] "}"
// arg-id            ::= integer
// integer           ::= digit+
// digit             ::= "0" | "1" | "2" | "3" | "4" | "5" | "6" | "7" | "8" | "9"
//
// format-spec       ::= [[fill] align] [width] ["." precision] [type]
// fill              ::= whitespace-char | literal-char
// align             ::= "<" | ">" | "^"
// width             ::= integer
// precision         ::= integer
// type              ::= "c" | "d" | "f" | "s" | "x" | "?"
//                     | "I" [ "l" ] strftime-template       % chrono instant; "l" selects local zone
//                     | "D" strftime-template               % packed udate (date-only)
// strftime-template ::= <strftime spec body up to closing '}'>
//

class ScanFmt {
public:
    ScanFmt(Trix *trx, const char *format, length_t length, Object *args_ptr, length_t count, Stream *input) :
            m_args_referenced{0},
            m_trx{trx},
            m_args_ptr{args_ptr},
            m_format_limit{format + length},
            m_format_ptr{format},
            m_input_stream{input},
            m_input_ptr{nullptr},
            m_next_arg_id{0},
            m_scanned_count{0},
            m_args_count{count},
            m_input_length{0} {}

    ScanFmt(Trix *trx, const char *format, length_t length, Object *args_ptr, length_t count, vm_t *input, length_t input_length) :
            m_args_referenced{0},
            m_trx{trx},
            m_args_ptr{args_ptr},
            m_format_limit{format + length},
            m_format_ptr{format},
            m_input_stream{nullptr},
            m_input_ptr{input},
            m_next_arg_id{0},
            m_scanned_count{0},
            m_args_count{count},
            m_input_length{input_length} {}

    ScanFmt() = delete;
    ScanFmt(const ScanFmt &) = delete;
    ScanFmt &operator=(const ScanFmt &) = delete;

    // The number of input items successfully matched and assigned
    // this can be fewer than provided for, or zero, upon an early matching failure
    //
    // EOF is returned if:
    //   an input failure before the first successful conversion
    //   a matching failure
    [[nodiscard]] int process() {
        // uses a bit mask to track whether an arg-id has been previously used as an input destination
        auto max_args_count = (sizeof(m_args_referenced) * 8);
        if (m_args_count >= max_args_count) {
            m_trx->error(Error::LimitCheck,
                         "number of arguments {} provided to ScanFmt exceeds maximum {}",
                         m_args_count,
                         max_args_count);
        }

        while (m_format_ptr < m_format_limit) {
            auto format_ch = *m_format_ptr++;
            auto input_ch = peek_input();

            // whitespace-char
            if (Trix::is_whitespace(format_ch)) {
                // skip all consecutive whitespace in the format-string
                while (Trix::is_whitespace(format_ch) && (m_format_ptr < m_format_limit)) {
                    format_ch = *m_format_ptr++;
                }

                // consume all consecutive whitespace from the input-stream
                while (Trix::is_whitespace(input_ch) && (input_ch != Trix::EOFc)) {
                    consume_input();
                    input_ch = peek_input();
                }

                if ((m_format_ptr == m_format_limit) && Trix::is_whitespace(format_ch)) {
                    break;
                }
            }

            if (format_ch == '{') {
                if (m_format_ptr >= m_format_limit) {
                    m_trx->error(Error::InvalidFormatString, "lone \'{{\' at end of format-string");
                } else {
                    // replacement-field or "{{"
                    format_ch = *m_format_ptr++;

                    if (format_ch == '{') {
                        // escaped-char
                        if (input_ch == format_ch) {
                            // consume "{" from the input stream
                            consume_input();
                        } else {
                            match_fail_error(static_cast<vm_t>(format_ch), input_ch);
                        }
                    } else {
                        // replacement-field "{" [arg-id] [":" format-spec] "}"
                        m_width = WIDTH_DEFAULT;
                        m_precision = PRECISION_DEFAULT;
                        m_fill = FILL_DEFAULT;
                        m_align = Align::None;
                        m_type = TYPE_DEFAULT;
                        m_chrono_template_ptr = nullptr;
                        m_chrono_template_len = 0;
                        m_chrono_local = false;

                        // [arg-id]
                        auto arg_id = 0;
                        format_ch = static_cast<char>(get_argid(static_cast<vm_t>(format_ch), &arg_id));

                        // [":" format-spec]
                        if (format_ch == ':') {
                            format_ch = static_cast<char>(process_format_spec());
                        }

                        // "}"
                        if (format_ch == '}') {
                            scan_input(arg_id);
                        } else {
                            m_trx->error(Error::InvalidFormatString,
                                         "ScanFmt format-str error: expected: \'}}\', encountered:\'{:c}\'",
                                         format_ch);
                        }
                    }
                }
            } else if (format_ch == '}') {
                if (m_format_ptr >= m_format_limit) {
                    m_trx->error(Error::InvalidFormatString, "lone \'}}\' at end of format-string");
                } else {
                    format_ch = *m_format_ptr++;

                    if (format_ch == '}') {
                        // escaped-char
                        if (input_ch == format_ch) {
                            // consume "}" from the input stream
                            consume_input();
                        } else {
                            match_fail_error(static_cast<vm_t>(format_ch), input_ch);
                        }
                    } else {
                        m_trx->error(Error::InvalidFormatString, "ScanFmt: format-str error unbalanced \'}}\'");
                    }
                }
            } else {
                // literal-char
                if (input_ch == format_ch) {
                    // match and consume one literal-char from the input stream
                    consume_input();
                } else {
                    match_fail_error(static_cast<vm_t>(format_ch), input_ch);
                }
            }
        }
        return m_scanned_count;
    }
private:
    enum struct Align : vm_t {
        None,
        Left,
        Right,
        Center,
        DefaultLeft,
        DefaultRight,
    };

    static constexpr int MIN_WIDTH{1};
    static constexpr int MAX_WIDTH{1024};
    static constexpr int MIN_PRECISION{1};
    static constexpr int MAX_PRECISION{1024};
    static constexpr int MANUAL_INDEXING{-1};

    static constexpr int WIDTH_DEFAULT{-1};
    static constexpr int PRECISION_DEFAULT{-1};
    static constexpr vm_t FILL_DEFAULT{' '};
    static constexpr vm_t TYPE_DEFAULT{' '};

    [[nodiscard]] vm_t next_format_ch() {
        if (m_format_ptr < m_format_limit) {
            return static_cast<vm_t>(*m_format_ptr++);
        } else {
            m_trx->error(Error::InvalidFormatString, "premature end of format-string encountered");
        }
    }

    [[noreturn]] void match_fail_error(vm_t expected, int got) {
        if (got == Trix::EOFc) {
            m_trx->error(Error::ScanMatchFail, "ScanFmt match failed, expected: \'{:c}\', encountered: end of input", expected);
        } else {
            m_trx->error(Error::ScanMatchFail, "ScanFmt match failed, expected: \'{:c}\', encountered:\'{:c}\'", expected, got);
        }
    }

    // Parse a decimal integer from the format string.  Returns the first
    // non-digit character; *value is unchanged if no digits are present.
    [[nodiscard]] vm_t get_integer(vm_t ch, int lower, int upper, int *value) {
        if ((ch >= '0') && (ch <= '9')) {
            // integer
            int i = 0;
            int ndigits = 0;
            while ((ch >= '0') && (ch <= '9')) {
                if (++ndigits > 9) {
                    m_trx->error(Error::RangeCheck, "format integer exceeds 9 digits");
                } else {
                    i = (i * 10) + (ch - '0');
                    ch = next_format_ch();
                }
            }
            if ((i < lower) || (i > upper)) {
                m_trx->error(Error::RangeCheck, "integer {} not within range of {}..{}", i, lower, upper);
            } else {
                *value = i;
                return ch;
            }
        } else {
            // not present
            return ch;
        }
    }

    [[nodiscard]] vm_t get_argid(vm_t ch, int *arg_id) {
        auto manual_index = ((ch >= '0') && (ch <= '9'));
        auto using_automatic_indexing = (m_next_arg_id > 0);
        if ((manual_index && using_automatic_indexing) || (!manual_index && (m_next_arg_id == MANUAL_INDEXING))) {
            m_trx->error(Error::InvalidFormatString, "mixed use of manual and automatic indexing");
        } else {
            int argid = 0;
            if (manual_index) {
                // manual argument index
                int ndigits = 0;
                while ((ch >= '0') && (ch <= '9')) {
                    if (++ndigits > 9) {
                        m_trx->error(Error::RangeCheck, "format arg-id exceeds 9 digits");
                    } else {
                        argid = (argid * 10) + (ch - '0');
                        ch = next_format_ch();
                    }
                }
                m_next_arg_id = MANUAL_INDEXING;
            } else {
                // automatic argument index
                argid = m_next_arg_id++;
            }

            // Bound-check before touching the single-use bitmask below: process() verifies
            // m_args_count < 64, so this guarantees argid < 64 and the `1ull << argid` shift
            // can never be UB (a malformed manual id like {99} would otherwise shift >= 64).
            if (argid >= m_args_count) {
                m_trx->error(Error::RangeCheck, "arg-id {} exceeds arg-count {}", argid, m_args_count);
            } else {
                if (manual_index) {
                    // Bitmask tracks single-use for arg-ids 0..63; sufficient since format
                    // strings with > 63 replacement fields are impractical.
                    auto bit = (1ull << argid);
                    if ((m_args_referenced & bit) == 0) {
                        m_args_referenced |= bit;
                    } else {
                        m_trx->error(Error::ScanDuplicateArgId, "arg-id {} has already been specified", argid);
                    }
                }

                *arg_id = argid;
                return ch;
            }
        }
    }

    // Object        c d f s x ? ' '
    // ------        ---------------
    // Byte          x x     x x  c
    // Integer         x     x    d
    // Long            x     x    d
    // UInteger        x     x    d
    // ULong           x     x    d
    // Address         x     x    d
    // Real              x        f
    // Double            x        f
    // Boolean       x     x      c
    // String        x     x   x  s
    //               c d f s x ? ' '
    [[nodiscard]] verify_t get_format_spec_type(vm_t ch) {
        switch (ch) {
        case 'c':
            return (Trix::VerifyNull | Trix::VerifyByte | Trix::VerifyBoolean | Trix::VerifyString);

        case 'd':
            return (Trix::VerifyNull | Trix::VerifyIntegers | Trix::VerifyAddress);

        case 'x':
            return (Trix::VerifyNull | Trix::VerifyIntegers | Trix::VerifyAddress);

        case 'f':
            return (Trix::VerifyNull | Trix::VerifyFloats);

        case 's':
            return (Trix::VerifyNull | Trix::VerifyBoolean | Trix::VerifyString);

        case '?':
            return (Trix::VerifyNull | Trix::VerifyByte | Trix::VerifyString);

        case 'I':
            return (Trix::VerifyNull | Trix::VerifyULong);

        case 'D':
            return (Trix::VerifyNull | Trix::VerifyUInteger);

        case TYPE_DEFAULT:
            return (Trix::VerifyNull | Trix::VerifyNumbers | Trix::VerifyAddress | Trix::VerifyBoolean | Trix::VerifyString);

        default:
            m_trx->error(Error::InvalidFormatString, "invalid type specifier \'{:c}\'", ch);
        }
    }

    [[nodiscard]] static Align get_align(vm_t ch) {
        if (ch == '<') {
            return Align::Left;
        } else if (ch == '>') {
            return Align::Right;
        } else {
            assert(ch == '^');

            return Align::Center;
        }
    }

    [[nodiscard]] vm_t process_format_spec() {
        // [[fill] align] [width] ["." precision] [type]
        auto ch = next_format_ch();
        if (ch != '}') {
            // [[fill] align]
            // fill is any character except '{' or '}', defaults to ' '.
            //
            // alignment allows for skipping characters before and/or after value.
            // align: '<' align value to the left, skips fill characters after value
            // align: '>' align value to the right, skips fill characters before value
            // align: '^' align value to the center, skips fill character before and after value
            // If an alignment is specified, the value to be parsed is assumed to be
            // properly aligned with the specified fill character (defaults to ' ').
            //
            // For format type specifiers other than 'c' (default for Byte and Boolean, available for String)
            // leading whitespace is skipped regardless of alignment specifiers.
            // This preceding whitespace is not counted as part of the field width.
            // Any fill characters skipped are included in the width.
            //
            // If a field width is specified, it will be taken to be the minimum number of characters
            // to be consumed from input.  If a field precision is specified, it will be taken to be the
            // maximum number of characters to be consumed from input.
            //
            // If either field width or precision is specified, but no alignment is specified:
            //   For format type specifier 'c': the default alignment is '<'.
            //                       Otherwise: the default alignment is '>'.
            //
            // For the '^' alignment, fill characters both before and after the value will be considered.
            // The number of fill characters doesn't have to be equal: input will be parsed until either
            // a non-fill character is encountered, or the (maximum) field precision is exhausted, after
            // which checking is done for the (minimum) field width.
            //
            if ((ch == '<') || (ch == '>') || (ch == '^')) {
                // assume only align specified
                m_fill = FILL_DEFAULT;
                auto align_ch = ch;
                m_align = get_align(align_ch);

                if (m_format_ptr < m_format_limit) {
                    ch = static_cast<vm_t>(*m_format_ptr++);
                    if ((ch == '<') || (ch == '>') || (ch == '^')) {
                        // both fill and align are specified
                        m_fill = align_ch;
                        m_align = get_align(ch);

                        if (m_format_ptr < m_format_limit) {
                            ch = static_cast<vm_t>(*m_format_ptr++);
                        }
                    }
                }
            } else if (m_format_ptr < m_format_limit) {
                // First char was not an align char; check if second char is (making first the fill).
                auto maybe_align = *m_format_ptr;
                if ((maybe_align == '<') || (maybe_align == '>') || (maybe_align == '^')) {
                    if ((ch == '{') || (ch == '}')) {
                        m_trx->error(Error::InvalidFormatString, "fill specifier cannot be \'{:c}\'", ch);
                    } else {
                        // both fill and align are specified
                        m_fill = ch;
                        m_align = get_align(static_cast<vm_t>(maybe_align));
                        if (++m_format_ptr < m_format_limit) {
                            ch = static_cast<vm_t>(*m_format_ptr++);
                        }
                    }
                }
            }

            // [width]
            // This specifies the minimum number of characters that will be read from the input source.
            // Any fill characters skipped are included in the width.
            // If value length is less than minimum length, it is an error.
            if (ch == '0') {
                // leading-zero in width position is the PrintFmt zero-pad flag; ScanFmt does not have one
                m_trx->error(Error::InvalidFormatString, "ScanFmt does not support the zero-pad \'0\' flag");
            } else {
                ch = get_integer(ch, MIN_WIDTH, MAX_WIDTH, &m_width);

                // ["." precision]
                // This specifies the maximum number of characters that will be read from the input source.
                // The value length will always be less than/equal the maximum length.
                if (ch == '.') {
                    ch = next_format_ch();
                    ch = get_integer(ch, MIN_PRECISION, MAX_PRECISION, &m_precision);
                    if (m_precision == PRECISION_DEFAULT) {
                        m_trx->error(Error::InvalidFormatString, "missing precision after \'.\'");
                    }
                }

                // validate width vs precision
                if ((m_width != WIDTH_DEFAULT) && (m_precision != PRECISION_DEFAULT) && (m_width > m_precision)) {
                    m_trx->error(Error::InvalidFormatString,
                                 "width {} must be less than or equal to precision {}",
                                 m_width,
                                 m_precision);
                }

                // [type]
                // "c" | "d" | "f" | "s" | "?"
                // "I" (instant) and "D" (udate) absorb the rest of the spec body
                // as a strftime template; see process_chrono_format_spec.
                if (ch != '}') {
                    m_type = ch;
                    ch = next_format_ch();
                    if ((m_type == 'I') || (m_type == 'D')) {
                        ch = process_chrono_format_spec(ch);
                    }
                }
            }
        }
        return ch;
    }

    // After parsing a `:I` or `:D` type letter, capture the optional `l`
    // local-zone flag (instants only) and the strftime template body up to
    // the closing `}`.  On entry `ch` is the character immediately after
    // the type letter; on return it is the closing `}` and m_format_ptr
    // has advanced past it.
    [[nodiscard]] vm_t process_chrono_format_spec(vm_t ch) {
        if ((m_type == 'I') && (ch == 'l')) {
            m_chrono_local = true;
            ch = next_format_ch();
        }
        const char *template_start = (m_format_ptr - 1);
        while (ch != '}') {
            if (m_format_ptr >= m_format_limit) {
                m_trx->error(Error::InvalidFormatString, "premature end of format-string in :I strftime template");
            } else {
                ch = static_cast<vm_t>(*m_format_ptr++);
            }
        }

        auto template_len = static_cast<length_t>((m_format_ptr - 1) - template_start);
        if (template_len > MaxStrftimeTemplateLength) {
            m_trx->error(
                    Error::LimitCheck, "strftime template length {} exceeds maximum {}", template_len, MaxStrftimeTemplateLength);
        } else {
            // strptime does not implement '%f' (fractional seconds); reject
            // it now so users get a clear diagnostic during format-spec
            // parse rather than a generic /scan-type-fail later.  '%%f' is
            // a literal "%f" sequence, not a specifier, so step past `%%`.
            for (length_t i = 0; (i + 1) < template_len; ++i) {
                if (template_start[i] == '%') {
                    if (template_start[i + 1] == '%') {
                        ++i;
                    } else if (template_start[i + 1] == 'f') {
                        m_trx->error(Error::InvalidFormatString, "fractional-second specifier '%f' is not supported on :I/:D scan");
                    }
                }
            }
            m_chrono_template_ptr = template_start;
            m_chrono_template_len = template_len;
        }
        return ch;
    }

    [[nodiscard]] int peek_input() {
        if (m_input_stream != nullptr) {
            return m_input_stream->peekc(m_trx);
        } else {
            return ((m_input_length == 0) ? Trix::EOFc : *m_input_ptr);
        }
    }

    void consume_input() {
        if (m_input_stream != nullptr) {
            // Stream source
            auto ch = m_input_stream->getc(m_trx);

            if (ch == Trix::EOFc) {
                m_trx->error(Error::ScanInputFail, "unexpected end of stream");
            }
        } else {
            // String source
            if (m_input_length == 0) {
                m_trx->error(Error::ScanInputFail, "string input exhausted");
            } else {
                --m_input_length;
                ++m_input_ptr;
            }
        }
    }

    [[nodiscard]] int scan_numeric(Object *arg_ptr, int max_count) {
        // skip leading whitespace, does not count against width
        auto ch = skip_whitespace(peek_input());

        Number number(m_trx);
        auto token_base = m_trx->m_token_base;
        auto token_ptr = token_base;
        auto remaining_count =
                ((max_count == PRECISION_DEFAULT) || (max_count > Trix::MaxTokenLength)) ? Trix::MaxTokenLength : max_count;
        while ((remaining_count != 0) && (ch != Trix::EOFc) && Trix::is_numeric(ch)) {
            *token_ptr++ = static_cast<vm_t>(ch);

            --remaining_count;
            consume_input();

            ch = peek_input();
        }

        auto token_length = static_cast<length_t>(token_ptr - token_base);
        auto arg_type = arg_ptr->type();

        // When trailing fill is expected (left or center align) and the fill
        // character is also a valid numeric character (e.g. '-', '+', '.'),
        // the greedy scan may have consumed trailing fill chars as part of
        // the token.  If the initial parse fails, trim trailing fill chars
        // from the token and retry with progressively shorter lengths.
        // The trimmed fill chars remain consumed from the input; scan_trailing
        // will find no additional fill to consume, and the total consumed count
        // (returned as token_length) correctly includes the pre-consumed fill.
        auto has_trailing_fill = ((m_align == Align::Left) || (m_align == Align::Center));
        auto trim_fill = (has_trailing_fill && Trix::is_numeric(m_fill));
        auto try_length = token_length;

        while (try_length > 0) {
            // When arg_ptr is null, use the no-hint scan (auto-detect the natural type).
            // Otherwise use scan_strict: the target type acts as an implicit suffix for
            // unsuffixed input, but no implicit lossy conversion is performed.
            // :x format type routes through the hex-parsing path instead of scan_impl.
            auto token_sv = Trix::make_sv(token_base, try_length);
            bool parsed_ok;
            if (m_type == 'x') {
                parsed_ok = arg_ptr->is_null() ? number.scan_hex(token_sv) : number.scan_hex_strict(token_sv, arg_type);
            } else {
                parsed_ok = arg_ptr->is_null() ? number.scan(token_sv) : number.scan_strict(token_sv, arg_type);
            }
            if (parsed_ok) {
                if (arg_ptr->is_null()) {
                    *arg_ptr = number.make_object();
                } else {
                    auto number_type = number.type();
                    if (number_type == arg_type) {
                        // number (Byte, Integer, UInteger, Long, ULong, Address, Real, or Double)
                        switch (number_type) {
                        case Object::Type::Byte:
                            arg_ptr->update_byte(number.byte_value());
                            break;

                        case Object::Type::Integer:
                            arg_ptr->update_integer(number.integer_value());
                            break;

                        case Object::Type::UInteger:
                            arg_ptr->update_uinteger(number.uinteger_value());
                            break;

                        case Object::Type::Long:
                            arg_ptr->update_long(m_trx, number.long_value());
                            break;

                        case Object::Type::ULong:
                            arg_ptr->update_ulong(m_trx, number.ulong_value());
                            break;

                        case Object::Type::Int128:
                            arg_ptr->update_int128(m_trx, number.int128_value());
                            break;

                        case Object::Type::UInt128:
                            arg_ptr->update_uint128(m_trx, number.uint128_value());
                            break;

                        case Object::Type::Address:
                            arg_ptr->update_address(m_trx, number.address_value());
                            break;

                        case Object::Type::Real:
                            arg_ptr->update_real(number.real_value());
                            break;

                        case Object::Type::Double:
                            arg_ptr->update_double(m_trx, number.double_value());
                            break;

                        case Object::Type::Null:
                        case Object::Type::Boolean:
                        case Object::Type::Operator:
                        case Object::Type::Mark:
                        case Object::Type::Name:
                        case Object::Type::Array:
                        case Object::Type::Packed:
                        case Object::Type::String:
                        case Object::Type::Stream:
                        case Object::Type::Dict:
                        case Object::Type::Set:
                        case Object::Type::SourceLoc:
                        case Object::Type::Curry:
                        case Object::Type::Thunk:
                        case Object::Type::Tagged:
                        case Object::Type::Record:
                        case Object::Type::Coroutine:
                        case Object::Type::PipeBuffer:
                        case Object::Type::Cell:
                        case Object::Type::Continuation:
                        case Object::Type::OpaqueHandle:
                            assert(false && "scan_numeric: logic error");
                            std::unreachable();
                        }
                    } else {
                        m_trx->error(Error::ScanTypeMismatch, "scan_numeric: scanned value does not match target type");
                    }
                }
                // Return full consumed count (including any trailing fill chars the
                // greedy scan absorbed), not the trimmed try_length used for parsing.
                return token_length;
            } else {
                // trim one trailing fill char and retry
                if (trim_fill && (try_length > 0) && (token_base[try_length - 1] == m_fill)) {
                    --try_length;
                } else {
                    break;
                }
            }
        }

        auto clipped = std::min<length_t>(token_length, 32);
        auto shown = Trix::make_sv(token_base, clipped);
        if (token_length > clipped) {
            m_trx->error(Error::ScanTypeFail, "scan_numeric: \'{}...\' is not a valid number for the target type", shown);
        } else {
            m_trx->error(Error::ScanTypeFail, "scan_numeric: \'{}\' is not a valid number for the target type", shown);
        }
    }

    // string
    //   ' ', s: accepts text until a whitespace character, or fill character if left or center aligned
    //        ?: accepts escaped-text \\, \space, \f, \n, \r, \t, \v until a whitespace character, or fill character
    //        c: accepts until the field width (which must be specified) is exhausted
    [[nodiscard]] int scan_string(Object *arg_ptr, int consumed_count, int max_count) {
        if (m_type == TYPE_DEFAULT) {
            m_type = 's';
        }
        if ((m_type != 'c') && (m_type != 's') && (m_type != '?')) {
            m_trx->error(Error::InvalidFormatString, "unsupported format type '{:c}' for string", m_type);
        } else {
            auto exit_whitespace = (m_type != 'c');
            auto exit_fill = exit_whitespace && ((m_align == Align::Left) || (m_align == Align::Center));
            auto honor_escape = (m_type == '?');

            // reserve 1 byte for nul terminator
            auto [buffer_base, vm_remaining] = m_trx->vm_start_alloc<vm_t>();
            auto buffer_ptr = buffer_base;
            auto buffer_limit = &buffer_base[std::min(vm_remaining - 1, static_cast<vm_size_t>(Trix::MaxStringLength))];

            auto ch = peek_input();
            auto width = WIDTH_DEFAULT;
            if (m_type == 'c') {
                if ((m_width == WIDTH_DEFAULT) && (m_precision == PRECISION_DEFAULT)) {
                    // error width/precision not specified
                    m_trx->error(Error::InvalidFormatString, "field width required for String :c format");
                } else if (m_precision != PRECISION_DEFAULT) {
                    width = max_count;
                } else {
                    width = ((m_width <= consumed_count) ? 0 : (m_width - consumed_count));
                }
            } else {
                assert((m_type == 's') || (m_type == '?'));

                // skip leading whitespace, does not count against width
                ch = skip_whitespace(ch);
            }

            auto valid = true;
            auto scanned_length = 0;
            while (true) {
                if ((exit_whitespace && Trix::is_whitespace(ch)) || (exit_fill && (ch == m_fill)) || (width == 0)) {
                    break;
                } else {
                    if (ch == Trix::EOFc) {
                        m_trx->error(Error::ScanInputFail, "EndOfStream encountered during ScanFmt");
                    } else if ((ch == '\\') && honor_escape) {
                        consume_input();
                        ch = peek_input();
                        if (ch == Trix::EOFc) {
                            m_trx->error(Error::ScanInputFail, "EndOfStream encountered during character escape ScanFmt");
                        } else if ((ch != '\\') && (ch != ' ')) {
                            auto unesc = Trix::unescape_letter(static_cast<vm_t>(ch));
                            if (unesc.has_value()) {
                                ch = unesc.value();
                            } else {
                                m_trx->error(Error::ScanInputFail, "unrecognized character escape during ScanFmt");
                            }
                        }

                        // Count the backslash; the escaped char itself is counted below.
                        ++scanned_length;
                    }

                    if (buffer_ptr >= buffer_limit) {
                        // stop immediately on overflow; do not drain input
                        valid = false;
                        break;
                    } else if (width != WIDTH_DEFAULT) {
                        --width;
                    }
                    ++scanned_length;
                    *buffer_ptr++ = static_cast<vm_t>(ch);

                    consume_input();
                    ch = peek_input();
                }
            }

            if (valid) {
                auto string_length = static_cast<length_t>(buffer_ptr - buffer_base);
                if (arg_ptr->is_null()) {
                    buffer_base[string_length] = '\0';
                    auto [_, offset] = m_trx->vm_end_alloc<vm_t>(string_length + 1);
                    *arg_ptr = Object::make_string(offset, string_length);
                } else {
                    // Copy scanned content into the existing string object (must be writable and large enough).
                    if (arg_ptr->has_readonly_access()) {
                        m_trx->vm_end_alloc();
                        m_trx->error(Error::InvalidAccess, "string is readonly");
                    } else {
                        auto vptr = arg_ptr->string_vptr(m_trx);
                        auto len = arg_ptr->string_length();
                        if (len < string_length) {
                            m_trx->vm_end_alloc();
                            m_trx->error(Error::ScanInputFail, "string is too small");
                        } else {
                            // Release the dynamic-alloc window before set_string_length,
                            // which may materialize an eqstring ref and itself call vm_alloc.
                            m_trx->vm_end_alloc();
                            std::copy_n(buffer_base, string_length, vptr);
                            arg_ptr->set_string_length(m_trx, string_length);
                        }
                    }
                }
                return scanned_length;
            } else {
                m_trx->vm_end_alloc();
                if (vm_remaining > static_cast<vm_size_t>(Trix::MaxStringLength)) {
                    m_trx->error(Error::LimitCheck, "(string) length exceeds maximum {}", Trix::MaxStringLength);
                } else {
                    m_trx->error(Error::VMFull, "while scanning a (string)");
                }
            }
        }
    }

    // byte
    //   c, ' ': accept next char
    //        ?: accept next char or escaped-char \\, \space, \f, \n, \r, \t, \v
    //        d: accept int
    //        x: accept hex int (optional 0x/0X prefix)
    [[nodiscard]] int scan_byte(Object *arg_ptr, int max_count) {
        if (m_type == TYPE_DEFAULT) {
            m_type = 'c';
        }
        if ((m_type != 'c') && (m_type != 'd') && (m_type != 'x') && (m_type != '?')) {
            m_trx->error(Error::InvalidFormatString, "unsupported format type '{:c}' for byte", m_type);
        } else {
            auto ch = peek_input();
            if (ch != Trix::EOFc) {
                vm_t value;
                length_t token_length;
                if ((m_type == 'c') || (m_type == '?')) {
                    consume_input();

                    if ((ch == '\\') && (m_type == '?')) {
                        // escaped character
                        if ((max_count != PRECISION_DEFAULT) && (max_count < 2)) {
                            m_trx->error(Error::ScanTypeFail, "scan_byte: escape sequence exceeds field width");
                        } else {
                            ch = peek_input();
                            if (ch == Trix::EOFc) {
                                m_trx->error(Error::ScanInputFail, "EndOfStream encountered during character escape ScanFmt");
                            } else if ((ch != '\\') && (ch != ' ')) {
                                auto unesc = Trix::unescape_letter(static_cast<vm_t>(ch));
                                if (unesc.has_value()) {
                                    ch = unesc.value();
                                } else {
                                    m_trx->error(Error::ScanInputFail, "unrecognized character escape during ScanFmt");
                                }
                            }

                            consume_input();
                            token_length = 2;
                        }
                    } else {
                        // standard character
                        token_length = 1;
                    }
                    value = static_cast<vm_t>(ch);
                } else {
                    assert((m_type == 'd') || (m_type == 'x'));

                    // skip leading whitespace, does not count against width
                    ch = skip_whitespace(ch);

                    Number number(m_trx);
                    auto token_base = m_trx->m_token_base;
                    auto token_ptr = token_base;
                    auto remaining_count = ((max_count == PRECISION_DEFAULT) || (max_count > Trix::MaxTokenLength))
                                                   ? Trix::MaxTokenLength
                                                   : max_count;
                    while ((remaining_count != 0) && (ch != Trix::EOFc) && Trix::is_numeric(ch)) {
                        *token_ptr++ = static_cast<vm_t>(ch);

                        --remaining_count;
                        consume_input();

                        ch = peek_input();
                    }
                    token_length = static_cast<length_t>(token_ptr - token_base);

                    auto token_sv = Trix::make_sv(token_base, token_length);
                    auto parsed_ok = (m_type == 'x') ? number.scan_hex_strict(token_sv, Object::Type::Byte)
                                                     : number.scan_strict(token_sv, Object::Type::Byte);
                    if (parsed_ok) {
                        value = number.byte_value();
                    } else {
                        auto clipped = std::min<length_t>(token_length, 32);
                        auto shown = Trix::make_sv(token_base, clipped);
                        if (token_length > clipped) {
                            m_trx->error(Error::ScanTypeFail, "scan_byte: \'{}...\' is not a valid byte literal", shown);
                        } else {
                            m_trx->error(Error::ScanTypeFail, "scan_byte: \'{}\' is not a valid byte literal", shown);
                        }
                    }
                }
                if (arg_ptr->is_null()) {
                    *arg_ptr = Object::make_byte(value);
                } else {
                    arg_ptr->update_byte(value);
                }
                return static_cast<int>(token_length);
            } else {
                m_trx->error(Error::ScanInputFail, "EndOfStream encountered during ScanFmt");
            }
        }
    }

    // boolean
    //   c, ' ': accepts next char 0, 1, f, t, F, T
    //        s: accepts false, true (any mixture of case)
    [[nodiscard]] int scan_boolean(Object *arg_ptr, int max_count) {
        if (m_type == TYPE_DEFAULT) {
            m_type = 'c';
        }
        if ((m_type != 'c') && (m_type != 's')) {
            m_trx->error(Error::InvalidFormatString, "unsupported format type '{:c}' for boolean", m_type);
        } else {
            auto ch = peek_input();
            if (ch != Trix::EOFc) {
                if (m_type == 'c') {
                    consume_input();

                    auto lower_ch = Trix::to_lowercase(ch);
                    if ((lower_ch == '0') || (lower_ch == 'f')) {
                        arg_ptr->update_boolean(false);
                    } else if ((lower_ch == '1') || (lower_ch == 't')) {
                        arg_ptr->update_boolean(true);
                    } else {
                        m_trx->error(Error::ScanTypeMismatch,
                                     "'boolean' scan format 'c' expects '0', '1', 'f', 't', 'F', or 'T, encountered {}",
                                     ch);
                    }
                    return 1;
                } else {
                    assert(m_type == 's');

                    // skip leading whitespace, does not count against width
                    ch = skip_whitespace(ch);

                    if (ch != Trix::EOFc) {
                        using namespace std::literals::string_view_literals;

                        char buffer[8] = {static_cast<char>(ch), '\0', '\0', '\0', '\0', '\0', '\0', '\0'};
                        auto false_sv = "FALSE"sv;
                        auto true_sv = "TRUE"sv;
                        auto upper_ch = Trix::to_uppercase(ch);
                        auto [sv, value] = [&]() -> std::pair<std::string_view *, bool> {
                            if (upper_ch == 'F') {
                                return std::pair{&false_sv, false};
                            } else if (upper_ch == 'T') {
                                return std::pair{&true_sv, true};
                            } else {
                                return std::pair{nullptr, false};
                            }
                        }();

                        if (sv != nullptr) {
                            auto length = Trix::sv_length(*sv);
                            if ((max_count != PRECISION_DEFAULT) && (max_count < length)) {
                                m_trx->error(Error::ScanTypeFail, "scan_boolean: field width too small for boolean literal");
                            } else {
                                auto ptr = buffer;
                                auto target = sv->data();
                                while ((ch != Trix::EOFc) && (upper_ch == *target++)) {
                                    consume_input();
                                    if (--length == 0) {
                                        arg_ptr->update_boolean(value);
                                        return Trix::sv_length(*sv);
                                    } else {
                                        *ptr++ = static_cast<char>(ch);

                                        ch = peek_input();
                                        upper_ch = Trix::to_uppercase(ch);
                                    }
                                }
                            }
                        }

                        if (ch != Trix::EOFc) {
                            m_trx->error(Error::ScanTypeMismatch,
                                         "'boolean' scan format 's' expects 'false', or 'true', encountered {}",
                                         buffer);
                        }
                    }
                }
            }
            m_trx->error(Error::ScanInputFail, "EndOfStream encountered during ScanFmt");
        }
    }

    [[nodiscard]] int consume_fill(int max_count) {
        auto count = 0;
        auto ch = peek_input();
        while ((ch != Trix::EOFc) && (count != max_count) && (ch == m_fill)) {
            ++count;
            consume_input();
            ch = peek_input();
        }
        return count;
    }

    [[nodiscard]] int consume_whitespace(int max_count) {
        auto count = 0;
        auto ch = peek_input();
        while ((ch != Trix::EOFc) && (count != max_count) && Trix::is_whitespace(ch)) {
            ++count;
            consume_input();
            ch = peek_input();
        }
        return count;
    }

    [[nodiscard]] int skip_whitespace(int ch) {
        while ((ch != Trix::EOFc) && Trix::is_whitespace(ch)) {
            consume_input();
            ch = peek_input();
        }
        return ch;
    }

    [[nodiscard]] int scan_leading(int max_count) {
        if ((m_align == Align::Right) || (m_align == Align::Center)) {
            return consume_fill(max_count);
        } else if (m_align == Align::DefaultRight) {
            return consume_whitespace(max_count);
        } else {
            return 0;
        }
    }

    [[nodiscard]] int scan_trailing(int max_count, int consumed_count) {
        if ((m_align == Align::Left) || (m_align == Align::Center)) {
            return consume_fill(max_count);
        } else if ((m_align == Align::DefaultLeft) &&
                   // Only consume trailing whitespace when a width/precision was specified
                   // and not yet satisfied -- without explicit align, trailing whitespace
                   // is only meaningful if the field has a minimum extent.
                   (((m_width != WIDTH_DEFAULT) && (consumed_count < m_width)) ||
                    ((m_precision != PRECISION_DEFAULT) && (consumed_count < m_precision)))) {
            return consume_whitespace(max_count);
        } else {
            return 0;
        }
    }

    [[nodiscard]] int scan_value(Object *arg_ptr, int consumed_count, int max_count) {
        switch (arg_ptr->type()) {
        case Object::Type::Null:
            if ((m_type == 'd') || (m_type == 'f') || (m_type == 'x')) {
                return scan_numeric(arg_ptr, max_count);
            } else if (m_type == 'c') {
                return scan_byte(arg_ptr, max_count);
            } else {
                assert((m_type == 's') || (m_type == '?'));

                return scan_string(arg_ptr, consumed_count, max_count);
            }

        case Object::Type::Byte:
            return scan_byte(arg_ptr, max_count);

        case Object::Type::Integer:
        case Object::Type::UInteger:
        case Object::Type::Long:
        case Object::Type::ULong:
        case Object::Type::Int128:
        case Object::Type::UInt128:
        case Object::Type::Address:
        case Object::Type::Real:
        case Object::Type::Double:
            return scan_numeric(arg_ptr, max_count);

        case Object::Type::Boolean:
            return scan_boolean(arg_ptr, max_count);

        case Object::Type::String:
            return scan_string(arg_ptr, consumed_count, max_count);

        case Object::Type::Operator:
        case Object::Type::Mark:
        case Object::Type::Name:
        case Object::Type::Array:
        case Object::Type::Packed:
        case Object::Type::Stream:
        case Object::Type::Dict:
        case Object::Type::Set:
        case Object::Type::SourceLoc:
        case Object::Type::Curry:
        case Object::Type::Thunk:
        case Object::Type::Tagged:
        case Object::Type::Record:
        case Object::Type::Coroutine:
        case Object::Type::PipeBuffer:
        case Object::Type::Cell:
        case Object::Type::Continuation:
        case Object::Type::OpaqueHandle:
        default:
            assert(false && "scan_value: logic error");
            std::unreachable();
        }
    }

    // Pull the current unread input window for chrono parse.  For memory
    // input that's the (m_input_ptr, m_input_length) slice; for stream input
    // it's the buffered window from Stream::space() after forcing one fill
    // if the buffer is exhausted.  Stream callers see only what's currently
    // buffered: a record straddling a fill boundary will fail with
    // ScanTypeFail and must be re-fetched via read-string + sscan-fmt.
    std::string_view chrono_input_view() {
        if (m_input_stream != nullptr) {
            auto [ptr, len] = m_input_stream->space(m_trx);
            if (len == 0) {
                static_cast<void>(m_input_stream->fill(m_trx));
                std::tie(ptr, len) = m_input_stream->space(m_trx);
            }
            return std::string_view{reinterpret_cast<const char *>(ptr), len};
        } else {
            return std::string_view{reinterpret_cast<const char *>(m_input_ptr), m_input_length};
        }
    }

    // Advance the input cursor past N consumed bytes.  Mirrors
    // chrono_input_view(): updates the right cursor for whichever input
    // source is active.
    void chrono_advance_input(length_t consumed) {
        if (m_input_stream != nullptr) {
            m_input_stream->advance_read(consumed);
        } else {
            m_input_ptr += consumed;
            m_input_length = static_cast<length_t>(m_input_length - consumed);
        }
    }

    // Copy the strftime template into a null-terminated stack buffer for
    // strptime.  process_chrono_format_spec already capped the template at
    // MaxStrftimeTemplateLength, so the buffer cannot overflow.
    void copy_chrono_template(char *out) {
        std::memcpy(out, m_chrono_template_ptr, m_chrono_template_len);
        out[m_chrono_template_len] = '\0';
    }

    // Copy a bounded prefix of the remaining scan input into a null-terminated
    // stack buffer for strptime.  Returns the number of bytes copied (which
    // bounds the maximum number of bytes strptime can consume).  Records
    // longer than MaxStrftimeInputLength will fail to parse; callers should
    // read-string + sscan-fmt for streamed long records.
    [[nodiscard]] length_t copy_chrono_input(std::string_view input_sv, char *out) {
        auto in_len = std::min<size_t>(input_sv.size(), MaxStrftimeInputLength);
        std::memcpy(out, input_sv.data(), in_len);
        out[in_len] = '\0';
        return static_cast<length_t>(in_len);
    }

    // Parse a packed udate from the remaining input using the strftime
    // template captured in m_chrono_*.  Year out of udate range or invalid
    // calendar dates surface as ScanTypeFail.  Updates the UInteger arg and
    // advances the input cursor by the number of bytes consumed.
    void scan_udate(Object *arg_ptr) {
        if ((m_chrono_template_ptr == nullptr) || (m_chrono_template_len == 0)) {
            m_trx->error(Error::InvalidFormatString, "empty :D strftime template");
        } else {
            auto input_sv = chrono_input_view();

            char tmpl_buf[MaxStrftimeTemplateLength + 1];
            char input_buf[MaxStrftimeInputLength + 1];
            copy_chrono_template(tmpl_buf);
            static_cast<void>(copy_chrono_input(input_sv, input_buf));

            tm parts{};
            auto prev_locale = uselocale(Trix::c_time_locale());
            char *end = strptime(input_buf, tmpl_buf, &parts);
            uselocale(prev_locale);

            auto parse_ok = (end != nullptr);
            auto year = parse_ok ? (parts.tm_year + 1900) : 0;
            std::chrono::year_month_day ymd{};
            if (parse_ok) {
                ymd = std::chrono::year_month_day{std::chrono::year{year},
                                                  std::chrono::month{static_cast<unsigned>(parts.tm_mon + 1)},
                                                  std::chrono::day{static_cast<unsigned>(parts.tm_mday)}};
                parse_ok = ymd.ok() && (year >= 0) && (year <= Trix::UDATE_YEAR_MAX);
            }
            if (!parse_ok) {
                auto clipped = std::min<size_t>(input_sv.size(), 32);
                m_trx->error(Error::ScanTypeFail, "scan :D failed to parse '{}'", std::string_view{input_sv.data(), clipped});
            } else {
                auto consumed = static_cast<length_t>(end - input_buf);
                chrono_advance_input(consumed);
                arg_ptr->update_uinteger(m_trx->pack_udate_from_ymd(ymd));
                ++m_scanned_count;
            }
        }
    }

    // Parse a chrono instant from the remaining input using the strftime
    // template captured in m_chrono_*.  m_chrono_local selects the local
    // zone (UTC otherwise).  Updates the ULong arg with ms since 1970 UTC
    // and advances the input cursor by the number of bytes consumed.  Stream
    // input parses against the currently-buffered window; records longer
    // than MaxStrftimeInputLength surface as ScanTypeFail (callers should
    // read-string + sscan-fmt for streamed long records).  Pre-1970 instants
    // surface as ScanTypeFail (Trix instants are ULong, so negative ms
    // cannot be represented).  Raises Error::Unsupported if local is
    // requested but the system zone database is unavailable.
    void scan_instant(Object *arg_ptr) {
        if ((m_chrono_template_ptr == nullptr) || (m_chrono_template_len == 0)) {
            m_trx->error(Error::InvalidFormatString, "empty :I strftime template");
        } else {
            auto input_sv = chrono_input_view();

            char tmpl_buf[MaxStrftimeTemplateLength + 1];
            char input_buf[MaxStrftimeInputLength + 1];
            copy_chrono_template(tmpl_buf);
            static_cast<void>(copy_chrono_input(input_sv, input_buf));

            tm parts{};
            auto prev_locale = uselocale(Trix::c_time_locale());
            char *end = strptime(input_buf, tmpl_buf, &parts);
            uselocale(prev_locale);

            auto parse_ok = (end != nullptr);
            std::chrono::year_month_day ymd{};
            if (parse_ok) {
                ymd = std::chrono::year_month_day{std::chrono::year{parts.tm_year + 1900},
                                                  std::chrono::month{static_cast<unsigned>(parts.tm_mon + 1)},
                                                  std::chrono::day{static_cast<unsigned>(parts.tm_mday)}};
                parse_ok = ymd.ok();
            }

            using std::chrono::duration_cast;
            using std::chrono::milliseconds;
            using std::chrono::seconds;
            using std::chrono::sys_days;
            using std::chrono::sys_time;
            int64_t ms_value = 0;
            if (parse_ok) {
                auto secs_of_day = seconds(parts.tm_hour * 3600 + parts.tm_min * 60 + parts.tm_sec);
                if (m_chrono_local) {
                    const std::chrono::local_time<seconds> local_tp{(sys_days{ymd} + secs_of_day).time_since_epoch()};
                    const auto *zone = m_trx->current_local_zone_or_raise();
                    auto sys_tp = zone->to_sys(local_tp);
                    ms_value = duration_cast<milliseconds>(sys_tp.time_since_epoch()).count();
                } else {
                    const sys_time<seconds> utc_tp = sys_days{ymd} + secs_of_day;
                    ms_value = duration_cast<milliseconds>(utc_tp.time_since_epoch()).count();
                }
            }

            if (!parse_ok || (ms_value < 0)) {
                auto clipped = std::min<size_t>(input_sv.size(), 32);
                m_trx->error(Error::ScanTypeFail, "scan :I failed to parse '{}'", std::string_view{input_sv.data(), clipped});
            } else {
                auto consumed = static_cast<length_t>(end - input_buf);
                chrono_advance_input(consumed);
                arg_ptr->update_ulong(m_trx, static_cast<ulong_t>(ms_value));
                ++m_scanned_count;
            }
        }
    }

    void scan_input(int arg_id) {
        // replacement-field "{" [arg-id] [":" format-spec] "}"
        auto arg_ptr = &m_args_ptr[arg_id];
        auto format_spec_type = get_format_spec_type(m_type);

        if (arg_ptr->is_null() && (m_type == TYPE_DEFAULT)) {
            m_trx->error(Error::ScanTypeFail, "type specifier must be provided for null Object arg-id {}", arg_id);
        } else if ((format_spec_type & Trix::TypeToVerify(arg_ptr->type())) == 0) {
            char buffer[24];
            static_cast<void>(m_trx->object_name(arg_ptr, buffer));
            m_trx->error(Error::TypeCheck, "type specifier \'{:c}\' not supported for {} arg-id {}", m_type, buffer, arg_id);
        } else if (m_type == 'I') {
            scan_instant(arg_ptr);
        } else if (m_type == 'D') {
            scan_udate(arg_ptr);
        } else {
            if (m_align == Align::None) {
                // alignment was not specified, set default based upon arg type
                // {:c} align left, all others align right
                m_align = ((m_type == 'c') || ((m_type == TYPE_DEFAULT) && (arg_ptr->is_byte() || arg_ptr->is_boolean())))
                                  ? Align::DefaultLeft
                                  : Align::DefaultRight;
            }

            auto leading_count = scan_leading(m_precision);
            auto consumed_count = leading_count;
            auto remaining_count = (m_precision == PRECISION_DEFAULT) ? m_precision : (m_precision - leading_count);
            if (remaining_count == 0) {
                m_trx->error(Error::ScanInputFail, "leading fill exhausted maximum width");
            } else {
                auto value_count = scan_value(arg_ptr, consumed_count, remaining_count);
                consumed_count += value_count;

                remaining_count = ((remaining_count == PRECISION_DEFAULT) ? remaining_count : (remaining_count - value_count));
                auto trailing_count = scan_trailing(remaining_count, consumed_count);
                consumed_count += trailing_count;

                if ((m_width != WIDTH_DEFAULT) && (consumed_count < m_width)) {
                    m_trx->error(Error::ScanInputFail, "scan did not satisfy the required minimum width");
                } else {
                    assert((m_precision == PRECISION_DEFAULT) || ((leading_count + value_count + trailing_count) <= m_precision));
                    ++m_scanned_count;
                }
            }
        }
    }

    uint64_t m_args_referenced;
    Trix *m_trx;
    Object *m_args_ptr;
    const char *m_format_limit;
    const char *m_format_ptr;
    Stream *m_input_stream;
    vm_t *m_input_ptr;
    int m_next_arg_id;
    int m_width{WIDTH_DEFAULT};
    int m_precision{PRECISION_DEFAULT};
    int m_scanned_count;
    length_t m_args_count;
    length_t m_input_length;
    vm_t m_fill{FILL_DEFAULT};
    Align m_align{Align::None};
    vm_t m_type{TYPE_DEFAULT};
    // Strftime template body for the `:I` (instant) format-spec.  When
    // m_type == 'I' this slice points into the format string between the
    // type letter (and optional 'l' local-zone flag) and the closing '}'.
    const char *m_chrono_template_ptr{nullptr};
    length_t m_chrono_template_len{0};
    bool m_chrono_local{false};
};
