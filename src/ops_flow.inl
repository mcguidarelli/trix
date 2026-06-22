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

//===--- Control Flow Operators ---===//
//
// Implements all control flow constructs: conditionals, loops, iteration,
// error handling, and program termination.  Based on:
//
//   PostScript: `if`, `ifelse` (if-else), `loop`, `exit`, `for`, `for-all`,
//   `repeat`, `stopped`/`stop`, `exec`, `run`, `bind`, `quit`.  These operators
//   manipulate the execution stack directly rather than using a program
//   counter, which is the defining characteristic of a threaded interpreter.
//
//   Forth: `do`/`loop`, `begin`/`while`/`repeat`, `execute` (Trix's `exec`).
//   The stack-oriented control flow model where loops and conditionals
//   consume their arguments from the operand stack.
//
//   Common Lisp / Scheme: `try`/`catch` as structured exception handling
//   (Trix's try-catch uses exec-stack barriers rather than setjmp/longjmp).
//   `finally` for guaranteed cleanup.
//
// --- Core concepts for maintainers ---
//
// EXEC-STACK CONTROL FLOW
//   All control flow in Trix works by pushing and popping the execution
//   stack.  There is no goto, no branch instruction, no program counter.
//
//   Conditionals: `if` pushes a proc onto the exec stack (or doesn't).
//   Loops: push a continuation operator (@loop, @while, @for, etc.) plus
//     the loop body.  The continuation fires after the body completes,
//     checks the termination condition, and either pushes the next
//     iteration or does nothing (ending the loop).
//   Exit: `exit` unwinds the exec stack to the enclosing loop marker
//     and discards it, terminating the loop.
//   Stop: `stop` unwinds to the nearest `stopped` barrier (@stop marker).
//
// LOOP CONSTRUCTS
//   loop         -- infinite loop (exit with `exit`)
//   while        -- condition + body (test before each iteration)
//   do-while     -- body + condition (test after each iteration)
//   repeat       -- execute body N times
//   for          -- numeric iteration (init incr limit): supports all
//                   numeric types (byte, integer, uinteger, long, ulong,
//                   real, double, address) with type-specific control ops
//   for-all      -- iterate over container elements (array, packed, string,
//                   dict, set, record)
//
// ERROR HANDLING
//   Trix uses exec-stack barriers for structured error handling, avoiding
//   the overhead of setjmp/longjmp:
//
//   try          -- execute proc; on success push error name (/no-error),
//                   on failure push the error name
//   try-catch    -- execute proc; on error, look up handler by error name
//                   in a dict and execute it
//   finally      -- execute proc; run cleanup block whether proc succeeds
//                   or fails (re-raises the error after cleanup)
//   throw        -- raise a named error
//   throw-with   -- raise a named error with attached data
//   rethrow      -- re-raise the most recent error
//
//   Error barriers (@try-barrier, @try-catch-barrier, @finally-barrier)
//   are markers on the exec stack.  When an error occurs, error() in
//   api.inl unwinds the exec stack looking for these markers.  This is
//   analogous to C++ stack unwinding but entirely within the Trix VM.
//
//   ERROR FLOW (what happens when trx->error() is called):
//
//     1. error() formats the error message into m_error_string_base.
//     2. error() updates errordict: /error-name, /error-message,
//        /error-data, /command (the operator that raised the error).
//     3. error() captures the operand/dict/error stacks into
//        errordict /ostack, /dstack, /estack (for diagnostic access).
//     4. error() calls try_catch_handler() which UNWINDS the exec stack:
//
//        Exec stack unwinding (top to bottom):
//        +--------------------------------------------------+
//        |  user proc body elements (discarded)              |
//        |  @call + companions (discarded)                   |
//        |  loop continuations (discarded)                   |
//        |  ...                                              |
//        |  @try-catch-barrier  <-- FOUND: try-catch path    |
//        |    or                                             |
//        |  @try-barrier        <-- FOUND: try path          |
//        |    or                                             |
//        |  @finally-barrier    <-- FOUND: finally path      |
//        |    or                                             |
//        |  @stop               <-- FOUND: stopped path      |
//        |    or                                             |
//        |  (bottom of stack)   <-- no handler: global path  |
//        +--------------------------------------------------+
//
//        During unwinding, special entries are handled:
//        - @run: closes the associated stream (file cleanup)
//        - @with-stream: closes the scoped stream
//        - @coroutine-complete: kills the coroutine (error in coroutine)
//
//     5a. TRY-CATCH path: @catch-error fires.  It looks up the error
//         name in the handler dict.  If found, executes the handler proc
//         (another trampoline).  If not found, checks /default handler.
//         If no handler at all, re-raises (becomes global error).
//
//     5b. TRY path: try installs [saved-depth][@try-rollback][@try-barrier].
//         @try-barrier runs only on success (pushes /no-error).  On error
//         the unwinder cuts back at @try-barrier and pushes the error name
//         (e.g., /type-check), then @try-rollback rolls the operand stack
//         back to saved-depth and replants the error name on top.  User
//         code checks the pushed name.
//
//     5c. FINALLY path: @finally-barrier runs the cleanup proc, then
//         @finally-reraise re-raises the original error (cleanup is
//         guaranteed to run, but the error is not swallowed).
//
//     5d. STOPPED path: stop_handler() fires.  Pushes true to the
//         operand stack (indicating stopped-by-error vs normal stop).
//
//     5e. GLOBAL path: no barrier found.  Looks up the error name in
//         handlersdict.  If found, executes the handler.  If not, or if
//         already in the global handler (recursion guard), prints the
//         error to stderr and resumes the interpreter loop.
//
//     6. error() throws Exception::Error (C++ exception).
//     7. The interpreter loop's catch block catches it, resets
//        m_scanner_stream to nullptr, and resumes the loop.
//
// STOPPED / STOP
//   PostScript's mechanism for catching `stop` signals.  `stopped` wraps
//   a proc with a @stop barrier; `stop` unwinds to the nearest barrier
//   and pushes true.  If the proc completes normally, @stop pushes false.
//   This predates try/catch and is used for simpler flow control (e.g.,
//   breaking out of nested loops, cancelling a computation).
//
// DISPATCH OPERATORS
//   case         -- look up a key in a dict, execute the associated proc
//   type-case    -- dispatch on the type of a value (dict maps type names
//                   to handlers)
//   select       -- ternary: `true_val false_val bool select`
//
// BIND
//   `bind` walks a procedure and replaces executable names with their
//   current values (early binding).  This is PostScript's `bind` operator:
//   it locks in operator references at definition time rather than lookup
//   time, improving performance and preventing redefinition surprises.
//
// --- Operators ---
//
//   exec         any --                Execute a value
//   exec-n       any* int --           Execute N values from stack
//   if           bool proc --          Conditional execution
//   if-else      bool proc proc --     Two-branch conditional
//   select       any any bool -- any   Ternary selection
//   loop         proc --               Infinite loop
//   while        cond body --          Pre-test loop
//   do-while     body --               Post-test loop
//   repeat       int proc --           Execute N times
//   for          init incr limit proc --  Numeric iteration
//   for-all      container proc --     Container iteration
//   exit         --                    Break from loop
//   case         dict key --           Dict-based dispatch
//   type-case    val dict --           Type-based dispatch
//   bind         proc -- proc          Early-bind names in proc
//   stopped      proc -- bool          Execute with stop barrier
//   stop         --                    Unwind to stopped barrier
//   run          str --                Execute the named script file
//   quit         --                    Terminate interpreter
//   try          proc -- name          Execute with error capture
//   try-catch    dict proc --          Execute with error dispatch
//   finally      cleanup proc --       Execute with guaranteed cleanup
//   throw        name --               Raise error
//   throw-with   name any --           Raise error with data
//   rethrow      --                    Re-raise last error
//   and?         bool proc -- bool     Short-circuit AND
//   or?          bool proc -- bool     Short-circuit OR
//   set-global   bool --               Set global-allocation flag (no auto-restore)
//   current-global  -- bool            Push global-allocation flag
//   precondition bool --               Guard: error if false
//   postcondition check body --        Run body, check result, error if false
//
// Control operators (internal, not user-visible):
//   @run              Stream frame marker for error cleanup
//   @handler-done     Clear global error handler recursion guard
//   @stop             Stopped-proc completion (push false)
//   @loop             Loop continuation
//   @while            While-loop continuation (re-test condition)
//   @do-while         Do-while continuation (test after body)
//   @*-repeat         Repeat continuation (decrement counter)
//   @*-for            Typed for-loop continuations (8 numeric types)
//   @try-barrier      Try success path (push /no-error)
//   @try-catch-barrier  Try-catch success path
//   @catch-error      Try-catch error dispatch
//   @finally-barrier  Finally success path (run cleanup)
//   @finally-reraise  Finally error path (run cleanup, re-raise)
//   @repl-barrier     Top-level REPL error barrier
//   @repl-recover     Post-error REPL recovery (print + re-arm)
//   @try-rollback     Rolls operand stack to saved depth, replants /error-name
//   @in-global        ${...}-style force-global allocation opener
//   @in-local         $${...}-style force-local allocation opener
//   @end-in-global    Restores saved allocation flag (closer for both)
//   @ensure-check     Postcondition result check after body completes
//

