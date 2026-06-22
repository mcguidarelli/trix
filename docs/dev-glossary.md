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

# Trix Glossary

Terms used throughout the codebase, comments, and documentation.
Alphabetical; cross-references in *italics*.

**aat_t** — Object Attribute Type.  The 1-byte header field encoding an
Object's type (5 bits), access mode (1 bit), special flag (1 bit), and
execute attribute (1 bit).  See `object.inl`.

**barrier (control-op)** — An exec-stack control operator that marks a
recovery point for the error walk: `@try-barrier`, `@try-catch-barrier`,
`@repl-barrier`, `@choice-barrier`, `@aggregate-reduce-barrier`, etc.
When an error unwinds past one, the *descriptor table* maps it to its
fail/recovery handler (e.g. `@aggregate-reduce-barrier` →
`@aggregate-reduce-fail`).  Distinct from the *save barrier* (a heap
watermark) despite the shared word.

**battery** — `./runtests.sh`: every suite in one run with a summary
table and a non-zero exit on any failure.  What CI executes.  See
[dev-testing.md](dev-testing.md).

**binding cache** — A one-word shortcut stored in each Name (`m_binding`)
pointing directly into a `DictEntry::m_value`.  Gives O(1) name lookup
when valid.  Invalidated by save/restore and dict stack changes.

**ChunkKind** — Per-block classification tag in `GvmBlock::m_kind`.
Drives `vm-global-info`'s histogram and the GC walker dispatch table.
Currently 31 kinds (Name, Dict, Set, Array, String, Long, ULong,
Address, Double, Int128, UInt128, Packed, CoroutineStacks,
CoroutineContext, Mailbox, Supervisor, ..., plus the `Free` sentinel
and reserved-for-future slots).

**companion object** — Data pushed onto the exec stack below a control
operator to preserve state across a trampoline.  Examples: loop counters,
saved operands, container references, indices.

**control operator** — An internal operator (prefixed with `@` in display
name, `at` in SystemName) that serves as a continuation in the trampoline
pattern.  Not user-visible; pushed onto the exec stack by standard operators
and fired by the interpreter after a user proc completes.

**coroutine quantum** — The per-coroutine operation budget before the
scheduler forces a yield (`--quantum`, `Config::m_coroutine_quantum`).
0 = unlimited (the default); the main coroutine always runs unlimited.

**deep-eq** — Recursive structural equality comparison that descends into
arrays, records, sets, dicts, and tagged values.  Depth-limited by
`MaxRecursionDepth` (64).  Contrast with `eq` (shallow, compares Object values
directly — except Strings, which compare by content).

**descriptor table** — `src/op_descriptor.inl`: one row per control
operator describing its unwind behavior — kind (Barrier/plain),
companion count, the fail handler an error rewrite installs, and the
backtrace boundary label (`[aggregate boundary]`).  The single source
of truth consumed by the error walk (`try_catch_handler`), the
capture/perform walks (`capture_op`/`perform_op`), and
`format_backtrace`; adding a control op without a row is a build-time
error.

**DictEntry** — A 20-byte record in a Dict's entry pool: `m_next`
(chain link), `m_key` (Object), `m_value` (Object).  Never relocated
after allocation (binding cache depends on stable offsets).

**dict stack** — The lookup chain for executable names: systemdict and
protocoldict at the bottom, userdict above, then any dicts pushed by
`begin` and any *frame dicts* from `|locals|` procs.  Lookup walks top
down; `def` writes to the top non-frame dict (frame dicts are bound by
`local-def`).

**dispatch row** — One entry in the consteval row lists of
`src/dispatch.inl` (`{SystemName::X, fn, "name"sv}`): the
specification from which the three dispatch tables are built at
compile time.  `verify_dispatch_tables()` static-asserts completeness
(every enum index exactly one row) in every build.

**eqref / eq-storage** — The shared per-kind scratch buffers behind the
`#=` lexical suffix (all kinds, incl. proc/set), the
`=string`/`=array`/`=dict` ops, and the `*-from-mark` collectors
(incl. `=set-from-mark`).  One buffer per kind, guarded by a
monotonic *generation counter*: creating the next `#=` value of a kind
invalidates outstanding refs of that kind (stale access raises
`/unsupported`); counters are not rolled back by restore and raise
`/limit-check` on 2^32 wrap.

