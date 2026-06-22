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

# Adding a New Operator to Trix

Step-by-step checklist for adding a new operator.  Follow this order —
each step depends on the previous ones compiling.

## Example: adding a `string-reverse` operator

### Step 1: Add the SystemName enum value

**File:** `src/enums.inl`

Add an entry to `enum struct SystemName`.  Place it in the appropriate
alphabetical/categorical section.

```cpp
StringReverse,       // string-reverse
```

Convention: PascalCase, matching the operator name with hyphens removed.

### Step 2: Implement the operator function

**File:** `src/ops_*.inl` (choose the appropriate category file)

For `string-reverse`, this goes in `src/ops_string.inl`:

```cpp
// string-reverse: str :- str
// Reverses the bytes of a read-write string in place.
// throws: opstack-underflow, type-check, read-only
static void string_reverse_op(Trix *trx) {
    trx->verify_operands(VerifyRWString);

    auto *str = trx->m_op_ptr;
    auto ptr = str->string_vptr(trx);
    auto length = str->string_length();
    std::reverse(ptr, ptr + length);
}
```

Conventions:
- Function name: `snake_case_op` matching the operator name.
- Stack effect comment: `// name: input :- output`
- List all throwable errors in `// throws:` comment.  A `VerifyRW*`
  mask (`VerifyRWString`/`VerifyRWArray`/`VerifyRWStream`/`VerifyRWDict`/
  `VerifyRWSet`) adds the `/read-only` raiser (`Error::ReadOnly`, via
  `verify_rw_access` in `src/verify.inl`) on a ReadOnly operand, distinct
  from the `type-check` raised on a plain type mismatch.
- Use `verify_operands()` for type checking (see verify.inl).
- Use `trx->error()` for runtime errors (it is `[[noreturn]]`).

### Step 3: Add the dispatch entry

**File:** `src/dispatch.inl`

Add a row to the `sysoperator_rows()` list, in the appropriate
section (alphabetical within category):

```cpp
{SystemName::StringReverse, string_reverse_op, "string-reverse"sv},
```

The last field is the operator's display name (used by `=`, `==`,
backtrace, and `:status:`).  The dispatch table is built from the row
list at compile time; a missing, duplicated, or out-of-range row fails
the build (`verify_dispatch_tables()` static_assert in `trix.h`).

### Step 4: Systemdict registration (automatic)

**File:** `src/dict.inl`, inside `Dict::init_systemdict()` — no edit needed.

Standard operators are registered in systemdict automatically.
`init_systemdict()` loops over the whole `FIRST_STD_OP`..`LAST_STD_OP` range
of the SystemName enum and binds each entry to a Name and an Operator object:

```cpp
for (auto i = +SystemName::FIRST_STD_OP; i <= +SystemName::LAST_STD_OP; ++i) {
    auto sys_name = static_cast<SystemName>(i);
    systemdict->put(trx, Name::make_system(trx, sys_name), Object::make_operator(sys_name));
}
```

So once Step 1 placed `StringReverse` in the standard-operator section of the
enum (inside that range), it is bound and inserted with no further action.
Control operators (`@`-prefixed) live in the separate `FIRST_CONTROL_OP`
range, outside the loop, and are deliberately left unregistered — internal,
not user-visible.

### Step 5: Update the reference documentation

**File:** `docs/trix-reference.md`

Add the operator to the appropriate section with its stack effect,
description, and error list.  Follow the existing format.

### Step 6: Add tests

**File:** `tests/test_*.trx` (appropriate test file)

Add test cases covering:
- Happy path (normal operation)
- Edge cases (empty input, boundary values)
- Error cases (wrong type, wrong access mode)
- Interaction with save/restore if the operator mutates state

Use the `rt-ok` assertion pattern:

```trix
(hello) dup string-reverse (olleh) eq (string-reverse: basic) rt-ok
```

### Step 7: Format, build, test

