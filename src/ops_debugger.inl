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

//===--- Interactive Script Debugger -- C++ Hooks ---===//
//
// Whole module is gated on TRIX_DEBUGGER (see src/build_config.inl).
// Release builds:
//   - SystemName entries for Breakpoint + Debug{Step,...} are absent.
//   - m_debug / m_debug_active members are absent (member_vars.inl).
//   - The dispatch-loop hook (interpreter.inl) folds out completely.
//
// Architecture: hybrid C++ state machine + Trix UI (lib/debugger.trx).
// At each step boundary the C++ hook invokes a user-installed Trix proc
// (`m_debug.m_event_callback`) with an event Tagged value describing why
// execution halted.  Without a callback installed the hook prints a terse
// stderr line and resumes -- useful as a low-fi trace when running with
// `-d` and no Trix UI installed.
//
// State machine (DebugState::Mode):
//   Off       -- hook never invoked (m_debug_active false).
//   StepIn    -- halt on next op whose (sid, line) differs from m_last_*.
//   StepOver  -- halt when exec-depth <= m_call_depth_snapshot AND line differs.
//   StepOut   -- halt when exec-depth <  m_call_depth_snapshot.
//   Continue  -- resume; only `breakpoint` / debug-break / debug-break-on-error
//                can re-arm a halt.
//   Halted    -- transient flag held during event-callback dispatch so a
//                nested debug_hook entry can short-circuit.
//
// This module provides the C++ hooks: set-up, state-management, and
// introspection ops.  Breakpoint tokens / conditional predicates and the
// full UI live on the Trix side (lib/debugger.trx).  The callback runs
// INLINE in the user coroutine (no coroutine-switch); the proc must
// return so the dispatch loop can continue.
#ifdef TRIX_DEBUGGER
//===--- DebugState ------------------------------------------------===//

// Debugger state -- holds all runtime state for the interactive script
// debugger.  Trix embeds this as a
// value member (m_debug) in member_vars.inl; the whole block plus
// every referenced member compiles out under !TRIX_DEBUGGER.
//
// Lives in this file rather than types.inl because it carries an
// Object value member -- Object is defined in object.inl which is
// included AFTER types.inl in the .inl chain (see trix.h ordering),
// but BEFORE ops_debugger.inl (which is included via ops_system.inl).
struct DebugState {
    enum class Mode : uint8_t { Off, StepIn, StepOver, StepOut, Continue, Halted };

    Mode m_mode{Mode::Off};
    // Source-line coalescing pair.  Updated at each halt; debug_hook
    // suppresses re-fires on the same line until a transition.
    // sid == 0 means "no prior line recorded" (matches INVALID_SID).
    stream_id_t m_last_sid{0};
    uint32_t m_last_line{0};
    // Exec-stack depth at last halt -- step-over compares <=,
    // step-out compares <.  Captured by debug-step-over/out ops.
    stack_depth_t m_call_depth_snapshot{0};
    // Breakpoint registry: Set of Name atoms.  nulloffset until the
    // first debug-break call lazily creates the Set in global VM.
    vm_offset_t m_breakpoints_offset{nulloffset};
    // Persistent sid -> path Dict (Integer sid keys, String path
    // values).  Populated at every file-stream open so `stream-name`
    // can return the path even after the stream is closed (e.g. for
    // procs that came from a `require`'d file that already closed
    // its stream).  Lives in global VM so the strings survive any
    // save/restore.  nulloffset until the first file open with the
    // debugger built in.
    vm_offset_t m_sid_path_cache_offset{nulloffset};
    // Per-event callback (Trix proc); null Object when unset.
    Object m_event_callback{};
    // True when debugger should fire on next user-level error.
    bool m_break_on_error{false};

    // Pending-error stash for break-on-error halts.  global_handler parks
    // the fatal error's identity here before queueing the inspector
    // session: the session itself raises and handles errors (watch evals
    // are try-bracketed), clobbering m_last_error, the m_last_error_*
    // root slots, and the shared m_error_string_base bytes.
    // @debug-error-resume restores the stash and re-enters global_handler.
    // The Object slots are GC roots -- walked next to m_event_callback in
    // gc.inl -- so a global GC during the session keeps them live.
    bool m_error_halt_armed{false};  // halt session in flight; re-entry guard
    // One-shot breakpoint latch.  A name-bp match at Name dispatch cannot
    // fire the halt directly: the queued session would land BELOW the
    // @call frames execute_name is about to push and would not run until
    // the whole call returned (with every nested bp dispatch suppressed
    // under Halted).  Instead the match latches, and the halt fires at
    // the next steppable dispatch -- the first op INSIDE the body, same
    // boundary step modes use -- so the session runs synchronously at
    // the breakpoint with the call's own frames live.
    bool m_bp_latched{false};
    // Exec-stack depth at the moment the current halt's event was queued.
    // While Halted, frame-source-locs walks from here instead of the live
    // top, so the inspector lists the HALTED program's frames -- not the
    // debugger UI's own @call frames stacked above the halt point.
    stack_depth_t m_halt_exec_depth{0};
    Error m_pending_error{};
    Object m_pending_error_name{};
    Object m_pending_error_msg{};
    Object m_pending_error_data{};
    Object m_pending_operator{};
    Object *m_pending_err_snapshot{nullptr};
    vm_t m_pending_msg_bytes[MaxErrorLength + 1]{};
};

//===--- Helpers ---------------------------------------------------===//

// Walk the exec stack from m_exec_ptr downward looking for the
// topmost @call.  Its companion SourceLoc sits two slots below.
// Returns sid=0 / line=0 when no @call is on the stack (top-level
// script code with no proc frame).
struct DebugSourcePos {
    stream_id_t sid;
    uint32_t line;
    uint16_t col;
};

[[nodiscard]] DebugSourcePos debug_current_source_pos() {
    for (auto p = m_exec_ptr; p >= (m_exec_base + 1); --p) {
        if (p->is_operator() && p->operator_is_call()) {
            auto loc_slot = p - 2;
            if ((loc_slot >= m_exec_base) && loc_slot->is_sourceloc()) {
                return {loc_slot->m_source_sid, loc_slot->m_source_line, loc_slot->m_source_pos};
            }
        }
    }
    return {static_cast<stream_id_t>(0), 0u, 0u};
}

