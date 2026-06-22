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

//===--- Scanner Implementation ---===//
//
// Implements the lexical scanner (tokenizer) that converts character input
// into (Lexeme, Object) pairs for the interpreter.  The scanner is the
// front end of the Trix language: it reads from streams or strings and
// produces the tokens that the interpreter dispatches.  Based on:
//
//   PostScript scanner: the `token` operator and the internal scanner that
//   reads from the current file.  Handles the lexical grammar: numbers,
//   names, strings, procedures, arrays, hex strings, and comments.  Trix
//   extends this with nestable block comments, infix expressions, suffix
//   notation (numeric types and string/procedure modifiers), scan-time
//   string interpolation, local-variable binding, set literals, dict-path
//   lookups, raw strings, and explicit executable-name syntax.
//
// --- Core concepts for maintainers ---
//
// SCANNER ENTRY POINTS
//   Stream::scanner(trx)  -- scan one token from a stream's buffer.
//     Called by the interpreter when an executable stream is on the exec
//     stack.  Returns (Lexeme, Object) or (EndOfStream, _).
//   Stream::scanner(trx, string)  -- scan one token from a string.
//     Called by execute_string() and the `token` operator.
//     Returns (Lexeme, Object, remaining-string).
//
// CHARACTER ATTRIBUTES (chattr_t)
//   Each byte value (and EOF) has a precomputed chattr_t attribute in the
//   sm_chdata[] table (indexed via sm_chattr, scanner_tables.inl).
//   Attributes classify characters as:
//   whitespace, delimiter, digit, letter, sign, etc.  The scanner uses
//   these to make fast per-character decisions without branching on
//   character ranges.
//
// TOKEN TYPES (Lexeme)
//   LiteralValue:     numeric literal, literal name (/foo), string, array,
//                     dict, boolean, null, mark -- pushed to operand stack.
//   ExecutableValue:  executable name (foo, \foo), executable string,
//                     executable array -- dispatched by the interpreter.
//   SyntaxError:      malformed token -- carries an error string.
//   EndOfStream:      no more tokens available.
//   EndOfProcedure:   matching '}' closing a procedure body being scanned.
//
// LEXICAL ELEMENTS
//   Comments:
//     % to end of line (line comment, skipped)
//     %{ ... %} (block comment, nestable, skipped)
//
//   Numbers:      delegated to Number::scan() (number.inl)
//
//   Names:
//     /name    (literal name)
//     name     (executable name -- triggers dictionary lookup)
//     \name    (executable name -- explicit backslash form)
//     //name   (immediate literal lookup -- resolved at scan time)
//     \\name   (immediate executable lookup -- resolved at scan time)
//     //:dict:key  (dictionary path -- hierarchical lookup through named dicts)
//     \\:dict:key  (executable dictionary path -- resolved at scan time)
//
//   Field access (see scan_field_access):
//     .name        sugar for /name get; chains: .a.b -> /a get /b get
//     .5 / .002    leading-dot real-number literals
//
//   Strings:
//     (text)       standard string; nested balanced (...) pairs kept verbatim.
//                  Escape sequences:
//                    \a \b \e \f \n \r \t \v \\ \( \)
//                    \NNN (1-3 octal digits), \xH[H] (1-2 hex digits)
//                    \uXXXX / \UXXXXXXXX (Unicode -> UTF-8), \^X (control)
//                    \CR / \LF (line continuation, suppressed)
//                    \{name} -- scan-time interpolation (see below)
//     <(text)>     raw string -- no escape processing, nestable parens
//     <hex>        hex string -- hex-encoded bytes (e.g., <48656C6C6F>)
//
//   String suffixes (apply to (...), <(...)>, and <hex> strings):
//     #=   use temporary =string storage (no heap allocation)
//     #$   force allocation in global VM region
//     #$$  force allocation in local VM region
//     #x   make executable; #l keep literal (default)
//     #r   make read-only; #w make read-write (default)
//     #b   make a single byte (string must be length 1)
//     #a   scan as byte array (each byte becomes an array element)
//     (=/$/$$ are mutually exclusive)
//
//   Procedures:
//     { body }     scanned recursively via scan_procedure; produces a
//                  Packed array by default for space efficiency.
//     Proc suffixes: #= (use =proc), #$ (force-global), #$$ (force-local),
//       #e (early bind), #l (late bind), #a (array not packed),
//       #p (force packed), #r (read-only), #w (read-write), #pw (packed + read-write).
//       First slot =/$/$$ are mutually exclusive; the rest combine freely.
//       Combinable: }#er (early-binding + read-only).
//
//   Arrays:       [ elements ] (scanned via scan_array)
//   Dicts:        << key value pairs >> (scanned via scan_dict)
//   Sets:         {{ elements }} (scanned via scan_set)
//   Array/Dict/Set suffixes (after ], >>, or }}): #[=|$|$$][r|w]
//     #= use =array/=dict/=set scratch; #$ force-global; #$$ force-local;
//     #r read-only; #w read-write (default).  =/$/$$ are mutually exclusive.
//
//   Infix expressions (see scanner_infix.inl):
//     $( expr )    strict infix math (no type promotion)
//     $[ expr ]    auto-promote infix math (widens types automatically)
//     Pratt parser (precedence climbing) converts infix notation to
//     postfix tokens that the interpreter executes normally.
//     Supports: + - * / % ** (power), comparisons, bitwise ops, ternary,
//     parenthesized sub-expressions, function calls, unary +/-/!/~.
//
//   $ prefix family (single $ = force-global / infix; doubled $$ = force-local):
//     $/foo        force-global literal name
//     $\foo        force-global executable name
//     ${ body }    force-global allocation scope block
//     $$/foo       force-local literal name
//     $$\foo       force-local executable name
//     $${ body }   force-local allocation scope block
//     $( expr )    strict infix expression
//     $[ expr ]    promoted (auto-widening) infix expression
//
//   Binary tokens: 0x80-0xFF range (compact bytecode encoding, see
//                  binary_tokens.inl)
//
// STRING INTERPOLATION
//   Inside (...) strings, \{name} triggers scan-time interpolation:
//   the name is looked up (like //) and its value is spliced into the
//   string as text via PrintFmt::process_object().  Supports dict paths:
//   \{:systemdict:key}.  All types are supported (numbers, booleans,
//   arrays, dicts, etc. are formatted as their default string form).
//
// SCAN PROCEDURE (scan_procedure)
//   { ... } creates a packed or regular array depending on content and
//   suffixes.  Nested procedures are handled recursively (depth limited
//   by MaxProcNesting).  The scanner accumulates body elements in the
//   operand-stack scratch region (via ProcScanState, above m_op_ptr up
//   to m_op_limit) and packs them into a Packed array for space
//   efficiency.  Suffix processing happens after the closing '}'.
//
// BINARY TOKENS
//   Bytes in the 0x80-0xFF range are binary-encoded tokens (see
//   binary_tokens.inl).  These provide compact bytecode representation
//   for snap-shot/thaw serialization and space-efficient program storage.
//

// A scanner emits a (Lexeme classification, Object payload) token: the Lexeme
// distinguishes LiteralValue / ExecutableValue / SyntaxError / EndOfStream /
// EndOfProcedure; the Object carries the scanned value or an error string.
using ScanToken = std::pair<Lexeme, Object>;

// Wrap an Object in the LiteralValue ScanToken returned throughout the scanner.
[[nodiscard]] static ScanToken literal_pair(Object val_obj) {
    return ScanToken{Lexeme::LiteralValue, val_obj};
}

// Wrap an Object in the ExecutableValue ScanToken returned throughout the scanner.
[[nodiscard]] static ScanToken executable_pair(Object val_obj) {
    return ScanToken{Lexeme::ExecutableValue, val_obj};
}

// Wrap an Object in a SyntaxError pair for scanner error returns.
[[nodiscard]] static ScanToken syntaxerror_pair(Object err_obj) {
    return ScanToken{Lexeme::SyntaxError, err_obj};
}

// Validate and strip underscore separators from a numeric token.
// Returns the stripped length (0 = invalid placement).
// Placement rules: '_' must be flanked by two valid digit characters in
// the local context.  Local context is determined per-region:
//   - BASE#DIGITS# radix region: hex digits (0-9, A-F, a-f).
//   - 0x / 0X prefix, up to the exponent introducer 'p'/'P': hex digits.
//   - 0o / 0O prefix: octal digits (0-7).
//   - 0b / 0B prefix: binary digits (0, 1).
//   - Everywhere else (decimal mantissa, exponent, etc.): decimal digits.
// Leading, trailing, doubled, or adjacent to sign/./e/E/p/P/#/suffix are invalid.
[[nodiscard]] static length_t strip_number_underscores(const vm_t *src, length_t token_length, vm_t *dst) {
    // BASE#DIGITS# radix region: positions strictly between the two '#' characters.
    auto radix_start = length_t{0};
    auto radix_end = length_t{0};
    auto first_hash = static_cast<const vm_t *>(std::memchr(src, '#', token_length));
    if (first_hash != nullptr) {
        radix_start = static_cast<length_t>((first_hash - src) + 1);
        auto last_hash = static_cast<const vm_t *>(std::memchr(first_hash + 1, '#', token_length - radix_start));
        radix_end = (last_hash != nullptr) ? static_cast<length_t>(last_hash - src) : 0;
    }

    // 0x / 0o / 0b prefix region: starts at index 2.  For hex (only form
    // that supports floats), region extends through any '.' fractional part
    // and stops at the exponent introducer 'p'/'P'.  For octal/binary the
    // region runs to token end; non-digit chars (type suffix letters etc.)
    // are naturally rejected by the predicate so an underscore adjacent to
    // them is invalid.
    enum struct PrefixKind { None, Hex, Octal, Binary };
    auto prefix_kind = PrefixKind::None;
    auto prefix_region_end = length_t{0};
    if ((token_length >= 3) && (src[0] == '0')) {
        auto p_lc = Trix::to_lowercase(src[1]);
        if (p_lc == 'x') {
            prefix_kind = PrefixKind::Hex;
        } else if (p_lc == 'o') {
            prefix_kind = PrefixKind::Octal;
        } else if (p_lc == 'b') {
            prefix_kind = PrefixKind::Binary;
        }
    }
    if (prefix_kind != PrefixKind::None) {
        prefix_region_end = token_length;
        if (prefix_kind == PrefixKind::Hex) {
            for (length_t i = 2; i < token_length; ++i) {
                auto c_lc = Trix::to_lowercase(src[i]);
                if (c_lc == 'p') {
                    prefix_region_end = i;
                    break;
                }
            }
        }
    }

    auto dptr = dst;
    for (length_t i = 0; i < token_length; ++i) {
        if (src[i] == '_') {
            // must not be first, last, or doubled
            if ((i == 0) || (i == (token_length - 1)) || (src[i - 1] == '_')) {
                return 0;
            } else {
                auto valid_neighbors = false;
                if ((radix_end > radix_start) && (i > radix_start) && (i < radix_end)) {
                    valid_neighbors = Trix::is_hexdigit(src[i - 1]) && Trix::is_hexdigit(src[i + 1]);
                } else if ((prefix_kind == PrefixKind::Hex) && (i >= 2) && (i < prefix_region_end)) {
                    valid_neighbors = Trix::is_hexdigit(src[i - 1]) && Trix::is_hexdigit(src[i + 1]);
                } else if ((prefix_kind == PrefixKind::Octal) && (i >= 2) && (i < prefix_region_end)) {
                    valid_neighbors = Trix::is_octaldigit(src[i - 1]) && Trix::is_octaldigit(src[i + 1]);
                } else if ((prefix_kind == PrefixKind::Binary) && (i >= 2) && (i < prefix_region_end)) {
                    auto is_bin = [](vm_t ch) { return ((ch == '0') || (ch == '1')); };
                    valid_neighbors = is_bin(src[i - 1]) && is_bin(src[i + 1]);
                } else {
                    valid_neighbors = Trix::is_digit(src[i - 1]) && Trix::is_digit(src[i + 1]);
                }
                if (!valid_neighbors) {
                    return 0;
                } else {
                    // skip the underscore
                }
            }
        } else {
            *dptr++ = src[i];
        }
    }
    return static_cast<length_t>(dptr - dst);
}

// Try parsing a token as a number, handling underscore separators.
// Returns number object on success, null object on failure.
[[nodiscard]] static Object try_parse_number(Trix *trx, std::string_view token_sv) {
    vm_t stripped[MaxNameLength];

    auto token_base = reinterpret_cast<const vm_t *>(token_sv.data());
    auto token_length = sv_length(token_sv);
    auto has_underscore = (token_length > 1) && (std::memchr(token_base, '_', token_length) != nullptr);
    if (has_underscore) {
        auto stripped_length = strip_number_underscores(token_base, token_length, stripped);
        if (stripped_length > 0) {
            token_sv = make_sv(stripped, stripped_length);
        }
    }
    const Number number(trx, token_sv);
    return (number.is_valid() ? number.make_object() : Object::make_null());
}

#include "binary_tokens.inl"

// Buffered scan of regular characters into the token buffer.
// Scans m_rptr..m_rlimit using raw pointers, copying regular characters to
// *token_ptr (up to token_limit) and incrementing token_length for each.
// Stops when a non-regular character is found or token_length exceeds
// MaxNameLength (early exit prevents uint16_t wrap).  Returns the final
// token_length; caller checks for overflow against MaxNameLength.
// Performance note: duplicates consume() logic intentionally; the loop
// condition guarantees (m_rptr != m_rlimit), and is_regular ensures no LF
// (no m_line_number update needed).  Update here if consume() changes.
[[nodiscard]] length_t scan_regular_chars(Trix *trx, vm_t *token_ptr, const vm_t *token_limit, length_t token_length) {
    auto done = false;
    while (true) {
        // refresh our view of the read buffer pointer and limit
        auto rptr = trx->offset_to_ptr<vm_t>(m_rptr_offset);
        auto rlimit = trx->offset_to_ptr<vm_t>(m_rlimit_offset);
        auto inner_start = rptr;
        while (rptr != rlimit) {
            auto ch = static_cast<int>(*rptr);
            if (!Trix::is_regular(ch)) {
                done = true;
                break;
            } else {
                if (token_ptr < token_limit) {
                    *token_ptr++ = static_cast<vm_t>(ch);
                }
                ++token_length;
                ++rptr;
                if (token_length > MaxNameLength) {
                    done = true;
                    break;
                }
            }
        }
        // batch the m_line_pos update from the inner loop: is_regular excludes LF, so
        // no m_line_number bumps can occur and the per-byte ++m_line_pos is reducible
        // to a single add at inner-loop exit.
        m_line_pos = static_cast<decltype(m_line_pos)>(m_line_pos + (rptr - inner_start));
        // update the stream view of the read buffer pointer
        m_rptr_offset = trx->ptr_to_offset(rptr);
        if (done || !fill(trx)) {
            return token_length;
        }
    }
}

// RAII guard for the scanner-side allocation flag.  Saves
// m_curr_alloc_global on construction, sets it true, restores it on
// destruction -- including on C++ unwind out of a scan_procedure /
// scan_name call that threw.  Use this at scanner scopes that want
// scanner-time allocations to land in global VM ($/foo, $\foo,
// ${...}).  The runtime-side @in-global op uses a different
// mechanism (saves the flag on the err stack) because the proc runs
// asynchronously through the dispatch loop, not synchronously within
// a C++ call frame.
struct GlobalAllocScope {
    Trix *trx;
    bool saved;

    explicit GlobalAllocScope(Trix *t) : trx(t), saved(t->m_curr_alloc_global) { trx->m_curr_alloc_global = true; }

    ~GlobalAllocScope() { trx->m_curr_alloc_global = saved; }
};

// RAII guard for the scanner-side allocation flag, force-local
// inverse of GlobalAllocScope.  Saves m_curr_alloc_global on
// construction, sets it false, restores it on destruction (including
// on C++ unwind out of a scan_procedure / scan_name call that threw).
// Used by $$/foo, $$\foo, $${...} to force names interned during the
// scoped scan to land in local VM, even when an enclosing ${...} or
// set-global true would otherwise intern them globally.
struct LocalAllocScope {
    Trix *trx;
    bool saved;

    explicit LocalAllocScope(Trix *t) : trx(t), saved(t->m_curr_alloc_global) { trx->m_curr_alloc_global = false; }

    ~LocalAllocScope() { trx->m_curr_alloc_global = saved; }
};