// @run: (exec stack: stream)
// Marks a stream frame on exec stack for error cleanup.
static void at_run_op(Trix *) {
    // used to locate stream object on execution stack and close if an error occurs
}

// @handler-done: (exec stack: --)
// Clears the global error handler recursion guard.
// throws: (none)
static void at_handler_done_op(Trix *trx) {
    // sentinel pushed below the global error handler proc; clears the recursion
    // guard so subsequent unhandled errors can invoke the handler again
    trx->m_in_global_handler = false;
}

// @repl-barrier: (exec stack: --)
// Top-level error barrier installed by Stream::init for Interactive modes.
// No-op under normal execution; its purpose is to stop error unwind so the
// REPL survives uncaught errors.  try_catch_handler rewrites it to
// @repl-recover when an error reaches the barrier.
static void at_replbarrier_op(Trix *) {
}

// @repl-recover: failing-op name :- --
// Replacement for @repl-barrier after an error was caught.  Prints the error,
// re-arms [@repl-barrier, @Run, stdedit] so the REPL continues reading.
// Quit remains below the barrier so `quit` / ^D still exit cleanly.
// try_catch_handler pushes the failing op below /error-name because
// m_last_operator_ptr is clobbered by dispatch before this op runs.
static void at_replrecover_op(Trix *trx) {
    trx->verify_operands(Trix::VerifyName, Trix::VerifyOperator);
    auto errname_obj = *trx->m_op_ptr--;
    auto failed_op_obj = *trx->m_op_ptr--;

    auto errstr = errname_obj.name_value(trx)->sv();
    auto opstr = failed_op_obj.operator_string(trx);
    auto msgstr = trx->m_last_error_msg_ptr->sv_value(trx);
    trx->diag_println("Trix {} '{}': {}", errstr, opstr, msgstr);

    // Without a live stdedit there is nowhere to re-arm; let the exec stack
    // drain and the interpreter will block in wait_for_exec_ready.  Only
    // reachable via bizarre runtime mutation; construction-time checks in
    // Stream::init guarantee m_stdedit != nullptr when the barrier is pushed.
    if (trx->m_stdedit != nullptr) {
        trx->require_exec_capacity(3);
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atReplBarrier);
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atRun);
        auto offset = trx->ptr_to_offset(trx->m_stdedit);
        *++trx->m_exec_ptr = Object::make_stream(offset, trx->m_stdedit->sid(), Object::ExecutableAttrib, Object::ReadOnlyAccess);

        // Clear m_last_operator_ptr so the next error -- if raised before any
        // real op runs (undefined-name, scanner failure) -- doesn't report this
        // recovery op as the culprit.  Mirrors the interpreter's catch-block reset.
        *trx->m_last_operator_ptr = Object::make_operator(SystemName::Interpreter);
    }
}

// @stop: :- bool
// Pushes false when stopped proc completes without stop.
// throws: opstack-overflow
static void at_stop_op(Trix *trx) {
    trx->require_op_capacity(1);

    *++trx->m_op_ptr = Object::make_boolean(false);
}

// @in-global: proc :- -- (scanner-emit-only; runtime side of ${...})
// Pops the body proc from operand stack, saves the prior
// m_curr_alloc_global on err stack, sets the flag true, pushes
// @end-in-global + body-proc on exec stack so the body runs under the
// global allocation flag.  The matching @end-in-global pops the saved
// flag and restores it.  On error unwind, try_catch_handler restores
// the flag from err stack and keeps searching.
//
// Not registered in systemdict (lives in the control-op range, where
// init.inl skips name registration).  The scanner emits this op after
// a ${...} block's body.
//
// throws: opstack-underflow, type-check, execstack-overflow, errstack-overflow
static void in_global_op(Trix *trx) {
    trx->verify_operands(VerifyProc);
    trx->require_exec_capacity(2);
    trx->require_error_capacity(1);

    auto proc = *trx->m_op_ptr--;

    *++trx->m_err_ptr = Object::make_boolean(trx->m_curr_alloc_global);
    trx->m_curr_alloc_global = true;

    *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atEndInGlobal);
    *++trx->m_exec_ptr = proc;
}

// @end-in-global: (normal-path barrier) restores m_curr_alloc_global
// from the saved Boolean on the err stack.  Shared closer for both
// @in-global and @in-local -- the saved Boolean carries whichever
// value the matching opener captured.
// throws: (none)
static void at_end_in_global_op(Trix *trx) {
    auto saved_flag_obj = *trx->m_err_ptr--;
    trx->m_curr_alloc_global = saved_flag_obj.boolean_value();
}

// @in-local: proc :- -- (scanner-emit-only; runtime side of $${...})
// Symmetric inverse of @in-global: pops the body proc, saves the prior
// m_curr_alloc_global on err stack, sets the flag FALSE, pushes
// @end-in-global + body-proc on exec stack so the body runs under the
// force-local allocation flag.  The matching @end-in-global pops the
// saved flag and restores it.  On error unwind, try_catch_handler
// restores the flag from err stack and keeps searching.
//
// throws: opstack-underflow, type-check, execstack-overflow, errstack-overflow
static void in_local_op(Trix *trx) {
    trx->verify_operands(VerifyProc);
    trx->require_exec_capacity(2);
    trx->require_error_capacity(1);

    auto proc = *trx->m_op_ptr--;

    *++trx->m_err_ptr = Object::make_boolean(trx->m_curr_alloc_global);
    trx->m_curr_alloc_global = false;

    *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atEndInGlobal);
    *++trx->m_exec_ptr = proc;
}

// set-global: bool :- --
// Direct surface for the global allocation flag.  Unlike ${...},
// set-global does NOT save/restore the previous value on a body's
// completion -- the caller is responsible for pairing it with a
// matching restore.  Prefer ${...} for scoped use; reach for set-global
// when the flag must span procedure boundaries or be driven by a
// runtime Boolean.
// throws: opstack-underflow, type-check
static void set_global_op(Trix *trx) {
    trx->verify_operands(VerifyBoolean);
    trx->m_curr_alloc_global = trx->m_op_ptr->boolean_value();
    --trx->m_op_ptr;
}

// current-global: :- bool
// Pushes the current value of m_curr_alloc_global.
// throws: opstack-overflow
static void current_global_op(Trix *trx) {
    trx->require_op_capacity(1);

    *++trx->m_op_ptr = Object::make_boolean(trx->m_curr_alloc_global);
}

// @loop: (exec stack: proc)
// Re-pushes proc and @loop for the next iteration.
// throws: execstack-overflow
static void at_loop_op(Trix *trx) {
    trx->require_exec_capacity(2);

    auto proc_ptr = trx->m_exec_ptr;
    *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atLoop);
    *++trx->m_exec_ptr = *proc_ptr;
}

// @while: (exec stack: body cond)
// Pops bool; if true re-pushes frame, if false cleans up.
// throws: execstack-overflow
static void at_while_op(Trix *trx) {
    trx->verify_operands(VerifyBoolean);
    auto keep = trx->m_op_ptr->boolean_value();
    --trx->m_op_ptr;

    if (keep) {
        trx->require_exec_capacity(3);

        auto cond_ptr = trx->m_exec_ptr;
        auto body_ptr = (cond_ptr - 1);

        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atWhile);
        *++trx->m_exec_ptr = *cond_ptr;
        *++trx->m_exec_ptr = *body_ptr;
    } else {
        trx->m_exec_ptr -= 2;
    }
}

