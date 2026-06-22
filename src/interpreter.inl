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

//===--- Interpreter Execution Dispatch ---===//
//
// The interpreter is the central execution engine of Trix.  It implements a
// classic threaded-code interpreter loop modeled on PostScript's execution
// model:
//
//   Adobe Systems, "PostScript Language Reference Manual" (the Red Book),
//   3rd edition, 1999.  Section 3.5: Execution.
//
//   The PostScript execution model: objects on the execution stack are
//   popped one at a time and dispatched by type.  Literal objects push to
//   the operand stack; executable objects are executed (operators call C++,
//   names are looked up, procedures iterate their bodies, strings/streams
//   are scanned for tokens).
//
// --- Core concepts for maintainers ---
//
// EXECUTION STACK
//   The execution stack is the program counter.  Each entry is an Object
//   waiting to be executed.  The interpreter pops one Object per iteration,
//   dispatches it by type, and repeats.  Procedures push their remaining
//   body back onto the stack; streams push themselves back after each token.
//
//   This is fundamentally different from a bytecode VM with a program
//   counter register.  In Trix, the "next instruction" is always whatever
//   is on top of the execution stack.  Control flow operators work by
//   manipulating the exec stack: `if` pushes a proc, `loop` pushes a proc
//   plus a continuation marker, `stop` unwinds to a marker.
//
// DISPATCH BY TYPE (10 executable dispatch paths)
//   The interpreter dispatches each Object by its type tag:
//
//     Literal / ignores-execute  -> push to operand stack (data)
//     Null                       -> no-op (silently ignored)
//     Operator                   -> call native C++ function
//     Name                       -> look up in dictionary stack, execute result
//     Array / Packed             -> execute as procedure (pop head, push rest)
//     String                     -> scan one token, execute it
//     Stream                     -> scan one token from stream, execute it
//     Curry                      -> push captured value, execute callable
//     Thunk                      -> force evaluation, push cached result
//     Continuation               -> restore captured exec/error stack segment
//
// NAME LOOKUP AND EXECUTION
//   Executable names are resolved through a three-level lookup:
//     1. Binding cache: O(1) cached offset from previous lookup
//     2. Path search: hierarchical //:dict:key path syntax
//     3. Dictionary stack: walk from top to bottom, hash lookup in each dict
//   The resolved value is then dispatched: literals push to operand stack,
//   executables are executed (with @call frame for backtrace tracking).
//
// TAIL CALL OPTIMIZATION (TCO)
//   When an executable name is the last element in a procedure body and
//   the exec stack top is an @call frame, the interpreter reuses the
//   existing frame instead of pushing a new one.  This makes tail-recursive
//   procedures constant-space.  TCO applies to direct recursion, mutual
//   recursion, exec-based dispatch, and single-layer closures (where
//   @end-locals sits above @call -- cleaned up early).  See the detailed
//   comment in execute_name() for coverage analysis and safety proof.
//
// PROCEDURE EXECUTION
//   Procedures (Array/Packed) are executed element-by-element: the head is
//   popped and dispatched, the tail is pushed back onto the exec stack.
//   Nested procedures (e.g., `{ { 1 2 add } exec }`) are pushed as data
//   (literal) when encountered as direct body elements, and only executed
//   when explicitly invoked via a name lookup or `exec`.
//
// INTERRUPT PROCESSING
//   Before each iteration, the interpreter checks for pending interrupts
//   via a lock-free atomic load (m_pending_irq).  If set, it takes the
//   mutex and processes interrupts in priority order:
//     Level0IRQ > ErrorIRQ > Level1IRQ > SuspendIRQ > InvokeIRQ > Level2IRQ > ExitIRQ
//   Interrupts are raised from external threads via raise_interrupt(),
//   raise_error(), or invoke() in api.inl.
//
// ERROR HANDLING
//   Errors throw Exception::Error (a C++ exception), which is caught by
//   the interpreter loop.  error() (api.inl formats the message and
//   delegates to error(Error, Object) in ops_system.inl) records the
//   error context (last-error name/message/data, err-stack snapshot) and
//   calls try_catch_handler() then global_handler() (both ops_system.inl)
//   to find and run the matching handler.  The interpreter loop catch
//   block resets m_scanner_stream and resumes.
//
// THE TRAMPOLINE PATTERN
//   The trampoline is the central architectural pattern in Trix.  It solves
//   a fundamental problem: how does a C++ operator call a user-provided Trix
//   proc and act on the result, when the proc executes asynchronously via
//   the exec stack (not as a C++ function call with a return value)?
//
//   The solution: the operator does NOT call the proc.  Instead, it:
//     1. Saves any state it needs for "after the proc" onto the exec stack
//        as companion Objects (integers, offsets, saved operands, etc.).
//     2. Pushes a control operator (@-prefixed) that will run after the proc.
//     3. Pushes the user proc itself (runs first, since exec stack is LIFO).
//     4. Returns immediately.  The C++ call stack unwinds completely.
//
//   The interpreter then executes the user proc normally.  When the proc
//   finishes, the control operator fires, reads the proc's result from the
//   operand stack, reads saved state from the exec stack, and continues.
//
//   This pattern is used pervasively:
//
//   LOOPS (ops_flow.inl):
//     `loop` pushes [@loop, proc].  @loop fires after proc, pushes
//     [@loop, proc] again (infinite cycle).  `exit` unwinds to @loop.
//     `while` pushes [@while, body, cond].  @while checks the boolean
//     result, pushes the next iteration or stops.
//     `for` pushes [@*-for, proc, limit, incr, control].  The typed @for
//     control op increments the counter, checks the limit, and continues.
//
//   ITERATION (ops_array_iteration.inl):
//     `for-all` pushes [@array-for-all, proc, container, index].  After each
//     proc call, @array-for-all advances the index and pushes the next
//     element + proc, or terminates when the container is exhausted.
//     `any`/`all` use the same pattern with a boolean accumulator and
//     short-circuit on the first decisive result.
//     `sort-by` pushes key-extraction procs one at a time, collecting
//     computed keys for comparison.
//
//   LAZY SEQUENCES (ops_lazy.inl):
//     `lazy-filter` pushes [@lazy-filter-test, tail-thunk, pred].  After
//     the predicate runs, @lazy-filter-test checks the boolean: if true,
//     builds a cons cell with the current element; if false, forces the
//     next tail element and pushes another @lazy-filter-test.
//     All lazy transformers (map, filter, scan, zip-with, etc.) use this
//     pattern to interleave user proc execution with sequence construction.
//
//   ERROR HANDLING (ops_flow.inl):
//     `try` pushes [@try-barrier, proc].  On success, @try-barrier pushes
//     /no-error.  On error, error() unwinds the exec stack to the barrier
//     and pushes the error name instead.
//     `try-catch` pushes [@try-catch-barrier, @catch-error, handler-dict,
//     proc].  @catch-error looks up the error name in the handler dict and
//     executes the matching handler proc (another trampoline!).
//     `finally` pushes [@finally-barrier, cleanup, proc].  Whether the proc
//     succeeds or fails, the cleanup proc runs.
//
//   COROUTINES (ops_coroutine.inl):
//     Blocking operations (pipe-get, actor-recv, coroutine-join) push a
//     continuation control operator, flush the running coroutine's state,
//     and call coroutine_schedule().  When the coroutine is rescheduled,
//     the continuation fires and either completes the operation or re-sleeps.
//     Example: pipe-get pushes [@pipe-get-retry], flushes, schedules.
//     When woken, @pipe-get-retry checks if data is available: if yes,
//     delivers the item; if no, re-sleeps.
//
//   REACTIVE CELLS (ops_reactive.inl):
//     `cell-get` on a dirty computed cell pushes [@cell-eval, cell-literal,
//     compute-proc].  The proc runs (possibly triggering further cell-gets),
//     then @cell-eval caches the result and registers dependencies.
//
//   RECORDS (ops_record.inl):
//     `record-map` pushes [@record-map-step, proc, record, new-offset,
//     field-index].  After each proc call, @record-map-step stores the
//     result in the new record and advances to the next field.
//
//   LOGIC / BACKTRACKING (ops_logic.inl):
//     `choice` pushes [save, index, alt-array, @choice-barrier, proc].
//     On success, @choice-barrier restores save and returns result.  On
//     Fail error, @choice-fail restores save, advances index, and pushes
//     the next alternative.  All alternatives exhausted -> re-raise Fail.
//     `find-all` uses the same save/restore pattern to collect all
//     succeeding alternatives into an array.  `once`, `naf`, `find-n`,
//     `aggregate`, `aggregate-reduce`, `choice-count`, `for-each-solution`,
//     and `unify-match` each have dedicated barriers with backtrack-or-
//     re-raise semantics.
//
//   PATTERN MATCHING (ops_match.inl):
//     `match` pushes [value, pairs-array, @match-test, first-test].
//     @match-test checks the boolean result: if true, executes the paired
//     body; if false, advances to the next test/body pair.  `cond` uses
//     the same @match-test trampoline with Mark as the dispatch value.
//
//   PIPELINES (ops_pipeline.inl):
//     Blocking pipe I/O: `pipe-put` pushes [buffer, @pipe-put-retry].
//     @pipe-put-retry checks capacity: if full, re-sleeps; if space,
//     writes and completes.  `pipe-get` uses @pipe-get-retry similarly.
//
//   GENSERVER (ops_genserver.inl):
//     `gen-server-start` pushes [spec, state, null, null, @gen-server-recv]
//     after running the init proc.  @gen-server-recv blocks on the actor
//     mailbox, categorizes messages as /gen-call or /gen-cast, and
//     dispatches to handler procs.  @gen-server-call-done and
//     @gen-server-cast-done update state and restart the recv loop.
//
//   SUPERVISION (ops_supervision.inl):
//     `supervisor-start` pushes [@supervisor-check].  @supervisor-check
//     blocks on the supervisor mailbox until a child death notification
//     arrives, then applies the restart strategy (/one-for-one,
//     /one-for-all, /rest-for-one) with intensity tracking.
//
//   ACTORS (ops_actor.inl):
//     `actor-recv` with match predicate pushes the predicate proc, then
//     a check op that scans the mailbox.  The check op advances the scan
//     index, re-runs the predicate, or blocks until a matching message
//     arrives.  `actor-send` on a full mailbox pushes [@actor-send-retry]
//     and blocks; when woken, the retry completes the send.
//
//   DELIMITED CONTINUATIONS (ops_continuation.inl):
//     `delimit` pushes [@delimit-barrier, body].  Body executes above the
//     barrier.  Normal completion: barrier is a no-op.  `capture` scans
//     for the barrier, memcpy's the exec segment + paired error entries
//     into a ContinuationContext on the VM heap, and invokes the handler.
//     execute_continuation restores the segment with a fresh barrier.
//
//   ALGEBRAIC EFFECTS (ops_effect.inl):
//     `handle-effect` pushes [@effect-barrier, body] with handler-dict on
//     the error stack.  Normal completion: barrier pops the companion.
//     `perform` scans for a matching @effect-barrier, does an inclusive
//     capture (barrier + companion included) to create a Continuation K,
//     and invokes the handler.  Deep handler semantics: resuming K
//     restores the barrier+companion, reinstating the handler scope.
//
//   WHY THIS WORKS
//     The trampoline avoids deep C++ call stacks.  No matter how complex
//     the Trix program (nested loops calling procs calling lazy sequences
//     calling coroutine yields), the C++ call stack never grows beyond
//     the interpreter loop + one operator dispatch.  All "call depth" is
//     on the exec stack, which is a managed, bounded resource with clear
//     overflow detection.
//
//     This also makes coroutine context switching trivial: saving the exec
//     stack pointer captures the entire continuation.  There is no C++
//     stack to save or restore.  Delimited continuations exploit the same
//     property: memcpy'ing a segment of the exec stack captures an
//     arbitrary computation for later resumption.
//
// COROUTINE QUANTUM
//   When coroutines are active, the interpreter decrements an ops counter
//   (m_coroutine_ops_remaining) after each dispatch.  When the counter
//   reaches zero, it auto-yields to the scheduler.  The quantum is per-
//   coroutine (ctx->m_quantum), reset on each context switch.
//
// IDLE WAIT
//   When the execution stack is empty and no interrupt is pending, the
//   interpreter parks on a condition variable (m_cond) until an external
//   thread raises an interrupt or the destructor sends ExitIRQ.  This
//   allows a Trix instance to serve as a long-running embedded interpreter
//   that processes work items on demand.  Normal startup anchors the exec
//   stack with a Quit floor so draining the startup streams exits instead;
//   --resident / Config::m_resident skips that floor (Stream::init), making
//   the idle wait the steady state once startup work drains.
//
// Helper for frame-dict pop sites: at_end_locals_op (ops_dict.inl),
// execute_name's TCO inline path (interpreter.inl below), and the dict-
// pop barrier in try_catch_handler (ops_system.inl).  All three:
//   * test m_dict_ptr > permanent floor
//   * read m_dict_ptr->m_dict offset, resolve to Dict *
//   * decrement m_dict_ptr
//   * clear the dict's cached name bindings
// Returns the popped Dict so the caller can choose whether to recycle
// it to the dict pool (sites in interpreter and ops_dict always
// recycle; try_catch_handler recycles only for @end-locals, since
// other dict-pop barriers don't own their frame dict's storage).
//
// `valid()` is false when no frame dict was on top of the stack
// (m_dict_ptr at-or-below the permanent floor) -- in that case the
// caller must not deref `dict`.
struct PoppedFrameDict {
    Dict *dict{nullptr};
    vm_offset_t offset{nulloffset};