// Scans a regular-character token starting with ch (already consumed from the stream).
// Accumulates subsequent regular characters using the buffered fast path, then attempts
// to parse the result as a number (Byte, Integer, UInteger, Long, ULong, Address, Real,
// Double).  Returns {LiteralValue, number} on success, or {ExecutableValue, Name} if
// the token is not a valid number literal.
// Raises: NumericalOverflow from number.scan() for out-of-range literals (e.g.
//   2147483648i, 3.5e38r); LimitCheck if the token length exceeds MaxNameLength;
//   VMFull from number.make_object() for Long/ULong/Address/Double numeric literals,
//   or from Name::make() for name tokens.
struct ProcScanState;  // forward decl
[[nodiscard]] ScanToken scan_number_or_name(Trix *trx, int ch, int proc_nesting = 0, ProcScanState *state = nullptr) {
    auto token_base = trx->m_token_base;
    auto token_limit = trx->m_token_limit;
    auto token_ptr = token_base;

    *token_ptr++ = static_cast<vm_t>(ch);
    auto token_length = scan_regular_chars(trx, token_ptr, token_limit, 1);
    token_ptr = token_base + token_length;

    // Number-shape tokens may contain up to one embedded '.' as a decimal point
    // (e.g., 3.14, 0x1.0p10, 1.5e10).  Non-number-shape tokens (names starting
    // with alpha/underscore) stop at '.' so field-access sugar can take over.
    // A '.' is accepted as number-interior only when followed by a character
    // that legitimately continues or terminates a number (digit/ws/EOF/
    // delimiter/exponent/float-suffix); otherwise it's left for scan_delimiter.
    auto has_dot = (ch == '.');
    auto is_number_shape = (Trix::is_integer(ch) || has_dot);
    while (is_number_shape && !has_dot && (token_length < MaxNameLength) && (peekc(trx) == '.')) {
        consume(trx);
        auto after_dot = peekc(trx);
        // Accept '.' into a number-shape token unless what follows clearly isn't token
        // content (i.e. a control char / syntax-error byte).  Accepting any regular char
        // covers digits, hex digits, exponent letters (e/E/p/P), suffix letters, underscore,
        // sign characters, etc.  End-of-token signals (EOF/whitespace/delimiter) also make
        // '.' a valid trailing decimal point ("3." = 3.0).  If Number::scan later rejects
        // the composite token, the whole token falls through to Name::make -- consistent
        // with pre-delimiter behavior for digit-led tokens that happen to contain a dot.
        auto dot_is_numeric = (after_dot == EOFc) || Trix::is_whitespace(after_dot) || Trix::is_delimiter(after_dot) ||
                              Trix::is_regular(after_dot);
        if (dot_is_numeric) {
            has_dot = true;
            if (token_ptr < token_limit) {
                *token_ptr++ = '.';
            }
            ++token_length;
            auto old_length = token_length;
            token_length = scan_regular_chars(trx, token_ptr, token_limit, token_length);
            token_ptr += (token_length - old_length);
        } else {
            ungetc(trx, '.');
            --m_line_pos;  // undo the consume()'s line-position bump ('.' is not LF)
            break;
        }
    }
    if (token_length <= MaxNameLength) {
        // $ prefix family dispatch.  Single $ means "force-global" /
        // "infix"; doubled $$ means "force-local" (symmetric inverse).
        //
        //   $(expr)    strict infix expression
        //   $[expr]    promoted (auto-widening) infix expression
        //   ${...}     force-global block
        //   $/foo      force-global literal name
        //   $\foo      force-global executable name
        //   $${...}    force-local block
        //   $$/foo     force-local literal name
        //   $$\foo     force-local executable name
        if ((token_length == 1) && (token_base[0] == '$')) {
            auto next = peekc(trx);
            if (next == '(') {
                consume(trx);
                return scan_infix_expression(trx, /*auto_promote=*/false, /*close_ch=*/')');
            } else if (next == '[') {
                // $[...] -- promoted (auto-widening) infix.  `[` and `]`
                // are not infix operators, so they serve as unambiguous
                // delimiters; the `$` prefix peek-dispatches before `[`
                // could be read as an array-literal opener.
                consume(trx);
                return scan_infix_expression(trx, /*auto_promote=*/true, /*close_ch=*/']');
            } else if (next == '{') {
                // ${...} -- force-global allocation scope block.  The body
                // is scanned with m_curr_alloc_global=true so literal names
                // interned during scanning go to global VM; the runtime
                // side fires a matching @in-global cleanup so the flag is
                // restored on body completion (or error unwind).
                consume(trx);
                return scan_global_block(trx, proc_nesting, state);
            } else if ((next == '/') || (next == '\\')) {
                // $/foo and $\foo -- force-global name prefixes.  Saves the
                // alloc-global flag, sets it true, calls scan_name for the
                // literal/executable name, and restores the flag.  The Name
                // struct is interned in the global VM region so it
                // survives save/restore.
                //
                // $//foo and $\\foo (immediate-lookup variants) are
                // rejected -- immediate lookup pushes a value, not a name,
                // so the global allocation prefix is meaningless there.
                consume(trx);
                if (peekc(trx) == next) {
                    trx->error(Error::SyntaxError, "$/ and $\\ do not accept immediate-lookup forms ($// or $\\\\)");
                } else {
                    const GlobalAllocScope scope(trx);
                    return scan_name(trx, next);
                }
            }
            // Fall through: bare `$` becomes a name (no following sigil
            // character matched above).
        } else if ((token_length == 2) && (token_base[0] == '$') && (token_base[1] == '$')) {
            auto next = peekc(trx);
            if (next == '{') {
                // $${...} -- force-local allocation scope block.  Inverse
                // of ${...}: body scans with m_curr_alloc_global=false so
                // literal names interned during the body stay in local VM
                // even if an enclosing ${...} or set-global true had
                // flipped the flag.  Same closer op (@end-in-global)
                // reused; only the opener (@in-local) differs.
                consume(trx);
                return scan_local_block(trx, proc_nesting, state);
            } else if ((next == '/') || (next == '\\')) {
                // $$/foo and $$\foo -- force-local name prefixes.  Symmetric
                // inverse of $/foo and $\foo: scoped scan with
                // m_curr_alloc_global=false so the Name interns in local
                // VM regardless of any enclosing global scope.
                consume(trx);
                if (peekc(trx) == next) {
                    trx->error(Error::SyntaxError, "$$/ and $$\\ do not accept immediate-lookup forms ($$// or $$\\\\)");
                } else {
                    const LocalAllocScope scope(trx);
                    return scan_name(trx, next);
                }
            }
            // Fall through: bare `$$` (no following sigil) becomes a name.
        }
        {
            auto token_sv = make_sv(token_base, token_length);
            auto number_obj = try_parse_number(trx, token_sv);
            if (number_obj.is_null()) {
                // executable name -- true, false, null, mark are NOT scanner keywords;
                // they are names in systemdict looked up by the interpreter, and can
                // be shadowed by user dictionaries (per PLRM 3rd ed., section 3.2-3.3)
                return executable_pair(Name::make(trx, token_sv, Object::ExecutableAttrib));
            } else {
                return literal_pair(number_obj);
            }
        }
    } else {
        trx->error(Error::LimitCheck, "name length {} exceeds maximum {}", token_length, MaxNameLength);
    }
}

// Scans a name token introduced by '/' (literal) or '\' (executable).
// ch: the opening delimiter, already consumed -- '/' or '\'.
// A doubled delimiter (//, \\) triggers immediate lookup: the resolved value
// is returned rather than the name itself.
// A lone '/' or '\' with no following regular characters produces the literal or
// executable name '/' or '\' respectively.  A doubled delimiter with no following
// regular characters (e.g. "// " or "\\ ") synthesizes '/' or '\' as the lookup
// key; the lookup then succeeds or raises Undefined as usual.
// Returns: {LiteralValue, Name} for a literal name (/name);
//   {ExecutableValue, Name} for an executable name (\name);
//   {LiteralValue, clone} for an immediate lookup (// or \\) whose value is
//   literal or ignores-execute; {ExecutableValue, clone} otherwise.
// Raises: Undefined for a failed immediate lookup; LimitCheck if the name exceeds
//   MaxNameLength; VMFull from Name::make() (literal/executable paths) or from
//   value->make_clone(trx) (immediate lookup path, for Long/ULong/Address/Double values).
[[nodiscard]] ScanToken scan_name(Trix *trx, int ch) {
    auto token_base = trx->m_token_base;
    auto token_limit = trx->m_token_limit;
    auto token_ptr = token_base;
    auto is_literal = (ch == '/');
    auto is_immediate = matchc(trx, static_cast<vm_t>(ch));

    // consume the name
    auto token_length = scan_regular_chars(trx, token_ptr, token_limit, 0);
    if (token_length <= MaxNameLength) {
        if (token_length == 0) {
            // The token /, a slash followed by no regular characters, is the literal name '/'
            // The token \, a backslash followed by no regular characters, is the executable name '\'
            token_base[0] = is_literal ? '/' : '\\';
            token_length = 1;
        }
        auto token_sv = make_sv(token_base, token_length);

        if (is_immediate) {
            // look up the name using the current dictionary stack or dictionary path and substitute the bound value
            // for // value Object is always pushed on the operand stack as if load operator was called
            // for \\ value Object is pushed on the operand stack if Object is literal or ignores execute attribute
            auto value_ptr = Name::string_search(trx, token_sv);
            if (value_ptr != nullptr) {
                auto lexeme = (is_literal || value_ptr->is_literal() || value_ptr->ignores_execute()) ? Lexeme::LiteralValue
                                                                                                      : Lexeme::ExecutableValue;
                return std::pair{lexeme, value_ptr->make_clone(trx)};
            } else {
                auto prefix = is_literal ? "//" : "\\\\";
                trx->error(Error::Undefined, "name {}{} is not associated with any Object", prefix, token_sv);
            }
        } else if (is_literal) {
            // literal name
            return literal_pair(Name::make(trx, token_sv, Object::LiteralAttrib));
        } else {
            // executable name
            return executable_pair(Name::make(trx, token_sv, Object::ExecutableAttrib));
        }
    } else {
        // identifier length exceeds implementation limit
        trx->error(Error::LimitCheck, "name length {} exceeds maximum {}", token_length, MaxNameLength);
    }
}

// Discriminator for which kind of composite literal is being scanned.
// Replaces the earlier convention of passing the opening-delimiter char
// (with '*' as a stand-in for proc, since procs share '{' with sets).
enum class BracketKind : uint8_t { String, Array, Dict, Set, Proc };

// Returns a string_view (not const char *) so fmt's type-deduction
// path matches the existing error() call sites that take string_view
// arguments -- avoiding a consteval-vs-by-ref instantiation that
// regressed when we tried bare const char *.
[[nodiscard]] static std::string_view bracket_name(BracketKind kind) {
    using namespace std::literals::string_view_literals;
    switch (kind) {
    case BracketKind::String:
        return "'string'"sv;

    case BracketKind::Array:
        return "'array'"sv;

    case BracketKind::Dict:
        return "'dict'"sv;

    case BracketKind::Set:
        return "'set'"sv;

    case BracketKind::Proc:
        return "'proc'"sv;
    }
    return "'unknown'"sv;  // unreachable; calms compiler warnings
}

// std::format_string<T...> instantiation with std::string_view arguments
// runs into a consteval check that fails in this header context, so we
// build the error message via std::format (heap-free; lands in a fixed
// buffer) and then hand the resulting view to the non-format overload
// of make_error_string.
[[nodiscard]] static Object make_suffix_error_string(Trix *trx, BracketKind kind) {
    char buf[128];
    auto result = std::format_to_n(
            buf, sizeof(buf), "whitespace, delimiter, or binary-token required after a {} suffix", bracket_name(kind));
    return Object::make_error_string(trx, std::string_view{buf, static_cast<size_t>(result.out - buf)});
}

[[nodiscard]] static Object make_suffix_separator_error_string(Trix *trx, BracketKind kind) {
    char buf[128];
    auto result = std::format_to_n(buf, sizeof(buf), "invalid {} suffix", bracket_name(kind));
    return Object::make_error_string(trx, std::string_view{buf, static_cast<size_t>(result.out - buf)});
}

// Allocation class for a literal suffix's first slot.  These four values are
// the mutually-exclusive choices accepted by #[=|$|$$]; encoding them as an
// enum (instead of three parallel bools) lets the type system enforce the
// mutex, makes call sites switch-readable, and leaves a clean extension point
// for any future first-slot class (e.g. region-scoped allocation).
enum class AllocClass : uint8_t {
    Default,   // no first-slot suffix; inherits the enclosing m_curr_alloc_global
    LocalVm,   // #$$  force allocation in local VM (overrides ${...} / set-global)
    GlobalVm,  // #$   force allocation in global VM region
    EqTmp,     // #=   eq* scratch storage (eqproc / eqstring / eqarray / eqdict / eqset)
};

// Parsed flags from the optional #[=|$|$$][a|p][e|l][r|w] suffix after a procedure's '}'.
// The first slot is one of `=`, `$`, `$$` -- captured by `alloc`.  The remaining
// axes (storage form, binding, access) compose freely with any first-slot choice.
struct ProcSuffix {
    AllocClass alloc{AllocClass::Default};
    bool is_packed{true};
    bool is_earlybinding{false};
    bool is_readonly{false};
    bool explicit_writable{false};
};

// Scratch state for procedure body scanning.  One instance is created on the stack
// by the outermost scanner() call that encounters '{', and shared (via pointer) with
// all nested scan_procedure / scanner calls for the duration of that scan.
struct ProcScanState {
    Object *ptr{nullptr};
    Object *limit{nullptr};
    Object *mark_floor{nullptr};    // lower bound for find_opstack_mark: prevents escape into real operand stack
    int overflow_bracket_depth{0};  // nesting depth of discarded [/<< marks during overflow scanning
    bool overflowed{false};         // true once any element has been discarded due to capacity overflow
    uint32_t additional_count{0};   // count of elements discarded due to overflow
};

// Result of scanning a #[=|$|$$][r|w] suffix after ']', '>>', or '}}'.
// object: make_null() on success; error string on failure.
// alloc: first-slot class (Default / LocalVm / GlobalVm / EqTmp); see AllocClass.
// is_rw: true if '#w' or no suffix (ReadWrite/ReadWriteFixed default); false for '#r'.
struct EqRwSuffix {
    Object obj{Object::make_null()};
    AllocClass alloc{AllocClass::Default};
    bool is_rw{true};
};

// dict/array/set suffix
//
// [#[=|$|$$][r|w]]
//
// =   use temporary =array/=dict/=set storage (ref-counted, fixed root)
// $   force allocation in global VM region (survives save/restore)
// $$  force allocation in local VM region (overrides enclosing global scope)
// r   read only access
// w   read write access (default)
// =/$/$$ are mutually exclusive (different storage classes); the absence of
// all three follows the enclosing m_curr_alloc_global default.
// Scans the optional #[=|$|$$][r|w] suffix that follows a ']' or '>>' or '}}'
// delimiter.  bracket_type: opening bracket char ('[', '<', or '{') for
// error messages.
// Returns: {make_null(), ...flags} on success (object.is_null() signals no
// error); {errstr, ...} on invalid suffix.  '#' alone or '#' followed by a
// non-'=', non-'$', non-alpha char is an error; so is '#=$' / '#$=' / etc.
[[nodiscard]] EqRwSuffix scan_eq_rw_suffix(Trix *trx, BracketKind kind) {
    EqRwSuffix suffix;

    auto ch = peekc(trx);
    if (ch == '#') {
        consume(trx);
        ch = peekc(trx);

        auto valid_suffix = false;
        if ((ch == '=') || (ch == '$') || Trix::is_alpha(ch)) {
            // #= XOR #$ XOR #$$ (mutually exclusive first slot, captured in suffix.alloc)
            if (ch == '=') {
                suffix.alloc = AllocClass::EqTmp;
                valid_suffix = true;

                consume(trx);
                ch = peekc(trx);
                if (ch == '$') {
                    consume(trx);
                    if (peekc(trx) == '$') {
                        trx->error(Error::SyntaxError, "{}#=$$ not allowed: #$$ and #= are mutually exclusive", bracket_name(kind));
                    } else {
                        trx->error(Error::SyntaxError, "{}#=$ not allowed: #$ and #= are mutually exclusive", bracket_name(kind));
                    }
                }
            } else if (ch == '$') {
                consume(trx);
                ch = peekc(trx);
                if (ch == '$') {
                    // #$$ -- force-local
                    consume(trx);
                    ch = peekc(trx);
                    suffix.alloc = AllocClass::LocalVm;
                    valid_suffix = true;
                    if (ch == '$') {
                        trx->error(Error::SyntaxError, "{}#$$$ not allowed (max two '$' after '#')", bracket_name(kind));
                    } else if (ch == '=') {
                        trx->error(Error::SyntaxError, "{}#$$= not allowed: #$$ and #= are mutually exclusive", bracket_name(kind));
                    }
                } else {
                    // #$ -- force-global
                    suffix.alloc = AllocClass::GlobalVm;
                    valid_suffix = true;
                    if (ch == '=') {
                        trx->error(Error::SyntaxError, "{}#$= not allowed: #$ and #= are mutually exclusive", bracket_name(kind));
                    }
                }
            }

            // #r #w
            if (Trix::is_alpha(ch)) {
                ch = Trix::to_lowercase(ch);
                if ((ch == 'r') || (ch == 'w')) {
                    valid_suffix = true;
                    suffix.is_rw = (ch == 'w');

                    consume(trx);
                } else {
                    valid_suffix = false;
                }
            }
        }

        if (valid_suffix) {
            ch = peekc(trx);
            if (!Trix::is_terminator(ch)) {
                suffix.obj = make_suffix_error_string(trx, kind);
            }
        } else {
            suffix.obj = make_suffix_separator_error_string(trx, kind);
        }
    }
    return suffix;
}