// @uinteger-repeat: (exec stack: proc count)
// Decrements count; if non-zero re-pushes frame and proc.
// throws: execstack-overflow
static void at_uinteger_repeat_op(Trix *trx) {
    auto count_ptr = trx->m_exec_ptr;
    auto count = count_ptr->uinteger_value();
    if (count == 0) {
        trx->m_exec_ptr -= 2;
    } else {
        trx->require_exec_capacity(2);

        auto proc_ptr = count_ptr - 1;
        count_ptr->update_uinteger(count - 1);
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atUIntegerRepeat);
        *++trx->m_exec_ptr = *proc_ptr;
    }
}

// @ulong-repeat: (exec stack: proc count)
// Decrements count; if non-zero re-pushes frame and proc.
// throws: execstack-overflow
static void at_ulong_repeat_op(Trix *trx) {
    auto count_ptr = trx->m_exec_ptr;
    auto count = count_ptr->ulong_value(trx);
    if (count == 0) {
        count_ptr->free_extvalue(trx);
        trx->m_exec_ptr -= 2;
    } else {
        trx->require_exec_capacity(2);

        auto proc_ptr = count_ptr - 1;
        count_ptr->update_ulong(trx, count - 1);
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atULongRepeat);
        *++trx->m_exec_ptr = *proc_ptr;
    }
}

// @*-for: (exec stack: proc limit incr control)
// Typed for-loop continuation. Pushes control value, increments control,
// re-pushes frame if not past limit. Handles overflow/underflow by forcing
// exit on the next iteration.
// throws: vm-full, execstack-overflow, opstack-overflow
template<typename T, SystemName SN>
static void at_typed_for_op(Trix *trx) {
    auto control_ptr = trx->m_exec_ptr;
    auto increment_ptr = (control_ptr - 1);
    auto limit_ptr = (increment_ptr - 1);

    // extract values (uses ExtValue for Long, ULong, Double)
    T control, increment, limit;
    if constexpr (std::is_same_v<T, vm_t>) {
        control = control_ptr->byte_value();
        increment = increment_ptr->byte_value();
        limit = limit_ptr->byte_value();
    } else if constexpr (std::is_same_v<T, integer_t>) {
        control = control_ptr->integer_value();
        increment = increment_ptr->integer_value();
        limit = limit_ptr->integer_value();
    } else if constexpr (std::is_same_v<T, uinteger_t>) {
        control = control_ptr->uinteger_value();
        increment = increment_ptr->uinteger_value();
        limit = limit_ptr->uinteger_value();
    } else if constexpr (std::is_same_v<T, long_t>) {
        control = control_ptr->long_value(trx);
        increment = increment_ptr->long_value(trx);
        limit = limit_ptr->long_value(trx);
    } else if constexpr (std::is_same_v<T, ulong_t>) {
        control = control_ptr->ulong_value(trx);
        increment = increment_ptr->ulong_value(trx);
        limit = limit_ptr->ulong_value(trx);
    } else if constexpr (std::is_same_v<T, real_t>) {
        control = control_ptr->real_value();
        increment = increment_ptr->real_value();
        limit = limit_ptr->real_value();
    } else {
        static_assert(std::is_same_v<T, double_t>);
        control = control_ptr->double_value(trx);
        increment = increment_ptr->double_value(trx);
        limit = limit_ptr->double_value(trx);
    }

    // termination check: signed/float types are bidirectional, unsigned always ascending
    bool done;
    if constexpr (std::is_signed_v<T> || std::is_floating_point_v<T>) {
        done = (increment > T{0}) ? (control > limit) : (control < limit);
    } else {
        done = (control > limit);
    }

    if (done) {
        // free ExtValues for types that use them
        if constexpr (std::is_same_v<T, long_t> || std::is_same_v<T, ulong_t> || std::is_same_v<T, double_t>) {
            control_ptr->free_extvalue(trx);
            increment_ptr->free_extvalue(trx);
            limit_ptr->free_extvalue(trx);
        }
        trx->m_exec_ptr -= 4;
    } else {
        trx->require_op_capacity(1);
        trx->require_exec_capacity(2);

        // push control value to operand stack
        if constexpr (std::is_same_v<T, vm_t>) {
            *++trx->m_op_ptr = Object::make_byte(control);
        } else if constexpr (std::is_same_v<T, integer_t>) {
            *++trx->m_op_ptr = Object::make_integer(control);
        } else if constexpr (std::is_same_v<T, uinteger_t>) {
            *++trx->m_op_ptr = Object::make_uinteger(control);
        } else if constexpr (std::is_same_v<T, long_t>) {
            *++trx->m_op_ptr = Object::make_long(trx, control);
        } else if constexpr (std::is_same_v<T, ulong_t>) {
            *++trx->m_op_ptr = Object::make_ulong(trx, control);
        } else if constexpr (std::is_same_v<T, real_t>) {
            *++trx->m_op_ptr = Object::make_real(control);
        } else {
            *++trx->m_op_ptr = Object::make_double(trx, control);
        }

        // update control and check for overflow/underflow
        if constexpr (std::is_floating_point_v<T>) {
            // floats: no overflow check
            if constexpr (std::is_same_v<T, real_t>) {
                control_ptr->update_real(control + increment);
            } else {
                control_ptr->update_double(trx, control + increment);
            }
        } else {
            // Unsigned wrapping addition, then cast back to T.  If wrap-around
            // occurred (detected below), force exit by clamping limit.
            using U = std::make_unsigned_t<T>;
            auto updated_control = static_cast<T>(static_cast<U>(control) + static_cast<U>(increment));

            if constexpr (std::is_same_v<T, vm_t>) {
                control_ptr->update_byte(updated_control);
            } else if constexpr (std::is_same_v<T, integer_t>) {
                control_ptr->update_integer(updated_control);
            } else if constexpr (std::is_same_v<T, uinteger_t>) {
                control_ptr->update_uinteger(updated_control);
            } else if constexpr (std::is_same_v<T, long_t>) {
                control_ptr->update_long(trx, updated_control);
            } else {
                static_assert(std::is_same_v<T, ulong_t>);
                control_ptr->update_ulong(trx, updated_control);
            }

            // Detect overflow/underflow by comparing updated vs original:
            // if positive increment but updated < original, the addition wrapped.
            // On wrap, force the NEXT fire to terminate by restoring the
            // pre-update control and clamping limit just past it.  Clamping limit
            // to updated_control +/- 1 (the obvious approach) FAILS when
            // updated_control is the type extreme: e.g. a byte wrapping exactly to
            // 0 gives 0 - 1 == 255, leaving limit unchanged at the max -> infinite
            // loop; and in the signed domain updated_control +/- 1 is itself
            // overflow UB at INT_MIN/INT_MAX.  The pre-update control is always in
            // range on a wrap (the extreme cannot have been reached without a
            // prior termination), so control - 1 (ascending) / control + 1
            // (descending) are well-defined and make the next (control vs limit)
            // test unconditionally true.  The value pushed for the current
            // iteration was already taken from the pre-update control, so
            // restoring it here only affects the next fire.
            if constexpr (std::is_signed_v<T>) {
                if (increment > T{0}) {
                    if (updated_control < control) {
                        if constexpr (std::is_same_v<T, integer_t>) {
                            control_ptr->update_integer(control);
                            limit_ptr->update_integer(static_cast<integer_t>(control - 1));
                        } else {
                            control_ptr->update_long(trx, control);
                            limit_ptr->update_long(trx, static_cast<long_t>(control - 1));
                        }
                    }
                } else if (updated_control > control) {
                    if constexpr (std::is_same_v<T, integer_t>) {
                        control_ptr->update_integer(control);
                        limit_ptr->update_integer(static_cast<integer_t>(control + 1));
                    } else {
                        control_ptr->update_long(trx, control);
                        limit_ptr->update_long(trx, static_cast<long_t>(control + 1));
                    }
                }
            } else if (updated_control < control) {
                if constexpr (std::is_same_v<T, vm_t>) {
                    control_ptr->update_byte(control);
                    limit_ptr->update_byte(static_cast<vm_t>(control - 1));
                } else if constexpr (std::is_same_v<T, uinteger_t>) {
                    control_ptr->update_uinteger(control);
                    limit_ptr->update_uinteger(control - 1);
                } else {
                    control_ptr->update_ulong(trx, control);
                    limit_ptr->update_ulong(trx, control - 1);
                }
            }
        }

        auto proc_obj = limit_ptr[-1];
        *++trx->m_exec_ptr = Object::make_control_operator(SN);
        *++trx->m_exec_ptr = proc_obj;
    }
}