    [[nodiscard]] bool valid() const { return (dict != nullptr); }
};

[[nodiscard]] PoppedFrameDict pop_frame_dict_if_present() {
    auto trx = this;
    if (m_dict_ptr > (m_dict_base + PermanentDictCount - 1)) {
        auto offset = m_dict_ptr->m_dict;
        auto dict = offset_to_ptr<Dict>(offset);
        --m_dict_ptr;
        dict->clear_name_bindings(trx);
        return {dict, offset};
    } else {
        return {};
    }
}

// The actionable interrupt bits: pending IRQs (m_interrupt) with the masked-off ones
// removed.  Computed identically at every fast-path re-publish and slow-path test site,
// so it lives in one place to keep the mask (and the interrupt_t re-cast) in lockstep.
[[nodiscard]] interrupt_t actionable_interrupts() const {
    return static_cast<interrupt_t>(m_interrupt & ~m_interrupt_mask);
}

[[nodiscard]] interrupt_t process_interrupt() {
    auto irq = NoIRQ;
    // relaxed is sufficient: the mutex in the slow path provides all ordering
    // guarantees for reading m_interrupt.  A stale-zero result simply means one
    // extra iteration before the IRQ is serviced, which is correct behavior.
    if (m_pending_irq.load(std::memory_order_relaxed) != NoIRQ) {
        // This lock is released via RAII before any re-entrant interpreter execution
        std::unique_lock<std::mutex> lk(m_mutex);
        auto pending_irq = actionable_interrupts();
        if (pending_irq != NoIRQ) {
            auto trx = this;
            auto ok = has_exec_capacity(1);

            // IRQ priority order (high -> low): Level0 > Error > Level1 > Suspend > Invoke > Level2 > Exit.
            // ErrorIRQ is serviced immediately after Level0 because error() resets the exec
            // stack; deferring it could allow further execution with a corrupted state.
            // SuspendIRQ precedes InvokeIRQ: the interpreter must not accept new invocations
            // while suspended (the caller holds the mutex and expects quiescence).
            // ExitIRQ is lowest because it terminates the interpreter; any work above it
            // (including all levels and invoke) should complete before teardown.
            if (ok && ((pending_irq & Level0IRQ) != 0)) {
                irq = Level0IRQ;
                m_interrupt ^= irq;
                *++m_exec_ptr = Name::make_system(trx, SystemName::L0Interrupt, Object::ExecutableAttrib);
            } else if ((pending_irq & ErrorIRQ) != 0) {
                // error() resets the exec stack so no has_exec_capacity check is necessary
                m_interrupt ^= ErrorIRQ;
                // update m_pending_irq now: error() throws and bypasses the store at line end
                m_pending_irq.store(actionable_interrupts(), std::memory_order_relaxed);
                if (m_external_error != Error::NoError) {
                    auto ext_error = m_external_error;
                    m_external_error = Error::NoError;
                    error(ext_error, "{} raised by ErrorIRQ", error_sv(ext_error));
                } else {
                    diag_println("Trix ErrorIRQ raised with no error set (internal error)");
                }
            } else if (ok && ((pending_irq & Level1IRQ) != 0)) {
                irq = Level1IRQ;
                m_interrupt ^= irq;
                *++m_exec_ptr = Name::make_system(trx, SystemName::L1Interrupt, Object::ExecutableAttrib);
            } else if ((pending_irq & SuspendIRQ) != 0) {
                m_interrupt &= ~SuspendIRQ;
                // Flush the running coroutine's stacks to its CoroutineContext
                // so external probes (snapshot, /threads listing) see a consistent
                // snapshot during the suspend wait.  No load on resume: the
                // in-memory stack pointers were not disturbed by the flush.
                if (m_running_coroutine != nulloffset) {
                    coroutine_flush_running();
                }
                // ResumeIRQ may already be set if resume raced with suspend;
                // do NOT clear it here -- the while condition detects it immediately.
                while ((actionable_interrupts() & (ResumeIRQ | ExitIRQ)) == 0) {
                    m_cond.wait(lk);
                }
                if ((actionable_interrupts() & ExitIRQ) != 0) {
                    // ExitIRQ arrived while suspended; leave it in m_interrupt for
                    // normal dispatch and skip ResumeIRQ consumption.
                } else {
                    m_interrupt ^= ResumeIRQ;
                }
            } else if (has_exec_capacity(2) && ((pending_irq & InvokeIRQ) != 0) && Stream::available(trx)) {
                // Note: when this condition is false, InvokeIRQ is NOT cleared and is retried
                // on the next process_interrupt() call.  A program that keeps the exec stack
                // near its limit or exhausts stream slots can starve InvokeIRQ indefinitely.
                // This is an intentional design constraint: invoke() is best-effort.
                m_interrupt ^= InvokeIRQ;
                if (m_invoke_data != nullptr) {
                    auto [valid, offset, sid] = Stream::open_memory(trx, m_invoke_data, m_invoke_length);
                    if (valid) {
                        irq = InvokeIRQ;  // signal success only when work is actually done
                        *++m_exec_ptr = Object::make_control_operator(SystemName::atRun);
                        *++m_exec_ptr = Object::make_stream(offset, sid, Object::ExecutableAttrib, Object::ReadOnlyAccess);
                        m_invoke_data = nullptr;
                        m_invoke_length = 0;
                    } else {
                        // open_memory failed; restore IRQ bit so it will be retried
                        m_interrupt |= InvokeIRQ;
                    }
                } else {
                    diag_println("Trix InvokeIRQ raised with no invoke data set (internal error)");
                }
            } else if (ok && ((pending_irq & Level2IRQ) != 0)) {
                irq = Level2IRQ;
                m_interrupt ^= irq;
                *++m_exec_ptr = Name::make_system(trx, SystemName::L2Interrupt, Object::ExecutableAttrib);
            } else if ((pending_irq & ExitIRQ) != 0) {
                irq = ExitIRQ;
                m_interrupt ^= irq;
            }
        }

        // Re-publish the updated m_interrupt value to the lock-free fast-path variable.
        // Store only the actionable (unmasked) bits: if all pending IRQs are masked the
        // fast path sees NoIRQ and skips the mutex, avoiding unnecessary lock contention.
        m_pending_irq.store(actionable_interrupts(), std::memory_order_relaxed);
    }
    return irq;
}