// Constructs a dict from all operand stack elements above the matching '<<' mark.
// Called by scan_delimiter when '>>' is seen outside a proc-body overflow context.
// mark_floor: lower bound passed to make_dict_from_mark to prevent the mark search
//   from escaping into the real operand stack when scanning inside a proc body;
//   nullptr when called from top-level scanner context.
// Returns: {LiteralValue, dict} on success; {SyntaxError, errstr} for an invalid suffix.
// Raises: UnmatchedMark if no '<<' mark is on the stack; RangeCheck for an odd
//   element count; TypeCheck for a null key; RangeCheck for an Inf/NaN key;
//   VMFull or LimitCheck from make_dict_from_mark / make_eqdict_from_mark.
// Note: the default access mode is ReadWriteFixed (not ReadWrite as for arrays).
// dict suffix
//
// [#[=][r|w]]
//
// =   use temporary =dict storage
// r   ReadOnly access
// w   ReadWriteFixed access (default)
[[nodiscard]] ScanToken scan_dict(Trix *trx, Object *mark_floor = nullptr) {
    auto [err_obj, alloc, is_rw] = scan_eq_rw_suffix(trx, BracketKind::Dict);
    if (err_obj.is_null()) {
        auto mode = is_rw ? Object::DictMode::ReadWriteFixed : Object::DictMode::ReadOnly;
        Object dict_obj;
        switch (alloc) {
        case AllocClass::EqTmp:
            dict_obj = Object::make_eqdict_from_mark(trx, mode, mark_floor);
            break;

        case AllocClass::GlobalVm:
            dict_obj = Object::make_global_dict_from_mark(trx, mode, mark_floor);
            break;

        case AllocClass::LocalVm: {
            const LocalAllocScope guard(trx);
            dict_obj = Object::make_dict_from_mark(trx, mode, mark_floor);
            break;
        }

        case AllocClass::Default:
            dict_obj = Object::make_dict_from_mark(trx, mode, mark_floor);
            break;
        }
        return literal_pair(dict_obj);
    } else {
        return syntaxerror_pair(err_obj);
    }
}

// Constructs a set from all operand stack elements above the matching '{{' mark.
// Called by scan_delimiter when '}}' is seen outside a proc-body overflow context.
// Elements must be literal names.
// set suffix
//
// [#[=][r|w]]
//
// =   use temporary =set storage (eq-set)
// r   ReadOnly access
// w   ReadWriteFixed access (default)
[[nodiscard]] ScanToken scan_set(Trix *trx, Object *mark_floor = nullptr) {
    auto [err_obj, alloc, is_rw] = scan_eq_rw_suffix(trx, BracketKind::Set);
    if (err_obj.is_null()) {
        auto mode = is_rw ? Object::DictMode::ReadWriteFixed : Object::DictMode::ReadOnly;
        Object set_obj;
        switch (alloc) {
        case AllocClass::EqTmp:
            set_obj = Object::make_eqset_from_mark(trx, mode, mark_floor);
            break;

        case AllocClass::GlobalVm:
            set_obj = Object::make_global_set_from_mark(trx, mode, mark_floor);
            break;

        case AllocClass::LocalVm: {
            const LocalAllocScope guard(trx);
            set_obj = Object::make_set_from_mark(trx, mode, mark_floor);
            break;
        }

        case AllocClass::Default:
            set_obj = Object::make_set_from_mark(trx, mode, mark_floor);
            break;
        }
        return literal_pair(set_obj);
    } else {
        return syntaxerror_pair(err_obj);
    }
}

// Constructs an array from all operand stack elements above the matching '[' mark.
// Called by scan_delimiter when ']' is seen outside a proc-body overflow context.
// mark_floor: lower bound passed to make_array_from_mark to prevent the mark search
//   from escaping into the real operand stack when scanning inside a proc body;
//   nullptr when called from top-level scanner context.
// Returns: {LiteralValue, array} on success; {SyntaxError, errstr} for an invalid suffix.
// Raises: UnmatchedMark if no '[' mark is on the stack; VMFull from
//   make_array_from_mark; LimitCheck from make_eqarray_from_mark if the array
//   length exceeds the =array capacity.
// array suffix
//
// [#[=][r|w]]
//
// =   use temporary =array storage
// r   ReadOnly access
// w   ReadWrite access (default)
[[nodiscard]] ScanToken scan_array(Trix *trx, Object *mark_floor = nullptr) {
    auto [err_obj, alloc, is_rw] = scan_eq_rw_suffix(trx, BracketKind::Array);
    if (err_obj.is_null()) {
        auto access = is_rw ? Object::ReadWriteAccess : Object::ReadOnlyAccess;
        Object arr_obj;
        switch (alloc) {
        case AllocClass::EqTmp:
            arr_obj = Object::make_eqarray_from_mark(trx, Object::LiteralAttrib, access, mark_floor);
            break;

        case AllocClass::GlobalVm:
            arr_obj = Object::make_global_array_from_mark(trx, Object::LiteralAttrib, access, mark_floor);
            break;

        case AllocClass::LocalVm: {
            const LocalAllocScope guard(trx);
            arr_obj = Object::make_array_from_mark(trx, Object::LiteralAttrib, access, mark_floor);
            break;
        }

        case AllocClass::Default:
            arr_obj = Object::make_array_from_mark(trx, Object::LiteralAttrib, access, mark_floor);
            break;
        }
        return literal_pair(arr_obj);
    } else {
        return syntaxerror_pair(err_obj);
    }
}

// Frees all ExtValues stored in the opstack scratch region above gc_mark_object,
// then resets m_op_ptr to gc_mark_object-1, discarding the region.
// Used in scan_delimiter's ']' and '>>' overflow paths when the matching '[' / '<<'
// was stored in the proc temp area before overflow started.
static void discard_overflow_mark(Trix *trx, Object *mark_floor) {
    auto [gc_mark_object, _] = trx->find_opstack_mark(mark_floor);
    maybe_free_extvalue_opstack(trx, gc_mark_object);
    trx->m_op_ptr = (gc_mark_object - 1);
}

// Checks whether any element above the '<<'/'['/'{{' mark is an executable name.
// Used by close_bracket to decide scan-time vs runtime collection construction:
// when executable names are present, the dict/array/set must be built at runtime.
[[nodiscard]] static bool has_executable_names(Trix *trx, Object *mark_floor) {
    auto [gc_mark_object, count] = trx->find_opstack_mark(mark_floor);
    for (auto p = gc_mark_object + 1; p <= trx->m_op_ptr; ++p) {
        if (p->is_name() && p->is_executable()) {
            return true;
        }
    }
    return false;
}

// Selects the correct *-from-mark SystemName given bracket type and suffix flags.
[[nodiscard]] static SystemName select_from_mark_op(BracketKind kind, bool is_eq, bool is_rw) {
    if (kind == BracketKind::Array) {
        static constexpr SystemName sysname[2][2] = {
                {  SystemName::ReadOnlyArrayFromMark,   SystemName::ArrayFromMark},
                {SystemName::ReadOnlyEqArrayFromMark, SystemName::EqArrayFromMark},
        };
        return sysname[is_eq][is_rw];
    } else if (kind == BracketKind::Dict) {
        static constexpr SystemName sysname[2][2] = {
                {  SystemName::ReadOnlyDictFromMark,   SystemName::DictFromMark},
                {SystemName::ReadOnlyEqDictFromMark, SystemName::EqDictFromMark},
        };
        return sysname[is_eq][is_rw];

    } else {
        // BracketKind::Set
        static constexpr SystemName sysname[2][2] = {
                {  SystemName::ReadOnlySetFromMark,   SystemName::SetFromMark},
                {SystemName::ReadOnlyEqSetFromMark, SystemName::EqSetFromMark},
        };
        return sysname[is_eq][is_rw];
    }
}

// Handles a closing bracket (>>, ], }}) during scanning.  If the procedure
// scanner is in overflow, absorbs the bracket silently; otherwise delegates
// to scan_fn to build the collection from the operand stack mark.
//
// Runtime fallback: when scanning inside a proc body and the elements between
// the mark contain executable names, the collection cannot be built at scan
// time (the names need runtime lookup).  Instead, the mark and elements are
// left in the proc body and a *-from-mark operator is appended, so the
// collection is built at runtime.  This is transparent to the user: they
// write [...], <<...>>, or {{...}} and get scan-time when possible, runtime when needed.
template<typename ScanFn>
[[nodiscard]] ScanToken close_bracket(Trix *trx, ProcScanState *state, Object *mark_floor, ScanFn scan_fn, BracketKind kind) {
    if ((state != nullptr) && (state->overflow_bracket_depth > 0)) {
        --state->overflow_bracket_depth;
        return literal_pair(Object::make_null());
    } else if ((state != nullptr) && state->overflowed) {
        discard_overflow_mark(trx, mark_floor);
        return literal_pair(Object::make_null());
    } else if ((state != nullptr) && has_executable_names(trx, mark_floor)) {
        // Runtime fallback: parse suffix, emit *-from-mark operator.
        // Mark + elements stay in proc temp; operator is appended after them.
        auto [err_obj, alloc, is_rw] = scan_eq_rw_suffix(trx, kind);
        if (err_obj.is_null()) {
            if (alloc == AllocClass::GlobalVm) {
                // The runtime *-from-mark ops always allocate at the
                // m_curr_alloc_global of the executing actor; #$ would need
                // a parallel global-* op family to force-globalise.  For
                // now, push the user toward ${...} which already wraps any
                // body (including executable-name bodies) cleanly.
                trx->error(Error::SyntaxError,
                           "{}#$ not supported when body contains executable names; wrap in ${{...}} instead",
                           bracket_name(kind));
            }
            if (alloc == AllocClass::LocalVm) {
                // Symmetric inverse of the #$ case above: forcing local on
                // a runtime-built collection would need a parallel local-*
                // op family.  Push toward $${...} which wraps the body in
                // a force-local scope.
                trx->error(Error::SyntaxError,
                           "{}#$$ not supported when body contains executable names; wrap in $${{...}} instead",
                           bracket_name(kind));
            }
            auto op_name = select_from_mark_op(kind, (alloc == AllocClass::EqTmp), is_rw);
            return literal_pair(Object::make_operator(op_name));
        } else {
            return syntaxerror_pair(err_obj);
        }
    } else {
        return scan_fn(trx, mark_floor);
    }
}

// scanner_infix.inl is included here (rather than near the end of
// scanner.inl) so scan_field_access below can use pack_infix_tokens
// and pack_and_buffer_infix_tail to share buffer-management code with
// scan_infix_expression.  The infix scanner depends only on early
// scanner.inl helpers (peekc, consume, skip_whitespace, try_parse_number,
// Name::make) which are defined above this point.
#include "scanner_infix.inl"

// Scans a field-access sugar sequence after a '.' delimiter.
// The first '.' has already been consumed by scanner()'s main dispatch.
// For input ".name" (or chained ".a.b.c..."), emits an alternating sequence
// of /name (literal) and get (executable well-known name) pairs.  Examples:
//   scanner input:   foo.bar        -> tokens emitted: foo, /bar, get
//   scanner input:   foo.a.b.c      -> tokens emitted: foo, /a, get, /b, get, /c, get
// Returns {LiteralValue, /first_name}; remaining tokens are packed into the
// per-stream m_infix_offset buffer and drained by scanner()'s priming path
// on subsequent scanner() calls.
// Raises: SyntaxError if no regular-char name follows '.' or a chained '.';
//   LimitCheck for oversized name tokens or over-long chains; VMFull for
//   buffer allocation.
[[nodiscard]] ScanToken scan_field_access(Trix *trx) {
    auto tokens_ptr = trx->m_infix_scratch_objs;
    auto count = length_t{0};
    auto get_obj = trx->wellknown_name(WellKnownName::Get, Object::ExecutableAttrib);

    while (true) {
        // Scan the field name (regular chars starting at m_rptr_offset).
        auto token_base = trx->m_token_base;
        auto token_limit = trx->m_token_limit;
        auto token_length = scan_regular_chars(trx, token_base, token_limit, 0);
        if (token_length == 0) {
            return syntaxerror_pair(Object::make_error_string(trx, "expected field name after '.'"));
        } else if (token_length > MaxNameLength) {
            trx->error(Error::LimitCheck, "field-access name length {} exceeds maximum {}", token_length, MaxNameLength);
        } else if ((count + 2) > MaxInfixTokens) {
            trx->error(Error::LimitCheck, "field-access: chain exceeds {} accesses", MaxInfixTokens / 2);
        } else {
            auto token_sv = make_sv(token_base, token_length);
            tokens_ptr[count++] = Name::make(trx, token_sv, Object::LiteralAttrib);
            tokens_ptr[count++] = get_obj;

            // Chain: another '.'?
            auto ch = peekc(trx);
            if (ch == '.') {
                consume(trx);  // consume the chained '.'
                auto after_dot = peekc(trx);
                if ((after_dot == EOFc) || !Trix::is_regular(after_dot) || Trix::is_digit(after_dot)) {
                    return syntaxerror_pair(Object::make_error_string(trx, "expected field name after '.'"));
                }
            } else {
                break;
            }
        }
    }

    // count is always >= 2 here.  Return tokens_ptr[0], buffer the rest.
    pack_and_buffer_infix_tail(trx, tokens_ptr, count, "field-access");
    return literal_pair(tokens_ptr[0]);
}

// Dispatches a delimiter character to the appropriate scanner sub-function.
// Called by scanner() for any character classified as a delimiter.
// ch: delimiter character (already consumed); proc_nesting: 0 at top level,
//   incremented by 1 for each nested '{' (enforces MaxProcNesting in scan_procedure).
// state: non-null only when scanning inside a procedure body.
// Returns: LiteralValue (most literals), ExecutableValue (\name),
//   EndOfProcedure (matching '}'), or SyntaxError -- directly for unbalanced
//   ')', '>', or '}', or propagated from delegated sub-functions (scan_text_string,
//   scan_base16_string, scan_raw_string, scan_array, scan_dict).
[[nodiscard]] ScanToken scan_delimiter(Trix *trx, int ch, int proc_nesting, ProcScanState *state) {
    auto mark_floor = (state != nullptr) ? state->mark_floor : nullptr;
    switch (ch) {
    case '(':  // start of literal (--string--)
        return scan_text_string(trx);

    case ')':  // end of literal (--string--) is consumed in scan_text_string
        return syntaxerror_pair(Object::make_error_string(trx, "unbalanced )"));

    case '/':   // /name, //value, //:rootdict:segment:...:leaf
    case '\\':  // \name, \\value, \\:rootdict:segment:...:leaf
        return scan_name(trx, ch);

    case '<':
        ch = peekc(trx);
        if (ch == '<') {
            // << --mark--, start of --dict--
            consume(trx);
            return literal_pair(Object::make_mark());
        } else if (ch == '(') {
            // start of raw string <(...)>: no escape processing
            consume(trx);
            return scan_raw_string(trx);
        } else {
            // start of literal base16 <--string-->: each pair of hex digits -> 1 output byte
            return scan_base16_string(trx);
        }

    case '>':
        ch = peekc(trx);
        if (ch == '>') {
            // >> end of --dict--
            consume(trx);
            return close_bracket(
                    trx, state, mark_floor, [this](Trix *ps2, Object *mf) { return scan_dict(ps2, mf); }, BracketKind::Dict);
        } else {
            // > end of base16 string construction, which should have been consumed
            return syntaxerror_pair(Object::make_error_string(trx, "unbalanced >"));
        }

    case '[':  // --mark--, start of --array--
        return literal_pair(Object::make_mark());

    case ']':  // end of --array--
        return close_bracket(
                trx, state, mark_floor, [this](Trix *ps2, Object *mf) { return scan_array(ps2, mf); }, BracketKind::Array);

    case '{':
        ch = peekc(trx);
        if (ch == '{') {
            // {{ --mark--, start of --set--
            consume(trx);
            return literal_pair(Object::make_mark());
        } else {
            // { start of procedure, an executable --array-- or --packed--
            // procedure is treated as data and pushed on the operand stack
            if (state != nullptr) {
                return scan_procedure(trx, proc_nesting + 1, state);
            } else {
                ProcScanState new_state{.ptr = (trx->m_op_ptr + 1), .limit = trx->m_op_limit, .mark_floor = (trx->m_op_ptr + 1)};
                return scan_procedure(trx, proc_nesting + 1, &new_state);
            }
        }

    case '}':
        ch = peekc(trx);
        if (ch == '}') {
            // }} end of --set--
            consume(trx);
            return close_bracket(
                    trx, state, mark_floor, [this](Trix *ps2, Object *mf) { return scan_set(ps2, mf); }, BracketKind::Set);
        } else {
            // } end of procedure
            if (proc_nesting > 0) {
                return std::pair{Lexeme::EndOfProcedure, Object::make_null()};
            } else {
                return syntaxerror_pair(Object::make_error_string(trx, "unbalanced }"));
            }
        }

    case '.':  // field-access sugar or real-number literal
        ch = peekc(trx);
        if (ch != EOFc) {
            if (Trix::is_digit(ch)) {
                // .5, .002, etc. -- real-number literal with '.' as already-consumed first char.
                return scan_number_or_name(trx, '.');
            } else if (Trix::is_regular(ch)) {
                // .name -- field-access sugar: .name -> /name get (chained: .a.b -> /a get /b get)
                return scan_field_access(trx);
            }
        }
        return syntaxerror_pair(Object::make_error_string(trx, "expected digit or field name after '.'"));

    case '%':  // comment
        // '%' is consumed by scanner()'s comment loop before scan_delimiter is called.
        trx->error(Error::InternalError, "scan_delimiter: % reached (broken comment logic)");
    }

    trx->error(Error::InternalError, "scan_delimiter: unhandled delimiter character {:c}", ch);
}

// Handle CR line-number fixup: increment m_line_number, reset m_line_pos,
// and consume the optional trailing LF of a CR+LF pair (without double-counting
// the line break, since getc() already handled LF internally).
void fixup_cr(Trix *trx) {
    ++m_line_number;
    m_line_pos = 1;
    if (((m_rptr_offset != m_rlimit_offset) || fill(trx)) && (trx->offset_to_value<vm_t>(m_rptr_offset) == ASCII_LF)) {
        ++m_rptr_offset;
    }
}