**error stack** — The fourth machine stack (`m_err_base`, default depth
64): companion records for active error-handling scopes (try-catch
handler dicts, finally cleanups, handler-scope identities), maintained
by the barrier ops and consulted by the error walk.

**exec stack** — The execution stack.  Functions as the program counter:
the interpreter pops one Object per iteration and dispatches it.
Procs push their remaining body back; streams push themselves back
after each token.

**executable** — An Object whose Execute bit is 1.  The interpreter
dispatches it by type: operators call C++, names are looked up, procs
iterate their bodies, etc.

**ExtValue** — An 8-byte extension slot on the VM heap for 64-bit values
(Long, ULong, Double, Address).  Managed by a per-save-level free list
with strict single-owner semantics; *value semantics* from the
program's view (relocated, never restore-fragile).  The 128-bit
counterpart is the *WideValue*.  See `dev-invariants.md`.

**fastbin** — A segregated free list for small global blocks
(`< 64` bytes, i.e. 32-56).  Four bins (32 / 40 / 48 / 56 bytes), exclusive
LIFO match, with a one-bin-up scan before falling through to the
general free list.

**frame dict** — The pooled dict created by a `|a b c|` locals
declaration at proc entry and popped at `@end-locals`.  Lives on the
dict stack like any other dict (so locals shadow outer names);
`local-def` binds into it.  Procs and executable strings stored in one
auto-execute on bare-name lookup.

**global handler** — The last-resort error path: when an error escapes
every barrier, `error()` looks up the error name in `handlersdict`
(default entry: `//default-handler`, which prints and exits with the
error's code).  A custom proc installed there runs on a reset exec
stack above a re-pushed *Quit floor*, so returning normally ends the
run cleanly (exit 0).

**global VM** — The journal-skipped region at the top of the VM
managed by the global allocator and a mark-sweep GC.  Allocations there survive
`save` / `restore` cycles.  Entered via `${...}`, `set-global`, the
per-form `$/foo` / `$\foo` / `<lit>#$` directives, or any runtime
`int -- container` op while `m_curr_alloc_global` is true.

**golden test** — A black-box test whose contract is exact bytes:
`tests/golden/<name>.trx` runs in a fresh interpreter and combined
stdout+stderr must equal the blessed `<name>.expected`.  See
[dev-testing.md](dev-testing.md).

**GvmBlock** — The 16-byte per-block header on every global-VM
allocation: `m_size`, `m_kind`, `m_mark_gen`, `m_obj_count`,
`m_next_in_work`, `m_magic = 0xDEADC0DE`.  Defined in
`src/gvm_heap.inl`.

**heap cell** — Umbrella term for *ExtValue* and *WideValue*: a
VM-heap slot backing a scalar Object, with per-save-level free lists
and sentinel validity.  `Object::uses_heap_cell()` /
`heap_cell_valid()` are the unified predicates.

**immediate lookup** — The scanner prefixes `//name` (push the bound
value as a literal) and `\name` (push executable / run on scan for
operators).  Resolved at *scan time*, not run time — inside a `{...}`
proc body a `//`-resolved Operator element is *executed* when the proc
runs, a classic trap.

**journal** — The linked list of `Save::Entry` records that capture old
values before mutation.  Replayed in reverse by `restore()` to undo
changes.  Also known as the save journal or undo log.

**length_t** — `uint16_t`; the element-count type for strings, arrays,
dicts, sets.  Its 65,535 cap is load-bearing (Object schema layout and
snapshot format) and permanent — do not propose widening it.

**literal** — An Object whose Execute bit (bit 7 of aat_t) is 0.  When
the interpreter encounters a literal on the exec stack, it pushes it to
the operand stack as data.  Contrast with executable.

**lvar** — Logic variable.  A tagged value (`/lvar [null] tag`) used by
the logic/backtracking system.  Binding is a journaled array put;
backtracking (restore) automatically unbinds.

**mark** — A sentinel Object (Type::Mark) pushed onto the operand stack
to delimit variadic arguments.  Used by `[`, `<<`, `{{`, and operators
like `print-fmt`.  `count-to-mark` counts elements above the nearest mark.

**mark generation** — The 1-bit flip-flop `m_gc_current_gen` (0 or 1)
flipped once per GC pass.  Each block stamps its `m_mark_gen` to the
current value when marked; the sweep frees any block whose `m_mark_gen`
does not match.  Flipping the bit makes every old mark stale by
definition, so there is no per-pass clear walk.  Exposed to user code
via the `gc-current-gen` status key.

**Name** — An interned string in the VM's name table (hash buckets +
chains).  Interning happens on first reference, in whichever region
(local/global) the allocation flag selects at that moment; entries
interned above save level 0 are unlinked on restore.  Hot names are
*pre-interned* to pin placement and timing.