void execute_value(Object value) {
    if (value.is_operator()) {
        // \operator calls native function
        value.operator_execute(this);
    } else if (!value.is_null()) {
        // \null is silently ignored
        require_exec_capacity(1);

        // push \Object on execution stack
        *++m_exec_ptr = value;
    }
}

void execute_name(Object name) {
    auto trx = this;
    auto value = Name::name_search(trx, &name);
    if (value == nullptr) [[unlikely]] {
        error(Error::Undefined, "executable name {} is not associated with any Object", name.name_sv(trx));
    } else if (value->is_literal() || value->ignores_execute()) {
        // a proc encountered indirectly via an \name lookup is pushed on the execution stack
        // and called as a procedure.
        // ignores_execute() is used (not pushop_direct()) because Array/Packed do NOT have
        // IgnoresExecute set -- they honor the executable attribute and are dispatched as procs.
        // clone value and push on operand stack
        require_op_capacity(1);

        *++m_op_ptr = value->make_clone(trx);
    } else if (value->is_operator()) {
        // Operator: self-identifying via m_last_operator_ptr; no @call frame needed.
        // Operators do not use ExtValue, so make_clone would be a struct copy -- dispatch directly
        // on the bound value.  No exec-stack slot is consumed, so no capacity check is required.
        value->operator_execute(trx);
    } else {
        // TCO shape detection: done BEFORE capacity reservation so that tail calls
        // (which grow the exec stack by only +1 slot) do not need to reserve the
        // full 4-slot frame that a non-TCO call would require.  Reserving 4
        // unconditionally would throw ExecStackOverflow on valid tail-recursive
        // programs running near the exec-stack limit.
        auto top_is_call = (m_exec_ptr >= (m_exec_base + 2)) && m_exec_ptr->is_operator() && m_exec_ptr->operator_is_call();
        auto top_is_end_locals_over_call = (m_exec_ptr >= (m_exec_base + 3)) && m_exec_ptr->is_operator() &&
                                           (m_exec_ptr->m_operator == +SystemName::atEndLocals) &&
                                           (m_exec_ptr - 1)->is_operator() && (m_exec_ptr - 1)->operator_is_call();
        auto is_tco = (top_is_call || top_is_end_locals_over_call);

        // Reserve exact capacity before cloning so an ExtValue-bearing clone cannot
        // leak on a late overflow.  (No current type reaches this branch with ExtValue
        // storage -- all ExtValue types have IgnoresExecute and were handled above --
        // but the pattern is preserved for future types that may violate that.)
        require_exec_capacity(is_tco ? 1 : 4);

        auto clone = value->make_clone(trx);
        if (!clone.is_null()) [[likely]] {
            // Non-operator executable (proc, string, stream): push a literal Name
            // companion followed by an @call marker so format_backtrace() can identify
            // which name was called.  at_call_op() consumes the companion on normal
            // return; try/catch unwind discards both along with the exec-stack region.
            //
            // Tail Call Optimization (TCO)
            // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~
            // When the current name is the last element of a proc body that was
            // itself invoked via a named call, the exec stack looks like this at
            // the moment execute_name() is entered (the interpreter has already
            // popped the current name):
            //
            //   m_exec_ptr --> @call           (caller's cleanup marker)
            //                  name-companion  (caller's literal Name)
            //                  sourceloc       (caller's source location)
            //                  ... rest ...
            //
            // Without TCO we would push a NEW 4-slot frame [loc][name][@call][clone]
            // on top, growing the exec stack by 4 per recursive call and exhausting
            // it at ~128 depth (512 / 4).
            //
            // With TCO we detect the tail position by checking whether m_exec_ptr
            // currently points at @call.  If so, we REPLACE the existing 3-slot
            // frame (overwrite the caller's SourceLoc and Name, keep @call) and
            // push only the new clone.  Net growth: +1 slot (the clone), constant
            // regardless of recursion depth.
            //
            // Coverage: TCO applies to all practical recursion patterns:
            //   1. Direct tail call:  /foo { ... foo } def
            //      foo is last element -> proc tail consumed -> @call exposed -> TCO.
            //   2. Via exec:  /foo { ... { foo } exec } def
            //      exec (an operator) pushes {foo} to exec stack and returns.
            //      Interpreter pops {foo}, execute_proc extracts foo (length=1),
            //      execute_name sees @call -> TCO.
            //   3. Via load+exec:  /foo { ... /foo load exec } def
            //      Same as (2): exec pushes the resolved proc, interpreter
            //      unrolls it, execute_name sees @call -> TCO.
            //   4. Mutual recursion:  /foo { ... bar } /bar { ... foo }
            //      Each named tail call replaces the caller's frame.
            //   5. N-way cycles:  /a { ... b } /b { ... c } /c { ... a }
            //      Generalizes (4).  Each tail call replaces the @call frame
            //      regardless of which name is being called -- no special N-way
            //      logic needed.
            //   6. Anonymous exec (dup exec): no @call frames are created by
            //      exec_op at all, so no frame accumulation -- inherently
            //      constant-space without any TCO logic needed.
            //
            // Why this is safe:
            //   - SourceLoc is a pure value (no VM allocation, no ExtValue).
            //   - The literal Name companion is a VM offset; overwriting does not
            //     leak because Name storage is immutable and shared.
            //   - @call remains in place and will clean up the new companions
            //     when the tail callee eventually returns.
            //
            // Why barriers are handled automatically:
            //   Any intervening exec-stack entry between the current name and
            //   the enclosing @call prevents TCO naturally:
            //     - remaining proc elements (tail not fully consumed)
            //     - error handling: @try-barrier, @try-catch-barrier,
            //       @finally-barrier, @with-stream
            //     - scope cleanup: @end-locals, @end-module, @dip
            //     - loops: @loop, @for, @while, @repeat, etc.
            //     - logic: @choice-barrier, @find-all-barrier, @once-barrier,
            //       @naf-barrier, and all backtracking barriers
            //     - reactive: @cell-eval, @batch-end, @batch-fire
            //     - continuations: @delimit-barrier, @effect-barrier
            //     - concurrency: @gen-server-recv, @supervisor-check,
            //       @pipe-put-retry, @pipe-get-retry
            //     - matching: @match-test
            //     - contracts: @ensure-check (intentional -- postcondition
            //       must run after the function returns)
            //   In all these cases m_exec_ptr does NOT point at @call, so the
            //   check below falls through to the normal (non-TCO) path.
            //
            // External vs internal barriers:
            //   External barriers (try, stopped, dip, finally, with-stream) sit
            //   BELOW the function's @call frame.  The initial call from the
            //   barrier body does not get TCO, but recursive tail calls within
            //   the function see @call on top and get full TCO.  In practice,
            //   external barriers have negligible impact on recursive TCO.
            //
            //   Internal barriers (@end-locals from closures) sit ABOVE @call
            //   inside the function body.  Without closure-aware TCO, every
            //   recursive call would see @end-locals instead of @call.
            //
            //   Closure TCO: when the exec stack top is @end-locals and @call
            //   is directly below it, we execute the @end-locals cleanup early
            //   (pop frame dict, clear bindings, recycle) and then reuse the
            //   @call frame.  This is safe because all tail-call arguments are
            //   already materialized on the operand stack -- no lazy references
            //   to the frame dict remain.  The new invocation's begin-locals
            //   will push a fresh frame dict + @end-locals, restoring the
            //   expected exec stack shape.  This eliminates the dict-stack
            //   overflow at ~22 depth for single-layer closure recursion.
            //
            //   Loop barriers (@loop, @for, @repeat) are reestablished each
            //   iteration.  Recursive calls inside loop bodies are always non-tail.
            //
            // Backtrace impact: intermediate tail-call frames are lost, which is
            // the expected and standard behavior for TCO (same as Scheme, Lua, etc.).
            //
            // Non-applicable cases:
            //   - Top-level stream execution (code scanned directly from a
            //     file via @run) has no @call frame -- not a recursion scenario.
            //   - Continuation dispatch (execute_continuation) bypasses
            //     execute_name entirely -- TCO is orthogonal to continuations.
            //   - @end-module above @call does not need TCO -- module bodies
            //     run once to define exports, they do not recurse.

            auto loc = Object::make_sourceloc(&m_last_scan_location);
            name.set_literal();  // guard: if ever visible to the interpreter loop, push as data not a name lookup

            if (top_is_call) {
                // Tail call: replace the existing @call frame's companions.
                //   m_exec_ptr[ 0] = @call       (kept)
                //   m_exec_ptr[-1] = old name    (overwritten)
                //   m_exec_ptr[-2] = old loc     (overwritten)
                ++m_tco_count;
                m_exec_ptr[-1] = name;
                m_exec_ptr[-2] = loc;
                *++m_exec_ptr = clone;
            } else if (top_is_end_locals_over_call) {
                // Closure tail call: @end-locals sits above @call.
                //
                // Discriminator: does the callee ALSO have a |locals| preamble
                // (i.e., will its body push its own frame dict)?  Cheap check:
                // scanner always emits locals-wrapped procs as packed, with
                // begin-locals as the last element.  Walk to the last element
                // and test for that.
                //
                // If YES -- "closure recursion" style.  Pop the caller's frame
                // dict eagerly so the dict stack stays bounded across
                // recursive tail calls.  This is the original closure-TCO
                // behavior.
                //
                // If NO  -- the callee will do its work against whatever dict
                // is topmost-writable.  Popping the caller's frame would silently
                // change def semantics in the callee: `/foo def` would land in
                // the grand-caller's frame (or userdict) instead of the caller's
                // frame.  Preserve the caller's frame instead, at the cost of
                // keeping @end-locals on the exec stack for the duration of the
                // tail-call chain.  Stack growth is still +1 per call; the
                // @end-locals entry persists, not accumulates.
                auto callee_has_begin_locals_tail = [&] {
                    if (clone.is_packed()) {
                        auto length = clone.m_arrays_length;
                        // Minimum locals-wrapped layout: [K N {body} begin-locals] = 4.
                        if (length < 4) {
                            return false;
                        } else {
                            const packed_data_t *packed_data = clone.packed_data_ptr(trx);
                            packed_data = Object::skip_packed(packed_data, static_cast<length_t>(length - 1));
                            auto [_, last_obj] = Object::extract_next_packed(trx, packed_data);
                            return (last_obj.is_operator() && (last_obj.m_operator == +SystemName::BeginLocals));
                        }
                    } else {
                        return false;
                    }
                }();

                if (callee_has_begin_locals_tail) {
                    //   m_exec_ptr[ 0] = @end-locals (consumed: frame popped)
                    //   m_exec_ptr[-1] = @call       (kept, reused)
                    //   m_exec_ptr[-2] = old name    (overwritten)
                    //   m_exec_ptr[-3] = old loc     (overwritten)
                    //
                    // The presence of @end-locals on exec implies a frame
                    // dict above the permanent floor on the dict stack
                    // (begin_locals always pushes both together).  The
                    // assert documents the invariant; the helper's own
                    // floor check is the runtime fallback.
                    assert(m_dict_ptr > (m_dict_base + PermanentDictCount - 1));
                    auto popped = pop_frame_dict_if_present();
                    if (popped.valid()) {
                        Dict::recycle(this, popped.dict, popped.offset);
                    }
                    // Pop @end-locals, exposing @call.
                    --m_exec_ptr;
                    ++m_tco_count;
                    m_exec_ptr[-1] = name;
                    m_exec_ptr[-2] = loc;
                    *++m_exec_ptr = clone;
                } else {
                    //   m_exec_ptr[ 0] = @end-locals (kept; fires at chain end)
                    //   m_exec_ptr[-1] = @call       (kept, reused)
                    //   m_exec_ptr[-2] = old name    (overwritten)
                    //   m_exec_ptr[-3] = old loc     (overwritten)
                    ++m_tco_count;
                    m_exec_ptr[-2] = name;
                    m_exec_ptr[-3] = loc;
                    *++m_exec_ptr = clone;
                }
            } else {
                // Normal call: push a full 4-slot frame.
                // (capacity already checked above, before make_clone)

                // Exec stack layout (bottom to top, first popped = top):
                //   clone   -- the executable value (proc/string/stream), runs first
                //   @call   -- control op: on return, pops the name+loc companions
                //   name    -- literal companion for backtrace identification
                //   loc     -- source location for backtrace
                *++m_exec_ptr = loc;
                *++m_exec_ptr = name;
                *++m_exec_ptr = Object::make_control_operator(SystemName::atCall);
                *++m_exec_ptr = clone;
            }
        }
    }
}