static void at_byte_for_op(Trix *trx) {
    at_typed_for_op<vm_t, SystemName::atByteFor>(trx);
}

static void at_integer_for_op(Trix *trx) {
    at_typed_for_op<integer_t, SystemName::atIntegerFor>(trx);
}

static void at_uinteger_for_op(Trix *trx) {
    at_typed_for_op<uinteger_t, SystemName::atUIntegerFor>(trx);
}

static void at_long_for_op(Trix *trx) {
    at_typed_for_op<long_t, SystemName::atLongFor>(trx);
}

static void at_ulong_for_op(Trix *trx) {
    at_typed_for_op<ulong_t, SystemName::atULongFor>(trx);
}

static void at_real_for_op(Trix *trx) {
    at_typed_for_op<real_t, SystemName::atRealFor>(trx);
}

static void at_double_for_op(Trix *trx) {
    at_typed_for_op<double_t, SystemName::atDoubleFor>(trx);
}

// @address-for: (exec stack: proc limit incr control)
// Address for-loop uses integer increment with address control/limit.
// throws: vm-full, execstack-overflow, opstack-overflow
static void at_address_for_op(Trix *trx) {
    auto control_ptr = trx->m_exec_ptr;
    auto increment_ptr = (control_ptr - 1);
    auto limit_ptr = (increment_ptr - 1);
    // Address loop values are arbitrary user integers (0 is a legitimate
    // bound), so ALL arithmetic and ordering happens in the unsigned-integer
    // domain: pointer arithmetic on values that are not live allocations --
    // null in particular -- is undefined behavior (UBSan: "applying non-zero
    // offset to null pointer"), and the wraparound detection below depends
    // on well-defined unsigned overflow that pointer types do not provide.
    auto control = reinterpret_cast<uintptr_t>(control_ptr->address_value(trx));
    auto increment = increment_ptr->integer_value();
    auto limit = reinterpret_cast<uintptr_t>(limit_ptr->address_value(trx));
    if ((increment > 0) ? (control > limit) : (control < limit)) {
        control_ptr->free_extvalue(trx);
        limit_ptr->free_extvalue(trx);
        trx->m_exec_ptr -= 4;
    } else {
        trx->require_op_capacity(1);
        trx->require_exec_capacity(2);

        *++trx->m_op_ptr = Object::make_address(trx, reinterpret_cast<address_t>(control));

        // increment is in units of vm_t (bytes)
        auto updated_control = (control + static_cast<uintptr_t>(increment));
        auto wrapped = (increment > 0) ? (updated_control < control) : (updated_control > control);
        if (wrapped) {
            // The counter stepped past the address-space edge: the value just
            // pushed is the final iteration.  Rewrite control/limit to a
            // canonical always-exits pair for the increment's direction.  (The
            // previous approach clamped limit to updated_control -/+ 1, which
            // itself wraps when the step lands exactly on 0 or the max
            // address -- e.g. `2a -1 0a ... for` reached 0, stepped to MAX,
            // clamped limit to MAX+1 == 0, and looped forever.)
            control_ptr->update_address(trx, reinterpret_cast<address_t>(uintptr_t{(increment > 0) ? 1u : 0u}));
            limit_ptr->update_address(trx, reinterpret_cast<address_t>(uintptr_t{(increment > 0) ? 0u : 1u}));
        } else {
            control_ptr->update_address(trx, reinterpret_cast<address_t>(updated_control));
        }

        auto proc_obj = limit_ptr[-1];
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atAddressFor);
        *++trx->m_exec_ptr = proc_obj;
    }
}

// bind: proc :- proc
// Resolves executable names in proc to their operator values.
// throws: vm-full, opstack-underflow, type-check
static void bind_op(Trix *trx) {
    trx->verify_operands(VerifyProc);

    auto top_ptr = trx->m_op_ptr;
    if (top_ptr->is_packed()) {
        auto save_policy =
                (top_ptr->save_level() != trx->m_curr_save_level) ? Object::SavePolicy::Save : Object::SavePolicy::DoNotSave;
        Object::bind_packed(trx, *top_ptr, save_policy);
    } else {
        auto [object_base, count] = top_ptr->array_value(trx);
        auto kind = top_ptr->is_eqarray(trx) ? Object::ArrayKind::EqArray : Object::ArrayKind::Normal;
        Object::bind_array(trx, object_base, count, kind);
    }
}

// case: dict key :- any
// Looks up key in dict and executes or returns the value.
// throws: execstack-overflow, opstack-underflow, type-check, undefined-case
static void case_op(Trix *trx) {
    trx->verify_operands(VerifyKey, VerifyDict);

    auto key_ptr = trx->m_op_ptr;
    auto container_ptr = (key_ptr - 1);

    auto dict = container_ptr->dict_value(trx);
    auto value = dict->get(trx, key_ptr);
    if (value == nullptr) {
        auto default_name_obj = trx->wellknown_name(WellKnownName::Default);
        value = dict->get(trx, &default_name_obj);
    }

    if (value == nullptr) {
        trx->error(Error::UndefinedCase, "missing default key in case container");
    } else if (value->is_executable() && value->honors_execute()) {
        trx->require_exec_capacity(1);

        key_ptr->maybe_free_extvalue(trx);
        trx->m_op_ptr -= 2;

        // push on execute stack
        *++trx->m_exec_ptr = value->make_copy();
    } else {
        key_ptr->maybe_free_extvalue(trx);

        // push a clone on operand stack
        *--trx->m_op_ptr = value->make_clone(trx);
    }
}

// exec: any :- --
// Pushes an executable object onto the exec stack for execution.
// Note: exec does not create an @call frame.  When a proc pushed by exec
// contains a name in tail position, the interpreter will unroll the proc
// via execute_proc and then dispatch the name via execute_name, which sees
// the enclosing @call frame and applies TCO.  For anonymous self-recursion
// (dup exec), no @call frames are involved at all, so exec-based recursion
// is inherently constant-space on the exec stack.
// throws: execstack-overflow, opstack-underflow
static void exec_op(Trix *trx) {
    trx->require_op_count(1);

    auto object = *trx->m_op_ptr;
    if (object.is_executable() && object.honors_execute()) {
        trx->require_exec_capacity(1);

        *++trx->m_exec_ptr = object;
        --trx->m_op_ptr;
    }
}

// exec-n: any1 ... anyN int :- --
// Pushes N objects onto the exec stack for sequential execution.
// throws: execstack-overflow, opstack-underflow, range-check, type-check
static void execn_op(Trix *trx) {
    trx->verify_operands(VerifyIntegers | VerifyNotNegative);

    auto n_ptr = trx->m_op_ptr;
    auto [valid, n] = n_ptr->integer_value(trx, 0, std::numeric_limits<stack_depth_t>::max() - 1);
    if (valid) {
        trx->require_op_count(n + 1);
        trx->require_exec_capacity(n);

        n_ptr->maybe_free_extvalue(trx);
        auto end_ptr = (n_ptr - n);
        // reverse-copy so the first operand ends up at exec stack top (executed first, LIFO)
        std::reverse_copy(end_ptr, n_ptr, (trx->m_exec_ptr + 1));
        trx->m_exec_ptr += n;
        trx->m_op_ptr -= (n + 1);
    } else {
        trx->error(Error::RangeCheck, "exec-n: count out of range");
    }
}

// quit: :- --
// Terminates the interpreter synchronously.  External threads still use
// raise_interrupt(ExitIRQ) for asynchronous shutdown; that IRQ is delivered
// when the interpreter returns to its outer loop, which cannot happen while
// stdedit is blocked inside readline.  Trix-side `quit` must be synchronous
// so the REPL exits on user request.
// throws: exit
[[noreturn]] static void quit_op(Trix *) {
    throw Exception::Exit;
}