// Find the Name of the currently-executing @call frame.  This is the
// companion Name literal that sits one slot below the topmost @call.
// Returns nulloffset when no @call frame is on the stack.
[[nodiscard]] vm_offset_t debug_current_name_offset() {
    for (auto p = m_exec_ptr; p >= m_exec_base; --p) {
        if (p->is_operator() && p->operator_is_call()) {
            auto name_slot = p - 1;
            if ((name_slot >= m_exec_base) && name_slot->is_name()) {
                return name_slot->m_name;
            }
        }
    }
    return nulloffset;
}

// Lazily create the breakpoints Dict, returning the existing one if
// present.  Maps bp Name -> Integer hit count.  Allocated in the
// global VM region so the Dict (and the integer counts) survive any
// user-script save/restore -- bp state is meta-execution, not
// transactional user state; restoring user code shouldn't pretend
// the hits the user observed never happened.
//
// (Breakpoints are a Dict<Name, Integer> -- the Integer is the hit
// count.  It uses `create_global_dict` rather than `create_set`:
// create_set silently lands in local VM at sl=0 because the
// m_curr_alloc_global flag doesn't reach the underlying vm_alloc,
// whereas create_global_dict lands in global VM via gvm_alloc.)
Dict *debug_breakpoints_dict() {
    if (m_debug.m_breakpoints_offset == nulloffset) {
        auto [dict, offset] = Dict::create_global_dict(this, 16);
        m_debug.m_breakpoints_offset = offset;
        return dict;
    } else {
        return offset_to_ptr<Dict>(m_debug.m_breakpoints_offset);
    }
}

// Lazily create the sid -> path Dict (Integer keys, String values).
// Allocated in the global VM region (gvm) so the Dict struct
// and its entry pool survive any user-script save/restore.  Dynamic
// mode -- after the region-aware expand_dict fix (dict.inl) global
// Dicts grow correctly without dangling pool blocks.  Initial cap
// 16; doubling kicks in when more streams open across long-running
// sessions.  Cap naturally bounds at stream_id_t max (256 distinct
// values) since put replaces existing entries on sid recycle.
Dict *debug_sid_path_dict() {
    if (m_debug.m_sid_path_cache_offset == nulloffset) {
        auto [dict, offset] = Dict::create_global_dict(this, 16, Object::DictMode::ReadWriteDynamic);
        m_debug.m_sid_path_cache_offset = offset;
        return dict;
    } else {
        return offset_to_ptr<Dict>(m_debug.m_sid_path_cache_offset);
    }
}

// Record the (sid, source-path) mapping for an open file stream.
// Called from Stream::open_file in stream.inl right after a sid has
// been assigned.  Only meaningful when filename_obj is a String;
// non-string sources (in-memory streams, stdio) carry no useful
// path.  The filename gets COPIED into global VM so the cache
// entry survives the originating stream's close and any restore
// that may rewind the originating string's allocation.  Overwrites
// any prior entry under the same sid -- sid recycling after a wrap
// means a stale entry would be incorrect anyway.
void debug_cache_sid_path(stream_id_t sid, Object filename_obj) {
    if (filename_obj.is_string()) {
        auto sv = filename_obj.sv_value(this);
        auto saved_global = m_curr_alloc_global;
        m_curr_alloc_global = true;
        auto length = sv_length(sv);
        auto [base, offset] = vm_alloc_dispatch<vm_t>(static_cast<vm_size_t>(length) + 1, ChunkKind::String);
        std::copy_n(reinterpret_cast<const vm_t *>(sv.data()), length, base);
        base[length] = '\0';
        auto path_obj = Object::make_string(offset, length, Object::LiteralAttrib, Object::ReadOnlyAccess);
        auto dict = debug_sid_path_dict();
        auto key = Object::make_integer(static_cast<integer_t>(sid));
        static_cast<void>(dict->put(this, key, path_obj));
        m_curr_alloc_global = saved_global;
    }
}

// Build an event Tagged value to hand to the callback.  /step-in /
// /step-over / /step-out / /breakpoint / /breakpoint-op / /error
// distinguish causes.  Payload is a 4-element packed:
//   [ sid line col callname-or-null ]
Object debug_make_event_at(vm_offset_t tag_offset) {
    auto pos = debug_current_source_pos();
    auto name_off = debug_current_name_offset();
    auto pc_name = (name_off != nulloffset) ? Object::make_name(name_off, offset_to_ptr<Name>(name_off)->length()) : Object{};
    Object slots[4] = {
            Object::make_integer(static_cast<integer_t>(pos.sid)),
            Object::make_integer(static_cast<integer_t>(pos.line)),
            Object::make_integer(static_cast<integer_t>(pos.col)),
            pc_name,
    };
    // Allocate the packed payload in global VM so it survives
    // through any save/restore the callback might do.
    auto saved_global = m_curr_alloc_global;
    m_curr_alloc_global = true;
    auto [ptr, packed_offset] = Object::make_packed_data(this, slots, 4);
    m_curr_alloc_global = saved_global;
    auto payload = (ptr != nullptr) ? Object::make_packed(packed_offset, 4) : Object::make_null();
    auto tag = Object::make_name(tag_offset, offset_to_ptr<Name>(tag_offset)->length());
    return Object::make_tagged(this, tag, payload);
}

//===--- Dispatch hook --------------------------------------------===//

// Called by dispatch_loop when m_debug_active && !curr_obj.ignores_execute().
// Filters by Mode + source-line / call-depth and fires the event
// callback (or a stderr fallback) when a halt boundary is reached.
// Breakpoint-name hit: at executable-Name dispatch, check if the entering
// Name matches any registered breakpoint.  Tested BEFORE the control-op /
// Stream / Packed skips so breakpoints fire universally (control ops are
// never user-defined names).  Tag is /breakpoint (matches the in-source
// `breakpoint` op), not /debug-break (the management op): both halt for the
// same user-visible reason.  Returns true (caller stops processing) when a
// breakpoint matched: bumps its hit count and LATCHES (m_bp_latched) rather
// than firing, so the halt lands at the first steppable op inside the body.
bool debug_try_latch_breakpoint(Object obj) {
    if ((m_debug.m_breakpoints_offset != nulloffset) && obj.is_name() && obj.is_executable()) {
        auto dict = offset_to_ptr<Dict>(m_debug.m_breakpoints_offset);
        auto probe = obj;
        probe.set_literal();  // Dict lookup compares literal Names by atom
        auto val_ptr = dict->get(this, &probe);
        if (val_ptr != nullptr) {
            // Hot-path increment: read the existing Integer count and
            // overwrite in place.  Dict is in global VM so journaling
            // is short-circuited (no save/restore rollback of counts).
            auto count = val_ptr->is_integer() ? val_ptr->integer_value() : integer_t{0};
            *val_ptr = Object::make_integer(count + 1);
            // Latch, don't fire -- see m_bp_latched in DebugState.
            m_debug.m_bp_latched = true;
            return true;
        }
    }
    return false;
}