void execute_curry(Object curry) {
    // Curry execution: dispatch captured value, then dispatch callable.
    // The 2 Objects [value, callable] are stored consecutively in VM.
    //
    // For curry (partial application): value is Literal -- pushed to operand stack.
    // For compose (function composition): value is Executable -- dispatched via execute_value.
    auto trx = this;
    auto pair = curry.curry_storage(trx);

    // Worst-case capacity for the direct pushes performed here: at most 2 op slots
    // (both value and callable are data) or 1 op + 1 exec (one executable).  Reserved
    // before make_clone so any ExtValue-bearing clone cannot leak on a late overflow.
    // execute_value() re-checks capacity internally for its own pushes.
    require_op_capacity(2);
    require_exec_capacity(1);

    // Root the curry + clones across the make_clones: under ${...} make_clone of an
    // ExtValue/WideValue capture is a global alloc that fires vm_global_gc.  `curry`
    // is a bare C local (the exec dispatch already consumed its op-stack slot), so an
    // unrooted GC sweeps the curry block -- dangling `pair` -- and the first clone is
    // swept across the second clone's GC.  Root the curry (keeps the block + both
    // captured ExtValues alive so `pair` stays valid) for the first clone, then hold
    // value + callable across the rest (execute_value below can allocate too).
    // Slot 1 = curry (keeps the block + both captured ExtValues alive so `pair` stays
    // valid); slot 2 = the value clone across the callable clone's GC.  Drop both
    // BEFORE the branch: value/callable are owned clones now, the branch pushes each
    // onto a stack with no allocation before the push, and execute_value() dispatches
    // arbitrary user code that requires an empty gc-root stack at the operator boundary.
    require_gc_root_capacity_more(2);
    *++m_gc_roots_ptr = curry;
    auto value = pair[0].make_clone(trx);
    *++m_gc_roots_ptr = value;
    auto callable = pair[1].make_clone(trx);
    gc_root_pop_n(2);

    if (value.is_literal() || value.ignores_execute()) {
        // Dispatch captured value, then callable.
        *++m_op_ptr = value;
        if (callable.is_literal() || callable.ignores_execute()) {
            *++m_op_ptr = callable;
        } else {
            execute_value(callable);
        }
    } else {
        // Compose path: execute the value (first callable).
        // Push callable to exec stack first so it runs AFTER value completes.
        if (callable.is_literal() || callable.ignores_execute()) {
            *++m_op_ptr = callable;
        } else {
            *++m_exec_ptr = callable;
        }
        execute_value(value);
    }
}

