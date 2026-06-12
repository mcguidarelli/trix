#!/usr/bin/env python3
"""C++ house-style checker / auto-fixer for two STYLE.md rules.

See STYLE.md "Braces and Parentheses" and "No C-style casts".  Neither rule is
covered by a mainline clang-tidy check, so this tool enforces them: it both
reports violations (`--check` exits non-zero, for CI) and rewrites them
(`--apply`).  It is text-preserving -- it only inserts parentheses / replaces
the `(void)` token span -- so a fix never reflows unrelated code (run
clang-format afterwards for any line-length wrapping).

Rule 1 -- parentheses.  Parses C++ controlling expressions (if/while/for/switch
conditions, return values, and the ternaries within them) with a real tokenizer
+ precedence-climbing parser, then inserts parentheses at the right operand
boundaries: each operand that contains a logical/comparison/bitwise/arithmetic
operator is wrapped at every nesting level; a return value gets an outer wrap; an
if/while/for/switch condition reuses its existing parens as the outer wrap.  Lone
primaries (identifier, member access, call, named cast, parenthesized atom,
unary) stay bare.  Same-precedence left-assoc chains render FLAT (`a && b && c`
unchanged); mixed precedence wraps (`a + b * c` -> `a + (b * c)`).

Rule 2 -- void casts.  Rewrites the C-style unused-value discard `(void)expr` to
`static_cast<void>(expr)`.  A function-parameter `void` (`f(void)`) is never
followed by an expression token, so it is left alone; `(void *)p` (a different
construct) is left alone.

Conservative by construction: any expression it cannot confidently tokenize or
parse (an ambiguous `<`/`>` that might be a non-cast template, an unexpected
token) is SKIPPED and reported, never mis-transformed.  Both transforms are
idempotent.  Known safe skips on the current tree: a handful of variable
templates (`std::numbers::e_v<real_t>`) and `getopt`-style assignment-in-while
calls, none of which need an edit.

Usage:
  cpp_style.py --check  [files...]   # CI gate: exit 1 if any violation remains
  cpp_style.py --report [files...]   # list sites (default), with per-file counts
  cpp_style.py --diff   [files...]   # before/after for every site
  cpp_style.py --apply  [files...]   # rewrite files in place
  cpp_style.py --void-only | --paren-only   # restrict to one rule
  cpp_style.py --self-test
Default files: src/*.inl + trix.h
"""
import glob
import sys

CAST_KW = {"static_cast", "reinterpret_cast", "const_cast", "dynamic_cast",
           "bit_cast", "std::bit_cast"}

# Binary operator binding power (higher binds tighter). Operators NOT listed
# here (assignment, etc.) are handled separately / not wrap-triggering.
BINOP_BP = {
    "*": 10, "/": 10, "%": 10,
    "+": 9, "-": 9,
    "<<": 8, ">>": 8,
    "<": 6, "<=": 6, ">": 6, ">=": 6, "<=>": 6,
    "==": 5, "!=": 5,
    "&": 4,
    "^": 3,
    "|": 2,
    "&&": 1,
    "||": 0,
}
ASSIGN_OPS = {"=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>="}
PREFIX_UNARY = {"!", "~", "-", "+", "*", "&", "++", "--"}

MULTI_OPS = ["<<=", ">>=", "->*", "<=>", "...", "::", "->", "++", "--",
             "<<", ">>", "<=", ">=", "==", "!=", "&&", "||",
             "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", ".*"]
SINGLE_OPS = set("+-*/%<>=!&|^~?:.(){}[],;")


class Tok:
    __slots__ = ("kind", "text", "start", "end", "line")

    def __init__(self, kind, text, start, end, line):
        self.kind = kind
        self.text = text
        self.start = start
        self.end = end
        self.line = line

    def __repr__(self):
        return f"<{self.kind}:{self.text!r}>"


class LexError(Exception):
    pass


class ParseError(Exception):
    pass