**nulloffset** — The sentinel value 0 for vm_offset_t fields, analogous
to nullptr for pointers.  Indicates "no value" or "end of list".

**Object** — The universal 8-byte tagged value; every Trix datum is
one.  Layout: *aat_t* attribute byte, save-level bytes, then a 4-byte
payload that is either an inline value (Integer, Boolean, ...) or a
`vm_offset_t` into the heap (containers, Names, *heap cells*).  Always
passed by value, never by reference.

**op stack** — The operand stack.  Holds data values being operated on.
Push via `*++m_op_ptr = obj;` pop via `auto x = *m_op_ptr--;`.

**OpaqueHandle** — The last defined Object type (type value 30, the 31st
of the 32 slots the 5-bit type field allows — one slot stays free),
claimed as a generic carrier for future object families: the payload kind lives in
a separate `HandleKind` byte (stored in the length slot), so new kinds
(currently Screen) never consume type-field slots or break snapshots.

**packed array** — A compressed representation of an Array where each
element is encoded as a 1-5 byte sequence (PackedType tag + payload)
instead of 8 bytes.  Read-only, sequential-access only.  Default
representation for `{ }` proc bodies.

**persist family** — The `def-persist`, `put-persist`,
`update-persist`, `undef-persist`, `set-add-persist`,
`set-remove-persist`, `array-store-persist` ops.
Mutate without journaling so the change survives `restore`.  Contrast
with `${...}` (allocates in global) and ordinary ops (journals and
rolls back).

**pre-intern** — Forcing a Name into the name table at a chosen moment
(usually startup, save level 0) so later references resolve through a
stable entry: `/name pop` for local, `$/name pop` for global.  The
runtime pre-interns its status/probe keys in `init.inl`; that list
must stay in sync with `status_lookup()`.

**PTY harness** — `tests/run_pty_tests.sh`: scenarios that need a real
controlling terminal (raw-mode reads, REPL recovery), run under
`script` + `stty` with timed input feeders and marker-grep assertions.
See [dev-testing.md](dev-testing.md).

**Quit floor** — The `Quit` operator the startup bootstrap pushes at the
bottom of the exec stack (`[Quit, @run, stream]`): when the program
drains, Quit throws the clean `Exception::Exit`.  Restored by `error()`
before invoking a *global handler* so a returning handler also exits
instead of parking the interpreter.

**restore-fragile** — A local-VM allocation that will be reclaimed by a
pending `restore` and therefore must not be stored anywhere that
outlives the save scope (a global container, a mailbox of a coroutine
that survives).  The engine raises `/invalid-access` or
`/invalid-restore` at the hazard point rather than dangling.  Scalar
*heap cells* are exempt — they are relocated, not invalidated.

**save barrier** — The local-VM heap floor (`m_vm_ptr` snapshot)
established by `save()`.  Allocations made above it are rolled back by
the matching `restore()`; a composite that must outlive the scope has
to be built below it or in the global VM.  Storing a restore-fragile
local Object into a global container trips `/invalid-restore`.

**save level** — A monotonically increasing counter (save_level_t)
incremented by each `save()`.  Every Object carries a save_level
recording when it was created or last modified.  Used by the journal
system to determine what needs rollback on `restore()`.

**save token** — The inline Integer returned by `save`: save level in
the low 8 bits, a generation-XOR-barrier checksum in the next 23.
Stale tokens (slot recycled by a later save/restore cycle) are rejected
with `/invalid-restore`.

**scratch arena** — The per-coroutine auxiliary stack
(`--scratch-depth`, default 128 Objects, max 4096) used by operators
that need bounded intermediate storage outside the operand stack
(sorting, find-all accumulation).  `scratch_push` grows it upward;
exhaustion raises `/limit-check`.

**SetEntry** — A 12-byte record in a Set's entry pool: `m_next`
(chain link), `m_key` (Object).  Like DictEntry but without a value.

**shim** — A small dispatch function for binary operators that handles
one specific type combination (e.g., Integer + Real).  The shim system
provides O(1) type dispatch via function pointer tables.  Operands must
be type-matched (`AllMustMatch`) — there is no implicit promotion.