// If the very first two bytes of a file stream are #!, consume the entire line.
// Strict POSIX: #! must be at byte offset 0 (no leading whitespace).
// String streams and REPL are excluded.
void skip_shebang(Trix *trx) {
    if ((m_rptr_offset == m_base_offset) && !is_string() && !is_stdio()) {
        if (peekc(trx) == '#') {
            consume(trx);  // consume the '#' (so peekc() below may legally fill across buffer boundary)
            if (peekc(trx) == '!') {
                // consume '!' and the rest of the line
                consume(trx);
                int sh_ch;
                do {
                    sh_ch = getc(trx);
                } while ((sh_ch != EOFc) && !Trix::is_comment_terminator(sh_ch));
                if (sh_ch == ASCII_CR) {
                    fixup_cr(trx);
                }
            } else {
                // not a shebang -- push the '#' back.  If a fill() happened during the
                // second peekc(), the new buffer starts at m_base_offset; ungetc places
                // the '#' in the unget slot at m_base_offset-1.
                ungetc(trx, '#');
                --m_line_pos;  // undo the '#' consume's line-pos bump
            }
        }
    }
}

// Consume a nestable block comment.  Called after '%{' has been detected --
// the '%' has been consumed by the caller but '{' has NOT.
// Tracks depth: %{ increments, %} decrements.  Returns when depth reaches 0.
// Raises SyntaxError on unterminated block comment (EOF before depth reaches 0).
void skip_block_comment(Trix *trx) {
    // consume the '{' of '%{'
    ++m_rptr_offset;
    ++m_line_pos;
    auto depth{1};
    while (depth > 0) {
        auto ch = getc(trx);
        if (ch == EOFc) {
            trx->error(Error::SyntaxError, "unterminated block comment");
        } else if (ch == '%') {
            // peek for '{' or '}'
            if (((m_rptr_offset != m_rlimit_offset) || fill(trx))) {
                auto next = trx->offset_to_value<vm_t>(m_rptr_offset);
                if (next == '{') {
                    ++m_rptr_offset;
                    ++m_line_pos;
                    ++depth;
                } else if (next == '}') {
                    ++m_rptr_offset;
                    ++m_line_pos;
                    --depth;
                }
            }
        } else if (ch == ASCII_CR) {
            fixup_cr(trx);
        }
        // LF: getc() already incremented m_line_number; nothing to do.
        // FF: CT but not a line break; ignore.
    }
}

// Buffered whitespace-skip: scans m_rptr..m_rlimit using raw pointers, handling
// LF for line-number tracking.  Internalizes the fill loop: returns only after
// a non-whitespace byte is visible at m_rptr_offset or the stream is exhausted.
// Invariant: whitespace includes LF; the byte left at m_rptr_offset on return
// is never LF, so scanner()'s post-skip ++m_line_pos needs no LF recheck.
void skip_whitespace(Trix *trx) {
    auto done = false;
    while (true) {
        // refresh our view of the read buffer pointer and limit
        auto rptr = trx->offset_to_ptr<vm_t>(m_rptr_offset);
        auto rlimit = trx->offset_to_ptr<vm_t>(m_rlimit_offset);
        auto line_start = rptr;  // baseline for bulk m_line_pos update; reset after each LF
        while (rptr != rlimit) {
            auto ch = *rptr;
            if (!Trix::is_whitespace(ch)) {
                done = true;
                break;
            } else if (ch == ASCII_LF) {
                ++m_line_number;
                m_line_pos = 1;
                ++rptr;
                line_start = rptr;
            } else {
                ++rptr;
            }
        }
        // batch the per-byte ++m_line_pos into one add at inner-loop exit; LF branches
        // above already reset m_line_pos and line_start so post-LF bytes are counted
        // against the new line.
        m_line_pos = static_cast<decltype(m_line_pos)>(m_line_pos + (rptr - line_start));
        // update the stream view of the read buffer pointer
        m_rptr_offset = trx->ptr_to_offset(rptr);
        if (done || !fill(trx)) {
            break;
        }
    }
}

// Consume a line comment (% to end-of-line).  Called after '%' has been consumed.
// Returns the comment-terminator character (LF, CR, or FF), or EOFc on end-of-stream.
// Handles CR line-number fixup and optional LF consumption after CR+LF.
[[nodiscard]] int skip_line_comment(Trix *trx) {
    int ch;
    do {
        ch = getc(trx);
        // EOFc guard is load-bearing: is_comment_terminator(EOFc) is false (sm_chattr[-1] = 0),
        // so without the explicit check this loop would spin past end-of-stream.
    } while ((ch != EOFc) && !Trix::is_comment_terminator(ch));

    // LF: getc() already incremented m_line_number and reset m_line_pos; nothing to do.
    // CR: fixup_cr handles m_line_number, m_line_pos, and optional trailing LF.
    // FF: a CT but not a line break; ignore it.
    if (ch == ASCII_CR) {
        fixup_cr(trx);
    }
    return ch;
}

// Skips whitespace, line comments (% to EOL), and block comments
// (%{...%}) without consuming the first non-whitespace byte.  Used
// by scan_local_bindings's two ws/comment skip loops (between `{`
// and `|...|`, and between names inside `|...|`) where the caller
// still wants to peekc() the next byte.
//
// Differs from skip_ws_and_comments in two ways:
//   1. Returns void -- the caller calls peekc() to inspect the next
//      byte.
//   2. Does NOT eat EOF; the caller decides what to do at end-of-stream.
// Raises: SyntaxError (from skip_block_comment on unterminated block).
void skip_ws_and_comments_keep_eof(Trix *trx) {
    while (true) {
        auto ch = peekc(trx);
        if (Trix::is_whitespace(ch)) {
            consume(trx);
        } else if (ch == '%') {
            consume(trx);
            if (peekc(trx) == '{') {
                skip_block_comment(trx);
            } else {
                static_cast<void>(skip_line_comment(trx));
            }
        } else {
            break;
        }
    }
}

// Skips whitespace, line comments (% to EOL), and block comments (%{...%}).
// Returns the first non-whitespace/non-comment character (>= 0), or EOFc if
// the stream is exhausted during whitespace skip or comment consumption.
// Raises: SyntaxError (from skip_block_comment on unterminated block comment).
[[nodiscard]] int skip_ws_and_comments(Trix *trx) {
    auto ch = EOFc;
    do {
        skip_whitespace(trx);
        if (m_rptr_offset == m_rlimit_offset) {
            ch = EOFc;
            break;
        } else {
            // consume the first non-whitespace byte; it cannot be LF (LF is whitespace)
            ch = static_cast<int>(trx->offset_to_value<vm_t>(m_rptr_offset++));
            ++m_line_pos;

            // consume comment (line or block)
            if (ch == '%') {
                if (((m_rptr_offset != m_rlimit_offset) || fill(trx)) && (trx->offset_to_value<vm_t>(m_rptr_offset) == '{')) {
                    skip_block_comment(trx);
                    ch = ' ';  // treat block comment as whitespace for outer do-while
                } else {
                    ch = skip_line_comment(trx);
                    if (ch == EOFc) {
                        break;
                    }
                }
            }
        }
    } while (Trix::is_whitespace(ch));
    return ch;
}

// Scans one token from the stream and returns {Lexeme, Object}.
// Dispatches to scan_number_or_name, scan_delimiter, or scan_binary_token based on
// the first non-whitespace character.
// Saves and restores m_last_operator_ptr around the sub-scan; updates m_last_scan_location
// on return so backtrace formatting has an accurate call-site position.
// proc_nesting and state are non-zero/non-null only when scanning inside a procedure body.
// Returns: {EndOfStream, errstr} on EOF during whitespace-skip or comment-consume;
//   {SyntaxError, errstr} for invalid characters; otherwise {Lexeme, Object} from the
//   dispatched sub-function (LiteralValue, ExecutableValue, EndOfProcedure, SyntaxError).
// Raises: VMFull (from sub-functions allocating strings, names, numbers, or arrays).
[[nodiscard]] ScanToken scanner(Trix *trx, int proc_nesting = 0, ProcScanState *state = nullptr) {
    // Return buffered infix tokens from a prior $( ) / $[ ] parse (packed byte stream).
    // Guard against restore() rolling m_infix_offset back to nulloffset while
    // m_infix_packed_size/read (not journaled) still indicate a pending drain: reset the
    // drain state so the next scan_infix/scan_field_access allocation starts clean.
    if (m_infix_offset == nulloffset) {
        m_infix_packed_size = 0;
        m_infix_packed_read = 0;
    }
    if (m_infix_packed_read < m_infix_packed_size) {
        auto base = trx->offset_to_ptr<packed_data_t>(m_infix_offset);
        auto [next, extracted] = Object::extract_next_packed(trx, base + m_infix_packed_read);
        m_infix_packed_read = static_cast<length_t>(next - base);
        if (m_infix_packed_read >= m_infix_packed_size) {
            m_infix_packed_size = 0;
            m_infix_packed_read = 0;
        }
        // extract_next_packed returns a borrowed reference -- clone ExtValue
        // types so the operand stack owns a real ExtValue allocation
        auto value_obj = extracted.make_clone(trx);
        auto lexeme = value_obj.is_literal() ? Lexeme::LiteralValue : Lexeme::ExecutableValue;
        return std::pair{lexeme, value_obj};
    } else {
        skip_shebang(trx);

        auto ch = skip_ws_and_comments(trx);
        if (ch == EOFc) {
            auto errstr = Object::make_error_string(trx, "EndOfStream encountered while skipping whitespace/comments");
            return std::pair{Lexeme::EndOfStream, errstr};
        } else {
            auto last_operator = *trx->m_last_operator_ptr;
            *trx->m_last_operator_ptr = Object::make_operator(SystemName::Scanner);
            trx->m_scanner_stream = this;

            auto [lexeme, value_obj] = [&]() -> ScanToken {
                if (Trix::is_regular(ch)) {
                    return scan_number_or_name(trx, ch, proc_nesting, state);
                } else if (Trix::is_delimiter(ch)) {
                    return scan_delimiter(trx, ch, proc_nesting, state);
                } else if (is_binary_token(ch)) {
                    return scan_binary_token(trx, ch);
                } else {
                    auto caret_ch = (ch == ASCII_DEL) ? '?' : static_cast<char>(ch + '@');
                    return syntaxerror_pair(Object::make_error_string(trx, "invalid character ^{}", caret_ch));
                }
            }();

            // Normal-path cleanup: sub-function returned without throwing.
            // Exception-path cleanup: handled by interpreter catch(Exception::Error) which
            // resets to nullptr / SystemName::Interpreter (not the pre-scan values --
            // intentional, an error fully resets the execution context).
            trx->m_last_scan_location = {m_line_number, m_line_pos, m_sid};
            trx->m_scanner_stream = nullptr;
            *trx->m_last_operator_ptr = last_operator;

            return std::pair{lexeme, value_obj};
        }
    }
}

// Result of scanning one token from a string object.
// token: the lexeme type of the scanned token.
// value: the scanned value (or error string on EndOfStream).
// string_obj: the input string with its read position advanced past the consumed token.
struct StringScanResult {
    Lexeme token;
    Object value;
    Object string_obj;
};

// Scans one token from string_obj and returns {token, value, updated_string_obj}.
// The returned string_obj has its read position advanced past the consumed token.
// Returns {EndOfStream, error, string_obj} if string_obj is empty.
[[nodiscard]] static StringScanResult scanner(Trix *trx, Object string_obj) {
    auto string = string_obj.sv_value(trx);
    auto length = sv_length(string);

    if (length != 0) {
        // one-shot token scan; the transient Stream cannot be active during snap-shot/thaw
        Stream stream(trx, string.data(), length, STRING_SID, string_obj.save_level());
        auto [token, value_obj] = stream.scanner(trx);
        string_obj.string_pop_count(trx, static_cast<length_t>(stream.m_rptr_offset - stream.m_base_offset));
        return StringScanResult{token, value_obj, string_obj};
    } else {
        auto errstr = Object::make_error_string(trx, "empty 'string'");
        return StringScanResult{Lexeme::EndOfStream, errstr, string_obj};
    }
}

// Parses the optional #[=|$|$$][a|p][e|l][r|w] suffix after a procedure's closing '}'.
// On success returns {null Object, populated ProcSuffix}.
// On error returns {error-string Object, partial ProcSuffix} -- suffix is unused on error.
[[nodiscard]] std::pair<Object, ProcSuffix> scan_proc_suffix(Trix *trx) {
    ProcSuffix suffix;
    auto ret_obj = Object::make_null();
    auto ch = peekc(trx);
    if (ch == '#') {
        consume(trx);
        ch = peekc(trx);

        auto valid_suffix = false;
        if ((ch == '=') || (ch == '$') || Trix::is_alpha(ch)) {
            // #= XOR #$ XOR #$$ (mutually exclusive first slot, captured in suffix.alloc)
            if (ch == '=') {
                valid_suffix = true;
                suffix.alloc = AllocClass::EqTmp;
                consume(trx);
                ch = peekc(trx);
                if (ch == '$') {
                    consume(trx);
                    if (peekc(trx) == '$') {
                        trx->error(Error::SyntaxError,
                                   "{}#=$$ not allowed: #$$ and #= are mutually exclusive",
                                   bracket_name(BracketKind::Proc));
                    }
                    trx->error(Error::SyntaxError,
                               "{}#=$ not allowed: #$ and #= are mutually exclusive",
                               bracket_name(BracketKind::Proc));
                }
            } else if (ch == '$') {
                consume(trx);
                ch = peekc(trx);
                if (ch == '$') {
                    // #$$ -- force-local
                    consume(trx);
                    ch = peekc(trx);
                    valid_suffix = true;
                    suffix.alloc = AllocClass::LocalVm;
                    if (ch == '$') {
                        trx->error(
                                Error::SyntaxError, "{}#$$$ not allowed (max two '$' after '#')", bracket_name(BracketKind::Proc));
                    }
                    if (ch == '=') {
                        trx->error(Error::SyntaxError,
                                   "{}#$$= not allowed: #$$ and #= are mutually exclusive",
                                   bracket_name(BracketKind::Proc));
                    }
                } else {
                    // #$ -- force-global
                    valid_suffix = true;
                    suffix.alloc = AllocClass::GlobalVm;
                    if (ch == '=') {
                        trx->error(Error::SyntaxError,
                                   "{}#$= not allowed: #$ and #= are mutually exclusive",
                                   bracket_name(BracketKind::Proc));
                    }
                }
            }

            // #a #p
            if (Trix::is_alpha(ch)) {
                ch = Trix::to_lowercase(ch);
                if ((ch == 'a') || (ch == 'p')) {
                    valid_suffix = true;
                    suffix.is_packed = (ch == 'p');
                    consume(trx);
                    ch = peekc(trx);
                }
            }

            // #e #l
            if (Trix::is_alpha(ch)) {
                ch = Trix::to_lowercase(ch);
                if ((ch == 'e') || (ch == 'l')) {
                    valid_suffix = true;
                    suffix.is_earlybinding = (ch == 'e');
                    consume(trx);
                    ch = peekc(trx);
                }
            }

            // #r #w
            if (Trix::is_alpha(ch)) {
                ch = Trix::to_lowercase(ch);
                if ((ch == 'r') || (ch == 'w')) {
                    valid_suffix = true;
                    suffix.is_readonly = (ch == 'r');
                    suffix.explicit_writable = !suffix.is_readonly;
                    consume(trx);
                } else {
                    valid_suffix = false;
                }
            }
        }

        if (valid_suffix) {
            ch = peekc(trx);
            if (!Trix::is_terminator(ch)) {
                ret_obj = make_suffix_error_string(trx, BracketKind::Proc);
            }
        } else {
            ret_obj = make_suffix_separator_error_string(trx, BracketKind::Proc);
        }
    }
    return std::pair{ret_obj, suffix};
}