// Source-line / call-depth stepping decision, reached once a dispatch has
// cleared the re-entrancy, breakpoint, and skip filters.  Compares the
// current source position against the last halt and fires the appropriate
// step event for the active Mode (or returns to keep running).
void debug_step_dispatch() {
    auto pos = debug_current_source_pos();
    auto same_line = (pos.sid == m_debug.m_last_sid) && (pos.line == m_debug.m_last_line);
    auto exec_depth = static_cast<stack_depth_t>(m_exec_ptr - m_exec_base + 1);

    switch (m_debug.m_mode) {
    case DebugState::Mode::Off:
    case DebugState::Mode::Halted:
        return;

    case DebugState::Mode::Continue:
        return;  // only breakpoints can halt under Continue

    case DebugState::Mode::StepIn:
        // Step in: fire on any new source line.
        if (!same_line) {
            debug_fire_event(SystemName::DebugStep);
        }
        return;

    case DebugState::Mode::StepOver:
        // Step over: frames deeper than the snapshot are inside a call -- keep
        // running; fire once back at (or above) the snapshot frame on a new line.
        if ((exec_depth <= m_debug.m_call_depth_snapshot) && !same_line) {
            debug_fire_event(SystemName::DebugStepOver);
        }
        return;

    case DebugState::Mode::StepOut:
        // Step out: fire once we have returned shallower than the caller.
        if (exec_depth < m_debug.m_call_depth_snapshot) {
            debug_fire_event(SystemName::DebugStepOut);
        }
        return;
    }
}

// True for dispatches the stepping hook must ignore:
//   - control ops (@call, @end-locals, @run, @loop, @try-*, ...) -- internal
//     cleanup markers, not user-visible operations, whose handlers depend on
//     specific exec-stack layouts immediately below the marker.  Firing here
//     would push our callback frame between the control op and its companions
//     (SourceLoc/Name for @call, end-of-locals dict for @end-locals, etc.),
//     corrupting at_*_op invariants.
//   - dispatching-side infrastructure objects whose handlers push MORE onto the
//     exec stack after the hook returns: an executable Stream (execute_stream
//     pushes the stream back + dispatches the scanned token, burying our
//     callback) and an executable Array/Packed (execute_proc pushes the
//     remaining body elements back on top of our callback).
// Skipping these means source-line stepping fires only where exec-stack
// mutation by the curr_obj handler is bounded: Name dispatches, operators, and
// literals.  The proc-body iteration is silent; the FIRST op INSIDE the body
// becomes the visible halt point.  Asynchronicity note: when stream dispatch is
// active (top-level script execution), the queued callback does not run until
// the stream loop yields (typically end-of-script or a halt that pops the
// stream); a truly synchronous halt requires a sub-dispatch design
// (lib/debugger.trx + cooperative coroutine handoff).
bool debug_hook_skips(Object obj) const {
    return ((obj.is_operator() && is_control_op_atom(obj.m_operator)) ||
            (obj.is_executable() && (obj.is_stream() || obj.is_array() || obj.is_packed())));
}

void debug_hook(Object obj) {
    using namespace std::literals;

    // Halted mode means we are inside the callback right now -- never re-fire
    // (re-entrancy guard).  Otherwise latch a freshly-hit breakpoint (and wait
    // for the first steppable op), and skip control ops + dispatching-side
    // infrastructure (see debug_hook_skips) that would corrupt exec-stack
    // invariants.
    if ((m_debug.m_mode != DebugState::Mode::Halted) && !debug_try_latch_breakpoint(obj) && !debug_hook_skips(obj)) {
        if (m_debug.m_bp_latched) {
            // First steppable dispatch after the bp'd Name matched -- the first
            // op inside its body.  Fire here so the session sees the call's
            // frames and operands live.
            m_debug.m_bp_latched = false;
            debug_fire_event(SystemName::Breakpoint);
        } else {
            // Source-line / call-depth stepping.
            debug_step_dispatch();
        }
    }
}

// Invoke the installed callback (or stderr fallback) with an event
// Tagged.  Updates m_last_sid / m_last_line so the next step
// coalesces against the current line.  Mode is held in Halted for
// the duration of the callback so nested debug_hook calls (from
// ops inside the callback) bail out.
void debug_fire_event(SystemName reason_tag) {
    debug_fire_event_at(m_systemname_offsets[+reason_tag]);
}

// Offset-tagged core: reason_offset is a pre-interned Name (SystemName or
// WellKnownName table entry).  Break-on-error events carry the wellknown
// /error tag, which has no SystemName atom.
void debug_fire_event_at(vm_offset_t reason_offset) {
    auto pos = debug_current_source_pos();
    m_debug.m_last_sid = pos.sid;
    m_debug.m_last_line = pos.line;
    m_debug.m_halt_exec_depth = static_cast<stack_depth_t>(m_exec_ptr - m_exec_base + 1);

    auto saved_mode = m_debug.m_mode;
    m_debug.m_mode = DebugState::Mode::Halted;

    if (m_debug.m_event_callback.is_null()) {
        // No UI installed -- terse stderr trace, then continue.
        auto reason_sv = offset_to_ptr<Name>(reason_offset)->sv();
        diag_println("[debug:{}] sid={} line={}", std::string_view(reason_sv.data(), sv_length(reason_sv)), pos.sid, pos.line);
        m_debug.m_mode = saved_mode;
    } else {
        // Queue the callback and the event on the EXEC stack.  Order
        // matters subtly:
        //
        //   1. NoOp -- guard against tail-call optimization.  Without
        //      this, when the callback (cb) is a one-element proc like
        //      `{ recorder-step }`, execute_proc pops the head and
        //      then execute_name's TCO check sees @call directly below
        //      cb on exec.  TCO would then REPLACE the enclosing @call
        //      frame's SourceLoc/Name with the callee's, hijacking
        //      whatever user-level proc was running when the hook
        //      interrupted.  NoOp (a plain operator, not an @call)
        //      breaks the pattern so TCO falls through to the normal
        //      4-slot push path.
        //   2. cb (executable proc) -- runs the user callback.
        //   3. event Tagged (literal) -- routes to operands when
        //      popped, landing on top of whatever the interrupted op
        //      pushed; the callback consumes it with `/e exch def`.
        //
        // Dispatch order: event -> operands; cb -> execute; NoOp ->
        // no-op cleanup.  All three pops happen AFTER the dispatch
        // loop finishes the op that fired this hook.
        require_exec_capacity(3);
        auto cb = m_debug.m_event_callback;
        cb.set_executable();
        *++m_exec_ptr = Object::make_operator(SystemName::NoOp);
        *++m_exec_ptr = cb;
        *++m_exec_ptr = debug_make_event_at(reason_offset);

        // Mode stays Halted while the callback runs; restored when the
        // callback issues debug-step/over/out/continue (each of those
        // sets a fresh mode + clears Halted).
    }
}