def tokenize(src):
    """Tokenize, skipping whitespace/comments/preprocessor lines. Returns a list
    of significant Tok objects. Raises LexError on unterminated literals."""
    toks = []
    i = 0
    n = len(src)
    line = 1
    at_line_start = True
    while i < n:
        c = src[i]
        if c == "\n":
            line += 1
            i += 1
            at_line_start = True
            continue
        if c in " \t\r\f\v":
            i += 1
            continue
        # preprocessor directive: skip whole logical line (honor \ continuation)
        if at_line_start and c == "#":
            while i < n:
                if src[i] == "\\" and i + 1 < n and src[i + 1] == "\n":
                    line += 1
                    i += 2
                    continue
                if src[i] == "\n":
                    break
                i += 1
            continue
        at_line_start = False
        # comments
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            while i < n and src[i] != "\n":
                i += 1
            continue
        if c == "/" and i + 1 < n and src[i + 1] == "*":
            i += 2
            while i + 1 < n and not (src[i] == "*" and src[i + 1] == "/"):
                if src[i] == "\n":
                    line += 1
                i += 1
            if i + 1 >= n:
                raise LexError("unterminated block comment")
            i += 2
            continue
        start = i
        # raw string  R"delim( ... )delim"
        if c == "R" and i + 1 < n and src[i + 1] == '"':
            j = i + 2
            delim = ""
            while j < n and src[j] != "(":
                delim += src[j]
                j += 1
            close = ")" + delim + '"'
            k = src.find(close, j)
            if k < 0:
                raise LexError("unterminated raw string")
            seg = src[i:k + len(close)]
            line += seg.count("\n")
            toks.append(Tok("str", seg, start, k + len(close), line))
            i = k + len(close)
            continue
        # string / char (with optional prefix u8/u/U/L already consumed as id? handle simple)
        if c == '"' or c == "'":
            q = c
            j = i + 1
            while j < n:
                if src[j] == "\\":
                    j += 2
                    continue
                if src[j] == q:
                    break
                if src[j] == "\n":
                    raise LexError("newline in literal")
                j += 1
            if j >= n:
                raise LexError("unterminated literal")
            j += 1
            # user-defined-literal suffix ("foo"sv, 'x'_tag) -- absorb it
            while j < n and (src[j].isalnum() or src[j] == "_"):
                j += 1
            kind = "str" if q == '"' else "char"
            toks.append(Tok(kind, src[i:j], start, j, line))
            i = j
            continue
        # number
        if c.isdigit() or (c == "." and i + 1 < n and src[i + 1].isdigit()):
            j = i
            if src[j] == "0" and j + 1 < n and src[j + 1] in "xX":
                j += 2
                while j < n and (src[j] in "0123456789abcdefABCDEF'"):
                    j += 1
            elif src[j] == "0" and j + 1 < n and src[j + 1] in "bB":
                j += 2
                while j < n and src[j] in "01'":
                    j += 1
            else:
                while j < n and (src[j].isdigit() or src[j] in ".'eE"):
                    if src[j] in "eE" and j + 1 < n and src[j + 1] in "+-":
                        j += 2
                        continue
                    j += 1
            # suffix
            while j < n and (src[j].isalnum() or src[j] == "_"):
                j += 1
            toks.append(Tok("num", src[i:j], start, j, line))
            i = j
            continue
        # identifier (possibly a string/char prefix like u8"" -- handle by lookahead)
        if c.isalpha() or c == "_":
            j = i
            while j < n and (src[j].isalnum() or src[j] == "_"):
                j += 1
            word = src[i:j]
            # string/char literal prefixes
            if j < n and src[j] in "\"'" and word in ("u8", "u", "U", "L"):
                # let the literal be scanned next pass; emit nothing, restart at j
                i = j
                # fall through to literal handling by continuing loop
                # but we must not lose position: just continue; next iter sees the quote
                continue
            toks.append(Tok("id", word, start, j, line))
            i = j
            continue
        # operators / punctuation
        matched = None
        for op in MULTI_OPS:
            if src.startswith(op, i):
                matched = op
                break
        if matched:
            toks.append(Tok("op", matched, start, i + len(matched), line))
            i += len(matched)
            continue
        if c in SINGLE_OPS:
            toks.append(Tok("op", c, start, i + 1, line))
            i += 1
            continue
        raise LexError(f"unexpected char {c!r} at {i} (line {line})")
    return toks