// unwind_exec_to: walks the exec stack from m_exec_ptr down to (target + 1),
// performing the cleanup side effects associated with each control operator
// in the discarded range:
//
//   - @try-catch-barrier, @finally-barrier, @delimit-barrier, @effect-barrier:
//     pop one err companion.
//   - @handler-scope: pop two err companions (identity + op-depth).
//   - @with-stream: pop err companion, close the stream if still open.
//   - @end-in-global: pop err companion (the saved alloc flag) and restore
//     m_curr_alloc_global from it.
//   - @run: close the stream at (scan + 1) if its sid still matches.
//   - @end-locals / other dict-pop: pop dict-stack entry, clear cached name
//     bindings, recycle the dict if it's a frame dict.
//   - Non-operator slots: free any ExtValue/WideValue heap cell.
//
// On exit m_exec_ptr = target.  Does NOT touch the slot AT target -- the caller
// decides how to handle it (drop, replace, popcount-adjust, splice above, etc).
// Safe to call with target == m_exec_ptr (no-op).  Shared by exit_op, stop_op,
// and abort-exec.
static void unwind_exec_to(Trix *trx, Object *target) {
    for (auto scan = trx->m_exec_ptr; scan > target; --scan) {
        if (scan->is_operator()) {
            if (scan->operator_is_trycatchbarrier() || scan->operator_is_finallybarrier() || scan->operator_is_delimitbarrier() ||
                scan->operator_is_effectbarrier()) {
                // Each contributes one err companion: try-catch/finally = handler,
                // delimit = op-depth, effect = handler-dict.
                if (trx->m_err_ptr >= trx->m_err_base) {
                    --trx->m_err_ptr;
                }
            } else if (scan->operator_is_endinglobal()) {
                // One companion: the saved alloc flag.  Pop it and restore
                // m_curr_alloc_global (mirror at_end_in_global_op) so the trim
                // leaves allocation routing correct.
                if (trx->m_err_ptr >= trx->m_err_base) {
                    auto saved_flag_obj = *trx->m_err_ptr--;
                    trx->m_curr_alloc_global = saved_flag_obj.boolean_value();
                }
            } else if (scan->operator_is_handlerscope()) {
                // @handler-scope: two companions (identity + op-depth).
                if ((trx->m_err_ptr - 1) >= trx->m_err_base) {
                    trx->m_err_ptr -= 2;
                }
            } else if (scan->operator_is_withstream()) {
                if (trx->m_err_ptr >= trx->m_err_base) {
                    auto stream_obj = *trx->m_err_ptr--;
                    auto [stream, sid] = stream_obj.stream_value(trx);
                    if (stream->is_open(sid)) {
                        stream->close(trx);
                    }
                }
            } else if (scan->operator_is_run()) {
                auto stream_ptr = (scan + 1);
                auto [stream, sid] = stream_ptr->stream_value(trx);
                if (stream->sid() == sid) {
                    stream->close(trx);
                }
            } else if (scan->operator_is_dictpop()) {
                if (trx->m_dict_ptr > (trx->m_dict_base + PermanentDictCount - 1)) {
                    auto dict_offset = trx->m_dict_ptr->m_dict;
                    auto dict = trx->offset_to_ptr<Dict>(dict_offset);
                    --trx->m_dict_ptr;
                    dict->clear_name_bindings(trx);
                    if (scan->operator_is_endlocals()) {
                        Dict::recycle(trx, dict, dict_offset);
                    }
                }
            }
        } else {
            scan->maybe_free_extvalue(trx);
        }
    }
    trx->m_exec_ptr = target;
}

// exit: :- --
// Exits the innermost enclosing loop/repeat/for/for-all.
// throws: invalid-exit
static void exit_op(Trix *trx) {
    // Pass 1: scan without side effects to find the loop control operator.
    // Do NOT modify error stack or free ExtValues here -- if no loop is found, error() needs them intact.
    auto ptr = trx->m_exec_ptr;
    while (ptr >= trx->m_exec_base) {
        if (ptr->is_operator()) {
            // Fence check FIRST: an op can be both a control op (so
            // exec-validate bounds-checks its companions via popcount) and
            // an exit fence (match-all's accumulation Mark cannot be
            // reconciled by exit).  @run and @stop are pure fences.
            if (ptr->operator_is_invalid_exit()) {
                trx->error(Error::InvalidExit, "exit: cannot cross a @run, @stop, or match-all boundary");
            } else if (ptr->operator_is_control()) {
                break;
            }
        }
        --ptr;
    }
    if (ptr < trx->m_exec_base) {
        trx->error(Error::InvalidExit, "exit: no enclosing loop found");
    } else {
        // Pass 2: unwind the range above the loop control operator.
        // @loop,
        // @uinteger-repeat, @ulong-repeat,
        // @array-for-all, @packed-for-all, @string-for-all, @dict-for-all,
        // @byte-for, @integer-for, @uinteger-for, @long-for, @ulong-for, @address-for, @real-for, @double-for
        unwind_exec_to(trx, ptr);
        // Pop the loop control operator and its frame slots (count varies by operator type).
        // Free any ExtValues in the discarded range before adjusting the stack pointer.
        auto new_ptr = (ptr - ptr->operator_popcount());
        maybe_free_extvalue_execstack(trx, new_ptr + 1);
        trx->m_exec_ptr = new_ptr;
    }
}

//              enter  exit
//              -----  ----
// m_exec_ptr-> -      @xxxfor
//                     control
//                     increment
//                     limit
//                     proc
// for: init incr limit proc :- --
// Executes proc for each value from init to limit by incr.
// throws: execstack-overflow, opstack-underflow, type-check
static void for_op(Trix *trx) {
    // Int128/UInt128 loop counters aren't supported (no @int128-for /
    // @uint128-for continuations); exclude them at the verify boundary
    // rather than via an explicit post-check.
    auto VerifyForNums = (VerifyIntegers64 | VerifyFloats | VerifyAddress);
    auto VerifyForIncr = (VerifyIntegers64 | VerifyFloats);
    trx->verify_operands(VerifyProc, VerifyForNums, VerifyForIncr, VerifyForNums);
    trx->require_exec_capacity(5);

    auto proc = trx->m_op_ptr;
    auto limit = (proc - 1);
    auto increment = (limit - 1);
    auto initial = (increment - 1);
    auto initial_type = initial->type();
    // when initial and limit are address, increment must be an integer
    if ((initial->type() != limit->type()) || (initial->is_address() && !increment->is_integer()) ||
        (!initial->is_address() && (increment->type() != initial->type()))) {
        trx->error(Error::TypeCheck, "for: initial, increment, and limit types are incompatible");
    } else {
        // Reject parameters that guarantee an infinite loop.
        if (initial->is_floating_point()) {
            if (initial->is_floating_point_nan(trx) || increment->is_floating_point_nan(trx) || limit->is_floating_point_nan(trx)) {
                trx->error(Error::RangeCheck, "for: NaN parameter would cause an infinite loop");
            }
        }
        if (increment->is_numeric_zero(trx)) {
            trx->error(Error::RangeCheck, "for: zero increment would cause an infinite loop");
        } else {
            *++trx->m_exec_ptr = *proc;
            *++trx->m_exec_ptr = *limit;
            *++trx->m_exec_ptr = *increment;
            *++trx->m_exec_ptr = *initial;
            trx->m_op_ptr = (initial - 1);

            switch (initial_type) {
            case Object::Type::Byte:
                *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atByteFor);
                break;

            case Object::Type::Integer:
                *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atIntegerFor);
                break;

            case Object::Type::UInteger:
                *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atUIntegerFor);
                break;

            case Object::Type::Long:
                *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atLongFor);
                break;

            case Object::Type::ULong:
                *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atULongFor);
                break;

            case Object::Type::Address:
                *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atAddressFor);
                break;

            case Object::Type::Real:
                *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atRealFor);
                break;

            case Object::Type::Double:
                *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atDoubleFor);
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
            case Object::Type::Int128:
            case Object::Type::UInt128:
            case Object::Type::OpaqueHandle:
                // verify_operands already rejects these; unreachable.
                trx->error(Error::InternalError, "for: unreachable type {}", Object::type_sv(initial_type));
            }
        }
    }
}