// Park the in-flight fatal error and queue an inspector halt.  Called by
// global_handler when break-on-error is armed: every live slot the error
// machinery reads gets stashed (the inspector session clobbers them via
// its own try-bracketed evals), then [@debug-error-resume, session] is
// queued on exec.  The dispatch loop runs the session on the erroring
// coroutine's intact at-raise stacks; when the callback returns,
// @debug-error-resume restores the stash and re-enters global_handler --
// the error then completes exactly as if the halt had never happened
// (handler dispatch on main, kill + reschedule for a dying coroutine,
// diag + Error-enum exit code preserved).
//
// Arm-first ordering: m_error_halt_armed goes true before anything that
// can throw (require_exec_capacity).  If the queueing itself errors, the
// re-entered global_handler sees armed=true, skips a second halt, and
// dispatches the stashed original fatally -- degraded but never cyclic.
void debug_error_halt() {
    m_debug.m_error_halt_armed = true;
    m_debug.m_pending_error = m_last_error;
    m_debug.m_pending_error_name = *m_last_error_name_ptr;
    m_debug.m_pending_error_msg = *m_last_error_msg_ptr;
    m_debug.m_pending_error_data = *m_last_error_data_ptr;
    m_debug.m_pending_operator = *m_last_operator_ptr;
    m_debug.m_pending_err_snapshot = m_error_err_ptr_snapshot;
    std::memcpy(m_debug.m_pending_msg_bytes, m_error_string_base, MaxErrorLength + 1);

    require_exec_capacity(4);
    *++m_exec_ptr = Object::make_control_operator(SystemName::atDebugErrorResume);
    debug_fire_event_at(m_wellknown_offsets[+WellKnownName::IKeyError]);
}

//===--- Ops --------------------------------------------------------===//

// breakpoint -- (in-source halt; renamed from `debug`)
// When the debugger is active, fires a /breakpoint-op event.  When
// inactive (no `-d` / no UI), prints a one-line file:line:msg trace
// to stderr and continues -- safe to leave in user code.
// throws: unsupported (in sandbox mode)
static void breakpoint_op(Trix *trx) {
    if (trx->m_sandbox) {
        trx->error(Error::Unsupported, "breakpoint: disabled in sandbox mode");
    } else if (trx->m_debug_active) {
        trx->debug_fire_event(SystemName::Breakpoint);
    } else {
        auto pos = trx->debug_current_source_pos();
        trx->diag_println("[breakpoint] sid={} line={} (debugger not active; continuing)", pos.sid, pos.line);
    }
}

// debug-step -- (set StepIn mode; activate hook)
// throws: (none)
static void debug_step_op(Trix *trx) {
    trx->m_debug.m_mode = DebugState::Mode::StepIn;
    trx->m_debug_active = true;
}

// debug-step-over -- (set StepOver mode; snapshot exec-depth)
// throws: (none)
static void debug_step_over_op(Trix *trx) {
    trx->m_debug.m_call_depth_snapshot = static_cast<stack_depth_t>(trx->m_exec_ptr - trx->m_exec_base + 1);
    trx->m_debug.m_mode = DebugState::Mode::StepOver;
    trx->m_debug_active = true;
}

// debug-step-out -- (set StepOut mode; snapshot exec-depth)
// throws: (none)
static void debug_step_out_op(Trix *trx) {
    trx->m_debug.m_call_depth_snapshot = static_cast<stack_depth_t>(trx->m_exec_ptr - trx->m_exec_base + 1);
    trx->m_debug.m_mode = DebugState::Mode::StepOut;
    trx->m_debug_active = true;
}

// debug-continue -- (resume; only breakpoints can halt next)
// throws: (none)
static void debug_continue_op(Trix *trx) {
    // If breakpoints are installed, leave m_debug_active true so the
    // dispatch hook keeps running (it checks breakpoints).  If not,
    // disable the hook entirely.
    trx->m_debug.m_mode = DebugState::Mode::Continue;
    trx->m_debug_active = (trx->m_debug.m_breakpoints_offset != nulloffset);
}

// debug-break /name -- (add name-resolution breakpoint)
// Hit count starts at 0 and increments in the dispatch hook on
// every match.  Re-setting an existing bp PRESERVES the count
// (the existing entry's value is not overwritten -- Dict::put
// returns the existing value pointer for keys already present).
// throws: opstack-underflow, type-check
static void debug_break_op(Trix *trx) {
    trx->verify_operands(VerifyName);
    auto name = *trx->m_op_ptr--;
    auto dict = trx->debug_breakpoints_dict();
    auto existing = dict->get(trx, &name);
    if (existing == nullptr) {
        static_cast<void>(dict->put(trx, name, Object::make_integer(0)));
    }
    trx->m_debug_active = true;  // breakpoints need the dispatch hook live
}

// debug-unbreak /name -- (clear name-resolution breakpoint)
// Removes the hit-count entry along with the bp itself.
// throws: opstack-underflow, type-check
static void debug_unbreak_op(Trix *trx) {
    trx->verify_operands(VerifyName);
    auto name_ptr = trx->m_op_ptr;
    if (trx->m_debug.m_breakpoints_offset != nulloffset) {
        auto dict = trx->offset_to_ptr<Dict>(trx->m_debug.m_breakpoints_offset);
        dict->undef(trx, name_ptr);
        // If the dict is now empty AND mode is Continue (no other
        // halt source), drop the dispatch hook.
        if ((dict->length() == 0) && (trx->m_debug.m_mode == DebugState::Mode::Continue)) {
            trx->m_debug_active = false;
        }
    }
    --trx->m_op_ptr;
}