// Scans the optional local variable binding preamble { |a b c| ... } or
// { |a b c|#N ... } where N is an optional capacity >= count-of-names (default: N==K).
// The empty-header form { ||#N ... } (K=0, N>=1) declares a no-args frame dict of
// capacity N for /foo def scratch use -- the caller pushes no stack values and binds
// names inside the body.
// On success returns an Integer Object carrying locals_count K when a preamble was
// scanned (K>=0), or Integer(-1) when no preamble was present.
// Scratch stack receives /n1 .../nK followed by TWO integers K and N (N on top) for the
// begin-locals opcode to consume at runtime; when the #N suffix is absent, N is emitted
// equal to K so the opcode layout is uniform.
// On error calls cleanup and returns an error-string Object (or throws via trx->error).
template<typename CleanupFn>
[[nodiscard]] Object scan_local_bindings(Trix *trx, ProcScanState *state, save_level_t curr_save_level, CleanupFn &&cleanup) {
    auto saved_rptr = m_rptr_offset;
    auto saved_line = m_line_number;
    auto saved_pos = m_line_pos;
    auto locals_count = length_t{0};
    // Skip whitespace and line/block comments so a stack-effect comment
    // between `{` and `|locals|` doesn't hide the pipe-header, and so
    // similar comments between names inside `|...|` don't break the
    // scan.  Both loops share the same grammar -- factored into
    // skip_ws_and_comments_keep_eof so a future grammar tweak (e.g.
    // a new comment form) touches one place.
    skip_ws_and_comments_keep_eof(trx);
    if (peekc(trx) == '|') {
        consume(trx);  // consume opening '|'
        while (true) {
            // skip whitespace and line/block comments between names
            skip_ws_and_comments_keep_eof(trx);
            auto ch = peekc(trx);
            if (ch == '|') {
                consume(trx);  // consume closing '|'
                break;
            } else if ((ch == EOFc) || (ch == '}')) {
                cleanup();
                return Object::make_error_string(trx, "unterminated local variable binding (missing closing '|')");
            } else {
                auto token_ptr = trx->m_token_base;
                auto token_limit = trx->m_token_limit;
                auto name_len = length_t{0};
                while (true) {
                    auto name_ch = peekc(trx);
                    if ((name_ch != EOFc) && Trix::is_regular(name_ch) && (name_ch != '|')) {
                        if (token_ptr < token_limit) {
                            *token_ptr++ = static_cast<vm_t>(name_ch);
                        }
                        ++name_len;
                        consume(trx);
                    } else {
                        break;
                    }
                }
                if (name_len == 0) {
                    cleanup();
                    return Object::make_error_string(trx, "empty name in local variable binding");
                } else if (name_len > MaxNameLength) {
                    cleanup();
                    return Object::make_error_string(trx, "local variable name length exceeds maximum");
                } else {
                    auto name_sv = std::string_view{reinterpret_cast<char *>(trx->m_token_base), name_len};
                    auto name_offset = Name::add(trx, name_sv);
                    auto name_obj = Object::make_name(name_offset, name_len, Object::LiteralAttrib);
                    if (state->ptr < state->limit) {
                        name_obj.set_save_level(curr_save_level);
                        *state->ptr++ = name_obj;
                        trx->m_op_ptr = (state->ptr - 1);
                    } else {
                        cleanup();
                        trx->error(Error::LimitCheck, "local variable binding: temporary storage exhausted");
                    }
                    ++locals_count;
                }
            }
        }
        // Optional capacity suffix: |a b|#N (absolute) or |a b|#+N (relative).  Parse
        // immediately after the closing '|' with no whitespace allowed between them
        // (matches #= / #r / #w suffix convention elsewhere in the scanner).
        //   #N   absolute total capacity; must satisfy N >= K (named-arg count).
        //   #+N  N additional slots beyond named args; total capacity = K + N.
        // The ||#N / ||#+N form (K=0) still REQUIRES the suffix -- a bare || is a
        // syntax error -- and the resulting capacity must be >= 1.
        auto capacity = locals_count;
        auto saw_capacity_suffix = false;
        if (peekc(trx) == '#') {
            consume(trx);
            auto relative = false;
            if (peekc(trx) == '+') {
                consume(trx);
                relative = true;
            }
            if (!Trix::is_digit(peekc(trx))) {
                cleanup();
                return Object::make_error_string(
                        trx,
                        relative ? "invalid locals-capacity suffix: '#+' must be followed by a decimal integer"
                                 : "invalid locals-capacity suffix: '#' must be followed by a decimal integer");
            }
            auto n = uint32_t{0};
            while (Trix::is_digit(peekc(trx))) {
                n = (n * 10) + static_cast<uint32_t>(peekc(trx) - '0');
                if (n > std::numeric_limits<length_t>::max()) {
                    cleanup();
                    trx->error(Error::LimitCheck,
                               "locals capacity exceeds maximum {}",
                               static_cast<uint32_t>(std::numeric_limits<length_t>::max()));
                }
                consume(trx);
            }
            if (relative) {
                auto total = static_cast<uint64_t>(n) + static_cast<uint64_t>(locals_count);
                if (total > std::numeric_limits<length_t>::max()) {
                    cleanup();
                    trx->error(Error::LimitCheck,
                               "locals capacity exceeds maximum {}",
                               static_cast<uint32_t>(std::numeric_limits<length_t>::max()));
                }
                capacity = static_cast<length_t>(total);
            } else if (n < locals_count) {
                cleanup();
                char buffer[96];
                auto [out, _] = std::format_to_n(
                        buffer, sizeof(buffer), "locals capacity {} less than declared name count {}", n, locals_count);
                return Object::make_error_string(trx, std::string_view{buffer, static_cast<size_t>(out - buffer)});
            } else {
                capacity = static_cast<length_t>(n);
            }
            saw_capacity_suffix = true;
        }
        if (locals_count == 0) {
            if (!saw_capacity_suffix) {
                cleanup();
                return Object::make_error_string(
                        trx, "empty local variable binding: use '||#N' or '||#+N' (N >= 1) for a no-args scratch frame dict");
            }
            if (capacity == 0) {
                cleanup();
                return Object::make_error_string(trx, "zero-capacity locals frame: use '||#N' or '||#+N' with N >= 1");
            }
        }
        // push K then N onto scratch stack (begin-locals pops N first, then K)
        if ((state->ptr + 2) > state->limit) {
            cleanup();
            trx->error(Error::LimitCheck, "local variable binding: temporary storage exhausted");
        } else {
            auto k_obj = Object::make_integer(static_cast<integer_t>(locals_count));
            k_obj.set_save_level(curr_save_level);
            *state->ptr++ = k_obj;
            auto n_obj = Object::make_integer(static_cast<integer_t>(capacity));
            n_obj.set_save_level(curr_save_level);
            *state->ptr++ = n_obj;
            trx->m_op_ptr = (state->ptr - 1);
        }
        return Object::make_integer(static_cast<integer_t>(locals_count));
    } else {
        // not a locals preamble -- rewind only if the buffer was
        // not refilled during the whitespace skip / peekc above.
        if (m_rptr_offset >= saved_rptr) {
            m_rptr_offset = saved_rptr;
            m_line_number = saved_line;
            m_line_pos = saved_pos;
        }
    }
    return Object::make_integer(-1);
}

// Local variable binding post-processing for scan_procedure.
// Transforms [/n1 /n2 ... /nK  K  N  body...] into [/n1 /n2 ... /nK  K  N  {body}  begin-locals].
// Called when a |...|#N or ||#N preamble was scanned; K may be 0 for the ||#N form
// (the begin-locals opcode handles K=0 by skipping the name/value pair loop).
// Modifies state->ptr and trx->m_op_ptr; returns the new body length.
// Raises VMFull or LimitCheck on failure (after calling cleanup).
template<typename CleanupFn>
[[nodiscard]] length_t finalize_local_bindings(
        Trix *trx, Object *base, ProcScanState *state, length_t locals_count, length_t length, CleanupFn &&cleanup) {
    auto curr_save_level = trx->m_curr_save_level;
    // Preamble = K names + K-integer + N-integer.
    auto preamble_count = static_cast<length_t>(locals_count + 2);
    if (length > preamble_count) {
        // non-empty body: wrap body elements into a nested executable packed proc
        auto body_start = (base + preamble_count);
        auto body_length = static_cast<length_t>(length - preamble_count);

        // The wrap rewrites body_start[0..body_length) to body_start[0..1] = {body-proc, begin-locals}.
        // body_length >= 2 shrinks the footprint; body_length == 1 grows by one slot and writes
        // body_start[1] at state->ptr -- which must be strictly below state->limit.  Without this
        // guard, a body filled to exactly the operand-stack limit overruns into VM beyond it.
        if ((body_start + 2) > state->limit) {
            cleanup();
            trx->error(Error::LimitCheck, "local variable binding: temporary storage exhausted");
        } else {
            auto [packed_ptr, body_offset] = Object::make_packed_data(trx, body_start, body_length, false);
            if (packed_ptr == nullptr) {
                cleanup();
                trx->error(Error::VMFull, "while constructing local binding body proc");
            } else {
                auto body_proc = Object::make_packed(body_offset, body_length, Object::ExecutableAttrib);

                // Do NOT free the original body elements' ExtValues here.  ULong /
                // Long / Address / Double / Real / 128-bit literals encode into the
                // packed byte stream as the OFFSET to their VM-heap ExtValue slot,
                // not as the value itself; freeing the ExtValue here would dangle
                // every such reference inside body_proc and crash on the first
                // begin-locals replay (extvalue: accessing freed or invalid
                // ExtValue at offset N).  The regular packed-proc path
                // (make_proc_object below) correctly preserves the ExtValues -- we
                // mirror it.

                // replace body elements with: {body-proc}  begin-locals
                body_start[0] = body_proc;
                body_start[0].set_save_level(curr_save_level);
                body_start[1] = Object::make_operator(SystemName::BeginLocals);
                body_start[1].set_save_level(curr_save_level);
                state->ptr = body_start + 2;
                trx->m_op_ptr = (state->ptr - 1);
                return static_cast<length_t>(state->ptr - base);
            }
        }
    } else {
        // edge case: locals with empty body -- just emit the setup/teardown
        // [/n1 ... /nK  K  N] -> append empty body proc + begin-locals
        if ((state->ptr + 2) <= state->limit) {
            auto empty_body = Object::make_packed(nulloffset, 0, Object::ExecutableAttrib);
            empty_body.set_save_level(curr_save_level);
            *state->ptr++ = empty_body;
            auto op = Object::make_operator(SystemName::BeginLocals);
            op.set_save_level(curr_save_level);
            *state->ptr++ = op;
            trx->m_op_ptr = (state->ptr - 1);
            return static_cast<length_t>(state->ptr - base);
        } else {
            cleanup();
            trx->error(Error::LimitCheck, "local variable binding: temporary storage exhausted");
        }
    }
}

// Construct the final procedure Object (packed or array) from validated body elements.
// Called after state restoration; uses base[0..length) which are still readable.
//
// GC-rooting contract: the caller MUST leave trx->m_op_ptr covering the body
// (i.e. at base+length-1, NOT yet lowered to saved_op_ptr) on entry.  This
// function's global-VM allocation -- make_packed_data / vm_alloc_dispatch_n --
// can trigger a mark-sweep GC (VMFull retry, or the debug vm-gc-stress hook),
// and a body element may be a global-VM sub-literal (e.g. a [...] array built
// inside ${...}); such an element is GC-reachable ONLY while it sits at/below
// m_op_ptr (walk_all_roots marks [m_op_base, m_op_ptr]).  Lowering m_op_ptr
// before the allocation would orphan the live array and the sweep would free
// it out from under the body proc -- a use-after-free once the proc replays.
// On EVERY exit m_op_ptr ends at saved_op_ptr: error paths via free_body (which
// also frees the body's ExtValues), success paths via the caller after the
// returned proc -- which now holds the body -- is itself rooted.
//
// For }#= (is_eqproc), the proc body lives in m_eqproc_storage_ptr (shared scratch that
// avoids VM-heap fragmentation for short-lived procs).  The returned Object has
// SpecialFlag set ("eqproc-ref"); the value slot holds m_generation (the creation-time
// counter, a union alias over the value slot) instead of a VM offset.  Accessors validate
// m_generation against the current counter: a mismatch means the storage was reused by
// a later }#= and the reference is stale, at which point access raises Unsupported rather
// than returning corrupted data.  Dict/array/record/tagged/cell all accept eqrefs at
// store time (the full 8-byte Object including SpecialFlag survives), so the read-time
// staleness check still fires on every subsequent access.  Eqrefs can also ride
// inside packed arrays: make_packed_data routes them through PackedExt subcodes
// (see PackedExtSubcode_Eq*); a ref that goes stale before the packed stream is
// replayed raises the usual stale-ref Unsupported error on access.
//
// Returns: {LiteralValue, proc} on success.
// Raises: VMFull, LimitCheck, Unsupported.
[[nodiscard]] static ScanToken
make_proc_object(Trix *trx, Object *base, length_t length, Object *saved_op_ptr, const ProcSuffix &suffix) {
    auto free_body = [&]() {
        for (length_t i = 0; i < length; ++i) {
            base[i].maybe_free_extvalue(trx);
        }
        // Drop the now-consumed body off the operand stack BEFORE the caller's
        // error() builds its operand preview -- free_body just invalidated the
        // body's ExtValues, so leaving them at/below m_op_ptr would let the
        // preview formatter read freed memory.  The success paths leave
        // m_op_ptr high so the body stays GC-rooted across the global-VM
        // allocation below (see the header comment); the caller lowers it to
        // saved_op_ptr once the body is safely committed to its proc storage.
        trx->m_op_ptr = saved_op_ptr;
    };

    // within Array and Packed, replace executable names with underlying operator value
    if (suffix.is_earlybinding) {
        Object::bind_array(trx, base, length, Object::ArrayKind::Normal);
    }

    // Before committing to eqproc storage reuse, verify the generation counter hasn't
    // exhausted its 32-bit range.  Wraparound would let a stale ref accidentally match a
    // future generation, defeating staleness detection.  Fail cleanly instead.
    auto check_eqproc_generation = [&]() {
        if (trx->m_eqproc_generation == std::numeric_limits<uint32_t>::max()) {
            free_body();
            trx->error(Error::LimitCheck, "}}#= generation counter exhausted (2^32 creations); cannot create more eqproc refs");
        }
    };

    // For {...}#$, temporarily set m_curr_alloc_global=true around the
    // allocation so make_packed_data's existing global-routing path fires
    // and the Array path below uses the dispatching allocator.  For
    // {...}#$$, set m_curr_alloc_global=false so an enclosing ${...} or
    // set-global true is overridden for this one literal.  #$ and #$$
    // are mutex with #= so we never fire this when eqproc storage is in play.
    auto saved_alloc_global = trx->m_curr_alloc_global;
    auto override_flag = false;
    if (suffix.alloc == AllocClass::GlobalVm) {
        trx->m_curr_alloc_global = true;
        override_flag = true;
    } else if (suffix.alloc == AllocClass::LocalVm) {
        trx->m_curr_alloc_global = false;
        override_flag = true;
    }
    struct AllocFlagRestore {
        Trix *trx;
        bool saved;
        bool restore;
        ~AllocFlagRestore() {
            if (restore) {
                trx->m_curr_alloc_global = saved;
            }
        }
    };
    const AllocFlagRestore restore_guard{trx, saved_alloc_global, override_flag};

    auto is_eqproc = (suffix.alloc == AllocClass::EqTmp);
    if (suffix.is_packed) {
        // packed
        if (is_eqproc) {
            check_eqproc_generation();
        }
        auto [packed_ptr, offset] = Object::make_packed_data(trx, base, length, is_eqproc);
        if (packed_ptr == nullptr) {
            free_body();
            trx->error(Error::VMFull, "while constructing a packed procedure");
        } else if (is_eqproc) {
            // bump generation; the eqproc-ref's value slot holds m_generation (the
            // creation-time counter); accessors resolve through trx->m_eqproc_storage_ptr
            ++trx->m_eqproc_generation;
            return literal_pair(Object::make_eqproc_packed(trx->m_eqproc_generation, length));
        } else {
            return literal_pair(Object::make_packed(offset, length, Object::ExecutableAttrib));
        }
    } else {
        // array
        auto eqproc_length = trx->m_eqproc_length;
        if (is_eqproc && (length > eqproc_length)) {
            free_body();
            trx->error(Error::LimitCheck, "proc length {} exceeds =proc max length {}", length, eqproc_length);
        } else {
            if (is_eqproc) {
                check_eqproc_generation();
            }
            auto access = suffix.is_readonly ? Object::ReadOnlyAccess : Object::ReadWriteAccess;
            if (is_eqproc) {
                // Free old eqproc Objects (if any) before overwriting with the new proc body.
                for (length_t i = 0; i < trx->m_eqproc_stored_length; ++i) {
                    trx->m_eqproc_storage_ptr[i].maybe_free_extvalue(trx);
                }
                trx->m_eqproc_stored_length = length;
                std::copy_n(base, length, trx->m_eqproc_storage_ptr);
                // bump generation; the eqproc-ref's value slot holds m_generation (the
                // creation-time counter); accessors resolve through trx->m_eqproc_storage_ptr
                ++trx->m_eqproc_generation;
                return literal_pair(Object::make_eqproc_array(trx->m_eqproc_generation, length, access));
            } else {
                // Local path uses the bump allocator's remaining-check + free_body
                // to surface VMFull cleanly; global path defers to gvm_alloc_n
                // (which raises VMFull internally if the global allocator cannot satisfy).
                // vm_alloc_dispatch_n routes by m_curr_alloc_global, which is
                // forced by the AllocFlagRestore guard above for #$ / #$$ (and
                // is otherwise inherited from the enclosing ${...} or set-global).
                if (!trx->m_curr_alloc_global && (trx->vm_remaining<Object>() < (length * sizeof(Object)))) {
                    free_body();
                    trx->error(Error::VMFull, "while constructing a proc");
                } else {
                    auto [alloc_ptr, alloc_offset] = trx->vm_alloc_dispatch_n<Object>(length, Trix::ChunkKind::Array);
                    std::copy_n(base, length, alloc_ptr);
                    return literal_pair(Object::make_array(alloc_offset, length, Object::ExecutableAttrib, access));
                }
            }
        }
    }
}

// Append a scanned element to the proc scratch area, or bookkeep overflow.  Flips
// *valid_proc from true to false on the first transition from within-capacity to
// overflow.  Maintains the invariant that state->ptr tracks one past m_op_ptr
// (make_array/dict_from_mark in a prior scanner() call may have lowered m_op_ptr).
// Returns true when the element was committed to the proc body, false on overflow.
// Caller uses the return value to gate side-tables that should only reflect
// committed elements (e.g. TRIX_DEBUGGER's per-op source-line table).
[[nodiscard]] bool
append_or_overflow(Trix *trx, Object value_obj, save_level_t curr_save_level, ProcScanState *state, bool *valid_proc) {
    state->ptr = (trx->m_op_ptr + 1);
    if (*valid_proc && (state->ptr < state->limit)) {
        value_obj.set_save_level(curr_save_level);
        *state->ptr++ = value_obj;
        trx->m_op_ptr = (state->ptr - 1);
        return true;
    } else {
        // Free any ExtValue before discarding the element.
        // Track discarded marks so ]/>> in scan_delimiter can skip make_array/dict_from_mark.
        value_obj.maybe_free_extvalue(trx);
        if (value_obj.is_mark()) {
            ++state->overflow_bracket_depth;
        }
        ++state->additional_count;
        if (*valid_proc) {
            *valid_proc = false;
            state->overflowed = true;
        }
        return false;
    }
}