// for-all: container proc :- --
// Executes proc for each element of array, packed, string, or dict.
// Each iteration verifies that the body returned the
// operand stack to the depth it had before the iteration's value(s)
// were pushed.  A body that left extra values (skip-without-pop) or
// over-consumed (popped past the iter values into the caller's frame)
// raises /range-check.  The saved-depth slot is pushed below the
// @*-for-all control op so each fire can compare current depth.
// throws: execstack-overflow, opstack-underflow, type-check
static void forall_op(Trix *trx) {
    trx->verify_operands(VerifyProc, VerifyArrays | VerifyString | VerifyDict | VerifySet | VerifyRecord);

    auto proc = trx->m_op_ptr;
    auto container = (proc - 1);
    if (container->length(trx) != 0) {
        // Saved-depth captures the operand-stack depth AFTER popping
        // (proc, container) -- i.e. the caller's pre-call depth -- so
        // each @*-for-all fire can verify the body's stack effect
        // matches "<iter-values> -- " (depth returns to this value).
        auto saved_depth = static_cast<integer_t>((trx->m_op_ptr - 2) - trx->m_op_base);
        if (container->is_record()) {
            trx->require_exec_capacity(5);

            *++trx->m_exec_ptr = *proc;
            *++trx->m_exec_ptr = *container;
            *++trx->m_exec_ptr = Object::make_uinteger(0);  // field index
            *++trx->m_exec_ptr = Object::make_integer(saved_depth);
            *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atRecordForAll);
        } else if (container->is_dict() || container->is_set()) {
            trx->require_exec_capacity(6);

            *++trx->m_exec_ptr = *proc;
            *++trx->m_exec_ptr = *container;
            *++trx->m_exec_ptr = Object::make_integer(-1);  // bucket_idx of -1 signals first call
            *++trx->m_exec_ptr = Object::make_uinteger(0);
            *++trx->m_exec_ptr = Object::make_integer(saved_depth);
            if (container->is_dict()) {
                *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atDictForAll);
            } else {
                *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atSetForAll);
            }
        } else {
            trx->require_exec_capacity(4);

            *++trx->m_exec_ptr = *proc;
            *++trx->m_exec_ptr = *container;
            *++trx->m_exec_ptr = Object::make_integer(saved_depth);
            if (container->is_array()) {
                *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atArrayForAll);
            } else if (container->is_string()) {
                *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atStringForAll);
            } else {
                *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atPackedForAll);
            }
        }
    }
    trx->m_op_ptr -= 2;
}

// if: bool proc :- --
// Executes proc if bool is true.
// throws: execstack-overflow, opstack-underflow, type-check
static void if_op(Trix *trx) {
    trx->verify_operands(VerifyProc, VerifyBoolean);

    auto proc_ptr = trx->m_op_ptr;
    auto boolean_ptr = (proc_ptr - 1);
    if (boolean_ptr->boolean_value()) {
        trx->require_exec_capacity(1);

        *++trx->m_exec_ptr = *proc_ptr;
    }
    trx->m_op_ptr -= 2;
}

// and?: bool proc :- bool
// Short-circuit boolean AND: if the bool is false, leave it on the
// stack and drop proc unevaluated; otherwise drop the bool, execute
// proc, and let its boolean result take the bool's slot.  Complement
// of or? for guarded expressions such as bounded-array access:
//     i len lt { src i get 10b ne } and?
// which evaluates src[i] only when the bounds hold.  Proc's output is
// trusted (no bool-type check), matching if-else's convention.
// throws: execstack-overflow, opstack-underflow, type-check
static void short_and_op(Trix *trx) {
    trx->verify_operands(VerifyProc, VerifyBoolean);

    auto proc_ptr = trx->m_op_ptr;
    auto boolean_ptr = (proc_ptr - 1);
    if (boolean_ptr->boolean_value()) {
        // true: drop bool and proc; schedule proc; its bool result
        // lands where the bool was.
        trx->require_exec_capacity(1);

        *++trx->m_exec_ptr = *proc_ptr;
        trx->m_op_ptr -= 2;
    } else {
        // false: drop proc only; the existing false remains on top.
        --trx->m_op_ptr;
    }
}

// or?: bool proc :- bool
// Short-circuit boolean OR: if the bool is true, leave it on the
// stack and drop proc unevaluated; otherwise drop the bool and
// execute proc, letting its boolean result take the slot.  Same
// trust-the-proc convention as and? and if-else.
// throws: execstack-overflow, opstack-underflow, type-check
static void short_or_op(Trix *trx) {
    trx->verify_operands(VerifyProc, VerifyBoolean);

    auto proc_ptr = trx->m_op_ptr;
    auto boolean_ptr = (proc_ptr - 1);
    if (!boolean_ptr->boolean_value()) {
        trx->require_exec_capacity(1);

        *++trx->m_exec_ptr = *proc_ptr;
        trx->m_op_ptr -= 2;
    } else {
        --trx->m_op_ptr;
    }
}

// if-else: bool proc1 proc2 :- --
// Executes proc1 if bool is true, proc2 if false.
// throws: execstack-overflow, opstack-underflow, type-check
static void ifelse_op(Trix *trx) {
    trx->verify_operands(VerifyProc, VerifyProc, VerifyBoolean);
    trx->require_exec_capacity(1);

    auto else_ptr = trx->m_op_ptr;
    auto if_ptr = (else_ptr - 1);
    auto boolean = (if_ptr - 1);
    *++trx->m_exec_ptr = boolean->boolean_value() ? *if_ptr : *else_ptr;
    trx->m_op_ptr -= 3;
}

// select: true_val false_val bool :- result
// Returns true_val if bool is true, false_val if false.
// Value-level conditional selection (non-short-circuit).
// throws: opstack-underflow, type-check
static void select_op(Trix *trx) {
    trx->verify_operands(VerifyBoolean, VerifyAny, VerifyAny);

    auto cond = trx->m_op_ptr;
    auto false_val = (cond - 1);
    auto true_val = (cond - 2);
    if (cond->boolean_value()) {
        false_val->maybe_free_extvalue(trx);
    } else {
        true_val->maybe_free_extvalue(trx);
        *true_val = *false_val;
    }
    trx->m_op_ptr -= 2;
}

//              enter  exit
//              -----  ----
// m_exec_ptr-> -      body
//                     @do-while
//                     body
// do-while: body :- --
// Executes body, then tests the boolean it leaves. Loops while true.
// The body always executes at least once.
// throws: execstack-overflow, opstack-underflow, type-check
static void dowhile_op(Trix *trx) {
    trx->verify_operands(VerifyProc);
    trx->require_exec_capacity(3);

    auto body_obj = *trx->m_op_ptr--;

    // frame: [body] [@do-while] [body-to-exec]
    *++trx->m_exec_ptr = body_obj;
    *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atDoWhile);
    *++trx->m_exec_ptr = body_obj;
}

// @do-while: (exec stack: body)
// Tests boolean on operand stack. If true, re-executes body.
// throws: execstack-overflow, opstack-underflow, type-check
static void at_dowhile_op(Trix *trx) {
    trx->verify_operands(VerifyBoolean);
    auto keep = trx->m_op_ptr->boolean_value();
    --trx->m_op_ptr;

    if (keep) {
        trx->require_exec_capacity(2);

        auto body_ptr = trx->m_exec_ptr;

        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atDoWhile);
        *++trx->m_exec_ptr = *body_ptr;
    } else {
        --trx->m_exec_ptr;
    }
}

// loop: proc :- --
// Executes proc repeatedly until exit is called.
// throws: execstack-overflow, opstack-underflow, type-check
static void loop_op(Trix *trx) {
    trx->verify_operands(VerifyProc);
    trx->require_exec_capacity(3);

    auto proc_obj = *trx->m_op_ptr--;
    *++trx->m_exec_ptr = proc_obj;
    *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atLoop);
    *++trx->m_exec_ptr = proc_obj;
}

// while: cond body :- --
// Executes cond; if true executes body and repeats.
// throws: execstack-overflow, opstack-underflow, type-check
static void while_op(Trix *trx) {
    trx->verify_operands(VerifyProc, VerifyProc);
    trx->require_exec_capacity(4);

    auto body_obj = *trx->m_op_ptr--;
    auto cond_obj = *trx->m_op_ptr--;

    // frame: [body] [cond] [@while] [cond-to-exec]
    *++trx->m_exec_ptr = body_obj;
    *++trx->m_exec_ptr = cond_obj;
    *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atWhile);
    *++trx->m_exec_ptr = cond_obj;
}