# ---------------------------------------------------------------------------
# AST. Each node has .start/.end offsets into the ORIGINAL source string and a
# .compound flag: True iff its top-level construct is an un-parenthesized binary
# or ternary operator (i.e. it must be wrapped when used as an operand).
# ---------------------------------------------------------------------------
class Node:
    __slots__ = ("kind", "start", "end", "compound", "kids", "ops")

    def __init__(self, kind, start, end, compound, kids=None, ops=None):
        self.kind = kind
        self.start = start
        self.end = end
        self.compound = compound
        self.kids = kids or []     # child Nodes (operands / sub-expressions)
        self.ops = ops or []       # operator strings between operands (binary)


class Parser:
    def __init__(self, toks, src):
        self.t = toks
        self.src = src
        self.i = 0

    def peek(self):
        return self.t[self.i] if self.i < len(self.t) else None

    def at(self, text):
        p = self.peek()
        return p is not None and p.text == text

    def expect(self, text):
        p = self.peek()
        if p is None or p.text != text:
            raise ParseError(f"expected {text!r} got {p!r}")
        self.i += 1
        return p

    # expression with precedence climbing; min_bp = minimum binding power
    def parse_expr(self, min_bp=-1):
        left = self.parse_unary()
        while True:
            p = self.peek()
            if p is None:
                break
            op = p.text
            # ternary
            if op == "?" and min_bp <= -1:
                self.i += 1
                then = self.parse_expr(-1)
                self.expect(":")
                els = self.parse_expr(-1)
                left = Node("ternary", left.start, els.end, True,
                            kids=[left, then, els])
                continue
            if op in ASSIGN_OPS and min_bp <= -2:
                self.i += 1
                rhs = self.parse_expr(-2)
                left = Node("assign", left.start, rhs.end, False,
                            kids=[left, rhs], ops=[op])
                continue
            if op not in BINOP_BP:
                break
            bp = BINOP_BP[op]
            if bp < min_bp:
                break
            self.i += 1
            right = self.parse_expr(bp + 1)
            # flatten same-precedence-level left-assoc chains
            if left.kind == "binary" and BINOP_BP[left.ops[0]] == bp:
                left.kids.append(right)
                left.ops.append(op)
                left.end = right.end
            else:
                left = Node("binary", left.start, right.end, True,
                            kids=[left, right], ops=[op])
        return left

    def parse_unary(self):
        p = self.peek()
        if p is None:
            raise ParseError("unexpected end of expr")
        if p.text in PREFIX_UNARY:
            self.i += 1
            operand = self.parse_unary()
            return Node("unary", p.start, operand.end, False, kids=[operand])
        if p.kind == "id" and p.text in ("sizeof", "alignof", "noexcept"):
            self.i += 1
            if self.at("("):
                inner = self.parse_paren_group()
                return Node("prim", p.start, inner.end, False, kids=[inner])
            operand = self.parse_unary()
            return Node("unary", p.start, operand.end, False, kids=[operand])
        return self.parse_postfix()

    def parse_paren_group(self):
        # consumes ( expr ) and returns a 'paren' node spanning the parens
        lp = self.expect("(")
        inner = self.parse_expr(-2)
        rp = self.expect(")")
        return Node("paren", lp.start, rp.end, False, kids=[inner])

    def parse_postfix(self):
        node = self.parse_primary()
        while True:
            p = self.peek()
            if p is None:
                break
            if p.text == "(":
                args = self.parse_call_args()
                node = Node("call", node.start, self.t[self.i - 1].end,
                            False, kids=[node] + args)
            elif p.text == "{":
                grp = self.parse_brace()
                node = Node("construct", node.start, grp.end, False,
                            kids=[node] + grp.kids)
            elif p.text == "[":
                self.expect("[")
                idx = self.parse_expr(-2)
                rb = self.expect("]")
                node = Node("index", node.start, rb.end, False, kids=[node, idx])
            elif p.text in (".", "->", ".*", "->*"):
                self.i += 1
                nm = self.peek()
                if nm is None:
                    raise ParseError("expected member name")
                # member name may be qualified / destructor ~Name / template
                self.i += 1
                end = nm.end
                # template method args:  ->vm_remaining<Object>()
                if self.at("<") and self.try_template():
                    end = self.t[self.i - 1].end
                node = Node("member", node.start, end, False, kids=[node])
            elif p.text in ("++", "--"):
                self.i += 1
                node = Node("postfix", node.start, p.end, False, kids=[node])
            else:
                break
        return node

    def parse_call_args(self):
        self.expect("(")
        args = []
        if self.at(")"):
            self.expect(")")
            return args
        while True:
            args.append(self.parse_expr(-2))
            if self.at(","):
                self.i += 1
                continue
            break
        self.expect(")")
        return args

    def parse_primary(self):
        p = self.peek()
        if p is None:
            raise ParseError("unexpected end")
        if p.kind in ("num", "str", "char"):
            self.i += 1
            return Node("prim", p.start, p.end, False)
        if p.text == "(":
            return self.parse_paren_group()
        if p.text == "{":
            return self.parse_brace()
        if p.text == "[":
            return self.parse_lambda()
        if p.text == "::" or p.kind == "id":
            return self.parse_name()
        raise ParseError(f"unexpected token {p!r}")

    def parse_lambda(self):
        # [capture] (params)? mutable? noexcept? (-> type)? { body }  -- atom.
        # Inner returns/conditions are processed independently by find_contexts,
        # so we consume the lambda whole without recursing.
        start = self.peek().start
        self.consume_balanced("[", "]")
        if self.at("("):
            self.consume_balanced("(", ")")
        # skip trailing-return / specifiers until the body brace
        guard = 0
        while not self.at("{"):
            p = self.peek()
            if p is None or p.text in (";", ",", ")"):
                raise ParseError("malformed lambda")
            guard += 1
            if guard > 40:
                raise ParseError("runaway lambda header")
            self.i += 1
        end = self.consume_balanced("{", "}")
        return Node("lambda", start, end, False)

    def consume_balanced(self, open_ch, close_ch):
        """self.peek() is open_ch; consume through the matching close_ch.
        Returns the end offset of the close token."""
        self.expect(open_ch)
        depth = 1
        while depth > 0:
            p = self.peek()
            if p is None:
                raise ParseError(f"unbalanced {open_ch}{close_ch}")
            if p.text == open_ch:
                depth += 1
            elif p.text == close_ch:
                depth -= 1
            self.i += 1
            if depth == 0:
                return p.end
        raise ParseError("unreachable")

    def parse_brace(self):
        lb = self.expect("{")
        kids = []
        if not self.at("}"):
            while True:
                kids.append(self.parse_expr(-2))
                if self.at(","):
                    self.i += 1
                    if self.at("}"):
                        break
                    continue
                break
        rb = self.expect("}")
        return Node("brace", lb.start, rb.end, False, kids=kids)

    def parse_name(self):
        start = self.peek().start
        end = start
        # optional leading :: (global scope)
        if self.at("::"):
            self.i += 1
        # qualified name with optional template args on each segment:
        #   ::? id ( <templ> )? ( :: id ( <templ> )? )*
        first = True
        while True:
            p = self.peek()
            if p is None or p.kind != "id":
                if first:
                    raise ParseError("expected id in name")
                break
            name = p.text
            end = p.end
            self.i += 1
            first = False
            # template args?  Name<...> only when it really looks like one.
            if self.at("<") and self.try_template():
                end = self.t[self.i - 1].end
            # cast keyword: Name<...>(arg) -- recurse into the cast argument
            if name in CAST_KW and self.i >= 1 and self.t[self.i - 1].text == ">" \
                    and self.at("("):
                arg = self.parse_paren_group()
                return Node("cast", start, arg.end, False, kids=[arg])
            if self.at("::"):
                self.i += 1
                continue
            break
        return Node("prim", start, end, False)

    def try_template(self):
        """self.peek() is '<'. Try to consume a balanced template-argument list.
        Returns True (and advances) only if it scans cleanly AND is followed by
        '(', '::' or '{' -- the contexts where a template name is used. Otherwise
        leaves self.i unchanged and returns False (so '<' stays a comparison).
        A false negative just means a missed wrap; never a wrong one."""
        save = self.i
        self.i += 1  # consume '<'
        depth = 1
        steps = 0
        while depth > 0:
            p = self.peek()
            steps += 1
            if p is None or steps > 64:
                self.i = save
                return False
            tx = p.text
            # tokens that cannot appear at top level of a template-arg-list here
            if tx in (";", "{", "}", "&&", "||", "?", "==", "!=", "<=", "<=>"):
                self.i = save
                return False
            if tx == "<":
                depth += 1
            elif tx == ">":
                depth -= 1
            elif tx == ">>":
                if depth >= 2:
                    depth -= 2
                else:
                    self.i = save
                    return False
            elif tx in (">=",):
                self.i = save
                return False
            self.i += 1
        nxt = self.peek()
        if nxt is not None and nxt.text in ("(", "::", "{"):
            return True
        self.i = save
        return False