// Finalize a successfully-scanned procedure: validate suffix constraints, fold in
// local-variable bindings, guard against nested }#=, restore scanner state, and
// construct the final proc Object.  Takes the cleanup lambda by template param so
// it runs with the enclosing scan_procedure frame's captured state on error paths.
//
// `body_lines_ptr` is a TRIX_DEBUGGER-only sidecar: the per-element source-
// line array built during the body-scan loop.  Pre-rewrite this vector
// is sized to the user's body length.  For non-|locals| procs the result
// is the body itself, so the array maps 1:1.  For |locals| procs,
// finalize_local_bindings rewrites base[0..1] into [inner-packed-body,
// begin-locals]; the user's lines correspond to the INNER body's elements,
// indexed at [preamble_count..length).  We stash both annotations here
// (the outer 2-element body gets no entry; the inner gets the slice).
template<typename CleanupFn>
[[nodiscard]] ScanToken finalize_valid_procedure(Trix *trx,
                                                 Object *base,
                                                 Object *saved_op_ptr,
                                                 Object *old_mark_floor,
                                                 int old_overflow_bracket_depth,
                                                 bool old_overflowed,
                                                 length_t locals_count,
                                                 bool has_locals_preamble,
                                                 ProcSuffix suffix,
                                                 ProcScanState *state,
                                                 CleanupFn &&cleanup
#ifdef TRIX_DEBUGGER
                                                 ,
                                                 std::vector<int32_t> *body_lines_ptr = nullptr
#endif
) {
    // Check #pw / #w before state restoration: cleanup() can still iterate base..state->ptr
    // and free ExtValues; after state->ptr = base that range would be gone.
    // suffix.is_packed covers both the explicit (#p) and default (no #a) packed cases;
    // packed procs are always ReadOnly (make_packed hardcodes ReadOnlyAccess).
    if (suffix.is_packed && suffix.explicit_writable) {
        cleanup();
        trx->error(Error::Unsupported, "packed procedure must be ReadOnly");
    } else {
        auto length = static_cast<length_t>(state->ptr - base);
        if (has_locals_preamble) {
            if (!suffix.is_packed) {
                // The user explicitly asked for array form (#a) but a |locals| / || preamble
                // forces packed: finalize_local_bindings wraps the body into a NESTED packed
                // proc, so #a on the outer would neither reach nor speed the body.  Warn rather
                // than silently drop the explicit suffix.
                std::print(stderr,
                           "trix: warning: line {}:{}: '#a' (array form) ignored on a procedure "
                           "with a |locals| preamble -- such procedures are always packed\n",
                           m_line_number,
                           m_line_pos);
            }
            length = finalize_local_bindings(trx, base, state, locals_count, length, cleanup);
            suffix.is_packed = true;  // the transformation relies on packed output
// Inner-body annotation.  finalize_local_bindings places the
// inner packed-body Object at base[preamble_count] (with
// begin-locals immediately after), where preamble_count =
// locals_count + 2.  Its m_packed is the inner body's
// storage offset.  body_lines tracks ONLY the user-body
// tokens (scan_local_bindings emits the preamble via direct
// *state->ptr++ writes, not through append_or_overflow), so
// body_lines.size() equals the inner body's length and we
// can stash it directly.  Must happen BEFORE the state reset
// below -- subsequent allocations could overwrite base slots.
#ifdef TRIX_DEBUGGER
            if ((body_lines_ptr != nullptr) && !body_lines_ptr->empty()) {
                auto preamble_count = static_cast<length_t>(locals_count + 2);
                auto inner = base[preamble_count];
                if (inner.is_packed() && !inner.is_eqproc_ref()) {
                    auto inner_offset = inner.m_packed;
                    if (inner_offset != nulloffset) {
                        trx->m_debug_proc_lines[inner_offset] = std::move(*body_lines_ptr);
                    }
                }
            }
#endif
        }

        // Check for nested }#= before state restoration: a nested }#= proc body element
        // would point into m_eqproc_storage_ptr; assembling the outer }#= would overwrite it.
        if (suffix.alloc == AllocClass::EqTmp) {
            auto eqproc_offset = trx->ptr_to_offset(trx->m_eqproc_storage_ptr);
            for (length_t i = 0; i < length; ++i) {
                if (base[i].is_sequence() && (base[i].offset() == eqproc_offset)) {
                    cleanup();
                    trx->error(Error::Unsupported, "}}#= may not contain a nested }}#= procedure");
                }
            }
        }

        state->ptr = base;
        state->mark_floor = old_mark_floor;
        state->overflow_bracket_depth = old_overflow_bracket_depth;
        state->overflowed = old_overflowed;

        // Keep m_op_ptr covering base[0..length) across make_proc_object so a GC
        // triggered by its global-VM allocation still marks the body elements
        // (including any ${...}-built global sub-literal) as roots.  make_proc_object
        // restores m_op_ptr to saved_op_ptr on its error paths (via free_body);
        // here we restore it on the success path, once the body has been copied
        // into -- or referenced by -- the returned (rooted) proc storage.
        auto proc = make_proc_object(trx, base, length, saved_op_ptr, suffix);
        trx->m_op_ptr = saved_op_ptr;
        return proc;
    }
}

// Shared body of scan_global_block / scan_local_block.  The caller establishes the
// Global/Local AllocScope around this call; this scans the body proc under that scope,
// emits it into the surrounding context, and returns `opener` (@in-global / @in-local)
// as the next token.  `exhausted_msg` is the scratch-exhausted error text for the
// enclosing block syntax.
[[nodiscard]] ScanToken
scan_scope_block(Trix *trx, int proc_nesting, ProcScanState *state, SystemName opener, const char *exhausted_msg) {
    ScanToken body_token{};
    if (state != nullptr) {
        body_token = scan_procedure(trx, proc_nesting + 1, state);
    } else {
        ProcScanState new_state{
                .ptr = (trx->m_op_ptr + 1),
                .limit = trx->m_op_limit,
                .mark_floor = (trx->m_op_ptr + 1),
        };
        body_token = scan_procedure(trx, proc_nesting + 1, &new_state);
    }

    auto [body_lexeme, body_proc] = body_token;
    if (body_lexeme != Lexeme::LiteralValue) {
        return body_token;  // SyntaxError / EndOfStream -- propagate
    } else {
        // Emit body_proc into the surrounding context, then return `opener` as the next
        // token so the caller dispatches it (top level) or stores it into the proc body
        // (when state != nullptr) for later execution.
        if (state != nullptr) {
            if (state->ptr >= state->limit) {
                body_proc.maybe_free_extvalue(trx);
                return syntaxerror_pair(Object::make_error_string(trx, exhausted_msg));
            } else {
                *state->ptr++ = body_proc;
                trx->m_op_ptr = (state->ptr - 1);
            }
        } else {
            trx->require_op_capacity(1);
            *++trx->m_op_ptr = body_proc;
        }
        return executable_pair(Object::make_control_operator(opener));
    }
}

// ${...} -- global allocation scope block.
//
// Called when the scanner has just consumed the opening '$' '{'.  Sets
// m_curr_alloc_global=true for the duration of body scanning so any
// literal Name (or future flag-honoring literal) interned inside lands
// in the global VM region.  Then emits a 2-token sequence into the
// surrounding context:
//   - body_proc as a literal value (pushed to op stack at runtime, or
//     stored in the enclosing proc's body)
//   - @in-global as the executable token returned to the caller, which
//     pops body_proc from op stack and runs it under
//     m_curr_alloc_global=true (with @end-in-global cleanup on normal
//     completion or err-stack-driven restore on error unwind)
//
// Returns: {ExecutableValue, @in-global op} on success; whatever
//   scan_procedure returned on failure (already cleaned up).
// Raises: whatever scan_procedure may raise.
[[nodiscard]] ScanToken scan_global_block(Trix *trx, int proc_nesting, ProcScanState *state) {
    const GlobalAllocScope scope(trx);
    return scan_scope_block(
            trx, proc_nesting, state, SystemName::atInGlobal, "${...}: scratch storage exhausted while emitting body proc");
}

// $${...} -- force-local allocation scope block.  Symmetric inverse of
// scan_global_block: scans the body under LocalAllocScope (sets
// m_curr_alloc_global=false) so literal Names interned in the body
// land in local VM regardless of any enclosing ${...} or set-global
// true.  Emits @in-local as the runtime opener; the closer
// (@end-in-global) is shared with the global form since it just
// restores whatever Boolean was saved on the err stack.
//
// Returns: {ExecutableValue, @in-local op} on success; whatever
//   scan_procedure returned on failure (already cleaned up).
// Raises: whatever scan_procedure may raise.
[[nodiscard]] ScanToken scan_local_block(Trix *trx, int proc_nesting, ProcScanState *state) {
    const LocalAllocScope scope(trx);
    return scan_scope_block(
            trx, proc_nesting, state, SystemName::atInLocal, "$${...}: scratch storage exhausted while emitting body proc");
}

// procedure suffix
//
// [#[=][a|p][e|l][r|w]]
//
// =   use temporary =proc storage
// a   Array
// p   Packed (default)
// e   early binding
// l   late binding (default)
// r   ReadOnly Array access
// w   ReadWrite Array access (default for Array, unsupported for Packed)
//
// Suffix letters must appear in the order listed above (left to right).
// E.g. }#er (early-binding, read-only) is accepted; }#re is rejected.
// Scans a procedure body from the current stream position up to the matching '}'.
// Collects body elements in state->ptr scratch storage (shared with the enclosing level),
// applies optional suffixes (#=, #a/#p, #e/#l, #r/#w), and returns a Packed or Array
// procedure Object.  proc_nesting is 1 for the outermost '{', incremented by 1 per
// nesting level; used to annotate error messages and guard against C++ stack overflow.
// Returns: {LiteralValue, proc} on success; {SyntaxError, errstr} for an invalid
//   suffix, a MaxProcNesting violation, or a SyntaxError/EndOfStream from scanner().
// Raises: LimitCheck if scratch storage is exhausted or the eqproc body exceeds
//   m_eqproc_length; VMFull from make_packed_data or vm_alloc<Object>; Unsupported
//   for '#pw'/'#w' on a packed proc or a nested '}#=' element inside a '}#=' proc.
[[nodiscard]] ScanToken scan_procedure(Trix *trx, int proc_nesting, ProcScanState *state) {
    // Guard against maliciously deep nesting causing C++ stack overflow.
    // Each nesting level recurses through scanner -> scan_delimiter -> scan_procedure.
    if (proc_nesting > MaxProcNesting) {
        return syntaxerror_pair(
                Object::make_error_string(trx, "'proc' nesting depth {} exceeds maximum {}", proc_nesting, MaxProcNesting));
    }
    auto base = state->ptr;
    auto saved_op_ptr = trx->m_op_ptr;
    auto old_mark_floor = state->mark_floor;
    auto old_overflow_bracket_depth = state->overflow_bracket_depth;
    auto old_overflowed = state->overflowed;
    state->mark_floor = (saved_op_ptr + 1);
    state->overflow_bracket_depth = 0;
    state->overflowed = false;
    auto curr_save_level = trx->m_curr_save_level;
    auto valid_proc = true;
    ProcSuffix suffix;
    // Line of the opening '{': captured here because scan_procedure is entered
    // after the '{' has been consumed but before any whitespace/newline skip.
    // Used to annotate EndOfStream-inside-'{' errors with the opener's line.
    auto opening_line = m_line_number;
    // cleanup: free ExtValues of scanned-but-discarded proc body elements, then restore state.
    //
    // When the throw originated from error() -> global_handler (m_in_global_handler is true),
    // global_handler has already repurposed the op stack -- it reset m_op_ptr to
    // m_op_base - 1 and pushed the six handler operands (dstack, estack, ostack,
    // error-msg, last-operator, last-error-name) at m_op_base..m_op_base+5.  Those
    // pushes may have physically overwritten the scanner's scratch area at base..
    // state->ptr-1 (when the outermost proc started with an empty op stack, base
    // sits at m_op_base, fully overlapping the handler operand zone; for inner
    // procs or procs entered with a non-empty op stack, base is strictly above
    // the handler zone and there is no overlap).  In either case, walking the
    // [base, state->ptr) range to free ExtValues is harmless because the
    // overwriting operands are non-ExtValue types -- but then resetting m_op_ptr
    // back to saved_op_ptr would wipe the handler operands and leave the handler
    // thinking the op stack is empty, producing a bogus OpStackUnderflow in
    // default_handler_op and a "recursive error during global_handler" at the
    // next level.  So when the handler has claimed the stack, skip cleanup
    // entirely; the handler now owns the operand stack from m_op_base up.
    auto cleanup = [&]() {
        if (!trx->m_in_global_handler) {
            // When the unwinding error was caught by an op-stack-planting
            // barrier (try / try-catch / repl), try_catch_handler has already
            // pushed its recovery objects (/error-name, plus the failing op
            // for repl) ABOVE the scanner scratch -- error() runs the walk
            // BEFORE throwing, so this cleanup executes after the plant.
            // Resetting m_op_ptr below them would hand the recovery atom
            // (@try-rollback / @catch-error / @repl-recover) stale stack
            // slots in their place.  Stash them and re-plant after the
            // reset.  Planted objects are inline (Names, Operators) -- no
            // heap-cell ownership to manage in the copy.
            auto planted_count = trx->m_planted_error_objects;
            Object planted[2] = {Object::make_null(), Object::make_null()};
            assert(planted_count <= 2);
            for (auto i = planted_count; i > 0; --i) {
                planted[i - 1] = *trx->m_op_ptr--;
            }
            for (auto p = base; p < state->ptr; ++p) {
                // Guard: error cascades can leave already-freed ExtValue/WideValue
                // Objects in the scratch area.  Their attribute byte still indicates
                // the heap-cell type but the offset is no longer live.  Skip them
                // to avoid a double-free on the heap free list.
                if (!p->heap_cell_freed(trx)) {
                    p->maybe_free_extvalue(trx);
                }
            }
            state->ptr = base;
            trx->m_op_ptr = saved_op_ptr;
            for (auto i = 0; i < planted_count; ++i) {
                *++trx->m_op_ptr = planted[i];
            }
            state->mark_floor = old_mark_floor;
            state->overflow_bracket_depth = old_overflow_bracket_depth;
            state->overflowed = old_overflowed;
        }
    };
    // Local variable binding preamble: { |a b c| body } or { ||#N body }.
    // scan_local_bindings returns Integer(K>=0) when a preamble was scanned, or
    // Integer(-1) when no preamble was present.  The ||#N form yields K=0 with a
    // preamble, distinguished here via has_locals_preamble so finalize runs.
    auto locals_count = length_t{0};
    auto has_locals_preamble = false;
    auto locals_count_obj = scan_local_bindings(trx, state, curr_save_level, cleanup);
    if (locals_count_obj.is_integer()) {
        auto k = locals_count_obj.integer_value();
        if (k >= 0) {
            locals_count = static_cast<length_t>(k);
            has_locals_preamble = true;
        }
    } else {
        return syntaxerror_pair(locals_count_obj);
    }

// Per-element source-line tracking for `proc-disasm` annotation.  Each
// committed body element gets the line it was scanned from (post-scanner
// m_line_number, which approximates the token's containing line for
// single-line tokens and reports the END line for multi-line tokens
// like cross-line string literals).  Only built when TRIX_DEBUGGER is
// compiled in -- entire vector + stash side-effect is conditional.
#ifdef TRIX_DEBUGGER
    std::vector<int32_t> body_lines{};
#endif

    while (true) {
        auto [token, value_obj] = [&] {
            try {
                return scanner(trx, proc_nesting, state);
            }
            catch (...) {
                // Resync state->ptr from m_op_ptr: if the throw came from
                // make_dict/array/set_from_mark, it already freed ExtValues
                // above the mark and lowered m_op_ptr.  Without this resync,
                // cleanup() would revisit those freed positions and hit an
                // assert in ExtValue::free (double-free of nulloffset).
                state->ptr = (trx->m_op_ptr + 1);
                cleanup();
                throw;
            }
        }();
        if ((token == Lexeme::SyntaxError) || (token == Lexeme::EndOfStream)) {
            char buffer[96];
            if (proc_nesting == 1) {
                auto [out, _] =
                        std::format_to_n(buffer, sizeof(buffer), " (while scanning a 'proc' opened at line {})", opening_line);
                value_obj =
                        Object::append_error_string(trx, value_obj, std::string_view{buffer, static_cast<size_t>(out - buffer)});
            } else {
                // annotate with depth so errors in deeply nested { { { ... } } } blocks are traceable
                auto [out, _] = std::format_to_n(buffer,
                                                 sizeof(buffer),
                                                 " (while scanning a nested 'proc' at depth {} opened at line {})",
                                                 proc_nesting,
                                                 opening_line);
                value_obj =
                        Object::append_error_string(trx, value_obj, std::string_view{buffer, static_cast<size_t>(out - buffer)});
            }
            cleanup();
            // EndOfStream inside an unclosed '{' is a hard syntax error, not a clean
            // end-of-input: execute_stream/execute_string silently drop EndOfStream at
            // the top level, so promote to SyntaxError here or the error would vanish.
            if (token == Lexeme::EndOfStream) {
                return syntaxerror_pair(value_obj);
            } else {
                return std::pair{token, value_obj};
            }
        } else if (token == Lexeme::EndOfProcedure) {
            Object err_obj;
            std::tie(err_obj, suffix) = scan_proc_suffix(trx);
            if (err_obj.is_null()) {
                break;
            } else {
                cleanup();
                return syntaxerror_pair(err_obj);
            }
        } else {
            [[maybe_unused]] auto committed = append_or_overflow(trx, value_obj, curr_save_level, state, &valid_proc);
#ifdef TRIX_DEBUGGER
            if (committed) {
                body_lines.push_back(static_cast<int32_t>(m_line_number));
            }
#endif
        }
    }

    if (valid_proc) {
        auto result = finalize_valid_procedure(trx,
                                               base,
                                               saved_op_ptr,
                                               old_mark_floor,
                                               old_overflow_bracket_depth,
                                               old_overflowed,
                                               locals_count,
                                               has_locals_preamble,
                                               suffix,
                                               state,
                                               cleanup
#ifdef TRIX_DEBUGGER
                                               ,
                                               &body_lines
#endif
        );
// Stash per-element source-line annotation against the result
// proc's body offset for non-|locals| procs.  |locals| procs are
// annotated INSIDE finalize_valid_procedure, against the inner
// packed-body's storage offset (see the inner-body branch up
// there) -- their `body_lines` was already moved-from at that
// point.  Eqproc-refs share the m_eqproc_storage buffer across
// many procs; keying by that offset would conflate them, so we
// skip annotation for }}#= (which isn't a proc-disasm target
// anyway).
#ifdef TRIX_DEBUGGER
        if (!has_locals_preamble && !body_lines.empty()) {
            auto result_obj = result.second;
            vm_offset_t body_offset = nulloffset;
            if (result_obj.is_packed() && !result_obj.is_eqproc_ref()) {
                body_offset = result_obj.m_packed;
            } else if (result_obj.is_array()) {
                body_offset = trx->ptr_to_offset(result_obj.array_objects(trx));
            }
            if (body_offset != nulloffset) {
                trx->m_debug_proc_lines[body_offset] = std::move(body_lines);
            }
        }
#endif
        return result;
    } else {
        // Free ExtValues for proc body elements accumulated before overflow, then restore state.
        cleanup();
        // Use limit-base (capacity for this nesting level) rather than a cached field so
        // nested proc error messages accurately reflect available space, not total capacity.
        auto capacity = static_cast<uint32_t>(state->limit - base);
        auto needed_count = (capacity + state->additional_count);
        trx->error(Error::LimitCheck,
                   "temporary storage of {} Objects exhausted while constructing a 'proc' of {} Objects",
                   capacity,
                   needed_count);
    }
}