// debug-break-on-error bool -- (toggle entry-on-error mode)
// throws: opstack-underflow, type-check
static void debug_break_on_error_op(Trix *trx) {
    trx->verify_operands(VerifyBoolean);
    trx->m_debug.m_break_on_error = trx->m_op_ptr->boolean_value();
    --trx->m_op_ptr;
}

// @debug-error-resume: (exec stack: --)
// Tail of a break-on-error inspector session (queued by
// debug_error_halt below the session callback).  Restores the parked
// fatal error -- the session's own try-bracketed evals clobbered the
// live slots and the shared message bytes -- and re-enters
// global_handler, where the armed flag routes past the halt branch and
// the error completes normally.  The exec/err stacks below this op are
// byte-identical to the at-raise state (the session is balanced and
// err-stack bytes at or below the live pointer are never overwritten),
// so the backtrace and handler dispatch see exactly what they would
// have seen without the halt.
// throws: <parked error>
[[noreturn]] static void at_debug_error_resume_op(Trix *trx) {
    trx->m_last_error = trx->m_debug.m_pending_error;
    *trx->m_last_error_name_ptr = trx->m_debug.m_pending_error_name;
    *trx->m_last_error_msg_ptr = trx->m_debug.m_pending_error_msg;
    *trx->m_last_error_data_ptr = trx->m_debug.m_pending_error_data;
    *trx->m_last_operator_ptr = trx->m_debug.m_pending_operator;
    trx->m_error_err_ptr_snapshot = trx->m_debug.m_pending_err_snapshot;
    std::memcpy(trx->m_error_string_base, trx->m_debug.m_pending_msg_bytes, MaxErrorLength + 1);
    trx->global_handler();
}

// debug-call-depth -- int (current exec-stack depth)
// throws: opstack-overflow
static void debug_call_depth_op(Trix *trx) {
    trx->require_op_capacity(1);
    auto depth = static_cast<integer_t>(trx->m_exec_ptr - trx->m_exec_base + 1);
    *++trx->m_op_ptr = Object::make_integer(depth);
}

// debug-pc -- name | null  (currently-executing @call's name, or null)
// throws: opstack-overflow
static void debug_pc_op(Trix *trx) {
    trx->require_op_capacity(1);
    auto name_off = trx->debug_current_name_offset();
    if (name_off != nulloffset) {
        auto npc = trx->offset_to_ptr<Name>(name_off);
        *++trx->m_op_ptr = Object::make_name(name_off, npc->length());
    } else {
        *++trx->m_op_ptr = Object::make_null();
    }
}

// debug-pc-source -- int int int  (sid, line, col of current pos)
// throws: opstack-overflow
static void debug_pc_source_op(Trix *trx) {
    trx->require_op_capacity(3);
    auto pos = trx->debug_current_source_pos();
    *++trx->m_op_ptr = Object::make_integer(static_cast<integer_t>(pos.sid));
    *++trx->m_op_ptr = Object::make_integer(static_cast<integer_t>(pos.line));
    *++trx->m_op_ptr = Object::make_integer(static_cast<integer_t>(pos.col));
}

// debug-on-event {proc} -- (install per-event callback)
// Pass null to clear the callback (revert to stderr fallback).
// throws: opstack-underflow, type-check
static void debug_on_event_op(Trix *trx) {
    trx->require_op_count(1);
    auto cb = *trx->m_op_ptr;
    if (!cb.is_null() && !cb.is_callable()) {
        trx->error(Error::TypeCheck, "debug-on-event: callback must be callable or null");
    } else {
        trx->m_debug.m_event_callback = cb;
        --trx->m_op_ptr;
    }
}

//===--- Pretty-print + introspection ops -----------------===//
//
// All snapshot arrays allocate in LOCAL VM at the current save level.
// Lifetime is bounded by save/restore: a snapshot captured at sl=N is
// unmapped if execution restores to sl<N (matches the user's mental
// model -- the snapshot is a transient observation, not durable).
// The Trix UI re-snaps on every halt boundary, so durability past
// a single halt is not required.  No R6 pointer-hygiene reject: the
// snapshot's storage is local, so locally-allocated Object handles
// copy in cleanly.
//
// The four stack-snapshot ops include every slot in the requested
// stack (control-op companions, Marks, ...) so the UI can filter as
// needed.  Top of stack = LAST element in the array.
//
// Special handling for SourceLoc: SourceLoc Objects are internal-only
// call-site markers.  They cannot be cloned (Object::make_clone
// asserts), so a snapshot Array containing a SourceLoc would crash
// any `array get` accessing it.  We substitute Null in the
// snapshot's place so the array is fully iterable.

// Shared helper: copy [base..top] (inclusive) into a freshly-allocated
// local-VM array, substituting Null for any SourceLoc slot.
Object debug_snapshot_range(Object *base, Object *top) {
    auto length = (top >= base) ? static_cast<length_t>(top - base + 1) : static_cast<length_t>(0);
    auto arr_obj = Object::make_empty_array(this, length);
    if (length > 0) {
        auto dst = arr_obj.array_objects(this);
        for (length_t i = 0; i < length; ++i) {
            auto src = base[i];
            dst[i] = src.is_sourceloc() ? Object::make_null() : src;
        }
    }
    arr_obj.set_save_level(m_curr_save_level);
    return arr_obj;
}

// op-stack-snapshot -- array  (copy of operand stack, top last)
// throws: opstack-overflow, vm-full
static void op_stack_snapshot_op(Trix *trx) {
    trx->require_op_capacity(1);
    auto arr = trx->debug_snapshot_range(trx->m_op_base, trx->m_op_ptr);
    *++trx->m_op_ptr = arr;
}