# ---------------------------------------------------------------------------
# Wrapping: collect (offset, char) insertions. Only '(' and ')' are inserted.
# ---------------------------------------------------------------------------
def collect_wraps(node, inserts):
    """Walk the AST; for each binary/ternary node, wrap any operand child that
    is itself compound. Recurse everywhere. `inserts` is a list of (pos, ch)."""
    if node.kind == "binary":
        for kid in node.kids:
            if kid.compound:
                inserts.append((kid.start, "("))
                inserts.append((kid.end, ")"))
            collect_wraps(kid, inserts)
        return
    if node.kind == "ternary":
        # cond, then, els: wrap each if compound
        for kid in node.kids:
            if kid.compound:
                inserts.append((kid.start, "("))
                inserts.append((kid.end, ")"))
            collect_wraps(kid, inserts)
        return
    # non-compound nodes: just recurse into children
    for kid in node.kids:
        collect_wraps(kid, inserts)


def apply_inserts(src, inserts):
    """inserts: list of (pos, ch). Apply right-to-left. Closing parens at a given
    pos must go before opening parens (so nested wraps nest correctly)."""
    # sort by pos desc; at equal pos, ')' before '(' when inserting right-to-left
    # we insert at pos; to keep correct nesting, process descending pos, and for
    # equal pos, insert ')' first then '(' so final order is '(' ... wait:
    # inserting at same pos repeatedly prepends; last inserted ends up leftmost.
    # We want for a boundary that is both an end-of-X and start-of-Y: the ')' of X
    # then the '(' of Y. Build a dict pos->(opens, closes) and splice.
    opens = {}
    closes = {}
    for pos, ch in inserts:
        if ch == "(":
            opens[pos] = opens.get(pos, 0) + 1
        else:
            closes[pos] = closes.get(pos, 0) + 1
    positions = sorted(set(list(opens) + list(closes)), reverse=True)
    out = src
    for pos in positions:
        # at a position, closing parens (ending a span here) come BEFORE opening
        # parens (starting a span here): "...X)" then "(Y..."
        text = ")" * closes.get(pos, 0) + "(" * opens.get(pos, 0)
        out = out[:pos] + text + out[pos:]
    return out