void execute_thunk(Object thunk) {
    // Thunk execution: force evaluation and push cached result.
    // The 3 Objects [state, proc, result] are stored consecutively in VM.
    auto trx = this;
    auto storage = thunk.thunk_storage(trx);
    auto state = storage[Object::ThunkStorageState].integer_value();

    if (state == Object::ThunkEvaluated) {
        // Already evaluated: push cached result.  Root the thunk across the clone:
        // `thunk` is a bare C local and `storage` points into its block, so under
        // ${...} the result clone's vm_global_gc would sweep the unrooted thunk and
        // free the cached-result ExtValue reachable only through it.
        require_op_capacity(1);

        require_gc_root_capacity_more(1);
        *++m_gc_roots_ptr = thunk;
        *++m_op_ptr = storage[Object::ThunkStorageResult].make_clone(trx);
        gc_root_pop_n(1);
    } else if (state == Object::ThunkUnevaluated) {
        require_exec_capacity(3);

        // Unevaluated: set Evaluating, push continuation, then execute proc
        auto curr_save_level = m_curr_save_level;
        if (storage[Object::ThunkStorageState].save_level() != curr_save_level) {
            Save::save_object(trx, &storage[Object::ThunkStorageState]);
        }
        storage[Object::ThunkStorageState].update_integer(Object::ThunkEvaluating);

        // Exec stack layout (bottom to top, first popped = top):
        //   thunk (literal)  -- companion below the barrier so the fail
        //                       handler can identify which thunk to reset.
        //   @force-complete  -- BarrierOp; captures result, marks Evaluated.
        //                       Unwinder swaps to @force-fail on error.
        //   proc             -- the thunk's procedure, runs first (pushed
        //                       below by execute_value).
        thunk.set_literal();
        *++m_exec_ptr = thunk;
        *++m_exec_ptr = Object::make_control_operator(SystemName::atForceComplete);

        auto proc = storage[Object::ThunkStorageProc].make_clone(trx);
        execute_value(proc);
    } else {
        error(Error::UndefinedResult, "circular thunk: force during evaluation");
    }
}