// frame-source-locs -- array
// Walks the exec stack from top to bottom, emitting one
// [sid line col name] Array per @call frame.  Result is ordered
// top-frame-first (index 0 = deepest halt point, subsequent
// indices = older callers).  Used by the debugger UI to render
// a frame list and navigate among them.  Returns an empty Array
// when no @call frames are on the stack (top-level halt).
// throws: opstack-overflow, vm-full
static void frame_source_locs_op(Trix *trx) {
    trx->require_op_capacity(1);
    *++trx->m_op_ptr = Object::make_mark();
    // While Halted, start at the halt-time top: the frames above it are
    // the inspector's own machinery, not the program being inspected.
    auto top = trx->m_exec_ptr;
    if (trx->m_debug.m_mode == DebugState::Mode::Halted) {
        auto halt_top = trx->m_exec_base + trx->m_debug.m_halt_exec_depth - 1;
        top = std::min(top, halt_top);
    }
    for (auto p = top; p >= (trx->m_exec_base + 1); --p) {
        if (p->is_operator() && p->operator_is_call()) {
            auto loc_slot = p - 2;
            auto name_slot = p - 1;
            if ((loc_slot >= trx->m_exec_base) && loc_slot->is_sourceloc() && name_slot->is_name()) {
                trx->require_op_capacity(1);
                auto row = Object::make_empty_array(trx, 4);
                row.set_save_level(trx->m_curr_save_level);
                auto row_data = row.array_objects(trx);
                row_data[0] = Object::make_integer(static_cast<integer_t>(loc_slot->m_source_sid));
                row_data[1] = Object::make_integer(static_cast<integer_t>(loc_slot->m_source_line));
                row_data[2] = Object::make_integer(static_cast<integer_t>(loc_slot->m_source_pos));
                auto name_len = trx->offset_to_ptr<Name>(name_slot->m_name)->length();
                row_data[3] = Object::make_name(name_slot->m_name, name_len, Object::LiteralAttrib);
                *++trx->m_op_ptr = row;
            }
        }
    }
    auto result = Object::make_array_from_mark(trx);
    trx->require_op_capacity(1);
    *++trx->m_op_ptr = result;
}

// exec-stack-snapshot -- array  (copy of exec stack, top last;
// includes control-op companions / SourceLocs / Names verbatim --
// UI filters as needed for the call-frame view)
// throws: opstack-overflow, vm-full
static void exec_stack_snapshot_op(Trix *trx) {
    trx->require_op_capacity(1);
    auto arr = trx->debug_snapshot_range(trx->m_exec_base, trx->m_exec_ptr);
    *++trx->m_op_ptr = arr;
}

// dict-stack-snapshot -- array  (copy of dict-frame references, top last)
// throws: opstack-overflow, vm-full
static void dict_stack_snapshot_op(Trix *trx) {
    trx->require_op_capacity(1);
    auto arr = trx->debug_snapshot_range(trx->m_dict_base, trx->m_dict_ptr);
    *++trx->m_op_ptr = arr;
}

// err-stack-snapshot -- array  (copy of error-stack companions, top last)
// throws: opstack-overflow, vm-full
static void err_stack_snapshot_op(Trix *trx) {
    trx->require_op_capacity(1);
    auto arr = trx->debug_snapshot_range(trx->m_err_base, trx->m_err_ptr);
    *++trx->m_op_ptr = arr;
}

// debug-breakpoints -- array  (snapshot of currently-active breakpoint Names)
// Returns an empty array if no breakpoints have been set yet.  The
// bp Set's contents are walked in iterator order (bucket-first); the
// UI sorts client-side if it cares about display order.
// throws: opstack-overflow, vm-full
static void debug_breakpoints_op(Trix *trx) {
    trx->require_op_capacity(1);
    if (trx->m_debug.m_breakpoints_offset == nulloffset) {
        *++trx->m_op_ptr = Object::make_empty_array(trx, 0);
    } else {
        auto dict = trx->offset_to_ptr<Dict>(trx->m_debug.m_breakpoints_offset);
        auto length = dict->length();
        auto arr_obj = Object::make_empty_array(trx, length);
        arr_obj.set_save_level(trx->m_curr_save_level);
        if (length > 0) {
            auto dst = arr_obj.array_objects(trx);
            vm_offset_t entry_offset = nulloffset;
            integer_t bucket_idx = -1;
            length_t i = 0;
            while (true) {
                auto entry = dict->next(trx, entry_offset, bucket_idx);
                if (entry.next_offset == nulloffset) {
                    break;
                } else if (i < length) {
                    dst[i++] = entry.key;
                }
                entry_offset = entry.next_offset;
                bucket_idx = entry.next_bucket;
            }
        }
        *++trx->m_op_ptr = arr_obj;
    }
}

// debug-bp-hits /name -- int  (hit count for the named bp; 0 when
// no bp is registered at that name).  Counts every dispatch-hook
// match -- including silent-skips on conditional bps whose
// predicate evaluated false.
// throws: opstack-underflow, type-check
static void debug_bp_hits_op(Trix *trx) {
    trx->verify_operands(VerifyName);
    auto name_ptr = trx->m_op_ptr;
    integer_t count = 0;
    if (trx->m_debug.m_breakpoints_offset != nulloffset) {
        auto dict = trx->offset_to_ptr<Dict>(trx->m_debug.m_breakpoints_offset);
        auto val_ptr = dict->get(trx, name_ptr);
        if ((val_ptr != nullptr) && val_ptr->is_integer()) {
            count = val_ptr->integer_value();
        }
    }
    *name_ptr = Object::make_integer(count);
}

// stream-name sid -- str | null  (lookup the source filename for a
// stream id)
// First walks the inuse stream list for an OPEN stream at this sid.
// If that fails, consults the persistent sid -> path cache populated
// at file-stream open time (debug_cache_sid_path).  The cache survives
// stream close + restore -- file procs scanned via `require` and then
// closed are still resolvable.  Returns null when neither path
// produces a hit OR the matched open stream is not file-backed (in-
// memory, stdio).  Cache entries are global-VM strings; we return
// them directly without copying.
// throws: opstack-underflow, range-check, type-check
static void stream_name_op(Trix *trx) {
    trx->verify_operands(VerifyIntegers64 | VerifyNotNegative);
    auto [valid, sid_val] = trx->m_op_ptr->uinteger_value(trx);
    if (!valid || (sid_val > std::numeric_limits<stream_id_t>::max())) {
        trx->error(Error::RangeCheck, "stream-name: sid out of range");
    } else {
        auto target_sid = static_cast<stream_id_t>(sid_val);
        for (auto s = trx->m_stream_inuse_list; s != nullptr; s = s->next_stream(trx)) {
            if (s->m_sid == target_sid) {
                if (s->m_source.is_string()) {
                    auto sv = s->source(trx);
                    *trx->m_op_ptr = Object::make_string(trx, sv);
                    return;
                } else {
                    break;
                }
            }
        }
        // Fall back to the persistent cache for closed-stream sids.
        if (trx->m_debug.m_sid_path_cache_offset != nulloffset) {
            auto dict = trx->offset_to_ptr<Dict>(trx->m_debug.m_sid_path_cache_offset);
            auto key = Object::make_integer(static_cast<integer_t>(target_sid));
            auto val_ptr = dict->get(trx, &key);
            if (val_ptr != nullptr) {
                *trx->m_op_ptr = *val_ptr;
                return;
            }
        }
        *trx->m_op_ptr = Object::make_null();
    }
}