//              enter  exit  exit             exit
//              -----  ----  ----             ----
// m_exec_ptr-> -      -     @uintegerrepeat  @ulongrepeat
//                           count            count
//                           proc             proc
//                           -                -
// repeat: int proc :- --
// Executes proc N times.
// throws: vm-full, execstack-overflow, opstack-underflow, type-check
static void repeat_op(Trix *trx) {
    // VerifyIntegers64 excludes Int128/UInt128 -- repeat's continuations
    // (@uinteger-repeat, @ulong-repeat) only handle up to 64-bit counts.
    // Matches for_op's explicit 128-bit rejection policy.
    trx->verify_operands(VerifyProc, VerifyIntegers64 | VerifyNotNegative);
    trx->require_exec_capacity(3);

    auto proc_obj = *trx->m_op_ptr--;
    auto count_obj = *trx->m_op_ptr--;
    auto [valid, uinteger_count] = count_obj.uinteger_value(trx);
    if (valid) {
        count_obj.maybe_free_extvalue(trx);

        if (uinteger_count != 0) {
            *++trx->m_exec_ptr = proc_obj;
            *++trx->m_exec_ptr = Object::make_uinteger(uinteger_count);
            *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atUIntegerRepeat);
        }
    } else {
        ulong_t ulong_count = 0;
        if (count_obj.is_long()) {
            auto long_count = count_obj.long_value(trx);
            ulong_count = static_cast<ulong_t>(long_count);
        } else {
            ulong_count = count_obj.ulong_value(trx);
        }
        count_obj.free_extvalue(trx);

        if (ulong_count != 0) {
            // should we verify that a ExtValue is available first, to avoid VMFull in make_ulong?

            *++trx->m_exec_ptr = proc_obj;
            *++trx->m_exec_ptr = Object::make_ulong(trx, ulong_count);
            *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atULongRepeat);
        }
    }
}

// run: str :- --
// Opens and executes the named script file.
// throws: execstack-overflow, file-open-error, filename-exists, filename-not-found, io-seek-error, limit-check,
// opstack-underflow, type-check
static void run_op(Trix *trx) {
    if (trx->m_sandbox) {
        trx->error(Error::Unsupported, "run: disabled in sandbox mode");
    } else {
        trx->verify_operands(VerifyString);
        trx->require_exec_capacity(2);

        auto str_ptr = trx->m_op_ptr;
        auto [stream_offset, sid] = Stream::open_file(trx, *str_ptr, 'r');
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atRun);
        *++trx->m_exec_ptr = Object::make_stream(stream_offset, sid, Object::ExecutableAttrib, Object::ReadOnlyAccess);
        --trx->m_op_ptr;
    }
}

// stop: :- --
// Signals stop to the innermost enclosing stopped context.
// throws: invalid-stop, opstack-overflow
static void stop_op(Trix *trx) {
    trx->require_op_capacity(1);

    // Pass 1: scan without side effects to find @stop.
    // Do NOT modify error stack, close streams, or free ExtValues here --
    // if no @stop is found, error() needs them intact.
    auto ptr = trx->m_exec_ptr;
    while (ptr >= trx->m_exec_base) {
        if (ptr->is_operator() && ptr->operator_is_stop()) {
            break;
        } else {
            --ptr;
        }
    }
    if (ptr < trx->m_exec_base) {
        trx->error(Error::InvalidStop, "stop: no enclosing stopped context found");
    } else {
        // Pass 2: unwind the range above @stop, then drop @stop itself.
        unwind_exec_to(trx, ptr);
        *++trx->m_op_ptr = Object::make_boolean(true);
        trx->m_exec_ptr = (ptr - 1);
    }
}

// stopped: proc :- bool
// Executes proc; pushes true if stopped, false otherwise.
// throws: execstack-overflow, opstack-underflow
static void stopped_op(Trix *trx) {
    trx->require_op_count(1);
    trx->require_exec_capacity(2);

    *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atStop);
    *++trx->m_exec_ptr = *trx->m_op_ptr--;
}

// @try-barrier: :- name
// Pushes /no-error when try proc completes without error.
// throws: opstack-overflow
static void at_trybarrier_op(Trix *trx) {
    trx->require_op_capacity(1);

    // return value for a proc that completes without any error
    *++trx->m_op_ptr = trx->error_name(Error::NoError);
}

// try: proc :- name
// Executes proc; pushes /no-error on success, /error-name on failure.
// On the error path, also rolls the operand stack back to the depth it
// had when `try` was called (just after the proc literal was popped),
// freeing extvalues from any residue the proc pushed before failing,
// so handlers see a clean stack with /error-name on top.  On the
// success path the proc's results are preserved with /no-error pushed
// above them.
// throws: execstack-overflow, opstack-underflow, type-check, vm-full
static void try_op(Trix *trx) {
    trx->verify_operands(VerifyProc);
    auto proc_obj = *trx->m_op_ptr--;
    auto saved_depth = static_cast<integer_t>(trx->m_op_ptr - trx->m_op_base);

    trx->require_exec_capacity(3);

    // exec layout: [saved-depth] [@try-rollback] [@try-barrier] [proc]
    *++trx->m_exec_ptr = Object::make_integer(saved_depth);
    *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atTryRollback);
    *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atTryBarrier);
    *++trx->m_exec_ptr = proc_obj;
}

// @try-rollback: cleanup hook that runs after @try-barrier (success path)
// or directly after the unwinder's atTryBarrier-cut (error path).  In
// both cases, consumes the [saved-depth] slot below itself; on the
// error path, also rolls the operand stack back to saved-depth and
// replants /error-name on top.
// exec stack layout: [saved-depth] [@try-rollback]
// op stack on entry: success path -> /no-error on top
//                    error path   -> residue + /error-name on top
// throws: opstack-underflow, internal-error
static void at_try_rollback_op(Trix *trx) {
    trx->require_op_count(1);
    auto saved_depth = trx->m_exec_ptr->integer_value();
    --trx->m_exec_ptr;

    auto status_obj = *trx->m_op_ptr;
    auto no_error = trx->error_name(Error::NoError);
    if (!status_obj.equal(trx, no_error)) {
        // Error path: residue from proc may sit between saved_depth and
        // the /error-name slot.  Free those extvalues, replant /error-name
        // at saved_depth + 1.
        auto error_name_obj = *trx->m_op_ptr--;
        auto target = trx->m_op_base + saved_depth;
        while (trx->m_op_ptr > target) {
            trx->m_op_ptr->maybe_free_extvalue(trx);
            --trx->m_op_ptr;
        }
        *++trx->m_op_ptr = error_name_obj;
    }
}

// @try-catch-barrier: (exec stack: --)
// Pops catch handler from error stack on normal completion.
// throws: internal-error
static void at_trycatchbarrier_op(Trix *trx) {
    if (trx->m_err_ptr < trx->m_err_base) {
        trx->error(Error::InternalError, "error stack underflow in @try-catch-barrier");
    } else {
        // try-catch proc executed without any error, pop the handler from error stack
        --trx->m_err_ptr;
    }
}

// @catch-error: (exec stack: --)
// Dispatches to matching handler in catch spec dict, or re-raises.
// Looks up /error-name first, then /default as fallback.
// throws: execstack-overflow, internal-error
static void at_catcherror_op(Trix *trx) {
    if (trx->m_err_ptr < trx->m_err_base) {
        trx->error(Error::InternalError, "error stack underflow in @catch-error");
    } else {
        trx->require_op_count(1);
        auto errorname = trx->m_op_ptr;
        auto container = *trx->m_err_ptr--;

        auto dict = container.dict_value(trx);
        auto value = dict->get(trx, errorname);
        if (value == nullptr) {
            auto default_name_obj = trx->wellknown_name(WellKnownName::Default);
            value = dict->get(trx, &default_name_obj);
        }

        if ((value == nullptr) || value->is_literal() || value->ignores_execute()) {
            // no matching handler and no /default -- re-raise to outer scope
            --trx->m_op_ptr;
            trx->error(trx->m_last_error, *trx->m_last_error_msg_ptr);
        } else {
            trx->require_exec_capacity(1);

            // call handler with /error-name on op stack
            *++trx->m_exec_ptr = *value;
        }
    }
}