void execute_continuation(Object continuation) {
    // One-shot delimited continuation: restore captured exec stack segment.
    // The resume value is already on the operand stack (top element).
    auto trx = this;

    // Restore-dangle detection: if the current save level has fallen below
    // the capture level, the ctx allocation and any VM offsets it references
    // (notably the captured frame dicts) were reclaimed by a restore.  The
    // capture save level is stashed in the Object itself so we can reject
    // cleanly without dereferencing the possibly-freed ctx.  Belt to
    // Save::reject_stale_value_into_eqref's braces: that put-site guard
    // blocks the common reachable path (K into root-EqArray during save
    // scope), this check still catches any internal path that slips a
    // stale K to execute_continuation.
    if (trx->m_curr_save_level < continuation.continuation_save_level()) {
        error(Error::InvalidAccess, "continuation invalidated by restore");
    } else {
        auto ctx = continuation.continuation_context(trx);

        if (ctx->is_spent()) {
            error(Error::InvalidAccess, "continuation is spent (one-shot)");
        } else {
            auto exec_count = ctx->m_exec_count;
            auto err_count = ctx->m_err_count;
            auto dict_count = ctx->m_dict_count;
            auto op_count = ctx->m_op_count;
            auto op_rewrite_count = ctx->m_op_rewrite_count;
            auto exec_rewrite_count = ctx->m_exec_rewrite_count;

            require_exec_capacity(exec_count + 1);  // +1 for fresh @delimit-barrier above the restored segment
            require_error_capacity(err_count + 1);  // +1 for fresh @delimit-barrier op-depth companion
            require_dict_capacity(dict_count);
            // Resume value is already on top of op stack; after restore we add
            // op_count slice items below it.  Net new capacity needed: op_count.
            require_op_capacity(op_count);

            // Snapshot the alloc-global flag at capture time BEFORE mark_spent
            // wipes the ctx state.  The captured body resumes with the same
            // m_curr_alloc_global value it had at capture, so allocations after
            // resume route to the same heap they would have without the
            // capture/resume detour.  The handler in between may have mutated the
            // flag (set-global, exit through outer @end-in-global, etc); without
            // this restore, the body would silently allocate to the wrong heap.
            auto alloc_global_at_capture = ctx->m_alloc_global_at_capture;

            ctx->mark_spent();

            trx->m_curr_alloc_global = alloc_global_at_capture;

            // Restore captured frame dicts first (outermost first).  Each pushed dict
            // gets its name bindings reinstated in the running coroutine's binding
            // table so the restored @end-locals markers, and any name references inside
            // the restored exec segment, resolve correctly.
            if (dict_count > 0) {
                auto dicts_src = &ctx->m_objects[exec_count + err_count];
                std::memcpy(m_dict_ptr + 1, dicts_src, dict_count * sizeof(Object));
                for (length_t i = 0; i < dict_count; ++i) {
                    auto dict_obj = m_dict_ptr[i + 1];
                    dict_obj.dict_value(trx)->set_name_bindings(trx);
                }
                m_dict_ptr += dict_count;
            }

            // Restore captured op slice below the resume value and record the
            // fresh barrier's op-depth at the PRE-slice position.  Rationale:
            // subsequent captures inside the restored body should treat the
            // restored slice + resume value as ambient state belonging to the
            // captured scope -- so the fresh barrier's "scope floor" is where
            // the slice would have started being pushed.  Recording the depth
            // AFTER restore would misplace the floor above the slice, and a
            // captured body that pops the resume value would drop below it.
            auto resume_value = *m_op_ptr--;
            auto fresh_op_depth = static_cast<integer_t>(m_op_ptr - m_op_base);
            if (op_count > 0) {
                auto op_src = &ctx->m_objects[exec_count + err_count + dict_count];
                std::memcpy(m_op_ptr + 1, op_src, op_count * sizeof(Object));
                m_op_ptr += op_count;
            }
            *++m_op_ptr = resume_value;

            // Push fresh @delimit-barrier (so nested captures within K find this
            // delimiter) with its op-depth companion on err stack.
            *++m_err_ptr = Object::make_integer(fresh_op_depth);
            *++m_exec_ptr = Object::make_control_operator(SystemName::atDelimitBarrier);

            // Rewrite captured @try-rollback saved-depth Integers BEFORE the
            // exec memcpy.  The captured exec segment holds saved-depth
            // Integers one slot below each captured @try-rollback (try_op
            // pushes saved-depth then @try-rollback).  Stale values trim the
            // live op stack to the capturer's frame on the error path; rewrite
            // each to fresh_op_depth so the rollback honours the resumer's
            // frame instead.  Mutating the ctx buffer is safe -- K is one-shot
            // and already mark_spent.
            if (exec_rewrite_count > 0) {
                auto exec_segment = &ctx->m_objects[0];
                auto exec_rewrite_array = &ctx->m_objects[exec_count + err_count + dict_count + op_count + op_rewrite_count];
                auto new_floor = Object::make_integer(fresh_op_depth);
                for (length_t i = 0; i < exec_rewrite_count; ++i) {
                    auto offset = static_cast<length_t>(exec_rewrite_array[i].integer_value());
                    exec_segment[offset] = new_floor;
                }
            }

            // Restore captured exec segment (stored bottom-first for memcpy)
            std::memcpy(m_exec_ptr + 1, &ctx->m_objects[0], exec_count * sizeof(Object));
            m_exec_ptr += exec_count;

            // Rewrite captured inner op-depth companions to fresh_op_depth BEFORE
            // the err memcpy.  The captured err segment holds op-depth Integers
            // for @delimit-barrier and @handler-scope that were recorded at
            // capture time -- absolute m_op_ptr - m_op_base values in the
            // CAPTURER's frame.  On restore at a different frame those depths
            // are stale; a subsequent capture inside the resumed body would
            // read them and trim the live op stack to the wrong floor (silent
            // operand corruption -- markers below the resumer's frame
            // disappear).  The rewrite collapses every captured inner op-depth
            // to fresh_op_depth, the segment's starting floor in the
            // resumer's frame.  Mutating the ctx buffer is safe because K is
            // one-shot and is already mark_spent above; if Trix ever goes
            // multi-shot, move this to operate on the live err stack
            // post-memcpy.
            if (op_rewrite_count > 0) {
                auto err_segment = &ctx->m_objects[exec_count];
                auto rewrite_array = &ctx->m_objects[exec_count + err_count + dict_count + op_count];
                auto new_floor = Object::make_integer(fresh_op_depth);
                for (length_t i = 0; i < op_rewrite_count; ++i) {
                    auto offset = static_cast<length_t>(rewrite_array[i].integer_value());
                    err_segment[offset] = new_floor;
                }
            }

            // Restore captured error stack entries (stored bottom-first for memcpy)
            if (err_count > 0) {
                std::memcpy(m_err_ptr + 1, &ctx->m_objects[exec_count], err_count * sizeof(Object));
                m_err_ptr += err_count;
            }
        }
    }
}