# ---------------------------------------------------------------------------
# Statement-context extraction over the whole-file token stream.
# ---------------------------------------------------------------------------
KEYWORDS_COND = {"if", "while", "switch"}


def find_contexts(toks):
    """Yield (kind, expr_start, expr_end, outer_wrap) tuples.
    kind in {cond, return, for}. expr_start/end are offsets of the expression
    text. outer_wrap True means add an outer paren around the whole expression."""
    ctxs = []
    n = len(toks)
    i = 0
    while i < n:
        t = toks[i]
        if t.kind == "id" and t.text in KEYWORDS_COND and i + 1 < n and toks[i + 1].text == "(":
            # condition spans the matching parens
            j = i + 1
            depth = 0
            while j < n:
                if toks[j].text == "(":
                    depth += 1
                elif toks[j].text == ")":
                    depth -= 1
                    if depth == 0:
                        break
                j += 1
            # expr is between toks[i+2] .. toks[j-1]; but an if/switch-init
            # statement (if (auto x = ...; cond)) puts the real condition after
            # the last top-level ';' -- skip past the init-statement.
            cstart = i + 2
            d = 0
            for kk in range(i + 2, j):
                tx = toks[kk].text
                if tx in ("(", "[", "{"):
                    d += 1
                elif tx in (")", "]", "}"):
                    d -= 1
                elif tx == ";" and d == 0:
                    cstart = kk + 1
            if j < n and j - 1 >= cstart:
                ctxs.append(("cond", cstart, j - 1, False))
            i = j + 1
            continue
        if t.kind == "id" and t.text == "return":
            # expr until ';' at depth 0
            j = i + 1
            if j < n and toks[j].text == ";":
                i = j + 1
                continue
            depth = 0
            k = j
            while k < n:
                tx = toks[k].text
                if tx in ("(", "[", "{"):
                    depth += 1
                elif tx in (")", "]", "}"):
                    depth -= 1
                elif tx == ";" and depth == 0:
                    break
                k += 1
            if k < n and k - 1 >= j:
                ctxs.append(("return", j, k - 1, True))
            i = k + 1
            continue
        if t.kind == "id" and t.text == "for" and i + 1 < n and toks[i + 1].text == "(":
            # for ( init ; COND ; incr ) -- process COND clause only
            j = i + 1
            depth = 0
            start = j + 1
            semis = []
            close = -1
            while j < n:
                tx = toks[j].text
                if tx == "(":
                    depth += 1
                elif tx == ")":
                    depth -= 1
                    if depth == 0:
                        close = j
                        break
                elif tx == ";" and depth == 1:
                    semis.append(j)
                j += 1
            if len(semis) == 2:
                cs, ce = semis[0] + 1, semis[1] - 1
                if ce >= cs:
                    ctxs.append(("cond", cs, ce, False))
            i = (close + 1) if close >= 0 else (i + 1)
            continue
        i += 1
    return ctxs


