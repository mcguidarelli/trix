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

//===--- PrintFmt: format-string output engine ---===//
//
// Implements Python-style format strings for structured output.  Based on:
//
//   Python PEP 3101 / str.format(): replacement fields with positional and
//   named arguments, format specs for alignment, padding, precision, and
//   type-specific formatting.  The grammar is a subset of Python's format
//   mini-language.
//
//   C printf: type specifiers (d, x, X, b, o, c, e, f, g, s) and
//   width/precision.  Trix adds Trix-specific types: T (type name),
//   O (object form), ? (boolean as "true"/"false"), I (chrono instant;
//   ULong ms since 1970 UTC formatted via strftime template), and D
//   (udate; UInteger packed date formatted via strftime template).
//
//   {fmt} (C++ library): the fill-and-align model, nested field references
//   for dynamic width/precision ({:{width}.{precision}}).
//
// --- Core concepts for maintainers ---
//
// USAGE
//   PrintFmt is used by several operators:
//     print-fmt    -- format to stdout (mark-based args)
//     fprint-fmt   -- format to a stream
//     sprint-fmt   -- format to a string
//     aprint-fmt   -- format to stdout (array args)
//     afprint-fmt  -- format to a stream (array args)
//     asprint-fmt  -- format to a string (array args)
//   Also used internally by `=` and `==` (object display) and string
//   interpolation (\{name} inside string literals).
//
// TWO OUTPUT MODES
//   Stream output: characters are written to a Stream via putc/putn.
//   Buffer output: characters are written to a vm_t[] memory buffer with
//     a capacity limit.  Overflow is tracked (dropped_count) but does not
//     error -- the caller checks the result.
//
// FORMAT SPEC PARSING
//   Each replacement field {[arg-id][:format-spec]} is parsed left to
//   right.  The format spec follows Python conventions:
//     [[fill]align][sign][#][0][width][.precision][type]
//   Width and precision can be literal integers or nested field references
//   ({N} to read width/precision from the argument list).
//
// TYPE DISPATCH
//   Each Trix Object type has a dedicated print function (print_integer,
//   print_real, print_string, etc.) that handles the type's format spec
//   interpretation.  Integer types support d/x/X/b/o/c; floats support
//   e/E/f/F/g/G; strings support s/x/?; booleans support ?; ULong adds
//   I (chrono instant via strftime template); UInteger adds D (udate via
//   strftime template); all types support T (type name) and O (object
//   form).  The "#" alt-flag adds 0x/0X/0b/0o
//   prefixes to integer output.
//
// ARGUMENT ACCESS
//   Arguments are accessed by positional index (implicit auto-increment
//   or explicit {N}).  Out-of-range indices raise Error::RangeCheck.
//   Mixing implicit and explicit indexing is not allowed.
//
// Format-string grammar:
// format-string     ::= (literal-char | escaped-char | replacement-field)*
// literal-char      ::= <all characters except "{" and "}">
// escaped-char      ::= "{{" | "}}"
//
// replacement-field ::= "{" [arg-id] [":" format-spec] "}"
// arg-id            ::= integer
// integer           ::= digit+
// digit             ::= "0" | "1" | "2" | "3" | "4" | "5" | "6" | "7" | "8" | "9"
//
// format-spec       ::= [[fill] align] [sign] ["#"] ["0"] [width] ["." precision] [type]
// fill              ::= literal-char
// align             ::= "<" | ">" | "^"
// sign              ::= "+" | "-" | " "
// width             ::= integer | "{" [arg-id] "}"
// precision         ::= integer | "{" [arg-id] "}"
// type              ::= "b" | "c" | "d" | "e|E" | "f|F" | "g|G" | "o" | "s" | "T" | "x" | "X" | "O" | "?"
//                     | "I" [ "l" ] strftime-template       % chrono instant; "l" selects local zone
//                     | "D" strftime-template               % chrono udate (UInteger packed date); no "l" flag
// strftime-template ::= <strftime spec body up to closing '}'>
//
public:
class PrintFmt {
public:
    // output to stream
    PrintFmt(Trix *trx, const char *format, length_t length, const Object *args_ptr, length_t count, Stream *output) :
            PrintFmt(trx, args_ptr, format, format + length, count, output, nullptr, 0) {}

    // output to memory buffer
    PrintFmt(Trix *trx,
             const char *format,
             length_t length,
             const Object *args_ptr,
             length_t count,
             vm_t *output,
             length_t capacity) :
            PrintFmt(trx, args_ptr, format, format + length, count, nullptr, output, capacity) {}

    PrintFmt() = delete;
    PrintFmt(const PrintFmt &) = delete;
    PrintFmt &operator=(const PrintFmt &) = delete;

    [[nodiscard]] std::pair<int, int> process() {
        while (m_format_ptr < m_format_limit) {
            auto ch = *m_format_ptr++;
            if ((ch == '{') && (m_format_ptr < m_format_limit)) {
                ch = *m_format_ptr++;
                if (ch == '{') {
                    // escaped-char "{{"
                    emit(static_cast<vm_t>(ch));
                } else {
                    // replacement-field "{" [arg-id] [":" format-spec] "}"
                    auto arg_id = 0;
                    ch = static_cast<char>(get_argid(static_cast<vm_t>(ch), &arg_id));

                    default_format_spec();
                    if (ch == ':') {
                        ch = static_cast<char>(process_format_spec());
                    }

                    if (ch == '}') {
                        process_output(arg_id);
                    } else {
                        m_trx->error(Error::InvalidFormatString, "encountered \'{:c}\' instead of \'}}\' after format-spec", ch);
                    }
                }
            } else if (ch == '}') {
                if (m_format_ptr < m_format_limit) {
                    ch = *m_format_ptr++;
                    if (ch == '}') {
                        // escaped-char "}}"
                        emit(static_cast<vm_t>(ch));
                    } else {
                        m_trx->error(Error::InvalidFormatString,
                                     "unmatched \'}}\' in format-string; use \'}}}}\' for a literal \'}}\'");
                    }
                } else {
                    m_trx->error(Error::InvalidFormatString, "lone \'}}' at end of format string");
                }
            } else if (ch == '{') {
                m_trx->error(Error::InvalidFormatString, "lone \'{{' at end of format string");
            } else {
                // literal-char
                emit(static_cast<vm_t>(ch));
            }
        }
        // stderr flush upon every completed output
        if ((m_output_stream == m_trx->m_stderr) && (m_output_count != 0)) {
            m_output_stream->flush(m_trx);
        }
        return std::pair{m_output_count, m_dropped_count};
    }

    [[nodiscard]] static int process_object(Trix *trx, const Object *val_ptr, Stream *output, bool object_form = false) {
        PrintFmt fmt(trx, output, object_form);
        fmt.print_object(val_ptr);
        auto output_count = fmt.m_output_count;
        if ((output == trx->m_stderr) && (output_count != 0)) {
            output->flush(trx);
        }
        return output_count;
    }

    [[nodiscard]] static std::pair<int, int>
    process_object(Trix *trx, const Object *val_ptr, vm_t *output, length_t capacity, bool object_form = false) {
        PrintFmt fmt(trx, output, capacity, object_form);
        fmt.print_object(val_ptr);
        return std::pair{fmt.m_output_count, fmt.m_dropped_count};
    }
private:
    static constexpr int MAX_WIDTH{1024};
    // Precision is parsed once with the most permissive upper bound, then each
    // print path enforces its own tighter limit at use time:
    //   numeric radix  -> 2..36 (compute_has_radix)
    //   float digits   -> 0..MAX_FLOAT_PRECISION (print_float_impl)
    //   string max-len -> 0..MAX_STRING_PRECISION (string_outputter)
    static constexpr int MAX_INT_PRECISION{36};
    static constexpr int MAX_FLOAT_PRECISION{34};
    static constexpr int MAX_STRING_PRECISION{MAX_WIDTH};
    static constexpr int MANUAL_INDEXING{-1};

    static constexpr int WIDTH_DEFAULT{-1};
    static constexpr int PRECISION_DEFAULT{-1};
    static constexpr vm_t FILL_DEFAULT{' '};
    static constexpr vm_t ALIGN_DEFAULT{' '};
    static constexpr vm_t SIGN_DEFAULT{'-'};
    static constexpr uint8_t ALT_DEFAULT{false};
    static constexpr uint8_t ZERO_DEFAULT{false};
    static constexpr vm_t TYPE_DEFAULT{' '};

    // single-object output to stream
    PrintFmt(Trix *trx, Stream *output, bool object_form) : PrintFmt(trx, nullptr, nullptr, nullptr, 1, output, nullptr, 0) {
        default_format_spec();
        if (object_form) {
            m_type = 'O';
        }
    }

    // single-object output to memory buffer
    PrintFmt(Trix *trx, vm_t *output, length_t capacity, bool object_form) :
            PrintFmt(trx, nullptr, nullptr, nullptr, 1, nullptr, output, capacity) {
        default_format_spec();
        if (object_form) {
            m_type = 'O';
        }
    }

    // common delegated constructor
    PrintFmt(Trix *trx,
             const Object *args_ptr,
             const char *format_ptr,
             const char *format_limit,
             length_t args_count,
             Stream *output_stream,
             vm_t *output_ptr,
             length_t output_capacity) :
            m_trx{trx},
            m_args_ptr{args_ptr},
            m_format_limit{format_limit},
            m_format_ptr{format_ptr},
            m_output_stream{output_stream},
            m_output_ptr{output_ptr},
            m_next_arg_id{0},
            m_args_count{args_count},
            m_output_capacity{output_capacity},
            m_output_count{0},
            m_dropped_count{0},
            m_depth{0} {}

    void default_format_spec() {
        m_width = WIDTH_DEFAULT;
        m_precision = PRECISION_DEFAULT;
        m_fill = FILL_DEFAULT;
        m_align = ALIGN_DEFAULT;
        m_sign = SIGN_DEFAULT;
        m_alt = ALT_DEFAULT;
        m_zero = ZERO_DEFAULT;
        m_type = TYPE_DEFAULT;
        m_chrono_template_ptr = nullptr;
        m_chrono_template_len = 0;
        m_chrono_local = false;
    }

    [[nodiscard]] vm_t next_format_ch() {
        if (m_format_ptr < m_format_limit) {
            return static_cast<vm_t>(*m_format_ptr++);
        } else {
            m_trx->error(Error::InvalidFormatString, "premature end of format-string encountered");
        }
    }