void execute_proc(Object proc) {
    auto length = proc.arrays_length();
    if (length != 0) {
        // get a copy of the head Object from the proc
        auto trx = this;
        // packed_pop_head (not _clone_head): both branches below call make_clone on value,
        // so an intermediate clone here would leak an ExtValue/WideValue cell per literal.
        auto value = proc.is_array() ? proc.array_pop_head(trx) : proc.packed_pop_head(trx);

        // A value encountered as a direct body element of a proc is pushed as data, not
        // executed as a sub-procedure.  pushop_direct() is used here (not ignores_execute())
        // because Array/Packed have PushOpDirect set: inside a proc body, a nested { }
        // is data.  In contrast, execute_name() uses ignores_execute() where a name-bound
        // proc IS executed.
        const bool pushes_operand = value.is_literal() || value.pushop_direct();

        if (length > 1) {
            // The proc slot freed by the interpreter's pop is reused for the tail push.
            // When the head will itself be pushed on the exec stack (executable Name, String,
            // or Stream -- the only types flagged with NeedsExecSlot) one additional free slot
            // is required before the tail push consumes the freed one.  Literal heads and
            // pushop_direct types land on the operand stack; operators dispatch in-place;
            // null is a no-op -- none of those need a second exec slot.
            if (!pushes_operand && value.needs_exec_slot()) {
                require_exec_capacity(2);
            }
            // push the remaining proc back on the execution stack
            *++m_exec_ptr = proc;
        }

        if (pushes_operand) {
            // push clone value on operand stack
            require_op_capacity(1);

            // make_clone allocates a fresh payload only for ExtValue/WideValue (immediates
            // and composites clone by struct copy); under ${...} that alloc is GLOBAL and can
            // fire vm_global_gc.  At that point `value` -- a borrowed head whose payload is
            // owned by proc's data -- is unrooted: array_pop_head/packed_pop_head advanced
            // proc past slot 0, so the re-pushed proc (length>1) no longer covers it, and a
            // LOCAL proc's local-array GC walk reads the advanced handle and misses the popped
            // slot entirely (a GLOBAL proc's header-based walk over-approximates and happens to
            // still cover it -- but LOCAL procs built by e.g. pipe_build_proc, baking a global
            // `init` literal, do not).  Root `value` itself across the clone so its source
            // survives.  Guarded on the clone-allocates predicate so the common literal push
            // (immediate/operator/composite) pays nothing.  (The dispatch loop reaches
            // execute_proc with an empty gc-root stack, so the one-off empty-assert holds.)
            if (value.uses_extvalue() || value.uses_widevalue()) {
                gc_root_push_oneoff(value);
                *++m_op_ptr = value.make_clone(trx);
                gc_root_pop_oneoff();
            } else {
                *++m_op_ptr = value.make_clone(trx);
            }
        } else {
            // Values reaching here are executable non-literals (Name/String/Stream), never
            // ExtValue/WideValue, so make_clone is a struct copy that never allocates -- no
            // GC-during-clone hazard, no rooting needed.
            execute_value(value.make_clone(trx));
        }
    }
}

void execute_input_token(Lexeme token, Object value) {
    // no need to clone value, Object was just created from input String/Stream
    if (token == Lexeme::LiteralValue) {
        require_op_capacity(1);

        *++m_op_ptr = value;
    } else {
        execute_value(value);
    }
}

void execute_string(Object string) {
    auto trx = this;
    auto [token, value, substring] = Stream::scanner(trx, string);
    if (token == Lexeme::SyntaxError) {
        value = Object::append_error_string(trx, value, " (while scanning an Executable 'string')");
        error(Error::SyntaxError, value);
    } else if (token != Lexeme::EndOfStream) {
        auto continuation = (substring.string_length() != 0);
        // slot guaranteed: this String was just popped from exec stack
        // However, if the continuation is pushed back AND the token also needs an exec-stack slot,
        // verify a second slot exists before the continuation push consumes the freed slot.
        if (continuation && (token == Lexeme::ExecutableValue) && !value.is_operator() && !value.is_null()) {
            require_exec_capacity(2);
        }
        if (continuation) {
            *++m_exec_ptr = substring;
        }
        execute_input_token(token, value);
    }
}

void execute_stream(Object stream_obj) {
    auto trx = this;
    auto [stream, sid] = stream_obj.stream_value(trx);
    if (stream->is_readable(sid)) {
        auto [token, value] = stream->scanner(trx);
        if (token == Lexeme::SyntaxError) {
            // The stream_obj was already popped from the exec stack by the interpreter
            // loop (m_exec_ptr--) before execute_stream() was called, and has not yet been
            // pushed back (that happens in the non-error path below).  The slot at
            // m_exec_ptr+1 therefore still holds this stream_obj and is physically valid
            // even though it is logically above the stack pointer.  try_catch_handler() and
            // stop_op() exploit this by reading run_object+1 when they encounter @run, so
            // they can close the stream.  Nothing may write to m_exec_ptr+1 between this
            // point and the throw, or that invariant would break.
            value = Object::append_error_string(trx, value, " (while scanning an Executable 'stream')");
            error(Error::SyntaxError, value);
        } else if (token != Lexeme::EndOfStream) {
            // slot guaranteed: this Stream was just popped from exec stack
            // However, if the token also needs an exec-stack slot, verify a second slot exists
            // before the stream push consumes the freed slot.
            if ((token == Lexeme::ExecutableValue) && !value.is_operator() && !value.is_null()) {
                require_exec_capacity(2);
            }
            *++m_exec_ptr = stream_obj;
            execute_input_token(token, value);
        }
    } else if (stream->is_open(sid)) {
        // Open but not readable: a write-only stream was pushed as executable.
        // Surface it as a programming error.
        error(Error::InvalidStreamAccess, "executable stream {} does not have Read access", stream->source(trx));
    } else {
        // Closed or stale sid: silently drop.  Typical path is fill_file
        // auto-closing a read-only file on EOF after the scanner has
        // already consumed a final token (e.g. trailing integer in a file
        // without terminating newline); the token was executed on the
        // previous iteration, and the stream object we just popped off
        // the exec stack refers to the now-closed slot.  No further data
        // to scan, no error to raise.
    }
}

// Wait until either ExitIRQ is raised or at least one Object is on the execution stack.
// Returns the pending IRQ; caller should exit the interpreter loop if ExitIRQ is returned.
[[nodiscard]] interrupt_t wait_for_exec_ready() {
    // Park until an interrupt produces exec-stack work or ExitIRQ is raised.
    while (true) {
        auto irq = process_interrupt();
        if ((irq == ExitIRQ) || (exec_count() != 0)) {
            return irq;
        } else {
            // Wait for a new interrupt or condition change.  The wait condition
            // also checks exec capacity: if a user IRQ is pending but the exec
            // stack is full, we must wait for capacity rather than spinning.
            std::unique_lock<std::mutex> lk(m_mutex);
            auto pending = actionable_interrupts();
            while ((pending == 0) || (!has_exec_capacity(1) && ((pending & (ErrorIRQ | ExitIRQ)) == 0))) {
                m_cond.wait(lk);
                pending = actionable_interrupts();
            }
        }
    }
}