def apply_region_edits(src, edits):
    """edits: list of (start, end, replacement). Non-overlapping. Applied
    right-to-left so offsets stay valid."""
    out = src
    for start, end, repl in sorted(edits, key=lambda e: (-e[0], -e[1])):
        out = out[:start] + repl + out[end:]
    return out


def line_of(src, pos):
    return src.count("\n", 0, pos) + 1


def find_void_casts(toks):
    """Find C-style void-discard casts `(void) operand`.  Returns a list of
    (cast_start, cast_end, operand_end) offsets, where [cast_start,cast_end) is
    the `(void)` text and operand_end is just past the discarded expression.
    A function-parameter `void` (`f(void)`) is never followed by an expression
    token, so the followed-by-expression test alone distinguishes a cast."""
    res = []
    n = len(toks)
    NON_OPERAND = {";", ",", ")", "]", "}", "{", "=", "->", ":", "const",
                   "noexcept", "&&", "||", ">", "<", "*"}
    i = 0
    while i < n - 3:
        if (toks[i].text == "(" and toks[i + 1].kind == "id"
                and toks[i + 1].text == "void" and toks[i + 2].text == ")"):
            nxt = toks[i + 3]
            if nxt.text in NON_OPERAND:
                i += 1
                continue
            # operand runs to the first ';' / ',' / unbalanced close at depth 0
            depth = 0
            k = i + 3
            opend = None
            while k < n:
                tx = toks[k].text
                if tx in ("(", "[", "{"):
                    depth += 1
                elif tx in (")", "]", "}"):
                    if depth == 0:
                        opend = toks[k - 1].end
                        break
                    depth -= 1
                elif tx in (";", ",") and depth == 0:
                    opend = toks[k - 1].end
                    break
                k += 1
            if opend is not None:
                res.append((toks[i].start, toks[i + 2].end, opend))
                i = k
                continue
        i += 1
    return res


def compute_void(src):
    """Rewrite `(void)x` -> `static_cast<void>(x)`. Returns (new_src, records)
    where records = list of (line, before_line, after_line)."""
    try:
        toks = tokenize(src)
    except LexError:
        return src, []
    casts = find_void_casts(toks)
    edits = []
    records = []
    for cast_start, cast_end, opend in casts:
        edits.append((cast_start, cast_end, "static_cast<void>("))
        edits.append((opend, opend, ")"))
        ln = line_of(src, cast_start)
        ls = src.rfind("\n", 0, cast_start) + 1
        le = src.find("\n", opend)
        before = src[ls:le if le >= 0 else len(src)]
        after = (before[:cast_start - ls] + "static_cast<void>("
                 + before[cast_end - ls:opend - ls] + ")" + before[opend - ls:])
        records.append((ln, before, after))
    return apply_region_edits(src, edits), records