// #a byte array suffix: convert string scratch bytes directly into an Array
// of Byte objects.  No intermediate string allocated on the VM heap.
// Called from scan_string_suffix after '#a' has been consumed.
// The scratch allocation (vm_start_alloc) is still open on entry.
[[nodiscard]] ScanToken scan_byte_array_suffix(Trix *trx, length_t length) {
    auto ch = peekc(trx);

    // Optional #r / #w (default: read-write)
    auto is_readonly = false;
    auto valid = true;
    if (Trix::is_alpha(ch)) {
        ch = Trix::to_lowercase(ch);
        if (ch == 'r') {
            is_readonly = true;
            consume(trx);
        } else if (ch == 'w') {
            consume(trx);
        } else {
            valid = false;
        }
    }

    if (valid) {
        ch = peekc(trx);
        valid = ((ch == EOFc) || Trix::is_terminator(ch));
    }

    if (valid) {
        // Seal scratch allocation (commit 0 bytes).  The byte data at
        // scratch_ptr[0..length-1] remains readable in memory because
        // vm_end_alloc_ptr(0) only clears m_vm_alloc_active and resets
        // m_vm_ptr to the scratch base -- it does not zero the region.
        auto scratch_ptr = trx->vm_end_alloc_ptr<vm_t>(0);

        // Allocate a properly Object-aligned array.  This may introduce
        // a 0-3 byte alignment gap between scratch_base and array_base.
        auto [objects, array_offset] = trx->vm_alloc_n<Object>(length);

        // In-place backward fill: read byte scratch_ptr[i], write objects[i].
        //
        // Safety proof (alignment-aware):
        //   byte i    lives at  scratch_base + i
        //   object j  writes at [array_base + j*8,  array_base + j*8 + 7]
        //   gap = array_base - scratch_base  (0..3 bytes from alignment)
        //
        //   We process j = length-1 down to 0.  When writing object j,
        //   all bytes i < j have NOT yet been read.  Could object j
        //   overwrite byte i?
        //     scratch_base + i >= array_base + j*8
        //     => i >= gap + j*8
        //   Since j > i:  gap + j*8 >= (i+1)*8 = 8i + 8 > i.
        //   Therefore i < gap + j*8 -- byte i is BELOW the write region.
        //   No unread byte is ever overwritten.
        //   For i == j: we read the byte before writing the Object.  Safe.
        auto curr_save_level = trx->m_curr_save_level;
        for (auto i = (length - 1); i >= 0; --i) {
            objects[i] = Object::make_byte(scratch_ptr[i]);
            objects[i].set_save_level(curr_save_level);
        }

        auto access = is_readonly ? Object::ReadOnlyAccess : Object::ReadWriteAccess;
        auto arr_obj = Object::make_array(array_offset, length, Object::LiteralAttrib, access);
        return literal_pair(arr_obj);
    } else {
        trx->vm_end_alloc();
        return syntaxerror_pair(Object::make_error_string(trx, "'string' using #a suffix must be #a, #ar, or #aw)"));
    }
}

// Handles #b suffix: extracts a single byte from a length-1 string scratch area.
// The vm_start_alloc is still open on entry; closed on all paths.
// Returns: {LiteralValue, byte} on success; {SyntaxError, errstr} on error.
[[nodiscard]] ScanToken scan_byte_suffix(Trix *trx, length_t length) {
    if (length == 1) {
        consume(trx);
        auto ch = peekc(trx);
        if (!Trix::is_terminator(ch)) {
            trx->vm_end_alloc();
            return syntaxerror_pair(make_suffix_error_string(trx, BracketKind::String));
        } else {
            // Invariant: we are still within the vm_alloc started by the
            // string scanner (scan_text_string / scan_base16_string /
            // scan_raw_string).  vm_end_alloc_ptr(0) peeks at the last
            // allocated byte without sealing the allocation.  No vm_alloc
            // call may be interleaved between string construction and here.
            auto string_ptr = trx->vm_end_alloc_ptr<vm_t>(0);
            auto b = *string_ptr;
            return literal_pair(Object::make_byte(b));
        }
    } else {
        trx->vm_end_alloc();
        return syntaxerror_pair(Object::make_error_string(trx, "'string' using #b suffix must be length 1"));
    }
}

// Processes the optional #... suffix that follows a scanned string body and
// returns the final {Lexeme, Object} pair.
// length: number of bytes already written into the active vm_start_alloc scratch area.
// Returns: {LiteralValue, byte} for #b; {LiteralValue, string} for no-suffix, #=, #l,
//   #r, or #w; {ExecutableValue, string} for #x; {SyntaxError, errstr} for all errors.
// Raises: LimitCheck when #= is used and length exceeds m_eqstring capacity.
// vm_alloc lifecycle: the caller opens a vm_start_alloc; this function closes it on
// every path via vm_end_alloc_ptr, vm_end_alloc, or error().
[[nodiscard]] ScanToken scan_string_suffix(Trix *trx, length_t length) {
    auto is_literal = true;
    auto is_readonly = false;
    auto is_eqstring = false;
    auto offset = nulloffset;
    auto ch = peekc(trx);
    if (ch == '#') {
        consume(trx);
        ch = peekc(trx);

        auto valid = false;
        if ((ch == '=') || (ch == '$') || Trix::is_alpha(ch)) {
            ch = Trix::to_lowercase(ch);

            if (ch == 'a') {
                // #a byte array suffix
                consume(trx);
                return scan_byte_array_suffix(trx, length);
            } else if (ch == 'b') {
                // #b single byte suffix
                return scan_byte_suffix(trx, length);
            } else if (ch == '=') {
                // #=
                valid = true;
                auto eqstring = trx->root_object(RootObject::EqString);
                auto eqstring_ptr = eqstring.string_vptr_raw(trx);
                auto eqstring_length = eqstring.string_length();
                if (length > eqstring_length) {
                    trx->error(Error::LimitCheck, "'string' length {} exceeds =string max length {}", length, eqstring_length);
                } else if (trx->m_eqstring_generation == std::numeric_limits<uint32_t>::max()) {
                    // Wraparound would let a stale ref accidentally match a future generation;
                    // fail cleanly instead of silently reusing a counter value.
                    trx->error(Error::LimitCheck,
                               ")#= generation counter exhausted (2^32 creations); cannot create more eqstring refs");
                } else {
                    auto ptr = trx->vm_end_alloc_ptr<vm_t>(0);
                    std::copy_n(ptr, length, eqstring_ptr);
                    eqstring_ptr[length] = '\0';
                    // Bump generation; the eqstring-ref's value slot holds m_generation
                    // (the creation-time counter, not a VM offset).  Accessors resolve
                    // through root_object(EqString).
                    ++trx->m_eqstring_generation;
                    offset = trx->m_eqstring_generation;
                    is_eqstring = true;

                    consume(trx);
                    ch = peekc(trx);
                    if (ch == '$') {
                        consume(trx);
                        if (peekc(trx) == '$') {
                            trx->error(Error::SyntaxError, "(#=$$ not allowed: #$$ and #= are mutually exclusive");
                        } else {
                            trx->error(Error::SyntaxError, "(#=$ not allowed: #$ and #= are mutually exclusive");
                        }
                    }
                }
            } else if (ch == '$') {
                consume(trx);
                ch = peekc(trx);
                if (ch == '$') {
                    // #$$ -- force-local: the scratch alloc is already
                    // local-VM, so we just fall through to the normal
                    // nul-terminate / vm_end_alloc commit at the bottom.
                    // Useful inside a `${...}` block to opt one string
                    // back to local without exiting the scope.
                    consume(trx);
                    ch = peekc(trx);
                    valid = true;
                    if (ch == '$') {
                        trx->error(Error::SyntaxError, "(#$$$ not allowed (max two '$' after '#')");
                    } else if (ch == '=') {
                        trx->error(Error::SyntaxError, "(#$$= not allowed: #$$ and #= are mutually exclusive");
                    }
                } else {
                    // #$  -- copy bytes from the local scratch alloc to the
                    // global VM region, cancel the local commit, return an
                    // Object pointing into global.  The Object's offset
                    // (returned from gvm_alloc) already encodes the global
                    // region; is_global() can recover it later.
                    valid = true;
                    auto src = trx->vm_end_alloc_ptr<vm_t>(0);  // peek-and-cancel local
                    auto [dst, global_offset] = trx->gvm_alloc<vm_t>(length + 1, Trix::ChunkKind::String);
                    std::copy_n(src, length, dst);
                    dst[length] = '\0';
                    offset = global_offset;
                    if (ch == '=') {
                        trx->error(Error::SyntaxError, "(#$= not allowed: #$ and #= are mutually exclusive");
                    }
                }
            }

            // #l #x
            if (Trix::is_alpha(ch)) {
                ch = Trix::to_lowercase(ch);
                if ((ch == 'l') || (ch == 'x')) {
                    valid = true;
                    is_literal = (ch == 'l');

                    consume(trx);
                    ch = peekc(trx);
                }
            }

            // #r #w
            if (Trix::is_alpha(ch)) {
                ch = Trix::to_lowercase(ch);
                if ((ch == 'r') || (ch == 'w')) {
                    valid = true;
                    is_readonly = (ch == 'r');

                    consume(trx);
                } else {
                    valid = false;
                }
            }
        }

        // vm_end_alloc_ptr was called only when the #= or #$ branch ran (offset != nulloffset)
        if (valid) {
            ch = peekc(trx);
            if (!Trix::is_terminator(ch)) {
                if (offset == nulloffset) {
                    trx->vm_end_alloc();
                }
                return syntaxerror_pair(make_suffix_error_string(trx, BracketKind::String));
            }
        } else {
            if (offset == nulloffset) {
                trx->vm_end_alloc();
            }
            return syntaxerror_pair(make_suffix_separator_error_string(trx, BracketKind::String));
        }
    }

    if (offset == nulloffset) {
        // nul-terminate: callers reserve 1 extra byte in string_limit for this \0,
        // so position [length] is within the alloc area; commit length+1 bytes
        trx->m_vm_alloc_active[length] = '\0';
        std::tie(std::ignore, offset) = trx->vm_end_alloc<vm_t>(length + 1);
    }

    auto access = is_readonly ? Object::ReadOnlyAccess : Object::ReadWriteAccess;
    auto attrib = is_literal ? Object::LiteralAttrib : Object::ExecutableAttrib;
    auto string_obj = is_eqstring ? Object::make_eqstring(offset, length, attrib, access)
                                  : Object::make_string(offset, length, attrib, access);
    return (is_literal ? literal_pair(string_obj) : executable_pair(string_obj));
}

// Scans a base-16 (hex) literal string <hexstring> from the current stream position
// up to the terminating '>'.  The opening '<' has already been consumed by scan_delimiter.
// Pairs of hex digits produce one output byte each; whitespace is ignored.
// An odd number of hex digits is a SyntaxError.
// Returns: {SyntaxError, errstr} on EOF, invalid character, or odd nibble count.
// Raises: LimitCheck if the string exceeds MaxStringLength; VMFull if VM heap is full.
// On success, delegates to scan_string_suffix() for optional #b/#=/#l/#x/#r/#w suffixes.
[[nodiscard]] ScanToken scan_base16_string(Trix *trx) {
    auto [string_base, vm_remaining] = trx->vm_start_alloc<vm_t>();
    // Wrap-guard: vm_start_alloc returning 0 would make (vm_remaining - 1)
    // wrap to UINT_MAX and the subsequent std::min() pick MaxStringLength,
    // letting the scanner overrun the heap.  vm_start_alloc reserves at
    // least the slack documented in vm_heap.inl, so this should hold.
    assert(vm_remaining >= 1);

    auto string_ptr = string_base;
    // Reserve 1 byte for the nul terminator written by scan_string_suffix
    auto string_limit = string_base + std::min(vm_remaining - 1, static_cast<vm_size_t>(MaxStringLength));
    auto high_nibble = true;  // true = next successful hex digit writes the high nibble of a new byte
    auto valid = true;

    while (true) {
        // fast path: bulk consume hex digits and whitespace directly from the read buffer.
        // Falls out on '>' (end), on any other non-hex/non-whitespace char (error), or on
        // buffer exhaust.
        {
            auto rptr = trx->offset_to_ptr<vm_t>(m_rptr_offset);
            auto rlimit = trx->offset_to_ptr<vm_t>(m_rlimit_offset);
            while (rptr != rlimit) {
                auto ch = *rptr;
                if (Trix::is_hexdigit(ch)) {
                    if (string_ptr < string_limit) {
                        auto v = Trix::hexdigit_to_value(ch);
                        if (high_nibble) {
                            *string_ptr = static_cast<vm_t>(v << 4);
                        } else {
                            *string_ptr++ |= static_cast<vm_t>(v);
                        }
                        high_nibble = !high_nibble;
                    } else {
                        valid = false;
                    }
                    ++rptr;
                    ++m_line_pos;
                } else if (Trix::is_whitespace(ch)) {
                    ++rptr;
                    if (ch == ASCII_LF) {
                        ++m_line_number;
                        m_line_pos = 1;
                    } else {
                        ++m_line_pos;
                    }
                } else {
                    break;
                }
            }
            m_rptr_offset = trx->ptr_to_offset(rptr);
        }

        // slow path: handle the triggering char (or buffer refill).
        auto ch = getc(trx);
        if (ch == EOFc) {
            trx->vm_end_alloc();
            return syntaxerror_pair(Object::make_error_string(trx, "EndOfStream encountered within a <string>"));
        } else if (Trix::is_hexdigit(ch)) {
            if (string_ptr < string_limit) {
                ch = Trix::hexdigit_to_value(ch);
                if (high_nibble) {
                    *string_ptr = static_cast<vm_t>(ch << 4);
                } else {
                    *string_ptr++ |= static_cast<vm_t>(ch);
                }
                high_nibble = !high_nibble;
            } else {
                valid = false;
            }
        } else if (ch == '>') {
            break;
        } else if (!Trix::is_whitespace(ch)) {
            trx->vm_end_alloc();
            return syntaxerror_pair(Object::make_error_string(trx, "invalid character encountered within a <string>"));
        }
    }

    if (valid) {
        if (high_nibble) {
            auto length = static_cast<length_t>(string_ptr - string_base);
            return scan_string_suffix(trx, length);
        } else {
            trx->vm_end_alloc();
            return syntaxerror_pair(Object::make_error_string(trx, "odd number of characters contained within a <string>"));
        }
    } else {
        trx->vm_end_alloc();
        if (vm_remaining >= MaxStringLength) {
            trx->error(Error::LimitCheck, "<string> length exceeds maximum {}", MaxStringLength);
        } else {
            trx->error(Error::VMFull, "while scanning a <string>");
        }
    }
}