// Format an Object into a fresh string with a width cap.  Uses
// PrintFmt's BUFFER-based overload (not stream-based) so oversize
// composites get silently truncated via `dropped_count` instead of
// raising /io-write-error.  The result is clamped to `n` chars
// with a "..." marker appended when truncation occurs.  Honors
// m_curr_alloc_global so the sandboxed `:p` eval can produce a
// result that survives a wrapping save/restore (the local-VM
// operand would otherwise trip /invalid-restore on the way out).
Object debug_format_object(Object obj, integer_t n) {
    if (n < 1) {
        error(Error::RangeCheck, "format-object: width cap must be >= 1");
    } else {
        auto make_string_here = [this](std::string_view sv) {
            auto length = sv_length(sv);
            auto [base, offset] = vm_alloc_dispatch<vm_t>(static_cast<vm_size_t>(length) + 1, ChunkKind::String);
            std::copy_n(reinterpret_cast<const vm_t *>(sv.data()), length, base);
            base[length] = '\0';
            return Object::make_string(offset, length, Object::LiteralAttrib, Object::ReadOnlyAccess);
        };
        // SourceLoc isn't a user-visible type; PrintFmt asserts on it.
        // Stack-snapshot ops can surface SourceLocs from the exec
        // stack, so format-object short-circuits them with a friendly
        // tag rather than crashing.
        if (obj.is_sourceloc()) {
            return make_string_here(std::string_view{"<sourceloc>"});
        } else {
            // Mark (end-of-stack sentinel) is also internal-only; PrintFmt
            // handles it as Operator now, but it's worth a friendlier label.
            // 4x the requested width gives slack for multi-byte UTF-8 and
            // for composites that nearly fit.  Cap at MaxStringLength to
            // bound VM usage.
            auto cap_raw = static_cast<vm_size_t>(n) * 4 + 64;
            auto cap = static_cast<length_t>(std::min<vm_size_t>(cap_raw, static_cast<vm_size_t>(MaxStringLength)));
            auto [buf, buf_off] = vm_alloc_dispatch<vm_t>(static_cast<vm_size_t>(cap) + 1, ChunkKind::String);
            auto obj_ptr = &obj;
            auto [output_count, dropped_count] = PrintFmt::process_object(this, obj_ptr, buf, cap, true);
            auto written = static_cast<length_t>(output_count);
            // The formatter may have stopped mid-UTF-8 or mid-token; the
            // resulting bytes are still a valid C-string after we nul-
            // terminate.  Truncation can happen either because the buffer
            // overflowed (dropped_count > 0) OR because the requested
            // width n is smaller than the produced bytes.
            auto truncated = (dropped_count > 0) || (written > static_cast<length_t>(n));
            auto out_len = truncated ? static_cast<length_t>(n) : written;
            if (truncated && (out_len >= 3)) {
                auto keep = static_cast<length_t>(out_len - 3);
                buf[keep + 0] = '.';
                buf[keep + 1] = '.';
                buf[keep + 2] = '.';
                buf[out_len] = '\0';
                return Object::make_string(buf_off, out_len, Object::LiteralAttrib, Object::ReadOnlyAccess);
            } else {
                buf[out_len] = '\0';
                return Object::make_string(buf_off, out_len, Object::LiteralAttrib, Object::ReadOnlyAccess);
            }
        }
    }
}

// format-object obj n -- str  (pretty-print to at most n chars)
// throws: opstack-underflow, range-check, type-check, vm-full
static void format_object_op(Trix *trx) {
    trx->verify_operands(VerifyIntegers64 | VerifyNotNegative);
    auto n_ptr = trx->m_op_ptr;
    auto obj_ptr = (n_ptr - 1);
    if (obj_ptr < trx->m_op_base) {
        trx->error(Error::OpStackUnderflow, "format-object: needs obj n on stack");
    } else {
        auto [valid, n_val] = n_ptr->uinteger_value(trx);
        if (!valid) {
            trx->error(Error::RangeCheck, "format-object: width cap out of range");
        } else {
            auto obj = *obj_ptr;
            auto result = trx->debug_format_object(obj, static_cast<integer_t>(n_val));
            obj_ptr->maybe_free_extvalue(trx);
            *obj_ptr = result;
            --trx->m_op_ptr;
        }
    }
}

// Disassemble one Object element (proc body item) into a row Array:
//   [sid line col name preview]
// line is populated from `m_debug_proc_lines` when the scanner
// tracked it for the parent proc; 0 when no entry exists (top-level
// unannotated procs, |locals| outer-body synthesized elements, or
// procs built outside scan_procedure).  sid + col stay 0 in v1 --
// the scanner only records line numbers, not (sid, line, col).
// name is the op's string form (operator name, "/foo" / "\\foo" for
// Names, or a type tag for literals).  preview is a 48-char
// format-object snapshot.
Object debug_disasm_row(Object elem, int32_t line) {
    auto sid_obj = Object::make_integer(0);
    auto line_obj = Object::make_integer(static_cast<integer_t>(line));
    auto col_obj = Object::make_integer(0);

    // Build the "name" string.  Operator -> sysoperator_value name;
    // Name -> the name's text with /\ prefix; everything else -> a
    // type tag like "<Integer>" so the row's first column always
    // reads as a category label.
    char name_buf[64];
    std::string_view name_sv;
    if (elem.is_operator()) {
        // operator_string handles both built-in (m_operator >= 0) and
        // user-defined (m_operator < 0) operators; calling sysoperator_value
        // directly would mis-convert a negative user-op index to a huge
        // name_index_t and read past the built-in table.
        name_sv = elem.operator_string(this);
    } else if (elem.is_name()) {
        auto n = offset_to_ptr<Name>(elem.m_name);
        auto raw = n->sv();
        auto raw_len = sv_length(raw);
        auto prefix = elem.is_executable() ? '\\' : '/';
        auto copy_len = std::min<length_t>(raw_len, static_cast<length_t>(sizeof(name_buf) - 2));
        name_buf[0] = prefix;
        std::copy_n(raw.data(), copy_len, name_buf + 1);
        name_sv = std::string_view{name_buf, static_cast<size_t>(copy_len + 1)};
    } else if (elem.is_sourceloc()) {
        name_sv = std::string_view{"<sourceloc>"};
    } else {
        // Use object_name with dashes=false (compact type tag like
        // "Integer", "Array", etc.).
        static_cast<void>(object_name(&elem, name_buf, /*upper=*/false, /*dashes=*/false));
        name_sv = std::string_view{name_buf};
    }
    auto name_obj = Object::make_string_dispatch(this, name_sv);

    // Preview: 48 chars of compact == form.  For non-printable
    // elements (SourceLoc) emit an empty preview rather than fail.
    auto preview = elem.is_sourceloc() ? Object::make_string_dispatch(this, std::string_view{}) : debug_format_object(elem, 48);

    // Under ${...} the row Array + both strings allocate in global VM:
    // make_string_dispatch keeps the strings in the same region as the row
    // (no local-into-global dangle after a restore), and make_empty_array
    // below can fire vm_global_gc on its VMFull retry -- root name_obj /
    // preview on the op stack across it (gated to keep the local path free).
    auto rooting{m_curr_alloc_global};
    if (rooting) {
        require_op_capacity(2);
        *++m_op_ptr = name_obj;
        *++m_op_ptr = preview;
    }

    // Build the row [sid line col name preview].
    auto row = Object::make_empty_array(this, 5);
    if (rooting) {
        m_op_ptr -= 2;
    }
    row.set_save_level(m_curr_save_level);
    auto row_data = row.array_objects(this);
    row_data[0] = sid_obj;
    row_data[1] = line_obj;
    row_data[2] = col_obj;
    row_data[3] = name_obj;
    row_data[4] = preview;
    return row;
}