    [[nodiscard]] vm_t get_integer(vm_t ch, int lower, int upper, int *value) {
        // (integer | "{" [arg_id] "}")
        if ((ch >= '0') && (ch <= '9')) {
            // integer
            int i = 0;
            int digits = 0;
            while ((ch >= '0') && (ch <= '9')) {
                if (++digits > 9) {
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
        } else if (ch == '{') {
            // "{" [arg-id] "}" -- a nested field reference supplies the width/precision.
            ch = next_format_ch();

            auto arg_id = 0;
            ch = get_argid(ch, &arg_id);  // shared mixed-index / 9-digit / arg-count checks

            auto i = 0;
            auto arg_ptr = &m_args_ptr[arg_id];
            if (arg_ptr->is_integral()) {
                auto [valid, val] = arg_ptr->integer_value(m_trx, lower, upper);
                if (valid) {
                    i = val;
                } else {
                    m_trx->error(Error::RangeCheck, "arg-id {} value {} not within range of {}..{}", arg_id, val, lower, upper);
                }
            } else {
                m_trx->error(Error::TypeCheck, "arg-id {} is not an integer", arg_id);
            }

            if (ch == '}') {
                *value = i;
                ch = next_format_ch();
                return ch;
            } else {
                m_trx->error(Error::InvalidFormatString, "encountered \'{:c}\' instead of \'}}\' after arg-id {}", ch, arg_id);
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
                int digits = 0;
                while ((ch >= '0') && (ch <= '9')) {
                    if (++digits > 9) {
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

            if (argid >= m_args_count) {
                m_trx->error(Error::RangeCheck, "arg-id {} exceeds arg-count {}", argid, m_args_count);
            } else {
                *arg_id = argid;
                return ch;
            }
        }
    }

    // For integer d/O spec, precision encodes the radix base (2..36).  Returns true
    // if a radix is specified (precision set), false if omitted.  Errors on out-of-range.
    [[nodiscard]] bool compute_has_radix() {
        if (m_precision == PRECISION_DEFAULT) {
            return false;
        } else if ((m_precision < 2) || (m_precision > MAX_INT_PRECISION)) {
            m_trx->error(Error::InvalidFormatString,
                         "invalid integer radix precision {}; must be 2..{}",
                         m_precision,
                         MAX_INT_PRECISION);
        }
        return true;
    }

    //                   E F G
    // Object        c d e f g s T x O ? ! ' '
    // ------        -------------------------
    // Null                    x X   x      s
    // Byte          x x       X X   x x    c
    // Integer         x         X   x      d
    // Long            x         X   x      d
    // UInteger        x         X   x      d
    // ULong           x         X   x      d
    // Address         x         X   x      d
    // Real              x x x   X   x      g
    // Double            x x x   X   x      g
    // Boolean       x x       x X   x      s
    // Operator                x X   x      s
    // Mark                    x X   x      s
    // Name                    x X   x      s
    // Array                   x X   x      s
    // Packed                  x X   x      s
    // String                  x X x x x x  s
    // Stream                  x X   x      s
    // Dict                    x X   x      s
    //               c d e f g s T x O ? ! ' '
    [[nodiscard]] verify_t get_format_spec_type(vm_t ch) {
        switch (ch) {
        case 'c':
            return (Trix::VerifyByte | Trix::VerifyBoolean | Trix::VerifyIntegers);

        case 'd':
            return (Trix::VerifyIntegers | Trix::VerifyAddress | Trix::VerifyBoolean);

        case 'X':
        case 'b':
        case 'o':
            return (Trix::VerifyIntegers | Trix::VerifyAddress | Trix::VerifyByte);

        case 'e':
        case 'E':
        case 'f':
        case 'F':
        case 'g':
        case 'G':
            return Trix::VerifyFloats;

        case 's':
            return (Trix::VerifyNull | Trix::VerifyByte | Trix::VerifyBoolean | Trix::VerifyOperator | Trix::VerifyMark |
                    Trix::VerifyName | Trix::VerifyArray | Trix::VerifyPacked | Trix::VerifyString | Trix::VerifyStream |
                    Trix::VerifyDict | Trix::VerifySet | Trix::VerifyCurry | Trix::VerifyThunk | Trix::VerifyTagged |
                    Trix::VerifyRecord | Trix::VerifyCoroutine | Trix::VerifyPipeBuffer | Trix::VerifyCell |
                    Trix::VerifyContinuation);

        case 'x':
            return (Trix::VerifyIntegers | Trix::VerifyAddress | Trix::VerifyByte | Trix::VerifyString);

        case '?':
            return (Trix::VerifyByte | Trix::VerifyString);

        case 'O':
        case 'T':
        case TYPE_DEFAULT:
            return Trix::VerifyAny;

        case 'I':
            return Trix::VerifyULong;

        case 'D':
            return Trix::VerifyUInteger;

        default:
            m_trx->error(Error::InvalidFormatString, "invalid type specifier \'{:c}\'", ch);
        }
    }

    [[nodiscard]] vm_t process_format_spec() {
        // [[fill] align] [sign] ["#"] ["0"] [width] ["." precision] [type]
        auto ch = next_format_ch();
        if (ch != '}') {
            // [[fill] align]
            // fill may be any character except '{' or '}', defaults to ' '
            // align: '<' field left aligned within the space (default for non-numbers).
            // align: '>' field right aligned within the space (default for numbers).
            // align: '^' field centered within the space
            if ((ch == '<') || (ch == '>') || (ch == '^')) {
                // align without fill
                m_fill = FILL_DEFAULT;
                m_align = ch;

                if (m_format_ptr < m_format_limit) {
                    ch = static_cast<vm_t>(*m_format_ptr++);
                    if ((ch == '<') || (ch == '>') || (ch == '^')) {
                        // fill and align are specified
                        m_fill = m_align;
                        m_align = ch;
                        if (m_format_ptr < m_format_limit) {
                            ch = static_cast<vm_t>(*m_format_ptr++);
                        }
                    }
                }
            } else if (m_format_ptr < m_format_limit) {
                // First char was not an align char; check if second char is (making first the fill).
                auto maybe_align = *m_format_ptr;
                if ((maybe_align == '<') || (maybe_align == '>') || (maybe_align == '^')) {
                    // fill and align are specified
                    if ((ch == '{') || (ch == '}')) {
                        m_trx->error(Error::InvalidFormatString, "fill specifier cannot be \'{:c}\'", ch);
                    } else {
                        m_fill = ch;
                        m_align = static_cast<vm_t>(maybe_align);
                        if (++m_format_ptr < m_format_limit) {
                            ch = static_cast<vm_t>(*m_format_ptr++);
                        }
                    }
                }
            }

            // [sign]
            // '+' sign should be used for negative and nonnegative numbers
            // '-' sign should be used for only negative numbers (default behavior)
            // ' ' leading space should be used with nonnegative numbers, and a minus sign with negative numbers
            if ((ch == '+') || (ch == '-') || (ch == ' ')) {
                m_sign = ch;
                ch = next_format_ch();
            }

            // ["#"]
            // select alternate output
            // if a numeric or ch suffix is present it will be uppercase
            // radix characters will be uppercase
            // boolean false/true will be uppercase
            // Real and Double without an exponent will end in .0
            if (ch == '#') {
                m_alt = true;
                ch = next_format_ch();
            }

            // ["0"]
            // preceding the width field by a zero '0' character enables sign-aware zero-padding for numeric
            // types. it forces the padding to be placed after the sign (if any) but before the digits.
            // used for printing fields in the form '+000000120', valid for numeric types
            // pads the field with leading zeros, following any indication of sign, to the field width
            if (ch == '0') {
                m_zero = true;
                ch = next_format_ch();
            }

            // [width]
            // (integer | "{" [arg_id] "}")
            // width is a decimal integer defining the minimum field width
            ch = get_integer(ch, 0, MAX_WIDTH, &m_width);

            // ["." precision]
            // (integer | "{" [arg_id] "}")
            // precision is a decimal number indicating how many digits should be displayed after the decimal
            // point for a floating-point value formatted with 'f', or before and after the decimal point for a
            // floating-point value formatted with 'g'
            // for UInteger and ULong precision specifies the radix base and must be 2..36
            // precision is not allowed for Integers, Byte, Boolean, or other Objects.
            // for non-number types precision indicates the maximum number of characters output
            if (ch == '.') {
                ch = next_format_ch();
                ch = get_integer(ch, 0, MAX_STRING_PRECISION, &m_precision);
                if (m_precision == PRECISION_DEFAULT) {
                    m_trx->error(Error::InvalidFormatString, "missing precision after \'.\'");
                }
            }

            // [type]
            // "c" | "d" | "e|E" | "f|F" | "g|G" | "s" | "T" | "x" | "O" | "?" | "!" | empty
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
        // Template span: from the position of the current `ch` up to (but not
        // including) the closing `}`.  next_format_ch() has already advanced
        // m_format_ptr past `ch`, so the span begins at (m_format_ptr - 1).
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
            m_chrono_template_ptr = template_start;
            m_chrono_template_len = template_len;
        }
        return ch;
    }

    void print_object(const Object *val_ptr) {
        auto val_type = val_ptr->type();
        switch (val_type) {
        case Object::Type::Null:
            print_null(val_ptr);
            break;

        case Object::Type::Byte:
            print_byte(val_ptr);
            break;

        case Object::Type::Integer:
            print_integer(val_ptr);
            break;

        case Object::Type::UInteger:
            print_uinteger(val_ptr);
            break;

        case Object::Type::Long:
            print_long(val_ptr);
            break;

        case Object::Type::ULong:
            print_ulong(val_ptr);
            break;

        case Object::Type::Address:
            print_address(val_ptr);
            break;

        case Object::Type::Real:
            print_real(val_ptr);
            break;

        case Object::Type::Double:
            print_double(val_ptr);
            break;

        case Object::Type::Boolean:
            print_boolean(val_ptr);
            break;

        case Object::Type::Operator:
            print_operator(val_ptr);
            break;

        case Object::Type::Mark:
            print_mark(val_ptr);
            break;

        case Object::Type::Name:
            print_name(val_ptr);
            break;

        case Object::Type::Array:
            print_array(val_ptr);
            break;

        case Object::Type::Packed:
            print_packed(val_ptr);
            break;

        case Object::Type::String:
            print_string(val_ptr);
            break;

        case Object::Type::Stream:
            print_stream(val_ptr);
            break;

        case Object::Type::Dict:
            print_dict(val_ptr);
            break;

        case Object::Type::Set:
            print_set(val_ptr);
            break;

        case Object::Type::Curry:
            print_curry(val_ptr);
            break;

        case Object::Type::Thunk:
            print_thunk(val_ptr);
            break;

        case Object::Type::Tagged:
            print_tagged(val_ptr);
            break;

        case Object::Type::Record:
            print_record(val_ptr);
            break;

        case Object::Type::Coroutine:
            print_coroutine(val_ptr);
            break;

        case Object::Type::PipeBuffer:
            print_pipe_buffer(val_ptr);
            break;

        case Object::Type::Cell:
            print_cell(val_ptr);
            break;

        case Object::Type::Continuation:
            print_continuation(val_ptr);
            break;

        case Object::Type::Int128:
            print_int128(val_ptr);
            break;

        case Object::Type::UInt128:
            print_uint128(val_ptr);
            break;

        case Object::Type::OpaqueHandle:
            print_handle(val_ptr);
            break;

        case Object::Type::SourceLoc:
        default:
            assert(false && "PrintFmt::print_object: unknown Object type");
            std::unreachable();
        }
    }

    void process_output(int arg_id) {
        const auto arg_ptr = &m_args_ptr[arg_id];
        auto format_spec_type = get_format_spec_type(m_type);
        if ((format_spec_type & Trix::TypeToVerify(arg_ptr->type())) == 0) {
            char type_buffer[ObjectNameBufferSize];
            auto type_length = m_trx->object_name(arg_ptr, type_buffer, false, false);
            m_trx->error(Error::InvalidFormatString,
                         "type specifier \'{:c}\' not supported for arg-id {} ({})",
                         m_type,
                         arg_id,
                         std::string_view(type_buffer, type_length));
        } else if (m_type == 'T') {
            print_type(arg_ptr);
        } else {
            print_object(arg_ptr);
        }
    }

    void emit(vm_t ch) {
        if (m_output_stream != nullptr) {
            // stream output
            m_output_stream->putc(m_trx, ch);
            ++m_output_count;
            if ((ch == Trix::ASCII_LF) && (m_output_stream == m_trx->m_stdout)) {
                // stdout flush upon every \n output
                m_output_stream->flush(m_trx);
            }
        } else {
            // memory output
            if (m_output_capacity != 0) {
                *m_output_ptr++ = ch;
                --m_output_capacity;
                ++m_output_count;
            } else {
                ++m_dropped_count;
            }
        }
    }

    // Emit a null-terminated C string literal.
    void emit_cstr(const char *str) {
        while (*str != '\0') {
            emit(static_cast<vm_t>(*str++));
        }
    }

    // Emit a string_view.
    void emit_sv(std::string_view sv) {
        for (auto ch : sv) {
            emit(static_cast<vm_t>(ch));
        }
    }

    // Check if a container offset is already in the visited set (cycle detection).
    [[nodiscard]] bool is_visited(vm_offset_t offset) const {
        for (int i = 0; i < m_depth; ++i) {
            if (m_visited[i] == offset) {
                return true;
            }
        }
        return false;
    }

    // Enter a container for recursive printing.  Checks for cycles.
    // Returns true if safe to recurse, false if cycle detected (already emitted --cycle--).
    [[nodiscard]] bool enter_container(vm_offset_t offset) {
        if (is_visited(offset)) {
            emit_cstr("--cycle--");
            return false;
        } else if (m_depth < MaxPrintDepth) {
            m_visited[m_depth] = offset;
        }
        return true;
    }

    // Recursively emit an object in compact O-form.
    // Used by print_array, print_dict, etc. to print child elements.
    void emit_sub_object(const Object *val_ptr) {
        if (m_depth >= MaxPrintDepth) {
            emit_cstr("...");
        } else {
            // Save all format spec state
            auto saved_type = m_type;
            auto saved_width = m_width;
            auto saved_precision = m_precision;
            auto saved_fill = m_fill;
            auto saved_align = m_align;
            auto saved_sign = m_sign;
            auto saved_alt = m_alt;
            auto saved_zero = m_zero;

            // Set defaults for recursive call
            default_format_spec();
            m_type = 'O';
            ++m_depth;
            print_object(val_ptr);
            --m_depth;

            // Restore format spec state
            m_type = saved_type;
            m_width = saved_width;
            m_precision = saved_precision;
            m_fill = saved_fill;
            m_align = saved_align;
            m_sign = saved_sign;
            m_alt = saved_alt;
            m_zero = saved_zero;
        }
    }

    [[nodiscard]] static std::pair<char, char> to_hex(vm_t value, bool uppercase) {
        static constexpr char upper[16] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
        static constexpr char lower[16] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
        auto ptr = uppercase ? upper : lower;
        return std::pair{ptr[(value >> 4) & 0x0F], ptr[value & 0x0F]};
    }
public:
    // Hand-rolled to_chars-like routine for 128-bit integers: libstdc++ does not
    // accept __int128/__uint128_t in std::to_chars.  Emits lowercase digits for
    // bases > 10 to match std::to_chars; callers uppercase the result as needed.
    // On overflow (insufficient capacity), returns {last, std::errc::value_too_large}.
    [[nodiscard]] static std::to_chars_result u128_to_chars(char *first, char *last, uint128_t value, int base) {
        assert((base >= 2) && (base <= 36));

        char tmp[130];
        auto tmp_limit = tmp + sizeof(tmp);
        auto t = tmp_limit;
        if (value == 0) {
            *(--t) = '0';
        } else {
            auto ubase = static_cast<uint128_t>(base);
            while (value != 0) {
                auto digit = static_cast<unsigned>(value % ubase);
                *(--t) = (digit < 10) ? static_cast<char>('0' + digit) : static_cast<char>('a' + digit - 10);
                value /= ubase;
            }
        }
        auto len = std::ptrdiff_t{tmp_limit - t};
        if ((last - first) < len) {
            return std::to_chars_result{last, std::errc::value_too_large};
        } else {
            std::copy(t, tmp_limit, first);
            return std::to_chars_result{first + len, std::errc{}};
        }
    }

    [[nodiscard]] static std::to_chars_result i128_to_chars(char *first, char *last, int128_t value, int base) {
        auto mag = static_cast<uint128_t>(value);
        if (value < 0) {
            if (first >= last) {
                return std::to_chars_result{last, std::errc::value_too_large};
            } else {
                *first++ = '-';
                mag = static_cast<uint128_t>(-mag);
            }
        }
        return u128_to_chars(first, last, mag, base);
    }

    // Dispatches to std::to_chars for std-supported integer types; to the
    // hand-rolled u128/i128 path for 128-bit types.
    template<typename T>
    [[nodiscard]] static std::to_chars_result trix_to_chars(char *first, char *last, T value, int base = 10) {
        if constexpr (std::is_same_v<T, int128_t>) {
            return i128_to_chars(first, last, value, base);
        } else if constexpr (std::is_same_v<T, uint128_t>) {
            return u128_to_chars(first, last, value, base);
        } else {
            return std::to_chars(first, last, value, base);
        }
    }
private:
    static void uppercase_range(char *begin, char *end) {
        std::ranges::for_each(begin, end, [](char &ch) {
            if ((ch >= 'a') && (ch <= 'z')) {
                ch = static_cast<char>(Trix::to_uppercase(ch));
            }
        });
    }

    [[nodiscard]] static std::optional<char> escape_letter(vm_t value) {
        switch (value) {
        case Trix::ASCII_BEL:
            return 'a';

        case Trix::ASCII_BS:
            return 'b';

        case Trix::ASCII_ESC:
            return 'e';

        case Trix::ASCII_FF:
            return 'f';

        case Trix::ASCII_LF:
            return 'n';

        case Trix::ASCII_CR:
            return 'r';

        case Trix::ASCII_HT:
            return 't';

        case Trix::ASCII_VT:
            return 'v';

        default:
            return std::nullopt;
        }
    }

    // Convert a byte to its printable representation.
    // format='?': isgraph chars pass through; others use \t, \^@, or \xFF.
    // format='s': isprint chars pass through (space included); parens/backslash escaped.
    [[nodiscard]] char *to_ch(vm_t format, char *ptr, vm_t ch) const {
        if ((format == '?') && std::isgraph(ch)) {
            // does not include space
            if (ch == '\\') {
                *ptr++ = '\\';
            }
            *ptr++ = static_cast<char>(ch);
        } else if (std::isprint(ch)) {
            // includes space
            if ((ch == '(') || (ch == ')') || (ch == '\\')) {
                *ptr++ = '\\';
            }
            *ptr++ = static_cast<char>(ch);
        } else {
            *ptr++ = '\\';

            auto esc_ch = escape_letter(ch);
            if (esc_ch.has_value()) {
                // \t
                *ptr++ = esc_ch.value();
            } else if (((ch >= Trix::ASCII_NUL) && (ch <= Trix::ASCII_US)) || (ch == Trix::ASCII_DEL)) {
                // \^@
                *ptr++ = '^';
                *ptr++ = (ch == Trix::ASCII_DEL) ? '?' : static_cast<char>(ch + '@');
            } else {
                // \x58
                auto [hi, lo] = to_hex(ch, m_alt);
                *ptr++ = 'x';
                *ptr++ = hi;
                *ptr++ = lo;
            }
        }

        return ptr;
    }

    // Emit a formatted numeric field.
    //   ptr..limit : the ASCII digit/radix characters to output
    //   has_radix  : true iff a "base#" prefix precedes the digits (zero-padding inserts after '#')
    //   is_negative: true iff a '-' sign should be prepended
    // prefix_len > 0: first prefix_len bytes of buffer are a literal prefix (e.g. "0x",
    // "0b", "0o") emitted before the sign -- analogous to has_radix emitting the "N#"
    // portion.  Only one of has_radix or prefix_len is set.
    void
    numeric_outputter(const char *ptr, const char *limit, bool has_radix = false, bool is_negative = false, int prefix_len = 0) {
        auto numeric_width = static_cast<int>(limit - ptr);
        auto sign_width = ((m_sign == '-') && !is_negative) ? 0 : 1;
        auto total_width = (sign_width + numeric_width);
        auto total_pad = (m_width > total_width) ? (m_width - total_width) : 0;
        auto align = m_align;
        auto zero = m_zero;

        if (total_pad != 0) {
            if (align == ALIGN_DEFAULT) {
                // '>' field right aligned within the space is default for numbers
                align = '>';
            } else {
                // if align option specified, the 0 character is ignored
                zero = false;
            }

            auto zero_pad = 0;
            auto left_pad = 0;
            auto right_pad = 0;
            if (zero) {
                zero_pad = total_pad;
            } else if (align == '>') {
                left_pad = total_pad;
            } else if (align == '<') {
                right_pad = total_pad;
            } else {
                assert(align == '^');

                left_pad = (total_pad / 2);
                right_pad = (total_pad - left_pad);
            }

            // [left pad]
            while (left_pad-- != 0) {
                emit(m_fill);
            }

            // [radix# or prefix] must precede sign so negative-value output is scanner-
            // compatible (e.g., "16#-05" and "0x-5"; "-16#5" is not).  Zero-pad path
            // inserts zeros between sign and digits to produce "16#-00005" / "0x-005".
            if (has_radix) {
                while (ptr < limit) {
                    auto ch = *ptr++;
                    emit(static_cast<vm_t>(ch));
                    if (ch == '#') {
                        break;
                    }
                }
            } else if (prefix_len > 0) {
                for (auto i = 0; i < prefix_len; ++i) {
                    emit(static_cast<vm_t>(*ptr++));
                }
            }

            // [sign]
            if (sign_width != 0) {
                emit(is_negative ? '-' : m_sign);
            }

            // [zeros]
            while (zero_pad-- != 0) {
                emit('0');
            }

            // value[suffix]
            while (ptr < limit) {
                emit(static_cast<vm_t>(*ptr++));
            }

            // [right pad]
            while (right_pad-- != 0) {
                emit(m_fill);
            }
        } else {
            // [radix# or prefix]
            if (has_radix) {
                while (ptr < limit) {
                    auto ch = *ptr++;
                    emit(static_cast<vm_t>(ch));
                    if (ch == '#') {
                        break;
                    }
                }
            } else if (prefix_len > 0) {
                for (auto i = 0; i < prefix_len; ++i) {
                    emit(static_cast<vm_t>(*ptr++));
                }
            }

            // [sign]
            if (sign_width != 0) {
                emit(is_negative ? '-' : m_sign);
            }

            // value[suffix]
            while (ptr < limit) {
                emit(static_cast<vm_t>(*ptr++));
            }
        }
    }

    void string_outputter(const char *ptr, const char *limit, int max_width = -1) {
        auto string_width = static_cast<int>(limit - ptr);
        if ((max_width >= 0) && (max_width < string_width)) {
            string_width = max_width;
            limit = (ptr + string_width);
        }
        auto total_pad = (m_width > string_width) ? (m_width - string_width) : 0;

        if (total_pad != 0) {
            auto left_pad = 0;
            auto right_pad = 0;
            if ((m_align == ALIGN_DEFAULT) || (m_align == '<')) {
                right_pad = total_pad;
            } else if (m_align == '>') {
                left_pad = total_pad;
            } else {
                assert(m_align == '^');

                left_pad = (total_pad / 2);
                right_pad = (total_pad - left_pad);
            }

            // [left pad]
            while (left_pad-- != 0) {
                emit(m_fill);
            }
            // string
            while (ptr < limit) {
                emit(static_cast<vm_t>(*ptr++));
            }
            // [right pad]
            while (right_pad-- != 0) {
                emit(m_fill);
            }
        } else {
            // string
            while (ptr < limit) {
                emit(static_cast<vm_t>(*ptr++));
            }
        }
    }

    void print_type(const Object *val_ptr) {
        char buffer[ObjectNameBufferSize];
        auto length = m_trx->object_name(val_ptr, buffer, m_alt);
        string_outputter(buffer, buffer + length);
    }

    // none, c: copy to output
    //       d: base-10 or radix
    //       O: base-10 or radix with suffix
    //       s: string with suffix
    //       ?: if printable copy to output else use escape or hex format
    //    x, X: bare hex digits (with # flag: 0x/0X prefix)
    //       b: bare binary digits (with # flag: 0b prefix)
    //       o: bare octal digits (with # flag: 0o prefix)
    void print_byte(const Object *val_ptr) {
        assert(val_ptr->is_byte());

        if (m_type == TYPE_DEFAULT) {
            m_type = 'c';
        }
        auto value = val_ptr->byte_value();

        // Bare display forms share a dispatch short-circuit.
        switch (m_type) {
        case 'x':
            print_integral_bare(value, 16, false);
            return;

        case 'X':
            print_integral_bare(value, 16, true);
            return;

        case 'b':
            print_integral_bare(value, 2, false);
            return;

        case 'o':
            print_integral_bare(value, 8, false);
            return;

        default:
            break;
        }
        if ((m_type != 'c') && (m_type != 'd') && (m_type != 'O') && (m_type != 's') && (m_type != '?')) {
            m_trx->error(Error::InvalidFormatString, "unsupported format type '{:c}' for byte", m_type);
        } else if (m_type == 'c') {
            const char buffer[2]{static_cast<char>(value), '\0'};
            string_outputter(&buffer[0], &buffer[1]);
        } else if ((m_type == 'd') || (m_type == 'O')) {
            // longest output: 2#10101010#b
            char buffer[16];
            auto limit = &buffer[sizeof(buffer)];

            auto output_suffix = (m_type == 'O');
            auto has_radix = compute_has_radix();
            if (has_radix) {
                // radix
                auto result = std::to_chars(buffer, limit, m_precision, 10);
                *result.ptr++ = '#';

                // value
                {
                    auto ptr = result.ptr;
                    result = std::to_chars(result.ptr, limit, value, m_precision);
                    if (m_alt && (m_precision > 10)) {
                        uppercase_range(ptr, result.ptr);
                    }
                }

                // [suffix]
                if (output_suffix) {
                    *result.ptr++ = '#';
                    *result.ptr++ = m_alt ? 'B' : 'b';
                }

                limit = result.ptr;
            } else {
                // value
                auto result = std::to_chars(buffer, limit, value, 10);

                // [suffix]
                if (output_suffix) {
                    *result.ptr++ = m_alt ? 'B' : 'b';
                }

                limit = result.ptr;
            }
            numeric_outputter(buffer, limit, has_radix);
        } else if (m_type == 's') {
            // (\x58)#b
            char buffer[12];
            auto ptr = buffer;

            *ptr++ = '(';
            ptr = to_ch(m_type, ptr, value);
            *ptr++ = ')';

            // suffix
            *ptr++ = '#';
            *ptr++ = m_alt ? 'B' : 'b';
            string_outputter(buffer, ptr);
        } else {
            assert(m_type == '?');

            char buffer[8];
            auto limit = to_ch(m_type, buffer, value);
            string_outputter(buffer, limit);
        }
    }

    // Bare-digits integer output in base 2/8/16.  With m_alt, prepend 0x/0X/0b/0o
    // (round-trips via scanner's scan_prefix path).  No Trix type suffix.
    void print_integral_bare(auto value, int base, bool uppercase) {
        assert((base == 2) || (base == 8) || (base == 16));

        auto is_negative = (value < 0);

        char buffer[sizeof(value) * 8 + 4];
        auto limit = &buffer[sizeof(buffer)];

        auto prefix_len = 0;
        auto digits_ptr = buffer;
        if (m_alt) {
            buffer[0] = '0';
            buffer[1] = (base == 16) ? (uppercase ? 'X' : 'x') : (base == 2) ? 'b' : 'o';
            prefix_len = 2;
            digits_ptr = buffer + 2;
        }

        auto result = trix_to_chars(digits_ptr, limit, value, base);
        // to_chars emits '-' for negatives; strip it since numeric_outputter emits sign.
        if (is_negative) {
            std::copy(digits_ptr + 1, result.ptr, digits_ptr);
            --result.ptr;
        }
        if (uppercase && (base == 16)) {
            uppercase_range(digits_ptr, result.ptr);
        }

        numeric_outputter(buffer, result.ptr, false, is_negative, prefix_len);
    }

    // Single-byte char output: integer value in 0..255 emitted as one byte.
    void print_integral_char(auto value) {
        if ((value < 0) || (value > 255)) {
            m_trx->error(Error::RangeCheck, "character format value {} not in range 0..255", value);
        } else {
            const char buffer[2]{static_cast<char>(value), '\0'};
            string_outputter(&buffer[0], &buffer[1]);
        }
    }

    // signed integer/long: base-10 or radix, optional suffix
    void print_signed_integral(auto value, char suffix_lower, char suffix_upper) {
        auto is_negative = (value < 0);

        char buffer[sizeof(value) * 8 + 16];
        auto limit = &buffer[sizeof(buffer)];

        auto output_suffix = (m_type == 'O');
        auto has_radix = compute_has_radix();
        if (has_radix) {
            // radix
            auto result = std::to_chars(buffer, limit, m_precision, 10);

            // value
            {
                auto ptr = result.ptr;
                if (is_negative) {
                    result = trix_to_chars(result.ptr, limit, value, m_precision);
                    // to_chars emits "-digits"; overwrite '-' with '#' for "radix#digits"
                    *ptr = '#';
                } else {
                    *result.ptr++ = '#';
                    result = trix_to_chars(result.ptr, limit, value, m_precision);
                }
                if (m_alt && (m_precision > 10)) {
                    uppercase_range(ptr, result.ptr);
                }
            }

            // [suffix]
            if (output_suffix) {
                *result.ptr++ = '#';
                *result.ptr++ = m_alt ? suffix_upper : suffix_lower;
            }

            numeric_outputter(buffer, result.ptr, has_radix, is_negative);
        } else {
            // value
            auto result = trix_to_chars(buffer, limit, value, 10);

            // [suffix]
            if (output_suffix) {
                *result.ptr++ = m_alt ? suffix_upper : suffix_lower;
            }

            auto start = is_negative ? &buffer[1] : buffer;
            numeric_outputter(start, result.ptr, has_radix, is_negative);
        }
    }

    // none, d: base-10 or radix
    //       O: base-10 or radix with suffix
    //    x, X: bare hex digits (with # flag: 0x/0X prefix)
    //       b: bare binary digits (with # flag: 0b prefix)
    //       o: bare octal digits (with # flag: 0o prefix)
    //       c: single byte codepoint (value must be 0..255)
    void print_integer(const Object *val_ptr) {
        assert(val_ptr->is_integer());

        if (m_type == TYPE_DEFAULT) {
            m_type = 'd';
        }
        auto value = val_ptr->integer_value();
        switch (m_type) {
        case 'd':
        case 'O':
            print_signed_integral(value, 'i', 'I');
            break;

        case 'x':
            print_integral_bare(value, 16, false);
            break;

        case 'X':
            print_integral_bare(value, 16, true);
            break;

        case 'b':
            print_integral_bare(value, 2, false);
            break;

        case 'o':
            print_integral_bare(value, 8, false);
            break;

        case 'c':
            print_integral_char(value);
            break;

        default:
            m_trx->error(Error::InvalidFormatString, "unsupported format type '{:c}' for integer", m_type);
        }
    }

    // unsigned integer/long: base-10 or radix, optional multi-char suffix
    void print_unsigned_integral(auto value, const char *suffix_lower, const char *suffix_upper) {
        char buffer[sizeof(value) * 8 + 16];
        auto limit = &buffer[sizeof(buffer)];

        auto output_suffix = (m_type == 'O');
        auto has_radix = compute_has_radix();
        if (has_radix) {
            // radix
            auto result = std::to_chars(buffer, limit, m_precision, 10);
            *result.ptr++ = '#';

            // value
            {
                auto ptr = result.ptr;
                result = trix_to_chars(result.ptr, limit, value, m_precision);
                if (m_alt && (m_precision > 10)) {
                    uppercase_range(ptr, result.ptr);
                }
            }

            // [suffix]
            if (output_suffix) {
                *result.ptr++ = '#';
                for (auto p = m_alt ? suffix_upper : suffix_lower; *p != '\0'; ++p) {
                    *result.ptr++ = *p;
                }
            }

            limit = result.ptr;
        } else {
            // value
            auto result = trix_to_chars(buffer, limit, value, 10);

            // [suffix]
            if (output_suffix) {
                for (auto p = m_alt ? suffix_upper : suffix_lower; *p != '\0'; ++p) {
                    *result.ptr++ = *p;
                }
            }

            limit = result.ptr;
        }
        numeric_outputter(buffer, limit, has_radix);
    }

    // none, d: base-10 or radix
    //       O: base-10 or radix with suffix
    //    x, X: bare hex digits (with # flag: 0x/0X prefix)
    //       b: bare binary digits (with # flag: 0b prefix)
    //       o: bare octal digits (with # flag: 0o prefix)
    void print_uinteger(const Object *val_ptr) {
        assert(val_ptr->is_uinteger());

        if (m_type == TYPE_DEFAULT) {
            m_type = 'd';
        }
        auto value = val_ptr->uinteger_value();
        switch (m_type) {
        case 'd':
        case 'O':
            print_unsigned_integral(value, "ui", "UI");
            break;

        case 'x':
            print_integral_bare(value, 16, false);
            break;

        case 'X':
            print_integral_bare(value, 16, true);
            break;

        case 'b':
            print_integral_bare(value, 2, false);
            break;

        case 'o':
            print_integral_bare(value, 8, false);
            break;

        case 'D':
            print_udate(value);
            break;

        default:
            m_trx->error(Error::InvalidFormatString, "unsupported format type '{:c}' for uinteger", m_type);
        }
    }

    // std::format has format_to_n (bounded, compile-time spec) and vformat_to
    // (unbounded, runtime spec) but no bounded-plus-runtime variant.  The two
    // chrono paths below format a runtime user strftime template into a fixed
    // buffer, so they feed vformat_to this sink: it stores up to the cap and
    // counts the would-be total, so output that overruns the buffer raises
    // LimitCheck instead of truncating silently.
    struct capped_sink {
        char *cursor_ptr;       // next slot; one-past-last-written on return
        char *const limit_ptr;  // one-past-the-buffer
        std::size_t total{0};   // chars that WOULD be written, unbounded
        using value_type = char;
        void push_back(char c) {
            if (cursor_ptr != limit_ptr) {
                *cursor_ptr++ = c;
            }
            ++total;
        }
    };

    // Format a udate using the m_chrono_template.  Output is a pure date,
    // time-of-day specifiers in the template are valid (chrono fills
    // hours/minutes/seconds with zeros) but typically not useful.
    // The width/align/fill prefix applies to the formatted result.
    // Raises InvalidFormatString on bad template.
    void print_udate(uinteger_t udate) {
        if ((m_chrono_template_ptr == nullptr) || (m_chrono_template_len == 0)) {
            m_trx->error(Error::InvalidFormatString, "empty :D strftime template");
        } else {
            try {
                auto ymd = m_trx->unpack_udate(udate);
                std::chrono::sys_days dp{ymd};

                char fmt_str[MaxStrftimeTemplateLength + 5];
                auto limit = &fmt_str[4 + m_chrono_template_len];

                fmt_str[0] = '{';
                fmt_str[1] = ':';
                std::copy_n(m_chrono_template_ptr, m_chrono_template_len, &fmt_str[2]);
                limit[-2] = '}';
                limit[-1] = '\0';

                char buffer[MaxStrftimeOutputLength];
                std::string_view spec{fmt_str, static_cast<std::size_t>(3 + m_chrono_template_len)};
                capped_sink sink{buffer, buffer + sizeof(buffer)};
                std::vformat_to(std::back_inserter(sink), spec, std::make_format_args(dp));
                if (sink.total > sizeof(buffer)) {
                    std::string_view tmpl{m_chrono_template_ptr, m_chrono_template_len};
                    m_trx->error(Error::LimitCheck,
                                 "strftime ':D' output for template '{}' needs {} bytes, exceeds maximum {}",
                                 tmpl,
                                 sink.total,
                                 sizeof(buffer));
                }
                string_outputter(buffer, sink.cursor_ptr, PRECISION_DEFAULT);
            }
            catch (const std::format_error &e) {
                std::string_view tmpl{m_chrono_template_ptr, m_chrono_template_len};
                m_trx->error(Error::InvalidFormatString, "invalid strftime template '{}': {}", tmpl, e.what());
            }
        }
    }

    // none, d: base-10 or radix
    //       O: base-10 or radix with suffix
    //    x, X: bare hex digits (with # flag: 0x/0X prefix)
    //       b: bare binary digits (with # flag: 0b prefix)
    //       o: bare octal digits (with # flag: 0o prefix)
    void print_long(const Object *val_ptr) {
        assert(val_ptr->is_long());

        if (m_type == TYPE_DEFAULT) {
            m_type = 'd';
        }
        auto value = val_ptr->long_value(m_trx);
        switch (m_type) {
        case 'd':
        case 'O':
            print_signed_integral(value, 'l', 'L');
            break;

        case 'x':
            print_integral_bare(value, 16, false);
            break;

        case 'X':
            print_integral_bare(value, 16, true);
            break;

        case 'b':
            print_integral_bare(value, 2, false);
            break;

        case 'o':
            print_integral_bare(value, 8, false);
            break;

        default:
            m_trx->error(Error::InvalidFormatString, "unsupported format type '{:c}' for long", m_type);
        }
    }

    // none, d: base-10 or radix
    //       O: base-10 or radix with suffix
    //    x, X: bare hex digits (with # flag: 0x/0X prefix)
    //       b: bare binary digits (with # flag: 0b prefix)
    //       o: bare octal digits (with # flag: 0o prefix)
    //       I: chrono instant (ms since 1970 UTC) formatted via strftime
    //          template body (m_chrono_*); optional `l` suffix selects local zone
    void print_ulong(const Object *val_ptr) {
        assert(val_ptr->is_ulong());

        if (m_type == TYPE_DEFAULT) {
            m_type = 'd';
        }
        auto value = val_ptr->ulong_value(m_trx);
        switch (m_type) {
        case 'd':
        case 'O':
            print_unsigned_integral(value, "ul", "UL");
            break;

        case 'x':
            print_integral_bare(value, 16, false);
            break;

        case 'X':
            print_integral_bare(value, 16, true);
            break;

        case 'b':
            print_integral_bare(value, 2, false);
            break;

        case 'o':
            print_integral_bare(value, 8, false);
            break;

        case 'I':
            print_instant(value);
            break;

        default:
            m_trx->error(Error::InvalidFormatString, "unsupported format type '{:c}' for ulong", m_type);
        }
    }

    // Format a ULong as a chrono instant via the m_chrono_template.
    // m_chrono_local selects the local zone (UTC otherwise).
    // The width/align/fill prefix applies to the formatted result.
    // Raises InvalidFormatString on bad template (caught from fmt) or empty
    // template; raises Unsupported if local is requested but zone lookup failed.
    void print_instant(ulong_t ms) {
        if ((m_chrono_template_ptr == nullptr) || (m_chrono_template_len == 0)) {
            m_trx->error(Error::InvalidFormatString, "empty :I strftime template");
        } else {
            try {
                using std::chrono::duration_cast;
                using std::chrono::milliseconds;
                using std::chrono::seconds;
                using std::chrono::sys_time;
                using std::chrono::zoned_time;

                char fmt_str[MaxStrftimeTemplateLength + 5];
                auto limit = &fmt_str[4 + m_chrono_template_len];

                fmt_str[0] = '{';
                fmt_str[1] = ':';
                std::copy_n(m_chrono_template_ptr, m_chrono_template_len, &fmt_str[2]);
                limit[-2] = '}';
                limit[-1] = '\0';

                char buffer[MaxStrftimeOutputLength];
                // Truncate to whole seconds for stable strftime output
                // ms precision is intentionally dropped at this layer.
                auto utc_tp = sys_time<seconds>(seconds(static_cast<int64_t>(ms / 1000)));
                std::string_view spec{fmt_str, static_cast<std::size_t>(3 + m_chrono_template_len)};
                capped_sink sink{buffer, buffer + sizeof(buffer)};
                if (m_chrono_local) {
                    const auto *zone = m_trx->current_local_zone_or_raise();
                    const zoned_time<seconds> zt{zone, utc_tp};
                    auto local_tp = zt.get_local_time();
                    std::vformat_to(std::back_inserter(sink), spec, std::make_format_args(local_tp));
                } else {
                    std::vformat_to(std::back_inserter(sink), spec, std::make_format_args(utc_tp));
                }
                if (sink.total > sizeof(buffer)) {
                    std::string_view tmpl{m_chrono_template_ptr, m_chrono_template_len};
                    m_trx->error(Error::LimitCheck,
                                 "strftime ':I' output for template '{}' needs {} bytes, exceeds maximum {}",
                                 tmpl,
                                 sink.total,
                                 sizeof(buffer));
                }
                string_outputter(buffer, sink.cursor_ptr, PRECISION_DEFAULT);
            }
            catch (const std::format_error &e) {
                std::string_view tmpl{m_chrono_template_ptr, m_chrono_template_len};
                m_trx->error(Error::InvalidFormatString, "invalid strftime template '{}': {}", tmpl, e.what());
            }
        }
    }

    // none, d: base-10 or radix
    //       O: base-10 or radix with suffix
    //    x, X: bare hex digits (with # flag: 0x/0X prefix)
    //       b: bare binary digits (with # flag: 0b prefix)
    //       o: bare octal digits (with # flag: 0o prefix)
    void print_int128(const Object *val_ptr) {
        assert(val_ptr->is_int128());

        if (m_type == TYPE_DEFAULT) {
            m_type = 'd';
        }
        auto value = val_ptr->int128_value(m_trx);
        switch (m_type) {
        case 'd':
        case 'O':
            print_signed_integral(value, 'q', 'Q');
            break;

        case 'x':
            print_integral_bare(value, 16, false);
            break;

        case 'X':
            print_integral_bare(value, 16, true);
            break;

        case 'b':
            print_integral_bare(value, 2, false);
            break;

        case 'o':
            print_integral_bare(value, 8, false);
            break;

        default:
            m_trx->error(Error::InvalidFormatString, "unsupported format type '{:c}' for int128", m_type);
        }
    }

    // none, d: base-10 or radix
    //       O: base-10 or radix with suffix
    //    x, X: bare hex digits (with # flag: 0x/0X prefix)
    //       b: bare binary digits (with # flag: 0b prefix)
    //       o: bare octal digits (with # flag: 0o prefix)
    void print_uint128(const Object *val_ptr) {
        assert(val_ptr->is_uint128());

        if (m_type == TYPE_DEFAULT) {
            m_type = 'd';
        }
        auto value = val_ptr->uint128_value(m_trx);
        switch (m_type) {
        case 'd':
        case 'O':
            print_unsigned_integral(value, "uq", "UQ");
            break;

        case 'x':
            print_integral_bare(value, 16, false);
            break;

        case 'X':
            print_integral_bare(value, 16, true);
            break;

        case 'b':
            print_integral_bare(value, 2, false);
            break;

        case 'o':
            print_integral_bare(value, 8, false);
            break;

        default:
            m_trx->error(Error::InvalidFormatString, "unsupported format type '{:c}' for uint128", m_type);
        }
    }

    // none, d: radix base-16
    //       O: radix base-16 with suffix
    //    x, X: bare hex digits (with # flag: 0x/0X prefix)
    //       b: bare binary digits (with # flag: 0b prefix)
    //       o: bare octal digits (with # flag: 0o prefix)
    void print_address(const Object *val_ptr) {
        assert(val_ptr->is_address());

        if (m_type == TYPE_DEFAULT) {
            m_type = 'd';
        }

        auto value = val_ptr->address_value(m_trx);
        auto uvalue = reinterpret_cast<uintptr_t>(value);
        switch (m_type) {
        case 'd':
        case 'O':
            break;

        case 'x':
            print_integral_bare(uvalue, 16, false);
            return;

        case 'X':
            print_integral_bare(uvalue, 16, true);
            return;

        case 'b':
            print_integral_bare(uvalue, 2, false);
            return;

        case 'o':
            print_integral_bare(uvalue, 8, false);
            return;

        default:
            m_trx->error(Error::InvalidFormatString, "unsupported format type '{:c}' for address", m_type);
        }

        // longest output: 16#0011223344556677#a
        char buffer[24];
        auto limit = &buffer[sizeof(buffer)];

        auto output_suffix = (m_type == 'O');
        // radix
        auto result = std::to_chars(buffer, limit, 16, 10);
        *result.ptr++ = '#';

        // value
        {
            auto ptr = result.ptr;
            result = std::to_chars(result.ptr, limit, uvalue, 16);
            if (m_alt) {
                uppercase_range(ptr, result.ptr);
            }
        }

        // [suffix]
        if (output_suffix) {
            *result.ptr++ = '#';
            if (m_alt) {
                *result.ptr++ = 'A';
            } else {
                *result.ptr++ = 'a';
            }
        }

        limit = result.ptr;
        numeric_outputter(buffer, limit, true);
    }

    // none: if precision is specified
    //         std::to_chars(first, last, value, std::chars_format::general, precision)
    //       else
    //         std::to_chars(first, last, value)
    //    O: none and append .0 if no exponent is present, append suffix
    //    e/E: scientific notation
    //    f/F: fixed notation
    //    g/G: general notation
    template<typename T>
    void print_float_impl(T value, char suffix_lower, char suffix_upper, const char *type_name) {
        if ((m_precision != PRECISION_DEFAULT) && (m_precision > MAX_FLOAT_PRECISION)) {
            m_trx->error(Error::InvalidFormatString, "float precision {} exceeds max {}", m_precision, MAX_FLOAT_PRECISION);
        } else {
            auto is_negative = std::signbit(value);
            auto is_finite = std::isfinite(value);

            char buffer[std::numeric_limits<T>::max_exponent10 + std::numeric_limits<T>::max_digits10 + 8];
            auto limit = &buffer[sizeof(buffer)];

            auto use_uppercase = ((m_type == 'E') || (m_type == 'F') || (m_type == 'G'));
            if (use_uppercase) {
                m_type = Trix::to_lowercase(m_type);
            }

            // value
            std::to_chars_result result{};
            if ((m_type == TYPE_DEFAULT) || (m_type == 'O')) {
                if (m_precision == PRECISION_DEFAULT) {
                    result = std::to_chars(buffer, limit, value);
                } else {
                    result = std::to_chars(buffer, limit, value, std::chars_format::general, m_precision);
                }
            } else {
                auto precision = (m_precision == PRECISION_DEFAULT) ? 6 : m_precision;
                if (m_type == 'e') {
                    result = std::to_chars(buffer, limit, value, std::chars_format::scientific, precision);
                } else if (m_type == 'f') {
                    result = std::to_chars(buffer, limit, value, std::chars_format::fixed, precision);
                } else {
                    assert(m_type == 'g');

                    result = std::to_chars(buffer, limit, value, std::chars_format::general, precision);
                }
            }

            if ((result.ptr == limit) || (result.ec == std::errc::value_too_large)) {
                m_trx->error(Error::LimitCheck, "print-fmt: {} value too large for format buffer", type_name);
            } else {
                if (is_finite) {
                    // find decimal point and exponent
                    auto has_decimal = (std::find(buffer, result.ptr, '.') != result.ptr);
                    auto exp_it = std::find_if(buffer, result.ptr, [](char c) { return ((c == 'e') || (c == 'E')); });
                    auto has_exp = (exp_it != result.ptr);
                    if (has_exp) {
                        *exp_it = use_uppercase ? 'E' : 'e';
                    }

                    // [.0] if no exponent is present
                    if ((m_alt || (m_type == 'O')) && !has_decimal && !has_exp) {
                        *result.ptr++ = '.';
                        *result.ptr++ = '0';
                    }

                    // [suffix]
                    if (m_type == 'O') {
                        *result.ptr++ = use_uppercase ? suffix_upper : suffix_lower;
                    }
                } else {
                    m_zero = false;

                    if (use_uppercase) {
                        std::ranges::for_each(buffer, result.ptr, [](char &ch) {
                            if (Trix::is_lowercase(ch)) {
                                // nan or inf
                                ch = static_cast<char>(Trix::to_uppercase(ch));
                            }
                        });
                    }
                }

                auto has_radix = false;
                numeric_outputter(is_negative ? (buffer + 1) : buffer, result.ptr, has_radix, is_negative);
            }
        }
    }

    void print_real(const Object *val_ptr) {
        assert(val_ptr->is_real());

        if ((m_type != TYPE_DEFAULT) && (m_type != 'e') && (m_type != 'E') && (m_type != 'f') && (m_type != 'F') &&
            (m_type != 'g') && (m_type != 'G') && (m_type != 'O')) {
            m_trx->error(Error::InvalidFormatString, "unsupported format type '{:c}' for real", m_type);
        }
        print_float_impl(val_ptr->real_value(), 'r', 'R', "real");
    }

    void print_double(const Object *val_ptr) {
        assert(val_ptr->is_double());

        if ((m_type != TYPE_DEFAULT) && (m_type != 'e') && (m_type != 'E') && (m_type != 'f') && (m_type != 'F') &&
            (m_type != 'g') && (m_type != 'G') && (m_type != 'O')) {
            m_trx->error(Error::InvalidFormatString, "unsupported format type '{:c}' for double", m_type);
        }
        print_float_impl(val_ptr->double_value(m_trx), 'd', 'D', "double");
    }

    // none, s: false, true or FALSE, TRUE
    //       c: f, t or F, T
    //       d: 0 or 1
    //       O: --false-- --true--
    void print_boolean(const Object *val_ptr) {
        assert(val_ptr->is_boolean());

        if (m_type == TYPE_DEFAULT) {
            m_type = 's';
        }
        if ((m_type != 'c') && (m_type != 'd') && (m_type != 's') && (m_type != 'O')) {
            m_trx->error(Error::InvalidFormatString, "unsupported format type '{:c}' for boolean", m_type);
        } else if (m_type == 'd') {
            char buffer[2]{val_ptr->boolean_value() ? '1' : '0', '\0'};
            numeric_outputter(buffer, buffer + 1);
        } else {
            using namespace std::literals::string_view_literals;

            auto sv = val_ptr->boolean_value() ? "--true--"sv : "--false--"sv;
            auto data = sv.data();
            auto limit = (data + Trix::sv_length(sv));
            auto max_width = -1;
            if (m_type != 'O') {
                if (m_alt) {
                    // upper case
                    sv = val_ptr->boolean_value() ? "TRUE"sv : "FALSE"sv;
                    data = sv.data();

                    // c is only first character
                    limit = (m_type == 'c') ? (data + 1) : (data + Trix::sv_length(sv));
                } else {
                    // lower case trim leading and trailing "--"
                    data += 2;

                    // c is only first character
                    limit = (m_type == 'c') ? (data + 1) : (limit - 2);
                }
                max_width = m_precision;
            }
            string_outputter(data, limit, max_width);
        }
    }

    // none, s: name
    //       O: /literal or \executable
    void print_name(const Object *val_ptr) {
        assert(val_ptr->is_name());

        if (m_type == TYPE_DEFAULT) {
            m_type = 's';
        }
        if ((m_type != 's') && (m_type != 'O')) {
            m_trx->error(Error::InvalidFormatString, "unsupported format type '{:c}' for name", m_type);
        } else {
            auto cstring_sv = val_ptr->name_sv(m_trx);
            auto ptr = cstring_sv.data();
            auto length = Trix::sv_length(cstring_sv);

            if (m_type == 'O') {
                char buffer[Trix::MaxNameLength + 8];
                auto limit = &buffer[1 + length];

                buffer[0] = val_ptr->is_executable() ? '\\' : '/';
                std::copy_n(ptr, length, &buffer[1]);
                string_outputter(buffer, limit);
            } else {
                auto limit = (ptr + length);
                string_outputter(ptr, limit, m_precision);
            }
        }
    }

    [[nodiscard]] char *print_text(char *ptr, const char *limit, const char *src_ptr, const char *src_limit) {
        *ptr++ = '(';
        // can be 4 characters, \x58, plus )
        while ((src_ptr < src_limit) && ((ptr + 5) < limit)) {
            auto value = *src_ptr++;
            ptr = to_ch(m_type, ptr, static_cast<vm_t>(value));
        }
        if (src_ptr < src_limit) {
            m_trx->error(Error::VMFull, "print_text: output buffer too small for string conversion");
        } else {
            *ptr++ = ')';
            return ptr;
        }
    }

    [[nodiscard]] char *print_base16(char *ptr, const char *limit, const char *src_ptr, const char *src_limit) {
        *ptr++ = '<';
        // needs 2 hex characters, plus >
        while ((src_ptr < src_limit) && ((ptr + 3) < limit)) {
            auto value = *src_ptr++;
            auto [hi, lo] = to_hex(static_cast<vm_t>(value), m_alt);
            *ptr++ = hi;
            *ptr++ = lo;
        }
        if (src_ptr < src_limit) {
            m_trx->error(Error::VMFull, "print_base16: output buffer too small for hex conversion");
        } else {
            *ptr++ = '>';
            return ptr;
        }
    }

    // 8 bit input
    // +--+--+--+--+--+--+--+--+  +--+--+--+--+--+--+--+--+  +--+--+--+--+--+--+--+--+
    // |A5|A4|A3|A2|A1|A0|B5|B4|  |B3|B2|B1|B0|C5|C4|C3|C2|  |C1|C0|D5|D4|D3|D2|D1|D0|
    // +--+--+--+--+--+--+--+--+  +--+--+--+--+--+--+--+--+  +--+--+--+--+--+--+--+--+
    //
    // none, s: copy to output
    //       ?: copy to output using escapes if not printable, \r \xFF
    //       O: string form: (text) with suffix l or x, r or w
    //       x: base16 form: <16> with suffix l or x, r or w
    void print_string(const Object *val_ptr) {
        assert(val_ptr->is_string());

        if (m_type == TYPE_DEFAULT) {
            m_type = 's';
        }
        if ((m_type != 's') && (m_type != '?') && (m_type != 'O') && (m_type != 'x')) {
            m_trx->error(Error::InvalidFormatString, "unsupported format type '{:c}' for string", m_type);
        } else {
            auto cstring_sv = val_ptr->sv_value(m_trx);
            auto src_ptr = cstring_sv.data();
            auto src_length = Trix::sv_length(cstring_sv);

            auto src_limit = (src_ptr + src_length);

            if (m_type == 's') {
                string_outputter(src_ptr, src_limit, m_precision);
            } else {
                // allocate temp buffer
                auto [ptr, available] = m_trx->vm_start_alloc<char>();
                if (available < (static_cast<vm_size_t>(src_length + 8))) {
                    m_trx->error(Error::VMFull, "print_string: insufficient VM space for string formatting");
                } else {
                    auto base = ptr;
                    auto limit = (ptr + (available - 8));

                    if (m_type == '?') {
                        // can be 4 characters, \x58
                        while ((src_ptr < src_limit) && ((ptr + 4) < limit)) {
                            auto value = *src_ptr++;
                            ptr = to_ch(m_type, ptr, static_cast<vm_t>(value));
                        }
                        if (src_ptr < src_limit) {
                            m_trx->error(Error::VMFull, "print_string: output buffer too small for escaped conversion");
                        }
                    } else {
                        // value
                        if (m_type == 'O') {
                            ptr = print_text(ptr, limit, src_ptr, src_limit);
                        } else if (m_type == 'x') {
                            ptr = print_base16(ptr, limit, src_ptr, src_limit);
                        }

                        // suffix
                        *ptr++ = '#';
                        if (m_alt) {
                            *ptr++ = val_ptr->is_executable() ? 'X' : 'L';
                            *ptr++ = val_ptr->has_readonly_access() ? 'R' : 'W';
                        } else {
                            *ptr++ = val_ptr->is_executable() ? 'x' : 'l';
                            *ptr++ = val_ptr->has_readonly_access() ? 'r' : 'w';
                        }
                    }

                    string_outputter(base, ptr);
                    m_trx->vm_end_alloc();
                }
            }
        }
    }

    // none, s: name
    //       O: --/add-- or --\add--
    void print_operator(const Object *val_ptr) {
        assert(val_ptr->is_operator());

        if (m_type == TYPE_DEFAULT) {
            m_type = 's';
        }
        if ((m_type != 's') && (m_type != 'O')) {
            m_trx->error(Error::InvalidFormatString, "unsupported format type '{:c}' for operator", m_type);
        } else {
            auto op_str = val_ptr->operator_string(m_trx);
            auto ptr = op_str.data();
            auto length = Trix::sv_length(op_str);

            if (m_type == 'O') {
                if (m_depth > 0) {
                    // Inside composite: bare operator name
                    auto limit = (ptr + length);
                    string_outputter(ptr, limit);
                } else {
                    // Top-level: --/add--
                    char buffer[Trix::MaxNameLength + 8];
                    auto limit = &buffer[5 + length];

                    buffer[0] = '-';
                    buffer[1] = '-';
                    buffer[2] = val_ptr->is_executable() ? '\\' : '/';
                    std::copy_n(ptr, length, &buffer[3]);
                    limit[-1] = '-';
                    limit[-2] = '-';

                    string_outputter(buffer, limit);
                }
            } else {
                // add (or ADD if alt)
                if (m_alt) {
                    char buffer[Trix::MaxNameLength];
                    std::copy_n(ptr, length, buffer);
                    uppercase_range(buffer, buffer + length);
                    string_outputter(buffer, buffer + length, m_precision);
                } else {
                    auto limit = (ptr + length);
                    string_outputter(ptr, limit, m_precision);
                }
            }
        }
    }

    // none, s: mark
    //       O: --mark--
    void print_mark([[maybe_unused]] const Object *val_ptr) {
        assert(val_ptr->is_mark());

        if (m_type == TYPE_DEFAULT) {
            m_type = 's';
        }
        if ((m_type != 's') && (m_type != 'O')) {
            m_trx->error(Error::InvalidFormatString, "unsupported format type '{:c}' for mark", m_type);
        } else {
            using namespace std::literals::string_view_literals;
            auto sv = "--mark--"sv;
            auto max_width = -1;
            if (m_type != 'O') {
                // drop leading "--" and trailing "--"
                sv = sv.substr(2, Trix::sv_length(sv) - 4);
                max_width = m_precision;
            }
            string_outputter(sv.data(), sv.data() + Trix::sv_length(sv), max_width);
        }
    }

    // none, s: null
    //       O: --/null-- or --\null--
    void print_null(const Object *val_ptr) {
        using namespace std::literals::string_view_literals;

        assert(val_ptr->is_null());

        if (m_type == TYPE_DEFAULT) {
            m_type = 's';
        }
        if ((m_type != 's') && (m_type != 'O')) {
            m_trx->error(Error::InvalidFormatString, "unsupported format type '{:c}' for null", m_type);
        } else {
            auto sv = val_ptr->is_literal() ? "--/null--"sv : "--\\null--"sv;
            auto max_width = -1;
            if (m_type == 's') {
                sv = m_alt ? "NULL"sv : "null"sv;
                max_width = m_precision;
            }
            string_outputter(sv.data(), sv.data() + Trix::sv_length(sv), max_width);
        }
    }

    // none, s: array
    //       O: [elem1 elem2 ...] or {elem1 elem2 ...}
    void print_array(const Object *val_ptr) {
        using namespace std::literals::string_view_literals;

        assert(val_ptr->is_array());

        if (m_type == TYPE_DEFAULT) {
            m_type = 's';
        }
        if ((m_type != 's') && (m_type != 'O')) {
            m_trx->error(Error::InvalidFormatString, "unsupported format type '{:c}' for array", m_type);
        } else if (m_type == 'O') {
            auto count = val_ptr->object_length();
            // Cycle check only for non-empty arrays (empty arrays may share offsets)
            if ((count == 0) || enter_container(val_ptr->offset())) {
                auto [open_ch, close_ch] = val_ptr->is_executable() ? std::pair{'{', '}'} : std::pair{'[', ']'};
                emit(static_cast<vm_t>(open_ch));
                auto elem_data = val_ptr->array_objects(m_trx);
                for (length_t i = 0; i < count; ++i) {
                    if (i != 0) {
                        emit(' ');
                    }
                    emit_sub_object(&elem_data[i]);
                }
                emit(static_cast<vm_t>(close_ch));
            }
        } else {
            auto sv = m_alt ? "ARRAY"sv : "array"sv;
            auto data = sv.data();
            string_outputter(data, (data + Trix::sv_length(sv)), m_precision);
        }
    }

    // none, s: packed
    //       O: [elem1 elem2 ...] or {elem1 elem2 ...}
    void print_packed(const Object *val_ptr) {
        using namespace std::literals::string_view_literals;

        assert(val_ptr->is_packed());

        if (m_type == TYPE_DEFAULT) {
            m_type = 's';
        }
        if ((m_type != 's') && (m_type != 'O')) {
            m_trx->error(Error::InvalidFormatString, "unsupported format type '{:c}' for packed", m_type);
        } else if (m_type == 'O') {
            auto [open_ch, close_ch] = val_ptr->is_executable() ? std::pair{'{', '}'} : std::pair{'[', ']'};
            emit(static_cast<vm_t>(open_ch));
            auto [packed_data, count] = val_ptr->packed_value(m_trx);
            for (length_t i = 0; i < count; ++i) {
                if (i != 0) {
                    emit(' ');
                }
                auto [next, elem] = Object::extract_next_packed(m_trx, packed_data);
                packed_data = next;
                emit_sub_object(&elem);
            }
            emit(static_cast<vm_t>(close_ch));
        } else {
            auto sv = m_alt ? "PACKED"sv : "packed"sv;
            auto data = sv.data();
            string_outputter(data, (data + Trix::sv_length(sv)), m_precision);
        }
    }

    // none, s: dict
    //       O: <</key1 val1 /key2 val2 ...>>
    void print_dict(const Object *val_ptr) {
        using namespace std::literals::string_view_literals;

        assert(val_ptr->is_dict());

        if (m_type == TYPE_DEFAULT) {
            m_type = 's';
        }
        if ((m_type != 's') && (m_type != 'O')) {
            m_trx->error(Error::InvalidFormatString, "unsupported format type '{:c}' for dict", m_type);
        } else if (m_type == 'O') {
            auto dict = val_ptr->dict_value(m_trx);
            // Cycle check only for non-empty dicts (empty dicts may share offsets)
            if ((dict->length() == 0) || enter_container(val_ptr->offset())) {
                emit('<');
                emit('<');
                auto entry = dict->next(m_trx, nulloffset, -1);
                bool first = true;
                while (entry.next_offset != nulloffset) {
                    if (!first) {
                        emit(' ');
                    }
                    first = false;
                    emit_sub_object(&entry.key);
                    emit(' ');
                    emit_sub_object(&entry.value);
                    entry = dict->next(m_trx, entry.next_offset, entry.next_bucket);
                }
                emit('>');
                emit('>');
            }
        } else {
            auto sv = m_alt ? "DICT"sv : "dict"sv;
            auto data = sv.data();
            string_outputter(data, data + Trix::sv_length(sv), m_precision);
        }
    }

    // none, s: set
    //       O: {{elem1 elem2 ...}}
    void print_set(const Object *val_ptr) {
        using namespace std::literals::string_view_literals;

        assert(val_ptr->is_set());

        if (m_type == TYPE_DEFAULT) {
            m_type = 's';
        }
        if ((m_type != 's') && (m_type != 'O')) {
            m_trx->error(Error::InvalidFormatString, "unsupported format type '{:c}' for set", m_type);
        } else if (m_type == 'O') {
            auto set = val_ptr->set_value(m_trx);
            // Cycle check only for non-empty sets (empty sets may share offsets)
            if ((set->length() == 0) || enter_container(val_ptr->offset())) {
                emit('{');
                emit('{');
                auto entry = set->set_next(m_trx, nulloffset, -1);
                bool first = true;
                while (entry.next_offset != nulloffset) {
                    if (!first) {
                        emit(' ');
                    }
                    first = false;
                    emit_sub_object(&entry.element);
                    entry = set->set_next(m_trx, entry.next_offset, entry.next_bucket);
                }
                emit('}');
                emit('}');
            }
        } else {
            auto sv = m_alt ? "SET"sv : "set"sv;
            auto data = sv.data();
            string_outputter(data, data + Trix::sv_length(sv), m_precision);
        }
    }

    // none, s: curry
    //       O: --curry functor arg--
    void print_curry(const Object *val_ptr) {
        using namespace std::literals::string_view_literals;

        assert(val_ptr->is_curry());

        if (m_type == TYPE_DEFAULT) {
            m_type = 's';
        }
        if ((m_type != 's') && (m_type != 'O')) {
            m_trx->error(Error::InvalidFormatString, "unsupported format type '{:c}' for curry", m_type);
        } else if (m_type == 'O') {
            auto pair = val_ptr->curry_storage(m_trx);
            emit_cstr("--curry ");
            emit_sub_object(&pair[0]);
            emit(' ');
            emit_sub_object(&pair[1]);
            emit_cstr("--");
        } else {
            auto sv = m_alt ? "CURRY"sv : "curry"sv;
            auto data = sv.data();
            string_outputter(data, data + Trix::sv_length(sv), m_precision);
        }
    }

    // none, s: tagged
    //       O: /tag payload
    void print_tagged(const Object *val_ptr) {
        using namespace std::literals::string_view_literals;

        assert(val_ptr->is_tagged());

        if (m_type == TYPE_DEFAULT) {
            m_type = 's';
        }
        if ((m_type != 's') && (m_type != 'O')) {
            m_trx->error(Error::InvalidFormatString, "unsupported format type '{:c}' for tagged", m_type);
        } else if (m_type == 'O') {
            auto pair = val_ptr->tagged_storage(m_trx);

            // Logic variable: --lvar:name:value-- or --lvar:unbound--
            if (pair[0].is_name() && (pair[0].name_offset() == m_trx->wellknown_offset(WellKnownName::LVar))) {
                auto payload = pair[1];
                auto elem_data = payload.array_objects(m_trx);
                auto arr_len = payload.object_length();
                auto sv = m_alt ? "LVAR"sv : "lvar"sv;

                // Extract debug name if present (named-var stores it in elem_data[1])
                auto debug_name = ((arr_len >= 2) && elem_data[1].is_name()) ? elem_data[1].name_sv(m_trx) : ""sv;

                char buffer[96];
                if (elem_data[0].is_null()) {
                    if (debug_name.empty()) {
                        auto [ptr, _] = std::format_to_n(buffer, sizeof(buffer), "--{}:unbound--", sv);
                        string_outputter(buffer, ptr);
                    } else {
                        auto [ptr, _] = std::format_to_n(buffer, sizeof(buffer), "--{}:{}:unbound--", sv, debug_name);
                        string_outputter(buffer, ptr);
                    }
                } else {
                    char val_buf[32];
                    auto [val_count, val_dropped] = PrintFmt::process_object(
                            m_trx, &elem_data[0], reinterpret_cast<vm_t *>(val_buf), static_cast<length_t>(sizeof(val_buf)));
                    if (val_dropped != 0) {
                        val_buf[29] = val_buf[30] = val_buf[31] = '.';
                    }
                    auto val_sv = std::string_view(val_buf, static_cast<size_t>(val_count));
                    if (debug_name.empty()) {
                        auto [ptr, _] = std::format_to_n(buffer, sizeof(buffer), "--{}:{}--", sv, val_sv);
                        string_outputter(buffer, ptr);
                    } else {
                        auto [ptr, _] = std::format_to_n(buffer, sizeof(buffer), "--{}:{}:{}--", sv, debug_name, val_sv);
                        string_outputter(buffer, ptr);
                    }
                }
                return;
            } else {
                emit_sub_object(&pair[0]);
                emit(' ');
                emit_sub_object(&pair[1]);
            }
        } else {
            auto sv = m_alt ? "TAGGED"sv : "tagged"sv;
            auto data = sv.data();
            string_outputter(data, data + Trix::sv_length(sv), m_precision);
        }
    }

    // none, s: thunk
    //       O: --thunk result-- (evaluated) or --thunk(state)-- (other)
    void print_thunk(const Object *val_ptr) {
        using namespace std::literals::string_view_literals;

        assert(val_ptr->is_thunk());

        if (m_type == TYPE_DEFAULT) {
            m_type = 's';
        }
        if ((m_type != 's') && (m_type != 'O')) {
            m_trx->error(Error::InvalidFormatString, "unsupported format type '{:c}' for thunk", m_type);
        } else {
            auto storage = val_ptr->thunk_storage(m_trx);
            auto state = storage[Object::ThunkStorageState].integer_value();

            if (m_type == 'O') {
                auto sv = m_alt ? "THUNK"sv : "thunk"sv;
                if (state == Object::ThunkEvaluated) {
                    emit_cstr("--");
                    emit_sv(sv);
                    emit(' ');
                    emit_sub_object(&storage[Object::ThunkStorageResult]);
                    emit_cstr("--");
                } else {
                    auto state_sv = (state == Object::ThunkEvaluating) ? "evaluating"sv : "unevaluated"sv;
                    char buffer[64];
                    auto [ptr, _] = std::format_to_n(buffer, sizeof(buffer), "--{}({})--", sv, state_sv);
                    string_outputter(buffer, ptr);
                }
            } else {
                auto sv = m_alt ? "THUNK"sv : "thunk"sv;
                auto data = sv.data();
                string_outputter(data, data + Trix::sv_length(sv), m_precision);
            }
        }
    }

    // none, s: record
    //       O: --record {/field1: val1 /field2: val2}--
    void print_record(const Object *val_ptr) {
        using namespace std::literals::string_view_literals;

        assert(val_ptr->is_record());

        if (m_type == TYPE_DEFAULT) {
            m_type = 's';
        }
        if ((m_type != 's') && (m_type != 'O')) {
            m_trx->error(Error::InvalidFormatString, "unsupported format type '{:c}' for record", m_type);
        } else if (m_type == 'O') {
            auto inst = val_ptr->record_instance(m_trx);
            auto schema = m_trx->offset_to_ptr<RecordSchema>(inst->m_schema);
            auto field_count = val_ptr->object_length();
            emit_cstr("--record {");
            for (length_t i = 0; i < field_count; ++i) {
                if (i != 0) {
                    emit(' ');
                }
                emit_sub_object(&schema->m_names[i]);
                emit(':');
                emit(' ');
                emit_sub_object(&inst->m_fields[i]);
            }
            emit_cstr("}--");
        } else {
            auto sv = m_alt ? "RECORD"sv : "record"sv;
            auto data = sv.data();
            string_outputter(data, data + Trix::sv_length(sv), m_precision);
        }
    }

    // none, s: coroutine
    //       O: --coroutine:status:used/cap--  (actor with mailbox)
    //       O: --coroutine:status--            (plain coroutine)
    void print_coroutine(const Object *val_ptr) {
        using namespace std::literals::string_view_literals;

        assert(val_ptr->is_coroutine());

        if (m_type == TYPE_DEFAULT) {
            m_type = 's';
        }
        if ((m_type != 's') && (m_type != 'O')) {
            m_trx->error(Error::InvalidFormatString, "unsupported format type '{:c}' for coroutine", m_type);
        } else if (m_type == 'O') {
            auto ctx = val_ptr->coroutine_context(m_trx);
            auto status_sv = (ctx->m_status == CoroutineContext::Dead)       ? "dead"sv
                             : (ctx->m_status == CoroutineContext::Running)  ? "running"sv
                             : (ctx->m_status == CoroutineContext::Ready)    ? "ready"sv
                             : (ctx->m_status == CoroutineContext::Sleeping) ? "sleeping"sv
                                                                             : "created"sv;

            // Extract debug name if present
            auto debug_name = (ctx->m_debug_name != nulloffset) ? m_trx->offset_to_ptr<Name>(ctx->m_debug_name)->sv() : ""sv;

            char buffer[96];
            if (ctx->m_mailbox != nulloffset) {
                auto mbx = m_trx->offset_to_ptr<MailboxHeader>(ctx->m_mailbox);
                auto sv = m_alt ? "ACTOR"sv : "actor"sv;
                if (debug_name.empty()) {
                    auto [ptr, _] = std::format_to_n(
                            buffer, sizeof(buffer), "--{}:{}:{}/{}--", sv, status_sv, mbx->m_count, mbx->m_capacity);
                    string_outputter(buffer, ptr);
                } else {
                    auto [ptr, _] = std::format_to_n(
                            buffer, sizeof(buffer), "--{}:{}:{}:{}/{}--", sv, debug_name, status_sv, mbx->m_count, mbx->m_capacity);
                    string_outputter(buffer, ptr);
                }
            } else {
                auto sv = m_alt ? "COROUTINE"sv : "coroutine"sv;
                if (debug_name.empty()) {
                    auto [ptr, _] = std::format_to_n(buffer, sizeof(buffer), "--{}:{}--", sv, status_sv);
                    string_outputter(buffer, ptr);
                } else {
                    auto [ptr, _] = std::format_to_n(buffer, sizeof(buffer), "--{}:{}:{}--", sv, debug_name, status_sv);
                    string_outputter(buffer, ptr);
                }
            }
        } else {
            auto sv = m_alt ? "COROUTINE"sv : "coroutine"sv;
            auto data = sv.data();
            string_outputter(data, data + Trix::sv_length(sv), m_precision);
        }
    }

    // none, s: pipe-buffer
    //       O: --pipe:count/cap--  or --pipe:closed--
    void print_pipe_buffer(const Object *val_ptr) {
        using namespace std::literals::string_view_literals;

        assert(val_ptr->is_pipe_buffer());

        if (m_type == TYPE_DEFAULT) {
            m_type = 's';
        }
        if ((m_type != 's') && (m_type != 'O')) {
            m_trx->error(Error::InvalidFormatString, "unsupported format type '{:c}' for pipe-buffer", m_type);
        } else if (m_type == 'O') {
            auto pipe = val_ptr->pipe_buffer_header(m_trx);
            auto sv = m_alt ? "PIPE"sv : "pipe"sv;
            char buffer[64];
            if (pipe->m_closed) {
                auto [ptr, _] = std::format_to_n(buffer, sizeof(buffer), "--{}:closed--", sv);
                string_outputter(buffer, ptr);
            } else {
                auto [ptr, _] = std::format_to_n(buffer, sizeof(buffer), "--{}:{}/{}--", sv, pipe->m_count, pipe->m_capacity);
                string_outputter(buffer, ptr);
            }
        } else {
            auto sv = m_alt ? "PIPEBUFFER"sv : "pipebuffer"sv;
            auto data = sv.data();
            string_outputter(data, data + Trix::sv_length(sv), m_precision);
        }
    }

    // none, s: cell
    //       O: --cell:value--  or --cell:dirty--  or --cell:disposed--
    void print_cell(const Object *val_ptr) {
        using namespace std::literals::string_view_literals;

        assert(val_ptr->is_cell());

        if (m_type == TYPE_DEFAULT) {
            m_type = 's';
        }
        if ((m_type != 's') && (m_type != 'O')) {
            m_trx->error(Error::InvalidFormatString, "unsupported format type '{:c}' for cell", m_type);
        } else if (m_type == 'O') {
            auto header = val_ptr->cell_header(m_trx);
            auto sv = m_alt ? "CELL"sv : "cell"sv;

            // Extract debug name if present
            auto debug_name = (header->m_debug_name != nulloffset) ? m_trx->offset_to_ptr<Name>(header->m_debug_name)->sv() : ""sv;

            char buffer[96];
            if (header->m_base == CellDisposed) {
                if (debug_name.empty()) {
                    auto [ptr, _] = std::format_to_n(buffer, sizeof(buffer), "--{}:disposed--", sv);
                    string_outputter(buffer, ptr);
                } else {
                    auto [ptr, _] = std::format_to_n(buffer, sizeof(buffer), "--{}:{}:disposed--", sv, debug_name);
                    string_outputter(buffer, ptr);
                }
            } else if (header->m_dirty != 0) {
                if (debug_name.empty()) {
                    auto [ptr, _] = std::format_to_n(buffer, sizeof(buffer), "--{}:dirty--", sv);
                    string_outputter(buffer, ptr);
                } else {
                    auto [ptr, _] = std::format_to_n(buffer, sizeof(buffer), "--{}:{}:dirty--", sv, debug_name);
                    string_outputter(buffer, ptr);
                }
            } else {
                // Print the cached value inline using process_object into a sub-buffer
                char val_buf[32];
                auto [val_count, val_dropped] = PrintFmt::process_object(
                        m_trx, &header->m_value, reinterpret_cast<vm_t *>(val_buf), static_cast<length_t>(sizeof(val_buf)));
                if (val_dropped != 0) {
                    val_buf[29] = val_buf[30] = val_buf[31] = '.';
                }
                auto val_sv = std::string_view(val_buf, static_cast<size_t>(val_count));
                if (debug_name.empty()) {
                    auto [ptr, _] = std::format_to_n(buffer, sizeof(buffer), "--{}:{}--", sv, val_sv);
                    string_outputter(buffer, ptr);
                } else {
                    auto [ptr, _] = std::format_to_n(buffer, sizeof(buffer), "--{}:{}:{}--", sv, debug_name, val_sv);
                    string_outputter(buffer, ptr);
                }
            }
        } else {
            auto sv = m_alt ? "CELL"sv : "cell"sv;
            auto data = sv.data();
            string_outputter(data, data + Trix::sv_length(sv), m_precision);
        }
    }

    // none, s: continuation
    //       O: --continuation:exec=N-- or --continuation:spent--
    void print_continuation(const Object *val_ptr) {
        using namespace std::literals::string_view_literals;

        assert(val_ptr->is_continuation());

        if (m_type == TYPE_DEFAULT) {
            m_type = 's';
        }
        if ((m_type != 's') && (m_type != 'O')) {
            m_trx->error(Error::InvalidFormatString, "unsupported format type '{:c}' for continuation", m_type);
        } else if (m_type == 'O') {
            auto ctx = val_ptr->continuation_context(m_trx);
            auto sv = m_alt ? "CONTINUATION"sv : "continuation"sv;
            char buffer[64];
            if (ctx->is_spent()) {
                auto [ptr, _] = std::format_to_n(buffer, sizeof(buffer), "--{}:spent--", sv);
                string_outputter(buffer, ptr);
            } else {
                auto [ptr, _] = std::format_to_n(buffer, sizeof(buffer), "--{}:exec={}--", sv, ctx->m_exec_count);
                string_outputter(buffer, ptr);
            }
        } else {
            auto sv = m_alt ? "CONTINUATION"sv : "continuation"sv;
            auto data = sv.data();
            string_outputter(data, data + Trix::sv_length(sv), m_precision);
        }
    }

    // none, s: opaque handle (kind name only: "screen" / "tilemap" / ...)
    //       O: --screen:COLSxROWS--, future --tilemap:...-- (kind-dependent
    //          with state-derived metadata wrapped in -- markers)
    void print_handle(const Object *val_ptr) {
        using namespace std::literals::string_view_literals;

        assert(val_ptr->is_handle());

        if (m_type == TYPE_DEFAULT) {
            m_type = 's';
        }
        if ((m_type != 's') && (m_type != 'O')) {
            m_trx->error(Error::InvalidFormatString, "unsupported format type '{:c}' for opaque-handle", m_type);
        } else {
            std::string_view sv;
            switch (val_ptr->handle_kind()) {
            case Object::HandleKind::Screen:
                sv = m_alt ? "SCREEN"sv : "screen"sv;
                break;
            }

            if (m_type == 'O') {
                char buffer[64];
                switch (val_ptr->handle_kind()) {
                case Object::HandleKind::Screen: {
                    auto state = m_trx->offset_to_ptr<ScreenState>(val_ptr->handle_offset());
                    auto [ptr, _] = std::format_to_n(buffer, sizeof(buffer), "--{}:{}x{}--", sv, state->m_cols, state->m_rows);
                    // O-form is the machine/debug-readable shape; like every sibling O-form
                    // printer it is NOT precision-truncated (precision only trims the bare
                    // type-name in the 's' path below).
                    string_outputter(buffer, ptr);
                    break;
                }
                }
            } else {
                auto data = sv.data();
                string_outputter(data, data + Trix::sv_length(sv), m_precision);
            }
        }
    }

    // none, s: stream
    //       O: --/stream sid(r)--, --\stream sid(w)--
    void print_stream(const Object *val_ptr) {
        using namespace std::literals::string_view_literals;

        assert(val_ptr->is_stream());

        if (m_type == TYPE_DEFAULT) {
            m_type = 's';
        }
        if ((m_type != 's') && (m_type != 'O')) {
            m_trx->error(Error::InvalidFormatString, "unsupported format type '{:c}' for stream", m_type);
        } else {
            auto sv = m_alt ? "STREAM"sv : "stream"sv;
            if (m_type == 'O') {
                // --/(r)stream 65535--
                char buffer[24];

                auto attrib = val_ptr->is_executable() ? '\\' : '/';
                auto access = [&]() -> char { return (val_ptr->has_write_access() ? (m_alt ? 'W' : 'w') : (m_alt ? 'R' : 'r')); }();
                auto sid = val_ptr->stream_sid();
                auto [ptr, _] = std::format_to_n(buffer, sizeof(buffer), "--{}{} {}({})--", attrib, sv, sid, access);
                string_outputter(buffer, ptr);
            } else {
                auto data = sv.data();
                string_outputter(data, (data + Trix::sv_length(sv)), m_precision);
            }
        }
    }

    Trix *m_trx;
    const Object *m_args_ptr;
    const char *m_format_limit;
    const char *m_format_ptr;
    Stream *m_output_stream;
    vm_t *m_output_ptr;
    int m_next_arg_id;
    int m_width;
    int m_precision;
    length_t m_args_count;
    length_t m_output_capacity;
    int m_output_count;   // not length_t: the stream-output path is unbounded and would wrap
    int m_dropped_count;  // (process() returns these as pair<int,int>)
    vm_t m_fill;
    vm_t m_align;
    vm_t m_sign;
    bool m_alt;
    bool m_zero;
    vm_t m_type;
    // Strftime template body for the `:I` (instant) format-spec.  When
    // m_type == 'I' this slice points into the format string between the
    // type letter (and optional 'l' local-zone flag) and the closing '}'.
    const char *m_chrono_template_ptr;
    length_t m_chrono_template_len;
    bool m_chrono_local;
    int m_depth;                           // recursion depth for object-form printing
    vm_offset_t m_visited[MaxPrintDepth];  // cycle detection: offsets of entered containers
};