// Scans a raw string <(...)> from the current stream position up to the matching ')>'.
// The opening '<(' has already been consumed by scan_delimiter.
// No escape processing: backslash is a literal byte.
// Parenthesis nesting is tracked: balanced '(' and ')' pairs are included in the output.
// CRLF normalization: a bare CR in string body is emitted as LF.
// Returns: {SyntaxError, errstr} on EOF or missing '>'.
// Raises: LimitCheck if the string exceeds MaxStringLength; VMFull if VM heap is full.
// On success, delegates to scan_string_suffix() for optional #b/#=/#l/#x/#r/#w suffixes.
[[nodiscard]] ScanToken scan_raw_string(Trix *trx) {
    auto [string_base, vm_remaining] = trx->vm_start_alloc<vm_t>();
    // Wrap-guard: see scan_base16_string.  vm_remaining == 0 would wrap
    // (vm_remaining - 1) to UINT_MAX and let std::min pick MaxStringLength.
    assert(vm_remaining >= 1);

    auto string_ptr = string_base;
    // Reserve 1 byte for the nul terminator written by scan_string_suffix
    auto string_limit = (string_base + std::min(vm_remaining - 1, static_cast<vm_size_t>(MaxStringLength)));
    auto string_nesting = int{0};
    auto valid = true;

    while (true) {
        // fast path: copy plain bytes directly from the read buffer to the string buffer.
        // Plain = not ')' (end/nesting), not '(' (nesting), not ASCII_CR (CRLF).  Raw
        // strings do not honour '\\'; SS includes it, so the slow path below emits it as
        // a literal byte.  Per-byte ++m_line_pos is batched into one add at inner-loop exit.
        {
            auto rptr = trx->offset_to_ptr<vm_t>(m_rptr_offset);
            auto rlimit = trx->offset_to_ptr<vm_t>(m_rlimit_offset);
            auto line_start = rptr;
            while ((rptr != rlimit) && (string_ptr < string_limit)) {
                auto ch = *rptr;
                if (Trix::is_string_stop(ch)) {
                    break;
                } else {
                    *string_ptr++ = ch;
                    ++rptr;
                    if (ch == ASCII_LF) {
                        ++m_line_number;
                        m_line_pos = 1;
                        line_start = rptr;
                    }
                }
            }
            m_line_pos = static_cast<decltype(m_line_pos)>(m_line_pos + (rptr - line_start));
            m_rptr_offset = trx->ptr_to_offset(rptr);
        }

        // slow path: handle one special byte (or buffer refill).
        auto ch = getc(trx);
        if (ch == EOFc) {
            trx->vm_end_alloc();
            return syntaxerror_pair(Object::make_error_string(trx, "EndOfStream encountered within a <(raw string)>"));
        } else if (ch == ')') {
            if (string_nesting == 0) {
                // expect '>' to close the <(...)> form
                auto close_ch = peekc(trx);
                if (close_ch == '>') {
                    consume(trx);
                    break;
                } else {
                    trx->vm_end_alloc();
                    return syntaxerror_pair(Object::make_error_string(trx, "expected '>' after ')' in <(raw string)>"));
                }
            } else {
                // decrease nesting depth and use the ")"
                --string_nesting;
            }
        } else if (ch == '(') {
            // increase nesting depth and use the "("
            ++string_nesting;
        } else if (ch == ASCII_CR) {
            // CR (or CR+LF) is a single line break: normalize to LF in output and
            // correct the line tracking that getc did for the CR itself.
            fixup_cr(trx);
            ch = ASCII_LF;
        }

        if (string_ptr < string_limit) {
            *string_ptr++ = static_cast<vm_t>(ch);
        } else {
            valid = false;
        }
    }

    if (valid) {
        auto length = static_cast<length_t>(string_ptr - string_base);
        return scan_string_suffix(trx, length);
    } else {
        trx->vm_end_alloc();
        if (vm_remaining >= MaxStringLength) {
            trx->error(Error::LimitCheck, "<(raw string)> length exceeds maximum {}", MaxStringLength);
        } else {
            trx->error(Error::VMFull, "while scanning a <(raw string)>");
        }
    }
}

// Parse a \u (4 hex digits) or \U (8 hex digits) escape, validate the codepoint,
// and encode it as UTF-8 into utf8_buf[0..3].  On success returns make_integer(N)
// where N is the index of the last UTF-8 byte written (0 for 1-byte, 3 for 4-byte);
// caller copies buf[0..N-1] then emits buf[N] as the next character.  On error
// returns an error-string Object.
[[nodiscard]] Object scan_unicode_escape(Trix *trx, vm_t *utf8_buf, int digit_count) {
    auto codepoint = std::uint32_t{0};
    for (int i = 0; i < digit_count; ++i) {
        auto hex_ch = peekc(trx);
        if (hex_ch == EOFc) {
            return Object::make_error_string(trx, "EndOfStream encountered within a (string)");
        } else if (Trix::is_hexdigit(hex_ch)) {
            consume(trx);
            codepoint = (codepoint * 16) + static_cast<std::uint32_t>(Trix::hexdigit_to_value(hex_ch));
        } else {
            auto ch = (digit_count == 4) ? 'u' : 'U';
            return Object::make_error_string(trx, "\\{} requires exactly {} hex digits", ch, digit_count);
        }
    }
    if ((codepoint >= 0xD800u) && (codepoint <= 0xDFFFu)) {
        return Object::make_error_string(trx, "surrogate codepoint U+{:04X} is not allowed in \\u/\\U escape", codepoint);
    } else if (codepoint > 0x10FFFFu) {
        return Object::make_error_string(trx, "\\U codepoint U+{:08X} exceeds maximum U+10FFFF", codepoint);
    } else {
        // Encode a Unicode codepoint (U+0000..U+10FFFF) as UTF-8 into buf[0..3].
        // count = (bytes_written - 1): 0 for 1-byte, 3 for 4-byte.  Caller memcpys
        // buf[0..count-1] and then emits buf[count] as the next character.
        auto count = integer_t{};
        if (codepoint <= 0x7Fu) {
            utf8_buf[0] = static_cast<vm_t>(codepoint);
            count = 0;
        } else if (codepoint <= 0x7FFu) {
            utf8_buf[0] = static_cast<vm_t>(0xC0u | (codepoint >> 6));
            utf8_buf[1] = static_cast<vm_t>(0x80u | (codepoint & 0x3Fu));
            count = 1;
        } else if (codepoint <= 0xFFFFu) {
            utf8_buf[0] = static_cast<vm_t>(0xE0u | (codepoint >> 12));
            utf8_buf[1] = static_cast<vm_t>(0x80u | ((codepoint >> 6) & 0x3Fu));
            utf8_buf[2] = static_cast<vm_t>(0x80u | (codepoint & 0x3Fu));
            count = 2;
        } else {
            utf8_buf[0] = static_cast<vm_t>(0xF0u | (codepoint >> 18));
            utf8_buf[1] = static_cast<vm_t>(0x80u | ((codepoint >> 12) & 0x3Fu));
            utf8_buf[2] = static_cast<vm_t>(0x80u | ((codepoint >> 6) & 0x3Fu));
            utf8_buf[3] = static_cast<vm_t>(0x80u | (codepoint & 0x3Fu));
            count = 3;
        }
        return Object::make_integer(count);
    }
}

// Process a backslash escape sequence inside a (string).  Called after the '\'
// has been consumed; reads the escape character and any following payload.
// Returns an EscapeResult with fields: obj, string_ptr, ch, use_ch, valid, interp_overflow.
// On success: result.obj is a null Object; ch holds the replacement byte (use_ch=true),
//   or use_ch=false for line continuations (\CR, \LF) and multi-byte outputs (\u/\U, \{})
//   that advance result.string_ptr directly.
// On syntax/format error: result.obj is an error-string Object (caller must vm_end_alloc).
// On \u/\U overflow: valid=false, use_ch=false; result.obj is null (caller loop continues).
// On \{} overflow: valid=false, use_ch=false, interp_overflow=true; result.obj is null
//   (caller loop continues to consume until ')').
// On \{} undefined/syntax: calls vm_end_alloc then trx->error() directly ([[noreturn]]).

// scan_interpolation_escape_impl return codes: >= 0 = bytes written to string buffer;
// negatives signal errors, with the returned Object carrying the error text.
static constexpr int InterpSyntax{-1};
static constexpr int InterpUndefined{-2};
static constexpr int InterpOverflow{-3};

struct EscapeResult {
    Object obj{Object::make_null()};  // null on success; error-string Object on failure
    vm_t *string_ptr{nullptr};
    int ch{0};
    bool use_ch{false};
    bool valid{false};
    bool interp_overflow{false};
};

[[nodiscard]] EscapeResult scan_text_escape(Trix *trx, vm_t *string_ptr, vm_t *string_limit) {
    EscapeResult result;
    auto ch = getc(trx);
    auto use_ch = true;
    auto valid = true;
    auto interp_overflow = false;
    // Single-letter escapes: \a \b \e \f \n \r \t \v
    auto unesc = Trix::unescape_letter(static_cast<vm_t>(ch));
    if (unesc.has_value()) {
        ch = unesc.value();
    } else {
        switch (ch) {
        case EOFc:
            result.obj = Object::make_error_string(trx, "EndOfStream encountered within a (string)");
            break;

        case '^': {
            auto caret_ch = peekc(trx);
            if (caret_ch == EOFc) {
                result.obj = Object::make_error_string(trx, "EndOfStream encountered within a (string)");
            } else if ((caret_ch >= '?') && (caret_ch <= '_')) {
                consume(trx);
                ch = (caret_ch == '?') ? ASCII_DEL : (caret_ch - '@');
            } else {
                result.obj = Object::make_error_string(trx, "invalid \\^ escape sequence encountered within a (string)");
            }
            break;
        }

        case '(':
        case ')':
        case '\\':
            // ch is "(", ")", or "\"
            break;

        case ASCII_CR:
            // \CR (or \CR+LF) is a line continuation: suppress output but correct
            // the source line tracking that getc did for the CR itself.
            fixup_cr(trx);
            use_ch = false;
            break;

        case ASCII_LF:
            // ignore \LF
            use_ch = false;
            break;

        case '0':
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7': {
            ch -= '0';
            auto octal_ch = peekc(trx);
            if (octal_ch == EOFc) {
                result.obj = Object::make_error_string(trx, "EndOfStream encountered within a (string)");
            } else if (Trix::is_octaldigit(octal_ch)) {
                consume(trx);
                ch = (ch * 8) + (octal_ch - '0');
                octal_ch = peekc(trx);
                if (octal_ch == EOFc) {
                    result.obj = Object::make_error_string(trx, "EndOfStream encountered within a (string)");
                } else if (Trix::is_octaldigit(octal_ch)) {
                    consume(trx);
                    ch = (ch * 8) + (octal_ch - '0');
                }
            }
            break;
        }

        case 'x': {
            auto hex_ch = peekc(trx);
            if (hex_ch == EOFc) {
                result.obj = Object::make_error_string(trx, "EndOfStream encountered within a (string)");
            } else if (Trix::is_hexdigit(hex_ch)) {
                consume(trx);
                ch = Trix::hexdigit_to_value(hex_ch);
                hex_ch = peekc(trx);
                if (hex_ch == EOFc) {
                    result.obj = Object::make_error_string(trx, "EndOfStream encountered within a (string)");
                } else if (Trix::is_hexdigit(hex_ch)) {
                    consume(trx);
                    ch = (ch * 16) + Trix::hexdigit_to_value(hex_ch);
                }
            } else {
                result.obj = Object::make_error_string(trx, "invalid \\x sequence encountered within a (string)");
            }
            break;
        }

        case 'u':    // \uXXXX -- exactly 4 hex digits, BMP Unicode -> UTF-8
        case 'U': {  // \UXXXXXXXX -- exactly 8 hex digits, full Unicode -> UTF-8
            vm_t utf8_buf[4];
            auto digit_count = (ch == 'u') ? 4 : 8;
            auto unicode_obj = scan_unicode_escape(trx, utf8_buf, digit_count);
            if (unicode_obj.is_integer()) {
                auto nbytes = unicode_obj.integer_value();
                if ((string_ptr + nbytes) < string_limit) {
                    std::memcpy(string_ptr, utf8_buf, static_cast<std::size_t>(nbytes));
                    string_ptr += nbytes;
                    ch = utf8_buf[nbytes];
                } else {
                    valid = false;
                    use_ch = false;
                }
            } else {
                result.obj = unicode_obj;
            }
            break;
        }

        case '{': {
            auto [output_count, err_obj] = scan_interpolation_escape_impl(this, trx, string_ptr, string_limit);
            if (output_count < 0) {
                if (output_count == Stream::InterpOverflow) {
                    valid = false;
                    use_ch = false;
                    interp_overflow = true;
                } else {
                    trx->vm_end_alloc();
                    if (output_count == Stream::InterpUndefined) {
                        trx->error(Error::Undefined, err_obj);
                    } else {
                        trx->error(Error::SyntaxError, err_obj);
                    }
                }
            } else {
                string_ptr += output_count;
                use_ch = false;
            }
            break;
        }

        default:
            result.obj = Object::make_error_string(trx, "invalid \\ escape sequence encountered within a (string)");
            break;
        }
    }

    result.string_ptr = string_ptr;
    result.ch = ch;
    result.use_ch = use_ch;
    result.valid = valid;
    result.interp_overflow = interp_overflow;
    return result;
}

// Scans a literal (string) from the current stream position up to the matching ')'.
// The opening '(' has already been consumed by scan_delimiter.
// Handles:
//   - nested (...) pairs -- balanced, included in output
//   - escape sequences: \a \b \e \f \n \r \t \v \\ \( \) \CR \LF \NNN \xH[H] \uXXXX \UXXXXXXXX \^X \{name}
//   - CRLF normalization: a bare CR in string body is emitted as LF
// Returns: {SyntaxError, errstr} on EOF or invalid escape sequence.
// Raises: LimitCheck if the string exceeds MaxStringLength; VMFull if VM heap is full.
// On success, delegates to scan_string_suffix() for optional #b/#=/#l/#x/#r/#w suffixes.
[[nodiscard]] ScanToken scan_text_string(Trix *trx) {
    auto [string_base, vm_remaining] = trx->vm_start_alloc<vm_t>();
    // Wrap-guard: see scan_base16_string.  vm_remaining == 0 would wrap
    // (vm_remaining - 1) to UINT_MAX and let std::min pick MaxStringLength.
    assert(vm_remaining >= 1);

    auto string_ptr = string_base;
    // Reserve 1 byte for the nul terminator written by scan_string_suffix
    auto string_limit = (string_base + std::min(vm_remaining - 1, static_cast<vm_size_t>(MaxStringLength)));
    auto string_nesting = int{0};
    auto valid = true;
    auto interp_overflow = false;

    while (true) {
        // fast path: copy plain bytes directly from the read buffer to the string buffer.
        // Plain = not ')' (end/nesting), not '(' (nesting), not '\\' (escape), not ASCII_CR (CRLF).
        // LF is plain here.  Per-byte ++m_line_pos is batched into one add at inner-loop exit.
        {
            auto rptr = trx->offset_to_ptr<vm_t>(m_rptr_offset);
            auto rlimit = trx->offset_to_ptr<vm_t>(m_rlimit_offset);
            auto line_start = rptr;
            while ((rptr != rlimit) && (string_ptr < string_limit)) {
                auto ch = *rptr;
                if (Trix::is_string_stop(ch)) {
                    break;
                } else {
                    *string_ptr++ = ch;
                    ++rptr;
                    if (ch == ASCII_LF) {
                        ++m_line_number;
                        m_line_pos = 1;
                        line_start = rptr;
                    }
                }
            }
            m_line_pos = static_cast<decltype(m_line_pos)>(m_line_pos + (rptr - line_start));
            m_rptr_offset = trx->ptr_to_offset(rptr);
        }

        // slow path: handle one special byte (or buffer refill).
        auto use_ch = true;
        auto ch = getc(trx);
        if (ch == EOFc) {
            trx->vm_end_alloc();
            return syntaxerror_pair(Object::make_error_string(trx, "EndOfStream encountered within a (string)"));
        } else if (ch == ')') {
            if (string_nesting == 0) {
                break;
            } else {
                --string_nesting;
            }
        } else if (ch == '\\') {
            auto result = scan_text_escape(trx, string_ptr, string_limit);
            if (!result.obj.is_null()) {
                trx->vm_end_alloc();
                return syntaxerror_pair(result.obj);
            } else {
                string_ptr = result.string_ptr;
                ch = result.ch;
                use_ch = result.use_ch;
                // valid / interp_overflow are cumulative across the whole string scan --
                // a non-overflowing escape must not clear a previously-set overflow.
                valid = valid && result.valid;
                interp_overflow = interp_overflow || result.interp_overflow;
            }
        } else if (ch == '(') {
            // increase nesting depth and use the "("
            ++string_nesting;
        } else if (ch == ASCII_CR) {
            // CR (or CR+LF) is a single line break: normalize to LF in output and
            // correct the line tracking that getc did for the CR itself.
            fixup_cr(trx);
            ch = ASCII_LF;
        }

        if (use_ch) {
            if (string_ptr < string_limit) {
                *string_ptr++ = static_cast<vm_t>(ch);
            } else {
                valid = false;
            }
        }
    }

    if (valid) {
        auto length = static_cast<length_t>(string_ptr - string_base);
        return scan_string_suffix(trx, length);
    } else {
        trx->vm_end_alloc();
        if (vm_remaining >= MaxStringLength) {
            if (interp_overflow) {
                trx->error(Error::LimitCheck, "(string) length exceeds maximum {} (during \\{{}} interpolation)", MaxStringLength);
            } else {
                trx->error(Error::LimitCheck, "(string) length exceeds maximum {}", MaxStringLength);
            }
        } else if (interp_overflow) {
            trx->error(Error::VMFull, "while scanning a (string) (during \\{{}} interpolation)");
        } else {
            trx->error(Error::VMFull, "while scanning a (string)");
        }
    }
}
