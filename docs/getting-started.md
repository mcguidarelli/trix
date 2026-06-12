<!--
   ______    _
  /_  __/___(_)_  __
   / / / __/ /\ \/ /       Stack-Based Interpreter & VM
  / / / / / /  > · <      C++23 · Single-Header Library
 /_/ /_/ /_/  /_/\_\     Copyright 2026 Mark Guidarelli

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    https://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# Getting Started: A Tour of Trix

Trix is a small stack-based language: you push values onto a stack, and
operators consume them.  If you have used a calculator in "RPN" mode -- or
written Forth or PostScript -- the shape is familiar.  This tour starts from
zero and walks through enough of the language to read and write real programs.
No prior stack-language experience is assumed.

> Two conventions used throughout: `=` **prints** the top of the stack (and
> removes it), and `(why) condition assert` **checks** a fact -- it raises an
> error if the condition is false.  The examples below assert their own results,
> which is exactly how these docs stay correct.  Reals print without a trailing
> `.0` (so `10.0` shows as `10`).

---

## 1. Install and run

Build the interpreter (see [BUILDING.md](../BUILDING.md) for options), then run
it a few different ways:

```console
$ ./build.sh                       # builds ./trix  (see BUILDING.md)
$ ./trix                           # start the interactive REPL
$ ./trix program.trx               # run a program file
$ echo '2 3 add =' | ./trix --stdin
5
```

With no arguments `trix` starts an interactive read-eval-print loop; pass a
filename to run a program, or `--stdin` to pipe one in.  The full command-line
reference -- every flag and startup mode -- is in [Running Trix](cli.md).

---

## 2. Values and the stack

There is one operand stack.  You push values; operators take their arguments
from the top.  Arithmetic reads left to right, with the operator last:

```trix
2 3 add =        % prints 5
```

`2` and `3` are pushed, then `add` pops both and pushes `5`, then `=` prints it.
Numbers, strings, booleans, and names are all values:

```trix
(2 + 3 is 5)                    2 3 add 5 eq assert
(the string "hi" has length 2)  (hi) length 2 eq assert
```

A string is written in parentheses: `(hello)`.  Booleans are `true` and
`false`.  `eq` compares two values and pushes a boolean; comparisons like `lt`,
`gt`, `le`, `ge` do the same.

---

## 3. Names and procedures

`def` binds a value to a name.  Write `/name` (with a leading slash) to mean the
name *itself*; write it bare to look it up:

```trix
/pi 3 def
(pi was looked up) pi 3 eq assert
```

A procedure is code in braces, `{ ... }`.  It is just a value until something
runs it -- binding it to a name and using that name is the usual way:

```trix
/double { 2 mul } def
(a procedure is reusable code) 21 double 42 eq assert
```

The slash matters: `/double` is the name as data, `double` runs the procedure.

---

## 4. Control flow

`if` takes a boolean and a procedure; `if-else` takes two procedures and runs
one of them:

```trix
(7 is the bigger one) 7 5 gt { (big) } { (small) } if-else (big) eq assert
```

Loops come in a few shapes -- `for` counts, `repeat` repeats a fixed number of
times, and `loop` runs until `exit`:

```trix
(for sums 1..5 onto a starting 0)  0  1 1 5 { add } for  15 eq assert
(repeat adds 1 three times)        0  3 { 1 add } repeat  3 eq assert
(loop counts up, then exits at 5)  0  { dup 5 ge { exit } if 1 add } loop  5 eq assert
```

(`for` takes `init increment limit proc`, so `1 1 5` counts 1, 2, 3, 4, 5.)

Booleans have a short-circuit form, `and?` / `or?`, that takes a **procedure**
on the right and only runs it when needed -- handy when the right-hand test is
expensive or unsafe:

```trix
(both sides true)                            true { 2 3 lt } and? assert
(the right side never runs when the left is false) false { 1 0 div } and? false eq assert
```

The plain `and` / `or` evaluate both sides eagerly.

---

## 5. Collections: arrays and dictionaries

An array is written in square brackets; `get` indexes it (from 0) and `length`
counts it:

```trix
(arrays index from zero) [ 10 20 30 ] 1 get 20 eq assert
```

A dictionary maps keys to values, written `<< /key value ... >>`:

```trix
(look a value up by key) << /x 1 /y 2 >> /y get 2 eq assert
```

`for-all` visits each element, leaving you to combine them -- here it sums an
array onto a starting accumulator of `0`:

```trix
(sum the array) 0 [ 1 2 3 4 ] { add } for-all 10 eq assert
```

Arrays, dictionaries, strings, and sets share `get` / `put` / `length` /
`for-all`; see [Collections](collections.md) for the full set.

---

## 6. Naming the stack

Juggling values with `exch` and `roll` gets hard to read.  `let` names the top
values so you can refer to them by name inside a `let ... end` block:

```trix
(name the top two values) 3 4 [/w /h] let w h mul end 12 eq assert
```

This is the readable alternative to positional stack manipulation, and it
extends to pulling fields out of structured data with `destructure`.  See
[Pattern Matching](pattern-matching.md).

---

## 7. When something goes wrong

Errors are values, not crashes.  Wrap a risky block in `try`, and it hands back
the **name** of the error that fired (or `/no-error` if the block ran clean) --
so your program decides what to do next:

```trix
(division by zero is caught, not fatal) { 1 0 div } try /div-by-zero eq assert
```

For richer recovery -- handler dictionaries keyed by error name, raising your
own errors with `throw`, attaching data with `throw-with` -- see
[Error Handling](error-handling.md) and the [Errors Cheatsheet](errors-cheatsheet.md).

---

## 8. Putting it together

A small program: factorial, defined with a procedure that calls itself.  It uses
everything above -- `def`, `dup`, a comparison, `if-else`, and recursion:

```trix
/factorial {
    dup 1 le                    % is n <= 1?
    { pop 1 }                   % base case: 1
    { dup 1 sub factorial mul } % else n * factorial(n-1)
    if-else
} def

(5 factorial is 120) 5 factorial 120 eq assert
```

Read it on the stack: `5` is `dup`'d, compared with `1`; since `5 > 1` the
else-branch runs `5 1 sub` -> `4`, recurses, and multiplies.  The recursion
unwinds to `120`.

---

## 9. Where to go next

- [Trix Reference](trix-reference.md) -- every operator, with stack effects.
- [Operator Cheatsheet](operator-cheatsheet.md) and
  [Types Cheatsheet](types-cheatsheet.md) -- quick lookups.
- [Scanner Syntax](scanner-syntax.md) -- names, numbers, strings, and literals
  in full.
- [From PostScript](from-postscript.md) -- if you already know PostScript.
- The feature guides: [Records](record.md), [Tagged Values](tagged-values.md),
  [Pattern Matching](pattern-matching.md), [Modules](modules.md),
  [Coroutines](coroutines.md), and more -- all indexed in the
  [documentation index](index.md).
- [examples/](../examples/README.md) -- complete programs to read and run.