```bash
clang-format -i --style=file src/ops_string.inl
./build.sh
./runtests.sh
```

## Checklist summary

| Step | File                     | Action                              |
| ---- | ------------------------ | ----------------------------------- |
| 1    | `src/enums.inl`          | Add `SystemName::YourOp` enum value |
| 2    | `src/ops_*.inl`          | Implement `your_op(Trix *trx)`      |
| 3    | `src/dispatch.inl`       | Add a `SystemName::YourOp` row      |
| 4    | *(none — automatic)*     | Enum range auto-registers it        |
| 5    | `docs/trix-reference.md` | Document stack effect and behavior  |
| 6    | `tests/test_*.trx`       | Add test cases                      |
| 7    | —                        | clang-format, build, test           |

## Special cases

### Operators that call user procs (trampoline)

If your operator needs to execute a user-provided proc and act on the
result, you must use the trampoline pattern (see interpreter.inl comment
block).  This requires:

- A control operator (`@your-op`, with `at` prefix in SystemName).
- The control operator goes in the same ops_*.inl file.
- Add it to dispatch.inl like any operator, but place its SystemName in the
  control-operator range (`FIRST_CONTROL_OP`.., before `FIRST_STD_OP`) so the
  systemdict auto-registration loop skips it — control operators are internal,
  not user-visible.

If the `@`-control atom participates in error unwinding, continuation
capture, or backtraces (it bears an exec-stack companion, opens/closes a
barrier, or carries a frame), it must also add a row to the `op_descriptor`
table (`src/op_descriptor.inl`) -- the single source of truth the
backtrace / unwind / capture walkers consume.  An atom whose `OpKind` /
`UnwindAction` is missing or wrong drifts those walkers apart and reintroduces
the err-scan bug class the table was built to kill.

### Operators that need new verify_t patterns

If your operator accepts a type combination not covered by existing
Verify* constants, add a new constant in `src/verify.inl`:

```cpp
static constexpr verify_t VerifyYourPattern = VerifyTypeA | VerifyTypeB | VerifyConstraint;
```

### Operators that interact with save/restore

If your operator mutates heap-resident data (array elements, dict
entries, etc.), ensure the mutation site calls the appropriate
Save::save_* function before modifying data.  See `save.inl` and
`docs/dev-invariants.md` for the journaling contract.

### Operators that build composite objects (GC-rooting)

If your operator builds a global composite (a curried/composed proc, a lazy
node chain, a record or array spine) across more than one allocation, the
in-flight temporaries must be rooted before the next allocation, or a
mid-build `vm_global_gc` can sweep them.  Use the gc-root stack
(`require_gc_root_capacity(n)` + raw `*++m_gc_roots_ptr = obj` pushes +
`reset_gc_root(n)` at the tail) — see the idiom in `STYLE.md` and the
contract in `docs/dev-invariants.md`.  Verify with a `tests/test_gc_stress_*`
regression under the debug `vm-gc-stress` hook.

### Operators that build a container result (region-aware results)

If your operator returns a new array / set / dict, its result must be
**region-aware**: built inside `${...}` it must land in the global VM region
so it can be stored into a global container and survive `restore`.  Pick one
of the four patterns (build-direct, promote-at-end, `make_clone_local` during
temp-live windows, or suppress-the-global-flag for two-phase collects) from
the decision table in `docs/vm-regions.md`.  Ops that hold a live temp dict /
buffer during the build must also respect the **temp-clobber invariant** (no
global allocation while temp scratch is live) documented there.

### Operators that block (coroutine yield)

Blocking operators (pipe-get, actor-recv, etc.) must use the trampoline:
push a continuation control operator, call `coroutine_flush_running()`,
then `coroutine_schedule()`.  The operator MUST NOT access Trix member
variables after `coroutine_schedule()` returns — a different coroutine
may now be running.  See `ops_coroutine.inl` comment block.