//===--- Interpreter Loop ---===//
//
// Two-level loop structure for performance:
//
//   Outer loop: handles IRQ processing, wait-for-exec-ready, and exception
//     recovery.  Runs once per IRQ check interval or after an exception.
//
//   Inner loop: tight dispatch loop that processes up to InterpreterIRQInterval
//     operations without checking for interrupts.  No function calls to
//     process_interrupt(), no atomic loads, no wait logic.  Exits when:
//       - exec stack drains (normal completion)
//       - IRQ countdown expires (periodic IRQ check)
//       - Exception thrown (error, coroutine yield, exit)
//
// IRQ latency: bounded by InterpreterIRQInterval operations (~10 microseconds
// at 100M ops/sec with the default interval of 1024).
//
// BoundedOps template parameter: when false (the common case -- m_max_ops == 0)
// the per-iteration execution-limit check is compiled out entirely.  m_max_ops
// is fixed at configuration time, so the choice is stable for the lifetime of
// a dispatch_loop() call; interpreter() picks the variant on each outer-loop
// entry.  In dev builds (Debugger == true) m_debug_active is NOT hoisted
// because debug_op may set it mid-loop and the next iteration must see the
// change to honor step semantics.  Release builds (Debugger == false) fold
// the whole step-hook check out at compile time.
//
template<bool BoundedOps>
void dispatch_loop() {
    auto trx = this;
    auto irq_countdown = InterpreterIRQInterval;

    while (m_exec_ptr >= m_exec_base) {
        // pop the next Object from the execution stack
        auto curr_obj = *m_exec_ptr--;
        ++m_op_count;

        if constexpr (BoundedOps) {
            if (m_op_count >= m_max_ops) [[unlikely]] {
                m_op_count = 0;  // reset so try-catch handler can execute
                curr_obj.maybe_free_extvalue(trx);
                error(Error::ExecutionLimit, "execution limit reached ({} operations)", m_max_ops);
            }
        }

// ignores_execute() types (SourceLoc etc.) are infrastructure-only and
// invisible to the user; suppress the step hook for them entirely rather
// than entering the hook to filter them out.
// Whole branch (m_debug_active load + debug_hook call) is gated out
// by TRIX_DBG when Debugger is false -- release builds pay zero
// per-iteration cost.  We use the macro rather than `if constexpr`
// because dispatch_loop is a template (BoundedOps) and two-phase
// lookup still requires m_debug_active / debug_hook to be declared
// when the discarded branch is visible.
#ifdef TRIX_DEBUGGER
        if (m_debug_active && !curr_obj.ignores_execute()) [[unlikely]] {
            debug_hook(curr_obj);
        }
#endif
        if (curr_obj.is_literal() || curr_obj.ignores_execute()) {
            // any Literal object or object that ignores Execute is pushed to the operand stack
            // fast single-push capacity check (pointer compare, no subtraction)
            if (m_op_ptr < m_op_push_limit) [[likely]] {
                *++m_op_ptr = curr_obj;
            } else {
                curr_obj.maybe_free_extvalue(trx);
                require_op_capacity(1);
            }
        } else {
            switch (+curr_obj.type()) {
            case +Object::Type::Null:
                // \null is silently ignored as a no-op
                break;

            case +Object::Type::Operator:
                // \operator calls native function
                curr_obj.operator_execute(trx);
                break;

            case +Object::Type::Name:
                // \name is looked up using the name cache, the dictionary stack, or a dictionary path
                // and the bound value is then executed
                execute_name(curr_obj);
                break;

            case +Object::Type::Array:
            case +Object::Type::Packed:
                // execute the head Object of the Proc (\array or \packed) container
                execute_proc(curr_obj);
                break;

            case +Object::Type::String:
                // execute the Object scanned from the \string input source
                execute_string(curr_obj);
                break;

            case +Object::Type::Stream:
                // execute the Object scanned from the \stream input source
                execute_stream(curr_obj);
                break;

            case +Object::Type::Curry:
                // execute the Curry: push captured value, then execute callable
                execute_curry(curr_obj);
                break;

            case +Object::Type::Thunk:
                // execute the Thunk: force evaluation and push cached result
                execute_thunk(curr_obj);
                break;

            case +Object::Type::Continuation:
                // execute the Continuation: restore captured exec stack segment
                execute_continuation(curr_obj);
                break;

            default:
                error(Error::InternalError, "interpreter: unhandled type {}", Object::type_sv(curr_obj.type()));
            }
        }

        // coroutine quantum check: auto-yield when ops counter expires
        if ((m_coroutine_ops_remaining != 0) && (--m_coroutine_ops_remaining == 0)) [[unlikely]] {
            auto ctx = offset_to_ptr<CoroutineContext>(m_running_coroutine);
            m_coroutine_ops_remaining = ctx->m_quantum;  // reset for next slice
            coroutine_flush_running();
            ctx->m_status = CoroutineContext::Ready;
            ready_queue_push(ctx);
            coroutine_schedule();
            irq_countdown = InterpreterIRQInterval;  // reset for new coroutine
        }

        // periodic IRQ check: break to outer loop every N operations
        if (--irq_countdown == 0) {
            break;
        }
    }
}

void interpreter() {
    auto trx = this;

    // interpreter is active
    m_active = true;

    while (true) {
        try {
            // wait for an actionable interrupt or a pending Object on the exec stack
            if (wait_for_exec_ready() == ExitIRQ) {
                break;
            } else {
                // Pick the dispatch variant for this outer-loop pass.  m_max_ops is configuration-
                // level and does not change at runtime, so a single check per pass is sufficient.
                if (m_max_ops == 0) [[likely]] {
                    dispatch_loop<false>();
                } else {
                    dispatch_loop<true>();
                }
            }
        }

        // catch by value as e is a trivial type and this is slightly more efficient
        catch (Exception e) {
            if (e != Exception::Error) {
                // Exception::Exit is the normal user-quit path (quit_op) and the
                // abnormal-script-error path (default_handler_op); both have already
                // printed their own diagnostic (or nothing, for clean quit), so
                // emit no additional message here.  default_handler_op sets
                // m_exit_code on the abnormal path; quit_op leaves it 0.
                if (e == Exception::EndOfStream) {
                    diag_println("Trix unhandled EndOfStream");
                    m_exit_code = static_cast<int>(Error::IOReadError);
                } else if (e != Exception::Exit) {
                    diag_println("Trix unhandled exception {}", static_cast<int>(e));
                    m_exit_code = 125;
                }
                break;
            } else {
                // Exception::Error: error processing was handled in error(), resume interpreter.
                // Reset m_scanner_stream: the error may have interrupted mid-scan, leaving
                // stale state that would confuse the next scanner() call.
                m_scanner_stream = nullptr;
                // The unwind is complete: any objects try_catch_handler planted on
                // the operand stack are in their final position (scanner cleanups
                // along the unwind have preserved them).  Clear the count so later
                // non-error cleanup calls don't stash live data.
                m_planted_error_objects = 0;
                *m_last_operator_ptr = Object::make_operator(SystemName::Interpreter);
            }
        }

        catch (const std::exception &e) {
            // terminate on uncaught standard exception, this is a design choice
            diag_println("Trix unhandled {} exception", e.what());
            break;
        }

        catch (...) {
            // terminate on uncaught unknown exception, this is a design choice
            diag_println("Trix unhandled unknown exception");
            break;
        }
    }

    // release any Stream resources
    Stream::stop(trx);

    // pipe close is handled by ~Trix so it also covers init-time and
    // uncaught-exception paths where interpreter() never completes.

    // allow destructor to continue
    m_active = false;
    m_cond.notify_all();
}