**sid (stream id)** — A `uint8_t` generation tag carried in every
Stream Object alongside the offset; a recycled stream slot bumps its
sid so stale Stream Objects are detected (closed-stream access) rather
than aliasing the new occupant.  Fixed sids: stdin 1, stdedit 2,
stdout 3, stderr 4, startup file 6.

**stdedit** — The interactive input stream: GNU readline over the
controlling terminal (prompt, history, line editing), distinct from
plain `stdin`.  The REPL is `[Quit, @repl-barrier, @run, stdedit]` on
the exec stack; errors recover via `@repl-recover`, ctrl-D drains to
the *Quit floor*.

**SystemName** — An enum value identifying a built-in operator, type
name, error name, or well-known name.  Indexes into systemdict and the
pre-interned name offset table.  The dispatch tables (see *dispatch
row*) map it to implementation and display name.

**targeted wakeup** — The pattern where a blocking coroutine stores its
context offset in the structure it's waiting on (pipe buffer, mailbox,
joiner field).  The producer/sender/dying coroutine sets it to Ready
directly — O(1), no scheduler scan.  Used by pipelines, actors, and join.

**temp region** — The transient allocation area growing downward from
`m_vm_limit` (watermark `m_vm_temp_ptr`, status key `vm-temp-used`):
short-lived working memory (sort buffers, format scratch) reclaimed
wholesale, checked against the upward-growing local VM on every
allocation.

**thunk** — A deferred computation: 3 Objects `[state, proc, result]` on
the VM heap.  Unevaluated until forced; result is cached after first
evaluation.  Building block for lazy sequences and computed reactive cells.

**trampoline** — The fundamental execution pattern in Trix.  An operator
that needs a user proc result pushes saved state + control operator +
proc onto the exec stack and returns.  The interpreter runs the proc;
the control operator fires after it completes and continues the work.
Avoids deep C++ call stacks.  See `interpreter.inl` comment block.
Corollary: any C++ operator that may block or sleep must trampoline —
it parks a continuation control op and yields, never blocks the thread.

**unwind walk** — `unwind_exec_to` (ops_flow.inl): when `exit`, `stop`,
or an error discards a range of the exec stack, this walk performs each
discarded control op's cleanup side effects — popping error-stack
companions, freeing companion *heap cells* (loop counters, address
bounds) — using a per-op `operator_is_*()` predicate chain.

**verify_t** — A 64-bit bitmask encoding acceptable types (low 31 bits,
one per Object type) and constraint flags (high bits, 32..45) for operand
validation.  Composable via bitwise OR.

**vm_offset_t** — A uint32_t byte offset from `m_vm_base`.  All internal
references use offsets rather than pointers for position-independence
(enables snap-shot/thaw).

**VM heap** — The contiguous memory region from which all runtime data
is allocated.  Carved into two ends: the **local VM** (bump-pointer,
grows upward from `m_vm_base`, journaled, rolled back on `restore`)
and the **global VM** (dlmalloc-managed, grows downward from
`m_vm_limit`, journal-skipped, reclaimed by mark-sweep GC).  See
[`gvm-heap-gc.md`](gvm-heap-gc.md) for the global side.

**WellKnownName** — The second pre-interned name enum (alongside
*SystemName*): runtime vocabulary that is data rather than operators —
coroutine statuses (`/ready`, `/dead`), supervision and gen-server
keys, transducer tags, introspection keys.  Backed by
`m_wellknown_offsets`, snapshot-friendly, built from its own dispatch
row list.

**WideValue** — The 16-byte variant of the *ExtValue* mechanism,
backing Int128 and UInt128.  Own per-save-level free list and active
counter (`:status:widevalue-*`), same single-owner rules, same restore
relocation; `maybe_free_extvalue` and `make_clone` handle both kinds.

**work queue (GC)** — The intrusive list of marked-but-not-yet-walked
global blocks during the GC mark phase, threaded through
`GvmBlock::m_next_in_work`.  Drained breadth-first; the per-block
link removes any fixed-capacity scratch limit.

**`${...}` block** — Scanner construct that runs the enclosed
scan-time block with `m_curr_alloc_global` set, restored on exit
(including on error unwind).  Per-coroutine state; allocations made
inside the block land in global VM.  `$${...}` is the force-local
inverse.