def compute_paren(src):
    """Rewrite under-parenthesized conditions/returns. Returns
    (new_src, edit_records, skip_records)."""
    try:
        toks = tokenize(src)
    except LexError as e:
        print(f"  !! LEX: {e}", file=sys.stderr)
        return src, [], []
    ctxs = find_contexts(toks)
    edits = []
    skips = []
    all_inserts = []
    for kind, ts, te, outer in ctxs:
        sub = toks[ts:te + 1]
        if not sub:
            continue
        expr_start = sub[0].start
        expr_end = sub[-1].end
        original = src[expr_start:expr_end]
        try:
            p = Parser(sub, src)
            node = p.parse_expr(-2)
            if p.i != len(sub):
                raise ParseError(f"trailing tokens at {sub[p.i]!r}")
        except ParseError as e:
            skips.append((sub[0].line, original, str(e)))
            continue
        inserts = []
        if outer and node.compound:
            inserts.append((node.start, "("))
            inserts.append((node.end, ")"))
        collect_wraps(node, inserts)
        if not inserts:
            continue
        new_expr = apply_inserts(original, [(pos - expr_start, ch) for pos, ch in inserts])
        if new_expr == original:
            continue
        edits.append((sub[0].line, original, new_expr))
        all_inserts.extend(inserts)
    new_src = apply_inserts(src, all_inserts) if all_inserts else src
    return new_src, edits, skips


def run(files, mode, rules=("void", "paren")):
    paren_edits = 0
    void_edits = 0
    skip_count = 0
    file_counts = {}
    check_fail = False
    for path in files:
        src0 = open(path).read()
        # rule 1: void-cast (apply first; disjoint from paren regions)
        if "void" in rules:
            src1, void_recs = compute_void(src0)
        else:
            src1, void_recs = src0, []
        # rule 2: parenthesization (computed on the void-fixed text)
        if "paren" in rules:
            final, p_edits, p_skips = compute_paren(src1)
        else:
            final, p_edits, p_skips = src1, [], []
        paren_edits += len(p_edits)
        void_edits += len(void_recs)
        skip_count += len(p_skips)
        if p_edits or void_recs:
            file_counts[path] = len(p_edits) + len(void_recs)
            check_fail = True
        if mode in ("report", "diff", "check"):
            for ln, before, after in void_recs:
                if mode == "diff":
                    print(f"{path}:{ln}  [void]")
                    print(f"  - {before.strip()}")
                    print(f"  + {after.strip()}")
                else:
                    print(f"{path}:{ln}  [void]  {before.strip()[:64]}")
            for ln, old, new in p_edits:
                if mode == "diff":
                    print(f"{path}:{ln}")
                    print(f"  - {old.strip()}")
                    print(f"  + {new.strip()}")
                else:
                    print(f"{path}:{ln}  {old.strip()[:70]}")
            for ln, old, why in p_skips:
                print(f"  ?? SKIP {path}:{ln}  {old.strip()[:60]}  ({why})", file=sys.stderr)
        if mode == "apply" and final != src0:
            open(path, "w").write(final)
    print(f"\n=== {paren_edits} paren + {void_edits} void edits across "
          f"{len(file_counts)} files; {skip_count} skipped (unparsed) ===")
    if mode == "report" and file_counts:
        for p in sorted(file_counts, key=lambda k: -file_counts[k]):
            print(f"  {file_counts[p]:4d}  {p}")
    if mode == "check":
        return 1 if check_fail else 0
    return 0