// If `proc` matches the canonical |locals| outer shape -- last
// element is the begin-locals operator and the second-to-last is
// a Packed proc (the inner user body) -- return that inner Packed.
// Otherwise return the input unchanged.  proc-disasm uses this to
// auto-unwrap so the user sees their actual ops instead of the
// synthesized `[/name1 ... K N inner-packed begin-locals]` shape.
// Single-level unwrap; nested |locals| procs would require the
// caller to recurse manually on a nested Packed value.
Object debug_unwrap_locals(Object proc) {
    if (proc.m_arrays_length < 2) {
        return proc;
    } else {
        Object last;
        Object second_last;
        if (proc.is_array()) {
            auto src = proc.array_objects(this);
            last = src[proc.m_arrays_length - 1];
            second_last = src[proc.m_arrays_length - 2];
        } else if (proc.is_packed() && !proc.is_eqproc_ref()) {
            auto packed_data = proc.packed_data_ptr(this);
            Object prev;
            Object curr;
            for (length_t i = 0; i < proc.m_arrays_length; ++i) {
                auto [next, obj] = Object::extract_next_packed(this, packed_data);
                packed_data = next;
                prev = curr;
                curr = obj;
            }
            second_last = prev;
            last = curr;
        } else {
            return proc;
        }
        if (!last.is_operator() || (last.m_operator != +SystemName::BeginLocals)) {
            return proc;
        } else if (!second_last.is_packed()) {
            return proc;
        } else {
            return second_last;
        }
    }
}

// proc-disasm proc -- array  (disassemble proc body into row records)
// Accepts both packed and array procs (literal or executable).  Each
// row is [sid line col name preview]; see debug_disasm_row.  When
// the parent proc was scanned by `scan_procedure` with TRIX_DEBUGGER
// built in, the per-element line column is populated from the
// matching entry in `m_debug_proc_lines`; otherwise it's 0.
// |locals| procs auto-unwrap to their inner user body so users
// don't see the synthesized `[/name K N inner-packed begin-locals]`
// outer shape.
// throws: opstack-underflow, type-check, vm-full
static void proc_disasm_op(Trix *trx) {
    trx->verify_operands(VerifyArrays);  // accept Array or Packed
    auto proc_ptr = trx->m_op_ptr;
    auto proc_obj = trx->debug_unwrap_locals(*proc_ptr);

    // Resolve the body offset to look up the scanner-side per-op
    // source-line array.  Packed eqproc-refs share the shared
    // m_eqproc_storage buffer and were intentionally not annotated,
    // so the lookup misses (returns lines defaulting to 0).
    vm_offset_t body_offset = nulloffset;
    if (proc_obj.is_packed() && !proc_obj.is_eqproc_ref()) {
        body_offset = proc_obj.m_packed;
    } else if (proc_obj.is_array()) {
        body_offset = trx->ptr_to_offset(proc_obj.array_objects(trx));
    }
    const std::vector<int32_t> *lines_ptr = nullptr;
    if (body_offset != nulloffset) {
        auto it = trx->m_debug_proc_lines.find(body_offset);
        if (it != trx->m_debug_proc_lines.end()) {
            lines_ptr = &it->second;
        }
    }
    auto line_at = [lines_ptr](length_t i) -> int32_t {
        if ((lines_ptr == nullptr) || (i >= lines_ptr->size())) {
            return 0;
        } else {
            return (*lines_ptr)[i];
        }
    };

    // Walk the body, building rows.  For arrays we read the storage
    // pointer + length once; for packeds we step via
    // extract_next_packed.  Either way we accumulate rows in a
    // temporary opstack-mark region, then materialise an Array.
    //
    // Push a mark, then push each row Object; finally rebuild an
    // array via make_array_from_mark.  This reuses the standard
    // [..] machinery and routes through m_curr_alloc_global if set.
    auto length = proc_obj.m_arrays_length;
    // length+1 must stay in int32: narrowing to stack_depth_t (uint16_t)
    // first wraps 65536 to 0 and defeats the capacity check.
    trx->require_op_capacity(length + 1);
    *++trx->m_op_ptr = Object::make_mark();

    if (proc_obj.is_array()) {
        auto src = proc_obj.array_objects(trx);
        for (length_t i = 0; i < length; ++i) {
            auto row = trx->debug_disasm_row(src[i], line_at(i));
            *++trx->m_op_ptr = row;
        }
    } else {
        // Packed: step element-by-element via extract_next_packed.
        auto packed_data = proc_obj.packed_data_ptr(trx);
        for (length_t i = 0; i < length; ++i) {
            auto [next, obj] = Object::extract_next_packed(trx, packed_data);
            packed_data = next;
            auto row = trx->debug_disasm_row(obj, line_at(i));
            *++trx->m_op_ptr = row;
        }
    }

    auto result = Object::make_array_from_mark(trx);
    // make_array_from_mark popped to mark - 1; proc_ptr's slot still
    // holds the original proc.  Overwrite with the result.
    *proc_ptr = result;
    // m_op_ptr is now at proc_ptr (mark - 1 == proc_ptr).
}

#endif  // end TRIX_DBG