// try-catch: dict proc :- --
// Executes proc; on error dispatches to handler in spec.
// throws: execstack-overflow, opstack-underflow, type-check
static void trycatch_op(Trix *trx) {
    trx->verify_operands(VerifyProc, VerifyDict);
    trx->require_exec_capacity(2);
    trx->require_error_capacity(1);

    // place @try-catch-barrier on exec stack which will execute after proc iff proc does not throw an error
    // @try-catch-barrier will pop the catch handler from error stack
    *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atTryCatchBarrier);

    // call proc
    *++trx->m_exec_ptr = *trx->m_op_ptr--;

    // push the catch handler on the error stack
    *++trx->m_err_ptr = *trx->m_op_ptr--;
}

// @finally-barrier: (exec stack: --)
// Normal completion: pops finally-block from error stack, executes it.
// throws: internal-error, execstack-overflow
static void at_finallybarrier_op(Trix *trx) {
    if (trx->m_err_ptr < trx->m_err_base) {
        trx->error(Error::InternalError, "error stack underflow in @finally-barrier");
    } else {
        auto finally_block = *trx->m_err_ptr--;
        trx->require_exec_capacity(1);
        *++trx->m_exec_ptr = finally_block;
    }
}

// @finally-reraise: (exec stack: --)
// Re-raises the original error after the finally-block has completed.
// throws: (re-raises m_last_error)
static void at_finally_reraise_op(Trix *trx) {
    trx->error(trx->m_last_error, *trx->m_last_error_msg_ptr);
}

// finally: finally-block proc :- --
// Executes proc; runs finally-block whether proc succeeds or fails.
// On failure, re-raises the original error after finally-block completes.
// throws: execstack-overflow, errstack-overflow, opstack-underflow, type-check
static void finally_op(Trix *trx) {
    trx->verify_operands(VerifyProc, VerifyProc);
    trx->require_exec_capacity(2);
    trx->require_error_capacity(1);

    auto proc_obj = *trx->m_op_ptr--;
    auto finally_block_obj = *trx->m_op_ptr--;

    // place @finally-barrier on exec stack (fires after proc completes normally)
    *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atFinallyBarrier);

    // call proc
    *++trx->m_exec_ptr = proc_obj;

    // push finally-block on error stack (used by both normal and error paths)
    *++trx->m_err_ptr = finally_block_obj;
}

// throw: name :- --
// Raises the named error. Sets /error-data to null. Any Name except /no-error
// may be thrown; unknown Names raise Error::UserError carrying the thrown Name.
// throws: invalid-throw, opstack-underflow, type-check
static void throw_op(Trix *trx) {
    trx->verify_operands(VerifyName);

    auto errorname = trx->m_op_ptr--;
    auto [valid, error] = trx->is_error_name(errorname);
    if (valid && (error == Error::NoError)) {
        trx->error(Error::InvalidThrow, "throw: /no-error is reserved");
    } else if (valid) {
        trx->error(error, "throw: {}", errorname->name_sv(trx));
    } else {
        *trx->m_last_error_name_ptr = *errorname;
        trx->error(Error::UserError, "throw: {}", errorname->name_sv(trx));
    }
}

// throw-with: name any :- --
// Raises the named error with user-supplied data stored in errordict /error-data.
// Any Name except /no-error may be thrown; unknown Names raise Error::UserError
// carrying the thrown Name.
// throws: invalid-throw, opstack-underflow, type-check
static void throw_with_op(Trix *trx) {
    trx->verify_operands(VerifyAny, VerifyName);

    auto data_obj = *trx->m_op_ptr--;
    auto errorname = trx->m_op_ptr--;
    auto [valid, error] = trx->is_error_name(errorname);
    if (valid && (error == Error::NoError)) {
        data_obj.maybe_free_extvalue(trx);
        trx->error(Error::InvalidThrow, "throw-with: /no-error is reserved");
    } else if (valid) {
        trx->set_root_object(RootObject::PendingErrorData, data_obj.make_clone(trx));
        trx->error(error, "throw: {}", errorname->name_sv(trx));
    } else {
        trx->set_root_object(RootObject::PendingErrorData, data_obj.make_clone(trx));
        *trx->m_last_error_name_ptr = *errorname;
        trx->error(Error::UserError, "throw: {}", errorname->name_sv(trx));
    }
}

// rethrow: :- --
// Re-raises the last error.
// throws: (none)
static void rethrow_op(Trix *trx) {
    trx->error(trx->m_last_error, *trx->m_last_error_msg_ptr);
}

// type-case: val dict :- result
// Dispatches on the type of val. Equivalent to: exch type exch case
// throws: opstack-underflow, type-check, undefined
static void typecase_op(Trix *trx) {
    trx->verify_operands(VerifyDict, VerifyAny);

    auto case_obj = trx->m_op_ptr;
    auto val_obj = (case_obj - 1);
    auto type = val_obj->type();
    val_obj->maybe_free_extvalue(trx);
    // replace val with type-name, then swap so key is on top for case_op
    *val_obj = *case_obj;
    *case_obj = trx->type_name(type);
    case_op(trx);
}

//===--- Contract Operators ---===//
//
// Lightweight domain-constraint checking.
//
//   precondition   bool --          Precondition: error if false
//   postcondition  check body --   Postcondition: run body, check result, error if false

// precondition: bool --
// Precondition guard.  Pops a boolean; raises Error::Require if false.
// Usage:  42 dup 0 ge precondition   % passes (42 >= 0)
//         -1 dup 0 ge precondition   % fails
// throws: opstack-underflow, require, type-check
static void precondition_op(Trix *trx) {
    trx->verify_operands(VerifyBoolean);

    auto result = trx->m_op_ptr->boolean_value();
    --trx->m_op_ptr;

    if (!result) {
        trx->error(Error::Require, "precondition: check failed");
    }
}

// ensure: check body ensure
// Postcondition.  Runs body-proc, then runs check-proc on the result.
// If check returns false, raises Error::Ensure.  The body's result
// remains on the stack on success.
//
// Stack: ... check body ensure
//   1. Pop check and body
//   2. Execute body (result on op stack)
//   3. @ensure-check: dup result, execute check, pop bool, error if false
// throws: execstack-overflow, opstack-underflow, type-check
static void postcondition_op(Trix *trx) {
    trx->verify_operands(VerifyProc, VerifyProc);

    auto body_obj = *trx->m_op_ptr;
    auto check_obj = *(trx->m_op_ptr - 1);

    trx->m_op_ptr -= 2;

    // Exec: [check-proc] [@ensure-check] [body-proc]
    trx->require_exec_capacity(3);
    *++trx->m_exec_ptr = check_obj;
    *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atEnsureCheck);
    *++trx->m_exec_ptr = body_obj;
}

// @ensure-check: after body completes, dup result, run check proc, verify.
// Exec frame: [check-proc] below control op.
static void at_ensure_check_op(Trix *trx) {
    trx->require_op_count(1);
    trx->require_op_capacity(1);
    trx->require_exec_capacity(1);

    // Dup the top of operand stack for the check proc
    auto dup_val = trx->m_op_ptr->make_clone(trx);
    *++trx->m_op_ptr = dup_val;

    // Replace frame with check execution:
    // The check proc runs, consuming the dup'd value, pushing bool.
    // After check: verify the bool with Error::Ensure (not Error::Require).
    auto check_obj = trx->m_exec_ptr;
    auto check = *check_obj;
    *check_obj = Object::make_operator(SystemName::PostconditionVerify);
    *++trx->m_exec_ptr = check;
}

// postcondition-verify: bool --
// Internal operator used by @ensure-check.  Like precondition but raises
// Error::Ensure so postcondition failures are distinguishable.
// throws: ensure, opstack-underflow, type-check
static void postcondition_verify_op(Trix *trx) {
    trx->verify_operands(VerifyBoolean);

    auto result = trx->m_op_ptr->boolean_value();
    --trx->m_op_ptr;

    if (!result) {
        trx->error(Error::Ensure, "postcondition: check failed");
    }
}