SELF_TESTS = [
    # (context, input_expr_statement, expected_output_statement)
    ("if", "if (entry->m_target == target_offset && (entry->m_ref_id & MonitorTypeMask) != 0) {",
     "if ((entry->m_target == target_offset) && ((entry->m_ref_id & MonitorTypeMask) != 0)) {"),
    ("ret", "return (m_aat & Mask) != 0;",
     "return ((m_aat & Mask) != 0);"),
    ("if", "if (count > available || x == y) {",
     "if ((count > available) || (x == y)) {"),
    ("if", "if (a && b && c) {",
     "if (a && b && c) {"),
    ("if", "if (a == b && c) {",
     "if ((a == b) && c) {"),
    ("ret", "return a + b * c;",
     "return (a + (b * c));"),
    ("if", "if (is_address()) {",
     "if (is_address()) {"),
    ("if", "if (s->m_raw_mode) {",
     "if (s->m_raw_mode) {"),
    ("ret", "return static_cast<vm_offset_t>(a - b);",
     "return static_cast<vm_offset_t>(a - b);"),
    ("if", "if ((block_size != 0) && ((block_size & (GvmBlockAlignment - 1)) == 0)) {",
     "if ((block_size != 0) && ((block_size & (GvmBlockAlignment - 1)) == 0)) {"),
    ("if", "if (val > std::numeric_limits<uint64_t>::max() / 1024) {",
     "if (val > (std::numeric_limits<uint64_t>::max() / 1024)) {"),
    ("ret", "return std::pair{grow_count, static_cast<vm_size_t>(grow_count * size)};",
     "return std::pair{grow_count, static_cast<vm_size_t>(grow_count * size)};"),
    ("ret", "return gvm_alloc<BindingEntry>(size, kind);",
     "return gvm_alloc<BindingEntry>(size, kind);"),
    ("if", "if (::realpath(argv[0], buf) != nullptr) {",
     "if (::realpath(argv[0], buf) != nullptr) {"),
    ("ret", "return floating_point_test(trx, [](auto v) { return a + b * c; });",
     "return floating_point_test(trx, [](auto v) { return a + b * c; });"),
    ("ret", "return (uint128_t{hi} << 64) | uint128_t{lo};",
     "return ((uint128_t{hi} << 64) | uint128_t{lo});"),
    ("ret", "return m_curr_alloc_global ? gvm_alloc<T>(size, kind) : vm_alloc<T>(size, kind);",
     "return (m_curr_alloc_global ? gvm_alloc<T>(size, kind) : vm_alloc<T>(size, kind));"),
]

VOID_TESTS = [
    ("    (void)str_ptr;", "    static_cast<void>(str_ptr);"),
    ("    (void)gc_work_pop();", "    static_cast<void>(gc_work_pop());"),
    ("    (void)skip_line_comment(trx);", "    static_cast<void>(skip_line_comment(trx));"),
    # not a cast -- function declaration parameter list: must NOT change
    ("void f(void) { return; }", "void f(void) { return; }"),
    # void-pointer cast is a different construct: must NOT change
    ("    p = (void *)q;", "    p = (void *)q;"),
    # already converted: idempotent
    ("    static_cast<void>(x);", "    static_cast<void>(x);"),
]


def self_test():
    ok = True
    for inp, exp in VOID_TESTS:
        got, _ = compute_void(inp)
        mark = "ok  " if got == exp else "FAIL"
        if got != exp:
            ok = False
        print(f"{mark} [void] {inp.strip()}")
        if got != exp:
            print(f"      got: {got}")
            print(f"      exp: {exp}")
    for ctx, inp, exp in SELF_TESTS:
        toks = tokenize(inp)
        ctxs = find_contexts(toks)
        if not ctxs:
            print(f"FAIL (no ctx): {inp}")
            ok = False
            continue
        kind, ts, te, outer = ctxs[0]
        sub = toks[ts:te + 1]
        es, ee = sub[0].start, sub[-1].end
        original = inp[es:ee]
        p = Parser(sub, inp)
        node = p.parse_expr(-2)
        inserts = []
        if outer and node.compound:
            inserts.append((node.start, "("))
            inserts.append((node.end, ")"))
        collect_wraps(node, inserts)
        new_expr = apply_inserts(original, [(x - es, ch) for x, ch in inserts])
        got = inp[:es] + new_expr + inp[ee:]
        mark = "ok  " if got == exp else "FAIL"
        if got != exp:
            ok = False
        print(f"{mark} {inp}")
        if got != exp:
            print(f"      got: {got}")
            print(f"      exp: {exp}")
    print("\nALL PASS" if ok else "\nSOME FAILED")
    return 0 if ok else 1


def main(argv):
    mode = "report"
    rules = ("void", "paren")
    files = []
    for a in argv[1:]:
        if a in ("--report", "--diff", "--apply", "--check"):
            mode = a[2:]
        elif a == "--void-only":
            rules = ("void",)
        elif a == "--paren-only":
            rules = ("paren",)
        elif a == "--self-test":
            return self_test()
        else:
            files.append(a)
    if not files:
        files = sorted(glob.glob("src/*.inl")) + ["trix.h"]
    return run(files, mode, rules)


if __name__ == "__main__":
    sys.exit(main(sys.argv) or 0)
