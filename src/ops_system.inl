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

//===--- System, Environment, Introspection, and Debug Operators ---===//
//
// Time (clock, time, @time, epoch-time, now), environment (getenv, setenv, getpid),
// VM introspection (vm-size, backtrace, status, stack-probe), interrupt
// management (disable-interrupts, enable-interrupts, interrupts-enabled?,
// interrupts-pending, clear-interrupts), assertion (assert), the interactive
// debugger operators (debug-step, debug-step-over, debug-step-out, debug-continue, debug-break, debug-unbreak,
// debug-break-on-error, debug-pc, debug-on-event, debug-breakpoints -- see ops_debugger.inl), pseudo-random number generation
// (rand-seed, rand-uinteger, rand-real, rand-double, rand-ulong, rand-int128, rand-uint128, and the rand-bounded-* variants), error
// handling internals (try-catch-handler, format-backtrace), and validation utilities.
//

// throws: (none)
static void interrupt_op(Trix *) {
    // default systemdict interrupt operator, it is expected a program will override in userdict
}

// clear-object: any :- any
// Resets a numeric, boolean, array, string, or dict to its zero/empty state.
// throws: opstack-underflow, type-check
static void clearobject_op(Trix *trx) {
    trx->verify_operands(VerifyNumbers | VerifyAddress | VerifyBoolean | VerifyRWArray | VerifyRWString | VerifyRWDict);
    auto object = trx->m_op_ptr;

    switch (object->type()) {
    case Object::Type::Byte:
        object->update_byte(0);
        break;

    case Object::Type::Integer:
        object->update_integer(0);
        break;

    case Object::Type::UInteger:
        object->update_uinteger(0);
        break;

    case Object::Type::Long:
        object->update_long(trx, 0);
        break;

    case Object::Type::ULong:
        object->update_ulong(trx, 0);
        break;

    case Object::Type::Address:
        object->update_address(trx, nullptr);
        break;

    case Object::Type::Real:
        object->update_real(0.0f);
        break;

    case Object::Type::Double:
        object->update_double(trx, 0.0);
        break;

    case Object::Type::Int128:
        object->update_int128(trx, int128_t{0});
        break;

    case Object::Type::UInt128:
        object->update_uint128(trx, uint128_t{0});
        break;

    case Object::Type::Boolean:
        object->update_boolean(false);
        break;

    case Object::Type::Array:
        object->array_clear(trx);
        break;

    case Object::Type::String:
        object->string_clear(trx);
        break;

    case Object::Type::Dict: {
        object->dict_clear(trx);
        break;
    }

    case Object::Type::Null:
    case Object::Type::Operator:
    case Object::Type::Mark:
    case Object::Type::Name:
    case Object::Type::Packed:
    case Object::Type::Stream:
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
        assert(false && "clearobject_op: logic error");
        std::unreachable();
    }
}

// default-handler: name op str arr arr arr :- --
// Default error handler: prints error info to stderr.
// throws: opstack-underflow, type-check
static void default_handler_op(Trix *trx) {
    //                  last_error_name
    //                              last_operator
    //                                              error_msg
    //                                                            ostack
    //                                                                         estack
    //                                                                                      dstack
    trx->verify_operands(VerifyName, VerifyOperator, VerifyString, VerifyArray, VerifyArray, VerifyArray);

    auto last_error_name = trx->m_op_ptr;
    auto last_operator = (last_error_name - 1);
    auto error_msg = (last_operator - 1);

    auto errstr = last_error_name->name_value(trx)->sv();
    auto opstr = last_operator->operator_string(trx);
    auto msgstr = error_msg->sv_value(trx);

    trx->diag_println("Trix {} '{}': {}", errstr, opstr, msgstr);
    // Record exit code for main() before throwing.  m_last_error carries
    // the underlying Error enum value (Error::UserError for arbitrary names
    // thrown via throw-with).  quit_op leaves m_exit_code at 0; only this
    // abnormal-error path sets it non-zero.
    trx->m_exit_code = +trx->m_last_error;
    // return to interpreter and exit
    throw Exception::Exit;
}

// clearinterrupts: :- --
// Clears all pending interrupts and external error state.
// throws: (none)
static void clearinterrupts_op(Trix *trx) {
    const std::unique_lock<std::mutex> lk(trx->m_mutex);
    // Only clear user-level IRQs (Level0/1/2).  System IRQs (ErrorIRQ,
    // SuspendIRQ, ResumeIRQ, InvokeIRQ, ExitIRQ) are never cleared to
    // prevent accidental loss of shutdown signals or error reports.
    constexpr auto user_irqs = static_cast<interrupt_t>(Level0IRQ | Level1IRQ | Level2IRQ);
    trx->m_interrupt &= ~user_irqs;
    trx->m_pending_irq.store(static_cast<interrupt_t>(trx->m_interrupt & ~trx->m_interrupt_mask), std::memory_order_relaxed);
}
// Masks all interrupts.
// throws: (none)
static void disableinterrupts_op(Trix *trx) {
    const std::unique_lock<std::mutex> lk(trx->m_mutex);
    trx->m_interrupt_mask = std::numeric_limits<interrupt_t>::max();
}

// enableinterrupts: :- --
// Unmasks all interrupts.
// throws: (none)
static void enableinterrupts_op(Trix *trx) {
    const std::unique_lock<std::mutex> lk(trx->m_mutex);
    trx->m_interrupt_mask = NoIRQ;
    trx->m_pending_irq.store(trx->m_interrupt, std::memory_order_relaxed);
}

// interrupts-enabled?: :- bool
// Returns true if interrupts are currently enabled (unmasked).
// throws: opstack-overflow
static void interruptsenabled_op(Trix *trx) {
    trx->require_op_capacity(1);
    const std::unique_lock<std::mutex> lk(trx->m_mutex);
    *++trx->m_op_ptr = Object::make_boolean(trx->m_interrupt_mask == NoIRQ);
}

// interrupts-pending: :- int
// Returns the pending user-level interrupt bitmask (Level0=1, Level1=4, Level2=64).
// throws: opstack-overflow
static void interruptspending_op(Trix *trx) {
    trx->require_op_capacity(1);
    const std::unique_lock<std::mutex> lk(trx->m_mutex);
    constexpr auto user_irqs = static_cast<interrupt_t>(Level0IRQ | Level1IRQ | Level2IRQ);
    auto pending = static_cast<integer_t>(trx->m_interrupt & user_irqs);
    *++trx->m_op_ptr = Object::make_integer(pending);
}

// query-status: name :- any
// Runtime lookup of a //:status: key by name.
// Unlike the // immediate path, this resolves at execution time,
// making it suitable for exec stack inspection inside procs.
// throws: opstack-underflow, type-check, undefined
static void query_status_op(Trix *trx) {
    trx->verify_operands(VerifyName);

    auto sv = trx->m_op_ptr->name_sv(trx);
    trx->m_op_ptr--;
    auto result = trx->status_lookup(sv);
    if (result != nullptr) {
        trx->require_op_capacity(1);
        *++trx->m_op_ptr = result->make_clone(trx);
    } else {
        trx->error(Error::Undefined, "query-status: unknown key '{}'", sv);
    }
}

// vm-global-info: -- dict
// Walk every block in the global VM region (above m_vm_global) and
// return a dict reporting per-kind counts and sizes.  Used to monitor
// global-region growth, diagnose retention, and verify that #$ / $/foo
// / ${...} allocations land in the expected ChunkKind bucket.
//
// Counts LIVE (allocated) blocks under /by-kind and surfaces FREE
// blocks separately at the top level (free blocks come from
// C++-internal frees during ExtValue / WideValue / Dict-entry
// overwrites; coalescing reduces fragmentation).
//
// Result shape:
//   <<
//     /total-blocks   int   (live + free, every physical block in heap)
//     /total-bytes    int   (live + free, includes headers + padding + tail tags)
//     /live-blocks    int   (live blocks only)
//     /live-bytes     int   (block bodies of live blocks)
//     /free-blocks    int   (free blocks awaiting reuse / coalesce)
//     /free-bytes     int
//     /payload-bytes  int   (live payloads only, excluding header / tail / pad)
//     /header-bytes   int   (live total - payload; per-block overhead)
//     /by-kind        <<
//       /name   <</count int /total-bytes int /payload-bytes int>>
//       /dict   <<...>>
//       /set    <<...>>
//       ... 23 entries total ...
//       /screen <<...>>
//     >>
//   >>
//
// Sub-dicts are populated for every ChunkKind, even those with zero
// entries -- consumers can iterate /by-kind and see a stable schema.
// throws: opstack-overflow
static void vm_global_info_op(Trix *trx) {
    using namespace std::literals::string_view_literals;

    trx->require_op_capacity(1);

    constexpr size_t kKindCount = static_cast<size_t>(MaxChunkKind);
    vm_size_t counts[kKindCount] = {};
    vm_size_t total_bytes_per_kind[kKindCount] = {};
    vm_size_t payload_bytes_per_kind[kKindCount] = {};

    vm_size_t total_blocks = 0;
    vm_size_t total_bytes = 0;
    vm_size_t live_blocks = 0;
    vm_size_t live_bytes = 0;
    vm_size_t free_blocks = 0;
    vm_size_t free_bytes = 0;
    vm_size_t live_payload = 0;

    trx->gvm_for_each(
            [&](vm_offset_t /*payload_offset*/, ChunkKind kind, vm_size_t payload_size, vm_size_t block_size, bool is_free) {
                total_blocks++;
                total_bytes += block_size;
                if (is_free) {
                    free_blocks++;
                    free_bytes += block_size;
                    return;
                } else {
                    auto idx = static_cast<size_t>(+kind);
                    if (idx >= kKindCount) {
                        idx = static_cast<size_t>(ChunkKind::Other);
                    }
                    counts[idx]++;
                    total_bytes_per_kind[idx] += block_size;
                    payload_bytes_per_kind[idx] += payload_size;
                    live_blocks++;
                    live_bytes += block_size;
                    live_payload += payload_size;
                }
            });

    auto count_key = Name::make(trx, "count"sv);
    auto totalb_key = Name::make(trx, "total-bytes"sv);
    auto payload_key = Name::make(trx, "payload-bytes"sv);

    auto [by_kind, by_kind_offset] = Dict::create_dict(trx, static_cast<length_t>(kKindCount));
    for (size_t i = 0; i < kKindCount; ++i) {
        auto [sub, sub_offset] = Dict::create_dict(trx, 3);
        sub->put(trx, count_key, Object::make_integer(static_cast<integer_t>(counts[i])));
        sub->put(trx, totalb_key, Object::make_integer(static_cast<integer_t>(total_bytes_per_kind[i])));
        sub->put(trx, payload_key, Object::make_integer(static_cast<integer_t>(payload_bytes_per_kind[i])));
        by_kind->put(trx, Name::make(trx, gvm_kind_name(static_cast<ChunkKind>(i))), Object::make_dict(sub_offset));
    }

    auto [result, result_offset] = Dict::create_dict(trx, 9);
    result->put(trx, Name::make(trx, "total-blocks"sv), Object::make_integer(static_cast<integer_t>(total_blocks)));
    result->put(trx, Name::make(trx, "total-bytes"sv), Object::make_integer(static_cast<integer_t>(total_bytes)));
    result->put(trx, Name::make(trx, "live-blocks"sv), Object::make_integer(static_cast<integer_t>(live_blocks)));
    result->put(trx, Name::make(trx, "live-bytes"sv), Object::make_integer(static_cast<integer_t>(live_bytes)));
    result->put(trx, Name::make(trx, "free-blocks"sv), Object::make_integer(static_cast<integer_t>(free_blocks)));
    result->put(trx, Name::make(trx, "free-bytes"sv), Object::make_integer(static_cast<integer_t>(free_bytes)));
    result->put(trx, Name::make(trx, "payload-bytes"sv), Object::make_integer(static_cast<integer_t>(live_payload)));
    result->put(trx, Name::make(trx, "header-bytes"sv), Object::make_integer(static_cast<integer_t>(live_bytes - live_payload)));
    result->put(trx, Name::make(trx, "by-kind"sv), Object::make_dict(by_kind_offset));

    *++trx->m_op_ptr = Object::make_dict(result_offset);
}

// stack-probe: name int :- any
// Probe a stack element by 0-indexed offset from top.
// name is one of: /op-stack, /exec-stack, /dict-stack, /error-stack.
// throws: opstack-underflow, range-check, type-check, undefined
static void stack_probe_op(Trix *trx) {
    trx->verify_operands(VerifyIntegers | VerifyNotNegative, VerifyName);

    auto index_ptr = trx->m_op_ptr;
    auto [valid, index] = index_ptr->integer_value(trx, 0);
    auto name_sv = (index_ptr - 1)->name_sv(trx);
    index_ptr->maybe_free_extvalue(trx);
    trx->m_op_ptr -= 2;

    using namespace std::literals::string_view_literals;

    Object *stack_ptr;
    Object *stack_base;
    std::string_view stack_name;

    if (name_sv == "op-stack"sv) {
        stack_ptr = trx->m_op_ptr;
        stack_base = trx->m_op_base;
        stack_name = "op"sv;
    } else if (name_sv == "exec-stack"sv) {
        stack_ptr = trx->m_exec_ptr;
        stack_base = trx->m_exec_base;
        stack_name = "exec"sv;
    } else if (name_sv == "dict-stack"sv) {
        stack_ptr = trx->m_dict_ptr;
        stack_base = trx->m_dict_base;
        stack_name = "dict"sv;
    } else if (name_sv == "error-stack"sv) {
        stack_ptr = trx->m_err_ptr;
        stack_base = trx->m_err_base;
        stack_name = "error"sv;
    } else {
        trx->error(Error::Undefined, "stack-probe: unknown stack '{}'", name_sv);
    }

    auto depth = static_cast<integer_t>(stack_ptr - stack_base + 1);
    if (!valid || (index >= depth)) {
        trx->error(Error::RangeCheck, "stack-probe: {}-stack index {} out of range (depth={})", stack_name, index, depth);
    } else {
        trx->require_op_capacity(1);
        *++trx->m_op_ptr = (stack_ptr - index)->make_clone(trx);
    }
}

// status: stream :- bool
// Tests whether the stream is open.
// throws: opstack-underflow, type-check
static void status_op(Trix *trx) {
    trx->verify_operands(VerifyStream);

    auto [stream, sid] = trx->m_op_ptr->stream_value(trx);
    *trx->m_op_ptr = Object::make_boolean(stream->is_open(sid));
}

// interactive?: stream :- bool
// Tests whether the stream is connected to an interactive terminal.
// Returns false for closed streams, string/memory streams, and file
// streams whose fd is not a tty.  Lets scripts distinguish a redirected
// or piped stdin from a human at the console, so REPL-style code can
// avoid blocking on read-line in batch mode.
// throws: opstack-underflow, type-check
static void interactive_op(Trix *trx) {
    trx->verify_operands(VerifyStream);

    auto [stream, sid] = trx->m_op_ptr->stream_value(trx);
    auto tty = false;
    if (stream->is_open(sid) && (stream->m_fd >= 0)) {
        tty = (::isatty(stream->m_fd) == 1);
    }
    *trx->m_op_ptr = Object::make_boolean(tty);
}

// last-error: :- name
// Pushes the error name of the last error.
// throws: opstack-overflow
static void last_error_op(Trix *trx) {
    trx->require_op_capacity(1);

    *++trx->m_op_ptr = *trx->m_last_error_name_ptr;
}

// last-error-data: :- any
// Pushes the error data of the last error (set by throw-with, null otherwise).
// throws: opstack-overflow
static void last_error_data_op(Trix *trx) {
    trx->require_op_capacity(1);

    *++trx->m_op_ptr = trx->m_last_error_data_ptr->make_clone(trx);
}

// last-error-message: :- string
// Pushes the error message string of the last error.
// throws: opstack-overflow
static void last_error_message_op(Trix *trx) {
    trx->require_op_capacity(1);

    *++trx->m_op_ptr = trx->m_last_error_msg_ptr->make_clone(trx);
}

// vm-size: any :- int
// Returns the number of VM heap bytes consumed by the operand's data storage.
// Value types (integers, booleans, etc.) return 0 (inline, no heap allocation).
// ExtValue types (Long, ULong, Double, Address) return 8.
// Containers return their full VM allocation:
//   String:  length + 1  (content + NUL terminator)
//   Array:   length * 8  (length * sizeof(Object))
//   Packed:  variable    (compressed encoding bytes)
//   Dict:    header + buckets + entry pool
//   Name:    header + string bytes
//   Curry:   16  (2 Objects)
// throws: opstack-underflow
static void vmsize_op(Trix *trx) {
    trx->require_op_count(1);

    auto object = trx->m_op_ptr;
    integer_t size = 0;

    switch (object->type()) {
    case Object::Type::String:
        size = static_cast<integer_t>(object->m_string_length + 1);
        break;

    case Object::Type::Array:
        size = static_cast<integer_t>(object->arrays_length() * sizeof(Object));
        break;

    case Object::Type::Packed: {
        // walk the packed data to compute total byte span
        auto data_start = trx->offset_to_ptr<const packed_data_t>(object->m_packed);
        auto data = data_start;
        auto count = object->arrays_length();
        for (length_t i = 0; i < count; ++i) {
            auto header = *data++;
            auto [length_size, value_size, _] = Object::packed_sizes(header);
            data += length_size;
            data += value_size;
        }
        size = static_cast<integer_t>(data - data_start);
        break;
    }

    case Object::Type::Dict: {
        auto dict = object->dict_value(trx);
        size = static_cast<integer_t>(dict->vm_size());
        break;
    }

    case Object::Type::Set: {
        auto set = object->set_value(trx);
        size = static_cast<integer_t>(set->vm_size());
        break;
    }

    case Object::Type::Name: {
        auto name = object->name_value(trx);
        // Name header is 16 bytes (offsetof(Name, m_data)), followed by string data
        size = static_cast<integer_t>(16 + name->length());
        break;
    }

    case Object::Type::Curry:
    case Object::Type::Tagged:
        size = static_cast<integer_t>(2 * sizeof(Object));
        break;

    case Object::Type::Thunk:
        size = static_cast<integer_t>(3 * sizeof(Object));
        break;

    case Object::Type::Record: {
        auto fc = object->object_length();
        auto inst_size = sizeof(vm_offset_t) + (fc * sizeof(Object));
        auto inst = object->record_instance(trx);
        auto schema = trx->offset_to_ptr<RecordSchema>(inst->m_schema);
        auto schema_size = offsetof(RecordSchema, m_names) + (schema->m_field_count * sizeof(Object));
        size = static_cast<integer_t>(inst_size + schema_size);
        break;
    }

    case Object::Type::Long:
    case Object::Type::ULong:
    case Object::Type::Double:
    case Object::Type::Address:
        size = static_cast<integer_t>(sizeof(Object::ExtValue));
        break;

    case Object::Type::Int128:
    case Object::Type::UInt128:
        size = static_cast<integer_t>(sizeof(Object::WideValue));
        break;

    case Object::Type::Null:
    case Object::Type::Byte:
    case Object::Type::Integer:
    case Object::Type::UInteger:
    case Object::Type::Real:
    case Object::Type::Boolean:
    case Object::Type::Operator:
    case Object::Type::Mark:
    case Object::Type::Stream:
    case Object::Type::SourceLoc:
    case Object::Type::Coroutine:
    case Object::Type::PipeBuffer:
    case Object::Type::Cell:
    case Object::Type::Continuation:
    case Object::Type::OpaqueHandle:
        size = 0;
        break;
    }

    object->maybe_free_extvalue(trx);
    *object = Object::make_integer(size);
}

// backtrace: :- array
// Returns an array of dicts representing the current call stack.
// Each dict has: /name (string), /file (string), /line (integer), /col (integer).
// throws: opstack-overflow, vm-full
static void backtrace_op(Trix *trx) {
    using namespace std::literals::string_view_literals;

    // Pass 1: count @call frames
    length_t frame_count = 0;
    for (auto object = trx->m_exec_ptr; object >= trx->m_exec_base; --object) {
        if (object->is_operator() && object->operator_is_call()) {
            auto companion = (object - 1);
            if ((companion >= trx->m_exec_base) && companion->is_name()) {
                ++frame_count;
            }
        }
    }

    // Pass 2: allocate result array and populate dicts
    auto result = Object::make_empty_array(trx, frame_count, Object::LiteralAttrib, Object::ReadOnlyAccess);
    // Under ${...} the result array is global; its per-frame dicts and strings
    // must allocate in the same region or they dangle after a restore.  Root the
    // half-built result on the op stack across those allocations (each can fire a
    // global GC), gated so the local path stays push-free.  gvm_alloc never
    // relocates an existing block, so result_objects stays valid.
    if (trx->m_curr_alloc_global) {
        trx->require_op_capacity(1);
        *++trx->m_op_ptr = result;
    }
    auto result_objects = result.array_objects(trx);
    length_t index = 0;

    for (auto object = trx->m_exec_ptr; object >= trx->m_exec_base; --object) {
        if (object->is_operator() && object->operator_is_call()) {
            auto companion = (object - 1);
            if ((companion >= trx->m_exec_base) && companion->is_name()) {
                auto name = trx->offset_to_ptr<Name>(companion->m_name);
                auto name_sv = name->sv();

                // create a dict with 4 entries: /name, /file, /line, /col
                auto [dict, dict_offset] =
                        trx->m_curr_alloc_global ? Dict::create_global_dict(trx, 4) : Dict::create_or_recycle(trx, 4);

                dict->put(trx, Name::make(trx, "name"), Object::make_string_dispatch(trx, name_sv));

                auto loc_slot = (object - 2);
                if ((loc_slot >= trx->m_exec_base) && loc_slot->is_sourceloc()) {
                    auto sid = loc_slot->m_source_sid;
                    auto line = loc_slot->m_source_line;
                    auto col = loc_slot->m_source_pos;
                    std::string_view filename = "<unknown>"sv;
                    if (sid == Stream::STRING_SID) {
                        filename = "<string>"sv;
                    } else {
                        for (auto s = trx->m_stream_inuse_list; s != nullptr; s = s->next_stream(trx)) {
                            if (s->m_sid == sid) {
                                filename = s->source_location(trx).source;
                                break;
                            }
                        }
                    }
                    dict->put(trx, Name::make(trx, "file"), Object::make_string_dispatch(trx, filename));
                    dict->put(trx, Name::make(trx, "line"), Object::make_integer(static_cast<integer_t>(line)));
                    dict->put(trx, Name::make(trx, "col"), Object::make_integer(static_cast<integer_t>(col)));
                }

                dict->set_readonly_access_no_save();
                auto dict_obj = Object::make_dict(dict_offset);
                dict_obj.set_readonly_access();
                result_objects[index++] = dict_obj;
            }
        }
    }

    if (trx->m_curr_alloc_global) {
        --trx->m_op_ptr;  // drop the in-flight result root
    }
    trx->require_op_capacity(1);
    *++trx->m_op_ptr = result;
}

// vm-heap-tracking ops (alloc-stats + side-table).  All are gated out in
// release builds (TRIX_HEAP_TRACKING undefined) -- the dispatch cases
// in dispatch.inl are also gated, so these function definitions are
// only referenced when the feature is enabled.  See src/build_config.inl.
#ifdef TRIX_HEAP_TRACKING
// alloc-stats :- array
// Returns an array of dicts, one per tracked (sid, line) site:
//   << /file (filename) /line N /count C /bytes B >>
// Sites are unordered.  Sort and slice on the Trix side as desired.
// Saturated tracking (more distinct sites than the table can hold) is
// reported via :status:alloc-saturated; the array still contains the
// sites that were captured before saturation.
//
// Tracking is paused for the duration of this op so the result-array's
// own allocations do not pollute the counters.
static void alloc_stats_op(Trix *trx) {
    using namespace std::literals::string_view_literals;

    auto was_paused = trx->m_alloc_tracking_paused;
    trx->m_alloc_tracking_paused = true;

    auto count = static_cast<length_t>(trx->m_alloc_stats_count);
    auto result = Object::make_empty_array(trx, count, Object::LiteralAttrib, Object::ReadOnlyAccess);
    if (trx->m_curr_alloc_global) {
        trx->require_op_capacity(1);
        *++trx->m_op_ptr = result;  // root across the per-entry global dict/string allocs
    }
    auto result_objects = result.array_objects(trx);

    auto file_name = Name::make(trx, "file");
    auto line_name = Name::make(trx, "line");
    auto count_name = Name::make(trx, "count");
    auto bytes_name = Name::make(trx, "bytes");

    length_t out_index = 0;
    for (size_t i = 0; (i < trx->m_alloc_stats_capacity) && (out_index < count); ++i) {
        const auto &entry = trx->m_alloc_stats[i];
        if (entry.m_key != 0) {
            auto sid = static_cast<stream_id_t>(entry.m_key >> 32);
            auto line = static_cast<uint32_t>(entry.m_key);

            std::string_view filename = "<unknown>"sv;
            if (sid == Stream::STRING_SID) {
                filename = "<string>"sv;
            } else if (sid != Stream::INVALID_SID) {
                for (auto s = trx->m_stream_inuse_list; s != nullptr; s = s->next_stream(trx)) {
                    if (s->m_sid == sid) {
                        filename = s->source_location(trx).source;
                        break;
                    }
                }
            }

            auto [dict, dict_offset] =
                    trx->m_curr_alloc_global ? Dict::create_global_dict(trx, 4) : Dict::create_or_recycle(trx, 4);
            dict->put(trx, file_name, Object::make_string_dispatch(trx, filename));
            dict->put(trx, line_name, Object::make_integer(static_cast<integer_t>(line)));
            dict->put(trx, count_name, Object::make_integer(static_cast<integer_t>(entry.m_count)));
            dict->put(trx, bytes_name, Object::make_integer(static_cast<integer_t>(entry.m_bytes)));
            dict->set_readonly_access_no_save();

            auto dict_obj = Object::make_dict(dict_offset);
            dict_obj.set_readonly_access();
            result_objects[out_index++] = dict_obj;
        }
    }

    if (trx->m_curr_alloc_global) {
        --trx->m_op_ptr;  // drop the in-flight result root
    }
    trx->require_op_capacity(1);
    *++trx->m_op_ptr = result;

    trx->m_alloc_tracking_paused = was_paused;
}

// clear-alloc-stats :-
// Resets BOTH the alloc-stats hash table and the per-block
// heap-track side-table, so a subsequent vm-heap-snapshot cursor
// measures from a fresh baseline.  After this op:
//   - every alloc-stats entry's key is zero (empty),
//   - the side-table cursor (m_heap_track_count) is zero,
//   - the side-table generation bumps so any prior vm-heap-snapshot
//     cursors are correctly detected as stale,
//   - both saturation flags are cleared.
// Tracking remains enabled (or disabled, if it was disabled).
static void clear_alloc_stats_op(Trix *trx) {
    std::fill_n(trx->m_alloc_stats, trx->m_alloc_stats_capacity, AllocStatsEntry{});
    trx->m_alloc_stats_count = 0;
    trx->m_alloc_stats_saturated = false;

    trx->m_heap_track_count = 0;
    trx->m_heap_track_saturated = false;
    ++trx->m_heap_track_generation;

    // Reset peak trackers to the live water-marks so a fresh
    // window starts measuring from "now".
    trx->m_vm_peak_ptr = trx->m_vm_ptr;
    trx->m_vm_peak_temp_used = static_cast<vm_size_t>(trx->m_vm_limit - trx->m_vm_temp_ptr);
}

// Pack a side-table snapshot cursor into a single ulong:
//   high 32 bits = m_heap_track_generation at capture time
//   low  32 bits = m_heap_track_count       at capture time
// 32-bit integer would truncate the generation half, so a 64-bit
// value (ulong, ExtValue-backed) is used.  The ExtValue pool recycles
// snapshot allocations across calls so the steady-state cost is just
// a free-list pop.
static ulong_t encode_heap_snapshot(uint32_t generation, uint32_t cursor) {
    return ((static_cast<ulong_t>(generation) << 32) | static_cast<ulong_t>(cursor));
}

// vm-heap-snapshot :- ulong
// Captures the current vm-heap-track cursor + generation as a single
// ulong.  Pass two of these to vm-heap-diff.  Tracking is paused
// across the (cheap) ExtValue allocation so the snapshot operation
// itself does not enter the side-table.
static void vm_heap_snapshot_op(Trix *trx) {
    auto was_paused = trx->m_alloc_tracking_paused;
    trx->m_alloc_tracking_paused = true;

    auto encoded = encode_heap_snapshot(trx->m_heap_track_generation, static_cast<uint32_t>(trx->m_heap_track_count));
    trx->require_op_capacity(1);
    *++trx->m_op_ptr = Object::make_ulong(trx, encoded);

    trx->m_alloc_tracking_paused = was_paused;
}

// vm-heap-diff   a b :- arr
// Aggregates the per-block side-table entries strictly between cursors
// a and b (b assumed >= a; values are swapped if not).  Returns an
// array of read-only dicts, one per (sid, line) site that allocated
// at least once in the window:
//   << /file (filename) /line N /count C /bytes B >>
//
// Both snapshots' generations must match the current
// m_heap_track_generation; if either is stale, the op throws
// /unsupported.  Staleness means a save/restore (or
// clear-alloc-stats) intervened and dropped side-table entries the
// snapshots were pointing at.
//
// Tracking is paused for the duration of this op so the result-array's
// own allocations do not extend the side-table.
static void vm_heap_diff_op(Trix *trx) {
    using namespace std::literals::string_view_literals;

    auto was_paused = trx->m_alloc_tracking_paused;
    trx->m_alloc_tracking_paused = true;

    trx->verify_operands(VerifyULong, VerifyULong);

    auto b_ptr = trx->m_op_ptr;
    auto a_ptr = (b_ptr - 1);
    auto a_packed = a_ptr->ulong_value(trx);
    auto b_packed = b_ptr->ulong_value(trx);

    auto a_gen = static_cast<uint32_t>(a_packed >> 32);
    auto b_gen = static_cast<uint32_t>(b_packed >> 32);
    auto a_cursor = static_cast<uint32_t>(a_packed);
    auto b_cursor = static_cast<uint32_t>(b_packed);

    if ((a_gen != trx->m_heap_track_generation) || (b_gen != trx->m_heap_track_generation)) {
        trx->error(Error::Unsupported,
                   "vm-heap-diff: snapshot is stale (heap-track generation moved; a save/restore or clear-alloc-stats "
                   "intervened)");
    }
    if (a_cursor > b_cursor) {
        std::swap(a_cursor, b_cursor);
    }
    if (b_cursor > trx->m_heap_track_count) {
        trx->error(Error::Unsupported,
                   "vm-heap-diff: snapshot cursor {} exceeds live entry count {}",
                   b_cursor,
                   trx->m_heap_track_count);
    }

    a_ptr->maybe_free_extvalue(trx);
    b_ptr->maybe_free_extvalue(trx);
    trx->m_op_ptr -= 2;

    // Pass 1: aggregate by m_key.  Use vm_temp_alloc (top-end region,
    // LIFO-reclaimable) so the scratch buffer doesn't survive the op.
    // Window is bounded by m_heap_track_count which is bounded by
    // HeapTrackTableSize, so the scratch is at worst ~256 KB.
    auto saved_temp = trx->m_vm_temp_ptr;
    auto window = static_cast<size_t>(b_cursor - a_cursor);
    auto scratch = (window > 0) ? trx->vm_temp_alloc<AllocStatsEntry>(static_cast<vm_size_t>(window))
                                : static_cast<AllocStatsEntry *>(nullptr);
    if (window > 0) {
        std::fill_n(scratch, window, AllocStatsEntry{});
    }
    auto scratch_count = size_t{0};

    for (auto i = static_cast<size_t>(a_cursor); i < static_cast<size_t>(b_cursor); ++i) {
        auto entry = trx->m_heap_track[i];
        // size = next entry's offset - this offset, except the last live
        // entry whose size = current m_vm_ptr - this offset.
        auto next_offset = ((i + 1) < trx->m_heap_track_count) ? trx->m_heap_track[i + 1].m_offset
                                                               : static_cast<vm_offset_t>(trx->m_vm_ptr - trx->m_vm_base);
        auto size = static_cast<uint64_t>(next_offset - entry.m_offset);

        // linear-probe scan into scratch
        auto found = false;
        for (auto j = size_t{0}; j < scratch_count; ++j) {
            if (scratch[j].m_key == entry.m_key) {
                ++scratch[j].m_count;
                scratch[j].m_bytes += size;
                found = true;
                break;
            }
        }
        if (!found) {
            scratch[scratch_count].m_key = entry.m_key;
            scratch[scratch_count].m_count = 1;
            scratch[scratch_count].m_bytes = size;
            ++scratch_count;
        }
    }

    // Pass 2: build a Trix-side array of read-only dicts.
    auto result =
            Object::make_empty_array(trx, static_cast<length_t>(scratch_count), Object::LiteralAttrib, Object::ReadOnlyAccess);
    if (trx->m_curr_alloc_global) {
        trx->require_op_capacity(1);
        *++trx->m_op_ptr = result;  // root across the per-entry global dict/string allocs
    }
    auto result_objects = result.array_objects(trx);

    auto file_name = Name::make(trx, "file");
    auto line_name = Name::make(trx, "line");
    auto count_name = Name::make(trx, "count");
    auto bytes_name = Name::make(trx, "bytes");

    for (auto i = size_t{0}; i < scratch_count; ++i) {
        const auto &entry = scratch[i];
        auto sid = static_cast<stream_id_t>(entry.m_key >> 32);
        auto line = static_cast<uint32_t>(entry.m_key);

        std::string_view filename = "<unknown>"sv;
        if (sid == Stream::STRING_SID) {
            filename = "<string>"sv;
        } else if (sid != Stream::INVALID_SID) {
            for (auto s = trx->m_stream_inuse_list; s != nullptr; s = s->next_stream(trx)) {
                if (s->m_sid == sid) {
                    filename = s->source_location(trx).source;
                    break;
                }
            }
        }

        auto [dict, dict_offset] = trx->m_curr_alloc_global ? Dict::create_global_dict(trx, 4) : Dict::create_or_recycle(trx, 4);
        dict->put(trx, file_name, Object::make_string_dispatch(trx, filename));
        dict->put(trx, line_name, Object::make_integer(static_cast<integer_t>(line)));
        dict->put(trx, count_name, Object::make_integer(static_cast<integer_t>(entry.m_count)));
        dict->put(trx, bytes_name, Object::make_integer(static_cast<integer_t>(entry.m_bytes)));
        dict->set_readonly_access_no_save();

        auto dict_obj = Object::make_dict(dict_offset);
        dict_obj.set_readonly_access();
        result_objects[i] = dict_obj;
    }

    if (trx->m_curr_alloc_global) {
        --trx->m_op_ptr;  // drop the in-flight result root
    }
    trx->require_op_capacity(1);
    *++trx->m_op_ptr = result;

    trx->vm_temp_restore(saved_temp);
    trx->m_alloc_tracking_paused = was_paused;
}

#endif  // end TRIX_HEAP_TRACK ops

// Parse a non-negative integer from a string_view suffix.
// //:status:key -- on-demand VM introspection.
// Computes a single value per key and returns a pointer to the scratch Object.
// No Dict or bulk refresh needed; each key is computed independently.
// IMPORTANT: when adding a new key here, also add it to the status_names[]
// pre-intern table in init() so the name is allocated before any save barrier.
// --- status_lookup subsystem helpers ---
// Each returns true if the key was handled, setting m_status_scratch_obj.

[[nodiscard]] bool status_lookup_vm(std::string_view key) {
    using namespace std::literals::string_view_literals;
    if (key == "vm-used"sv) {
        m_status_scratch_obj = Object::make_integer(static_cast<integer_t>(m_vm_ptr - m_vm_base));
    } else if (key == "vm-temp-used"sv) {
        m_status_scratch_obj = Object::make_integer(static_cast<integer_t>(m_vm_global - m_vm_temp_ptr));
    } else if (key == "vm-free"sv) {
        // Free space sits between LOCAL (top at m_vm_ptr) and TEMP (bottom at m_vm_temp_ptr).
        m_status_scratch_obj = Object::make_integer(static_cast<integer_t>(m_vm_temp_ptr - m_vm_ptr));
    } else if (key == "vm-total"sv) {
        m_status_scratch_obj = Object::make_integer(static_cast<integer_t>(m_vm_limit - m_vm_base));
    } else if (key == "vm-alloc-since-save"sv) {
        if (m_curr_save_level > Save::BASE) {
            auto barrier = m_save_stack[m_curr_save_level];
            m_status_scratch_obj =
                    Object::make_integer(static_cast<integer_t>(m_vm_ptr - m_vm_base) - static_cast<integer_t>(barrier));
        } else {
            m_status_scratch_obj = Object::make_integer(static_cast<integer_t>(m_vm_ptr - m_vm_base));
        }
    } else if (key == "vm-alloc-active"sv) {
        m_status_scratch_obj = Object::make_boolean(m_vm_alloc_active != nullptr);
    } else if (key == "vm-global-used"sv) {
        m_status_scratch_obj = Object::make_integer(static_cast<integer_t>(m_vm_limit - m_vm_global));
    } else if (key == "vm-global-num-alloc"sv) {
        // O(1) live count of allocated user blocks in the global region
        // (gc-scratch excluded).  Mirror of //:vm-global-info:user-blocks
        // without the O(N) heap walk.
        m_status_scratch_obj = Object::make_integer(static_cast<integer_t>(m_gvm_user_block_count));
    } else if (key == "vm-global-num-free"sv) {
        // O(1) count of blocks currently on the global VM free list (main
        // list + fastbins combined).  Maintained by the four lower-level
        // free-list mutators in gvm_heap.inl.
        m_status_scratch_obj = Object::make_integer(static_cast<integer_t>(m_gvm_free_block_count));
    } else if (key == "gc-runs"sv) {
        m_status_scratch_obj = Object::make_long(this, static_cast<long_t>(m_gc_run_count));
    } else if (key == "gc-last-reclaimed"sv) {
        m_status_scratch_obj = Object::make_integer(static_cast<integer_t>(m_gc_last_reclaimed_bytes));
    } else if (key == "gc-in-progress"sv) {
        m_status_scratch_obj = Object::make_boolean(m_in_gc);
    } else if (key == "gc-current-gen"sv) {
        // Current generation counter; bumped (mod 256, with a forced
        // clear-walk on wrap) by gc_advance_generation each pass.
        // Useful for tests that want to assert "GC ran exactly N times
        // in this scope", and for exercising the wrap-pass clear walk
        // by running 256+ passes in a row.
        m_status_scratch_obj = Object::make_integer(static_cast<integer_t>(m_gc_current_gen));
    } else if (key == "vm-validate"sv) {
        auto trx = this;
        bool valid = true;

        if (m_vm_ptr > m_vm_temp_ptr) {
            valid = false;
        }

        auto validate_object = [trx](Object obj) -> bool {
            if (obj.uses_extvalue()) {
                auto off = obj.extvalue_offset();
                if (off == nulloffset) {
                    return false;
                } else if (!trx->valid_active_offset<Object::ExtValue>(off)) {
                    return false;
                } else if (!Object::ExtValue::valid(trx, off)) {
                    return false;
                }
            } else if (obj.uses_widevalue()) {
                auto off = obj.offset();
                if (off == nulloffset) {
                    return false;
                } else if (!trx->valid_active_offset<Object::WideValue>(off)) {
                    return false;
                } else if (!Object::WideValue::valid(trx, off)) {
                    return false;
                }
            } else if (obj.uses_vm()) {
                auto off = obj.offset();
                if (off == nulloffset) {
                    return false;
                } else if (!trx->valid_active_offset<vm_t>(off)) {
                    return false;
                }
            }
            return true;
        };

        for (auto p = m_op_base; valid && (p <= m_op_ptr); ++p) {
            if (!validate_object(*p)) {
                valid = false;
            }
        }
        for (auto p = m_exec_base; valid && (p <= m_exec_ptr); ++p) {
            // sourceloc slots are not Objects; validate everything else
            if (!p->is_sourceloc() && !validate_object(*p)) {
                valid = false;
            }
        }
        for (auto p = m_dict_base; valid && (p <= m_dict_ptr); ++p) {
            if (!validate_object(*p)) {
                valid = false;
            }
        }
        if (valid) {
            valid = Name::validate_chains(trx);
        }
        if (valid) {
            valid = Object::ExtValue::validate_free_lists(trx);
        }
        if (valid) {
            valid = Object::WideValue::validate_free_lists(trx);
        }
        if (valid) {
            // Scheduler / wakeup invariants (timer list, ready queues,
            // stdin slot, mailbox + pipe registrations, orphaned-sleeper
            // rule).  See ops_coroutine.inl::scheduler_validate.
            valid = scheduler_validate();
        }

        m_status_scratch_obj = Object::make_boolean(valid);
    } else {
        return false;
    }
    return true;
}

[[nodiscard]] bool status_lookup_stacks(std::string_view key) {
    using namespace std::literals::string_view_literals;
    // depths
    if (key == "op-depth"sv) {
        m_status_scratch_obj = Object::make_integer(op_count());
    } else if (key == "exec-depth"sv) {
        m_status_scratch_obj = Object::make_integer(static_cast<integer_t>(m_exec_ptr - m_exec_base + 1));
    } else if (key == "dict-depth"sv) {
        m_status_scratch_obj = Object::make_integer(static_cast<integer_t>(m_dict_ptr - m_dict_base + 1));
    } else if (key == "error-depth"sv) {
        m_status_scratch_obj = Object::make_integer(static_cast<integer_t>(m_err_ptr - m_err_base + 1));
        // limits
    } else if (key == "max-op-depth"sv) {
        m_status_scratch_obj = Object::make_integer(static_cast<integer_t>(m_op_limit - m_op_base));
    } else if (key == "max-exec-depth"sv) {
        m_status_scratch_obj = Object::make_integer(static_cast<integer_t>(m_exec_limit - m_exec_base));
    } else if (key == "max-dict-depth"sv) {
        m_status_scratch_obj = Object::make_integer(static_cast<integer_t>(m_dict_limit - m_dict_base));
    } else if (key == "max-error-depth"sv) {
        m_status_scratch_obj = Object::make_integer(static_cast<integer_t>(m_err_limit - m_err_base));
    } else if (key == "max-save-depth"sv) {
        m_status_scratch_obj = Object::make_integer(static_cast<integer_t>(m_max_save_level));
        // high-water marks
    } else if (key == "op-high-water"sv) {
        auto hw = (m_op_ptr > m_op_high_water_ptr) ? m_op_ptr : m_op_high_water_ptr;
        m_status_scratch_obj = Object::make_integer(static_cast<integer_t>(hw - m_op_base + 1));
    } else if (key == "exec-high-water"sv) {
        m_status_scratch_obj = Object::make_integer(static_cast<integer_t>(m_exec_high_water_ptr - m_exec_base + 1));
    } else if (key == "dict-high-water"sv) {
        m_status_scratch_obj = Object::make_integer(static_cast<integer_t>(m_dict_high_water_ptr - m_dict_base + 1));
    } else if (key == "error-high-water"sv) {
        m_status_scratch_obj = Object::make_integer(static_cast<integer_t>(m_err_high_water_ptr - m_err_base + 1));
    } else {
        return false;
    }
    return true;
}

[[nodiscard]] bool status_lookup_save(std::string_view key) {
    using namespace std::literals::string_view_literals;
    if (key == "save-level"sv) {
        m_status_scratch_obj = Object::make_integer(static_cast<integer_t>(m_curr_save_level));
    } else if (key == "save-journal-entries"sv) {
        m_status_scratch_obj = Object::make_integer(static_cast<integer_t>(m_save_journal_count[m_curr_save_level]));
    } else if (key == "save-journal-bytes"sv) {
        m_status_scratch_obj = Object::make_integer(static_cast<integer_t>(m_save_journal_bytes[m_curr_save_level]));
    } else if (key == "frame-dict-pool-evictions"sv) {
        m_status_scratch_obj = Object::make_ulong(this, m_frame_dict_pool_evictions);
    } else if (key == "save-vm-barrier"sv) {
        if (m_curr_save_level > Save::BASE) {
            m_status_scratch_obj = Object::make_integer(static_cast<integer_t>(m_save_stack[m_curr_save_level]));
        } else {
            m_status_scratch_obj = Object::make_integer(0);
        }
    } else if (key == "save-name-barrier"sv) {
        if (m_curr_save_level > Save::BASE) {
            m_status_scratch_obj = Object::make_integer(static_cast<integer_t>(m_vm_ptr - m_vm_base));
        } else {
            m_status_scratch_obj = Object::make_integer(0);
        }
    } else if (key == "save-journal-validate"sv) {
        m_status_scratch_obj = Object::make_boolean(Save::validate_journal(this));
    } else {
        return false;
    }
    return true;
}

[[nodiscard]] bool status_lookup_exec(std::string_view key) {
    using namespace std::literals::string_view_literals;
    if (key == "exec-validate"sv) {
        bool valid = true;
        for (auto p = m_exec_base; valid && (p <= m_exec_ptr); ++p) {
            if (p->is_operator()) {
                auto idx = p->m_operator;
                if (idx >= 0) {
                    if (idx >= SYSTEMNAME_COUNT) {
                        valid = false;
                        break;
                    } else if (p->operator_is_call()) {
                        auto name_slot = (p - 1);
                        auto loc_slot = (p - 2);
                        if ((name_slot < m_exec_base) || (loc_slot < m_exec_base)) {
                            valid = false;
                        } else if (!name_slot->is_name() || !loc_slot->is_sourceloc()) {
                            valid = false;
                        }
                    } else if (p->operator_is_control()) {
                        auto popcount = p->operator_popcount();
                        if ((p - popcount) < m_exec_base) {
                            valid = false;
                        }
                    }
                }
            }
        }
        m_status_scratch_obj = Object::make_boolean(valid);
    } else if (key == "exec-top-is-call"sv) {
        auto is_call = (m_exec_ptr >= m_exec_base) && m_exec_ptr->is_operator() && m_exec_ptr->operator_is_call();
        m_status_scratch_obj = Object::make_boolean(is_call);
    } else if (key == "exec-top-is-barrier"sv) {
        auto is_barrier = (m_exec_ptr >= m_exec_base) && m_exec_ptr->is_operator() && m_exec_ptr->operator_is_control();
        m_status_scratch_obj = Object::make_boolean(is_barrier);
    } else if (key == "exec-top-type"sv) {
        if (m_exec_ptr >= m_exec_base) {
            m_status_scratch_obj = type_name(m_exec_ptr->type());
        } else {
            m_status_scratch_obj = Object::make_null();
        }
    } else if (key == "call-depth"sv) {
        integer_t count = 0;
        for (auto p = m_exec_base; p <= m_exec_ptr; ++p) {
            if (p->is_operator() && p->operator_is_call()) {
                ++count;
            }
        }
        m_status_scratch_obj = Object::make_integer(count);
    } else if (key == "op-count"sv) {
        m_status_scratch_obj = Object::make_ulong(this, m_op_count);
    } else if (key == "tco-count"sv) {
        m_status_scratch_obj = Object::make_ulong(this, m_tco_count);
    } else {
        return false;
    }
    return true;
}

[[nodiscard]] bool status_lookup_extvalue(std::string_view key) {
    using namespace std::literals::string_view_literals;
    if (key == "extvalue-free"sv) {
        integer_t count = 0;
        for (save_level_t i = 0; i <= m_curr_save_level; ++i) {
            auto offset = m_extvalue_free_list[i];
            while (offset != nulloffset) {
                ++count;
                offset = offset_to_value<vm_offset_t>(offset);
            }
        }
        m_status_scratch_obj = Object::make_integer(count);
    } else if (key == "extvalue-active"sv) {
        m_status_scratch_obj = Object::make_integer(m_extvalue_active_count);
    } else if (key == "extvalue-validate"sv) {
        m_status_scratch_obj = Object::make_boolean(Object::ExtValue::validate_free_lists(this));
    } else if (key == "widevalue-free"sv) {
        m_status_scratch_obj = Object::make_integer(Object::WideValue::free_list_count(this));
    } else if (key == "widevalue-active"sv) {
        m_status_scratch_obj = Object::make_integer(m_widevalue_active_count);
    } else if (key == "widevalue-validate"sv) {
        m_status_scratch_obj = Object::make_boolean(Object::WideValue::validate_free_lists(this));
    } else {
        return false;
    }
    return true;
}

[[nodiscard]] bool status_lookup_streams(std::string_view key) {
    using namespace std::literals::string_view_literals;
    if (key == "stream-inuse"sv) {
        integer_t count = 0;
        for (auto s = m_stream_inuse_list; s != nullptr; s = s->next_stream(this)) {
            ++count;
        }
        m_status_scratch_obj = Object::make_integer(count);
    } else if (key == "stream-free"sv) {
        integer_t count = 0;
        for (auto s = m_stream_free_list; s != nullptr; s = s->next_stream(this)) {
            ++count;
        }
        m_status_scratch_obj = Object::make_integer(count);
    } else if (key == "stream-count"sv) {
        integer_t count = 0;
        for (auto s = m_stream_inuse_list; s != nullptr; s = s->next_stream(this)) {
            ++count;
        }
        for (auto s = m_stream_free_list; s != nullptr; s = s->next_stream(this)) {
            ++count;
        }
        m_status_scratch_obj = Object::make_integer(count);
    } else if (key == "stream-buffer-size"sv) {
        m_status_scratch_obj = Object::make_integer(static_cast<integer_t>(m_stream_buffer_size));
    } else {
        return false;
    }
    return true;
}

[[nodiscard]] bool status_lookup_names(std::string_view key) {
    using namespace std::literals::string_view_literals;
    if (key == "name-count"sv) {
        auto [nc, nvu, nmc, nac] = Name::status(this);
        m_status_scratch_obj = Object::make_integer(nc);
    } else if (key == "name-vm-used"sv) {
        auto [nc, nvu, nmc, nac] = Name::status(this);
        m_status_scratch_obj = Object::make_integer(static_cast<integer_t>(nvu));
    } else if (key == "name-max-chain"sv) {
        auto [nc, nvu, nmc, nac] = Name::status(this);
        m_status_scratch_obj = Object::make_integer(nmc);
    } else if (key == "name-avg-chain"sv) {
        auto [nc, nvu, nmc, nac] = Name::status(this);
        m_status_scratch_obj = Object::make_real(nac);
    } else if (key == "name-bucket-count"sv) {
        m_status_scratch_obj = Object::make_integer(static_cast<integer_t>(m_name_bucket_count));
    } else {
        return false;
    }
    return true;
}

[[nodiscard]] bool status_lookup_config(std::string_view key) {
    using namespace std::literals::string_view_literals;
    if (key == "userdict-length"sv) {
        m_status_scratch_obj = Object::make_integer(static_cast<integer_t>(m_userdict->length()));
    } else if (key == "userdict-maxlength"sv) {
        m_status_scratch_obj = Object::make_integer(static_cast<integer_t>(m_userdict->maxlength()));
    } else if (key == "eqstring-length"sv) {
        m_status_scratch_obj = Object::make_integer(static_cast<integer_t>(root_object(RootObject::EqString).object_length()));
    } else if (key == "eqarray-length"sv) {
        m_status_scratch_obj = Object::make_integer(static_cast<integer_t>(root_object(RootObject::EqArray).object_length()));
    } else if (key == "eqproc-length"sv) {
        m_status_scratch_obj = Object::make_integer(static_cast<integer_t>(m_eqproc_length));
    } else if (key == "eqstring-generation"sv) {
        // Monotonic counter bumped on every )#= / =string creation.  Staleness detection
        // compares this against each eqstring ref's creation-time value.
        m_status_scratch_obj = Object::make_uinteger(static_cast<uinteger_t>(m_eqstring_generation));
    } else if (key == "eqarray-generation"sv) {
        m_status_scratch_obj = Object::make_uinteger(static_cast<uinteger_t>(m_eqarray_generation));
    } else if (key == "eqproc-generation"sv) {
        m_status_scratch_obj = Object::make_uinteger(static_cast<uinteger_t>(m_eqproc_generation));
    } else if (key == "eqdict-generation"sv) {
        m_status_scratch_obj = Object::make_uinteger(static_cast<uinteger_t>(m_eqdict_generation));
    } else if (key == "eqset-generation"sv) {
        m_status_scratch_obj = Object::make_uinteger(static_cast<uinteger_t>(m_eqset_generation));
    } else if (key == "locals-pool-count"sv) {
        // Counts dicts in the frame-dict free list (m_frame_dict_pool), i.e.
        // dicts recycled by |locals|#N end-locals for reuse by subsequent
        // begin-locals.  The generic m_dict_pool (pipeline / record / actor
        // / try-catch handler dicts) is NOT counted here -- see
        // Dict::create_or_recycle vs Dict::create_or_recycle_frame_dict for
        // the frame-dict allocator's pool split.
        integer_t count = 0;
        for (length_t i = 0; i < MaxDictPoolSize; ++i) {
            auto offset = m_frame_dict_pool[i];
            while (offset != nulloffset) {
                ++count;
                // Dict's first field is m_pool (doubles as the free-list
                // next pointer when the dict is on the pool); read it via
                // offset_to_value to sidestep Dict's access control, same
                // trick as the generic dict-pool walk used to do.
                offset = offset_to_value<vm_offset_t>(offset);
            }
        }
        m_status_scratch_obj = Object::make_integer(count);
    } else if (key == "locals-overflow-count"sv) {
        // Counts dicts in m_frame_dict_overflow, i.e. recycled |locals|#N
        // dicts whose maxlength exceeds MaxDictPoolSize (the indexed pool
        // covers maxlength 1..MaxDictPoolSize only).  Mixed sizes thread
        // a single chain via Dict::m_pool; alloc scans linearly for a
        // maxlength match.  Pre-fix these dicts were dropped on recycle,
        // permanently leaking ~584+ bytes per call -- chase via this
        // counter when triaging |locals|#N memory growth.
        integer_t count = 0;
        auto offset = m_frame_dict_overflow;
        while (offset != nulloffset) {
            ++count;
            offset = offset_to_value<vm_offset_t>(offset);
        }
        m_status_scratch_obj = Object::make_integer(count);
    } else if (key == "interrupt-level"sv) {
        m_status_scratch_obj = Object::make_integer(static_cast<integer_t>(m_interrupt));
    } else if (key == "interrupt-mask"sv) {
        m_status_scratch_obj = Object::make_integer(static_cast<integer_t>(m_interrupt_mask));
    } else if (key == "last-scan-line"sv) {
        m_status_scratch_obj = Object::make_integer(static_cast<integer_t>(m_last_scan_location.m_line));
    } else if (key == "last-scan-col"sv) {
        m_status_scratch_obj = Object::make_integer(static_cast<integer_t>(m_last_scan_location.m_col));
    } else if (key == "last-scan-sid"sv) {
        m_status_scratch_obj = Object::make_integer(static_cast<integer_t>(m_last_scan_location.m_sid));
    } else if (key == "last-error"sv) {
        m_status_scratch_obj = *m_last_error_name_ptr;
#ifdef TRIX_HEAP_TRACKING
    } else if (key == "alloc-sites"sv) {
        m_status_scratch_obj = Object::make_integer(static_cast<integer_t>(m_alloc_stats_count));
    } else if (key == "alloc-saturated"sv) {
        m_status_scratch_obj = Object::make_boolean(m_alloc_stats_saturated);
    } else if (key == "alloc-tracking"sv) {
        m_status_scratch_obj = Object::make_boolean(!m_alloc_tracking_paused);
    } else if (key == "heap-track-count"sv) {
        m_status_scratch_obj = Object::make_integer(static_cast<integer_t>(m_heap_track_count));
    } else if (key == "heap-track-capacity"sv) {
        m_status_scratch_obj = Object::make_integer(static_cast<integer_t>(m_heap_track_capacity));
    } else if (key == "heap-track-saturated"sv) {
        m_status_scratch_obj = Object::make_boolean(m_heap_track_saturated);
    } else if (key == "heap-track-generation"sv) {
        m_status_scratch_obj = Object::make_integer(static_cast<integer_t>(m_heap_track_generation));
    } else if (key == "vm-peak-used"sv) {
        m_status_scratch_obj = Object::make_integer(static_cast<integer_t>(m_vm_peak_ptr - m_vm_base));
    } else if (key == "vm-peak-temp-used"sv) {
        m_status_scratch_obj = Object::make_integer(static_cast<integer_t>(m_vm_peak_temp_used));

#endif
    } else {
        return false;
    }
    return true;
}

[[nodiscard]] bool status_lookup_coroutine(std::string_view key) {
    using namespace std::literals::string_view_literals;
    if (key == "coroutine-count"sv) {
        m_status_scratch_obj = Object::make_integer(static_cast<integer_t>(m_live_coroutine_count));
    } else if (key == "coroutine-total"sv) {
        // Count non-dead coroutines in registry (dead entries linger until freed)
        integer_t count = 0;
        auto start = m_coroutine_head;
        if (start != nulloffset) {
            auto cur = start;
            do {
                if (offset_to_ptr<CoroutineContext>(cur)->m_status != CoroutineContext::Dead) {
                    ++count;
                }
                cur = offset_to_ptr<CoroutineContext>(cur)->m_next;
            } while (cur != start);
        }
        m_status_scratch_obj = Object::make_integer(count);
    } else if (key == "coroutine-stack-free"sv) {
        integer_t count = 0;
        auto offset = m_coroutine_stack_free;
        while (offset != nulloffset) {
            ++count;
            offset = offset_to_value<vm_offset_t>(offset);
        }
        m_status_scratch_obj = Object::make_integer(count);
    } else if (key == "coroutine-ctx-free"sv) {
        integer_t count = 0;
        auto offset = m_coroutine_ctx_free;
        while (offset != nulloffset) {
            ++count;
            offset = offset_to_ptr<CoroutineContext>(offset)->m_next;
        }
        m_status_scratch_obj = Object::make_integer(count);
    } else if (key == "coroutine-is-main"sv) {
        m_status_scratch_obj = Object::make_boolean(m_running_coroutine == m_main_context);
    } else if (key == "coroutine-running"sv) {
        m_status_scratch_obj = Object::make_integer(static_cast<integer_t>(m_running_coroutine));
    } else if (key == "coroutine-ready"sv) {
        // Count Ready coroutines (excluding running, including both priority levels)
        integer_t count = 0;
        auto start = m_coroutine_head;
        if (start != nulloffset) {
            auto cur = start;
            do {
                auto ctx = offset_to_ptr<CoroutineContext>(cur);
                if ((ctx->m_status == CoroutineContext::Ready) && !(ctx->m_flags & CoroutineContext::FlagSuspended) &&
                    (cur != m_running_coroutine)) {
                    ++count;
                }
                cur = ctx->m_next;
            } while (cur != start);
        }
        m_status_scratch_obj = Object::make_integer(count);
    } else if (key == "coroutine-sleeping"sv) {
        // Count Sleeping coroutines (timed sleep, not blocked)
        integer_t count = 0;
        auto start = m_coroutine_head;
        if (start != nulloffset) {
            auto cur = start;
            do {
                auto ctx = offset_to_ptr<CoroutineContext>(cur);
                if ((ctx->m_status == CoroutineContext::Sleeping) && !(ctx->m_flags & CoroutineContext::FlagBlocked) &&
                    !(ctx->m_flags & CoroutineContext::FlagSuspended)) {
                    ++count;
                }
                cur = ctx->m_next;
            } while (cur != start);
        }
        m_status_scratch_obj = Object::make_integer(count);
    } else if (key == "coroutine-blocked"sv) {
        // Count blocked coroutines (waiting on producer: pipe/actor/supervision/join)
        integer_t count = 0;
        auto start = m_coroutine_head;
        if (start != nulloffset) {
            auto cur = start;
            do {
                auto ctx = offset_to_ptr<CoroutineContext>(cur);
                if (ctx->m_flags & CoroutineContext::FlagBlocked) {
                    ++count;
                }
                cur = ctx->m_next;
            } while (cur != start);
        }
        m_status_scratch_obj = Object::make_integer(count);
    } else if (key == "coroutine-suspended"sv) {
        // Count suspended coroutines
        integer_t count = 0;
        auto start = m_coroutine_head;
        if (start != nulloffset) {
            auto cur = start;
            do {
                auto ctx = offset_to_ptr<CoroutineContext>(cur);
                if (ctx->m_flags & CoroutineContext::FlagSuspended) {
                    ++count;
                }
                cur = ctx->m_next;
            } while (cur != start);
        }
        m_status_scratch_obj = Object::make_integer(count);
    } else if (key == "sched-count"sv) {
        m_status_scratch_obj = Object::make_long(this, static_cast<int64_t>(m_sched_count));
    } else if (key == "sched-ready-pops"sv) {
        m_status_scratch_obj = Object::make_long(this, static_cast<int64_t>(m_sched_ready_pops));
    } else if (key == "sched-timer-wakes"sv) {
        m_status_scratch_obj = Object::make_long(this, static_cast<int64_t>(m_sched_timer_wakes));
    } else if (key == "sched-real-sleeps"sv) {
        m_status_scratch_obj = Object::make_long(this, static_cast<int64_t>(m_sched_real_sleeps));
    } else if (key == "ready-queue-depth"sv) {
        m_status_scratch_obj = Object::make_integer(static_cast<integer_t>(m_ready_queue_depth));
    } else if (key == "ready-high-queue-depth"sv) {
        m_status_scratch_obj = Object::make_integer(static_cast<integer_t>(m_ready_high_queue_depth));
    } else if (key == "timer-list-depth"sv) {
        m_status_scratch_obj = Object::make_integer(static_cast<integer_t>(m_timer_list_depth));
    } else if (key == "coroutine-starved"sv) {
        // Count non-dead, non-running coroutines whose last run time exceeds the starvation threshold.
        // Coroutines that have never run (m_last_run_time_ns == 0) are excluded.
        integer_t count = 0;
        if ((m_starvation_threshold_ns > 0) && (m_coroutine_head != nulloffset)) {
            auto now = monotonic_ns();
            auto start = m_coroutine_head;
            auto cur = start;
            do {
                auto ctx = offset_to_ptr<CoroutineContext>(cur);
                if ((ctx->m_status != CoroutineContext::Dead) && (ctx->m_status != CoroutineContext::Running) &&
                    (ctx->m_last_run_time_ns > 0) && ((now - ctx->m_last_run_time_ns) > m_starvation_threshold_ns)) {
                    ++count;
                }
                cur = ctx->m_next;
            } while (cur != start);
        }
        m_status_scratch_obj = Object::make_integer(count);
    } else if (key == "starvation-threshold"sv) {
        // Return threshold in milliseconds (for human readability)
        m_status_scratch_obj = Object::make_integer(static_cast<integer_t>(m_starvation_threshold_ns / 1'000'000ULL));
    } else {
        return false;
    }
    return true;
}

[[nodiscard]] bool status_lookup_composability(std::string_view key) {
    using namespace std::literals::string_view_literals;
    if (key == "protocol-count"sv) {
        auto registry = offset_to_ptr<Dict>(m_protocol_registry_offset);
        m_status_scratch_obj = Object::make_integer(static_cast<integer_t>(registry->length()));
    } else if (key == "protocol-method-count"sv) {
        auto registry = offset_to_ptr<Dict>(m_protocol_registry_offset);
        integer_t total = 0;
        auto entry_offset = nulloffset;
        integer_t bucket_idx = -1;
        while (true) {
            auto [next_offset, next_idx, key_obj, value] = registry->next(this, entry_offset, bucket_idx);
            if (next_offset == nulloffset) {
                break;
            } else if (value.is_dict()) {
                auto protocol_dict = value.dict_value(this);
                total += static_cast<integer_t>(protocol_dict->length());
            }
            entry_offset = next_offset;
            bucket_idx = next_idx;
        }
        m_status_scratch_obj = Object::make_integer(total);
    } else if (key == "gen-ref-counter"sv) {
        m_status_scratch_obj = Object::make_ulong(this, m_gen_ref_counter);
    } else {
        return false;
    }
    return true;
}

[[nodiscard]] const Object *status_lookup(std::string_view key) {
    if (status_lookup_vm(key) || status_lookup_stacks(key) || status_lookup_save(key) || status_lookup_exec(key) ||
        status_lookup_extvalue(key) || status_lookup_streams(key) || status_lookup_names(key) || status_lookup_config(key) ||
        status_lookup_coroutine(key) || status_lookup_composability(key)) {
        return &m_status_scratch_obj;
    }
    return nullptr;
}

// clock: :- ulong
// Pushes monotonic clock value in microseconds.
// throws: vm-full, opstack-overflow
static void clock_op(Trix *trx) {
    trx->require_op_capacity(1);

    auto now = std::chrono::steady_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
    *++trx->m_op_ptr = Object::make_ulong(trx, static_cast<ulong_t>(us));
}

// time: proc :- int
// Executes proc and pushes elapsed microseconds.
// throws: vm-full, execstack-overflow, opstack-underflow, type-check
static void time_op(Trix *trx) {
    trx->verify_operands(VerifyProc);
    trx->require_exec_capacity(3);

    auto proc = *trx->m_op_ptr--;

    auto now = std::chrono::steady_clock::now();
    auto start = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();

    // exec stack: [start-timestamp] [@time] [proc]
    *++trx->m_exec_ptr = Object::make_ulong(trx, static_cast<ulong_t>(start));
    *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atTime);
    *++trx->m_exec_ptr = proc;
}

// @time: (exec stack: start-timestamp)
// Computes elapsed time and pushes result to operand stack.
// throws: vm-full, opstack-overflow
static void at_time_op(Trix *trx) {
    trx->require_op_capacity(1);

    auto start = trx->m_exec_ptr->ulong_value(trx);
    trx->m_exec_ptr->free_extvalue(trx);
    --trx->m_exec_ptr;

    auto now = std::chrono::steady_clock::now();
    auto end = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
    auto elapsed = static_cast<ulong_t>(end) - start;
    *++trx->m_op_ptr = Object::make_ulong(trx, elapsed);
}

// epoch-time: :- ulong
// Pushes milliseconds since the Unix epoch.
// throws: vm-full, opstack-overflow
static void epochtime_op(Trix *trx) {
    trx->require_op_capacity(1);

    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    *++trx->m_op_ptr = Object::make_ulong(trx, static_cast<ulong_t>(ms));
}

// now: :- ulong
// Pushes monotonic-clock milliseconds since an unspecified steady epoch.
// Use for elapsed-time deltas (subtract two `now` readings).  The clock
// never moves backward and is unaffected by wall-clock jumps; epoch
// is implementation-defined and meaningless on its own -- only
// differences are portable.  See `epoch-time` for wall-clock ms and
// `time { proc }` for proc-elapsed microseconds.
// throws: vm-full, opstack-overflow
static void now_op(Trix *trx) {
    trx->require_op_capacity(1);

    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    *++trx->m_op_ptr = Object::make_ulong(trx, static_cast<ulong_t>(ms));
}

// command-line-args: :- arr
// Pushes an array of strings, one per argv element after the script filename.
// Empty array if there were none.  Strings are copied into the VM heap so
// they survive snap-shot/thaw of materialized values, but the underlying
// argc/argv pointers themselves are process-scope and NOT saved -- a thawed
// image inherits the new run's argv on the next call.
// Sandbox-safe: argv reading is not a security concern (mirrors getenv).
// throws: vm-full, opstack-overflow
static void commandlineargs_op(Trix *trx) {
    trx->require_op_capacity(1);

    auto count = static_cast<length_t>(trx->m_script_argc);
    auto [dst_ptr, dst_offset] = trx->vm_alloc_n<Object>(count);
    for (length_t i = 0; i < count; ++i) {
        auto sv = std::string_view{trx->m_script_argv[i]};
        dst_ptr[i] = Object::make_string(trx, sv);
    }
    *++trx->m_op_ptr = Object::make_array(dst_offset, count);
}

// executable-path: :- str
// Pushes the full resolved filesystem path of the running Trix binary
// (realpath of argv[0], captured at startup), or an empty string if it could
// not be resolved (argc == 0, or realpath failed).  Lets a snapshot/fork
// example re-exec the SAME build to thaw its image: a hard-coded `./trix`
// would otherwise thaw an opt image with the debug binary (or vice versa),
// a build mismatch the image's operator-table signature then rejects.
// Strings are copied into the VM heap (the argv-backed buffer is process-scope).
// Sandbox-safe: exposes only the path already used as the lib/ search root.
// throws: vm-full, opstack-overflow
static void executablepath_op(Trix *trx) {
    trx->require_op_capacity(1);

    auto path_sv = (trx->m_exe_path != nullptr) ? std::string_view{trx->m_exe_path} : std::string_view{};
    *++trx->m_op_ptr = Object::make_string(trx, path_sv);
}

// getenv: str :- str true | false
// Looks up an environment variable.
// throws: vm-full, opstack-overflow, opstack-underflow, type-check
static void getenv_op(Trix *trx) {
    trx->verify_operands(VerifyString);

    auto name_cstr = trx->m_op_ptr->string_cstr(trx);
    auto value = std::getenv(name_cstr);

    if (value != nullptr) {
        trx->require_op_capacity(1);
        *trx->m_op_ptr = Object::make_string(trx, std::string_view{value});
        *++trx->m_op_ptr = Object::make_boolean(true);
    } else {
        *trx->m_op_ptr = Object::make_boolean(false);
    }
}

// sleep_op moved to ops_coroutine.inl (coroutine-aware version)

// getpid: :- int
// Pushes the process ID of the current process.
// throws: opstack-overflow
static void getpid_op(Trix *trx) {
    trx->require_op_capacity(1);
    *++trx->m_op_ptr = Object::make_integer(static_cast<integer_t>(::getpid()));
}

// setenv: str str :- --
// Sets the environment variable named by the first string to the second string's value.
// throws: opstack-underflow, type-check
static void setenv_op(Trix *trx) {
    trx->verify_operands(VerifyString, VerifyString);

    auto value_cstr = trx->m_op_ptr->string_cstr(trx);
    auto name_cstr = (trx->m_op_ptr - 1)->string_cstr(trx);
    if (::setenv(name_cstr, value_cstr, 1) != 0) {
        auto errno_str = errno_string();
        trx->error(Error::IOWriteError, "setenv: cannot set '{}': {}/{}", name_cstr, errno, errno_str);
    } else {
        trx->m_op_ptr -= 2;
    }
}

// system: str :- int
// Runs a shell command via sh -c and returns its exit code.
// Returns -1 if the command cannot be executed.
// throws: opstack-underflow, type-check, unsupported
static void system_op(Trix *trx) {
    if (trx->m_sandbox) {
        trx->error(Error::Unsupported, "system: disabled in sandbox mode");
    } else {
        trx->verify_operands(VerifyString);

        auto cstr = trx->m_op_ptr->string_cstr(trx);
        auto status = std::system(cstr);
        const integer_t exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        *trx->m_op_ptr = Object::make_integer(exit_code);
    }
}

// shell: str :- str int
// Runs a shell command, captures its stdout as a string, and returns the exit code.
// Stderr passes through to the parent process stderr.
// throws: io-read-error, opstack-overflow, opstack-underflow, type-check, unsupported, vm-full
static void shell_op(Trix *trx) {
    if (trx->m_sandbox) {
        trx->error(Error::Unsupported, "shell: disabled in sandbox mode");
    } else {
        trx->verify_operands(VerifyString);

        auto cstr = trx->m_op_ptr->string_cstr(trx);
        auto fp = ::popen(cstr, "r");
        if (fp == nullptr) {
            auto errno_str = errno_string();
            trx->error(Error::IOReadError, "shell: popen failed: {}/{}", errno, errno_str);
        } else {
            // vm_start_alloc below raises VMFull when the heap is full, which would
            // strand the popen handle (and its pipe fds + the forked child) -- the op
            // body has no RAII guard around fp.  Check capacity first (vm_remaining
            // exactly predicts the vm_start_alloc throw) and pclose before raising.
            if (trx->vm_remaining<vm_t>() == 0) {
                ::pclose(fp);
                trx->error(Error::VMFull, "shell: no VM space for command output");
            } else {
                // Read stdout into VM string via streaming alloc
                auto [string_base, vm_remaining] = trx->vm_start_alloc<vm_t>();
                auto string_ptr = string_base;
                auto string_limit = string_base + std::min(vm_remaining, static_cast<vm_size_t>(MaxStringLength));

                for (;;) {
                    auto space = static_cast<size_t>(string_limit - string_ptr);
                    if (space == 0) {
                        break;
                    } else {
                        auto n = std::fread(string_ptr, 1, space, fp);
                        if (n == 0) {
                            break;
                        } else {
                            string_ptr += n;
                        }
                    }
                }

                auto string_length = static_cast<length_t>(string_ptr - string_base);
                auto result_base = trx->vm_end_alloc_ptr<vm_t>(static_cast<vm_size_t>(string_length));
                auto string_offset = trx->ptr_to_offset(result_base);

                auto status = ::pclose(fp);
                const integer_t exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

                trx->require_op_capacity(1);
                *trx->m_op_ptr = Object::make_string(string_offset, string_length);
                *++trx->m_op_ptr = Object::make_integer(exit_code);
            }
        }
    }
}

// hostname: :- str
// Pushes the hostname of the current machine.
// throws: io-read-error, opstack-overflow, unsupported, vm-full
static void hostname_op(Trix *trx) {
    if (trx->m_sandbox) {
        trx->error(Error::Unsupported, "hostname: disabled in sandbox mode");
    } else {
        trx->require_op_capacity(1);

        char buf[256];
        if (::gethostname(buf, sizeof(buf)) != 0) {
            auto errno_str = errno_string();
            trx->error(Error::IOReadError, "hostname: {}/{}", errno, errno_str);
        } else {
            buf[sizeof(buf) - 1] = '\0';
            *++trx->m_op_ptr = Object::make_string(trx, std::string_view{buf});
        }
    }
}

// assert: str bool :- --
// Pops a boolean (top) and a message string (below); raises assert-failed with
// the message when the boolean is false.
// throws: opstack-underflow, type-check, assert-failed
static void assert_op(Trix *trx) {
    trx->verify_operands(VerifyBoolean, VerifyString);

    auto bool_ptr = trx->m_op_ptr;
    if (bool_ptr->boolean_value()) {
        trx->m_op_ptr -= 2;
    } else {
        trx->error(Error::AssertFailed, bool_ptr[-1]);
    }
}

static void fe_except_bool_impl(Trix *trx, int (*action)(int), std::string_view op_name) {
    trx->verify_operands(VerifyUInteger);

    auto object = trx->m_op_ptr;
    auto excepts = object->uinteger_value();

    if ((excepts & ~static_cast<uinteger_t>(FE_ALL_EXCEPT)) == 0) {
        auto result = action(static_cast<int>(excepts));
        *object = Object::make_boolean(result == 0);
    } else {
        trx->error(Error::RangeCheck, "{}: invalid exception flags", op_name);
    }
}
// Searches the exec stack for the innermost @try-barrier, @try-catch-barrier,
// @finally-barrier, or @with-stream.
// If found: closes any @run streams in the discarded range, frees any ExtValues
// in that range, cuts/rewrites the exec stack, and throws Exception::Error
// to resume the interpreter at the barrier.
// If not found (or op stack is full for try/try-catch): returns normally so the
// caller can escalate to global_handler().
void try_catch_handler() {
    auto trx = this;
    // Deferred heap-cell cleanup: track whether any ExtValue- or WideValue-
    // bearing objects exist between m_exec_ptr and the barrier, then free
    // them in one pass only if a matching barrier is found.
    auto free_extvalue = false;
    auto run_count = 0;
    for (auto object = m_exec_ptr; object >= m_exec_base; --object) {
        if (object->is_operator()) {
            if (object->operator_is_barrier()) {
                // Dispatch on UnwindAction from the descriptor table.
                // Three actions
                // exist: PopAndContinue (transparent barriers like
                // @delimit/@effect/@handler-scope/@end-in-global pop their
                // companions and keep searching), Stop (@try-barrier trims
                // exec and is done), and Replace (@try-catch / @finally /
                // logic-family etc. splice in their @*-fail atom).
                const auto &desc = op_descriptor_for(object->m_operator);

                if (desc.unwind == UnwindAction::PopAndContinue) {
                    // @end-in-global has a side effect: restore the saved
                    // m_curr_alloc_global flag from its Boolean companion.
                    // Other PopAndContinue barriers just discard their err
                    // companions (1 for delimit/effect, 2 for handler-scope).
                    if (object->operator_is_endinglobal()) {
                        auto saved_flag_obj = *m_err_ptr--;
                        m_curr_alloc_global = saved_flag_obj.boolean_value();
                    } else {
                        m_err_ptr -= desc.err_companions;
                    }
                } else {
                    // Stop or Replace: this barrier catches the error.
                    // try/try-catch/repl push /error-name on op stack.
                    if (object->operator_is_errorpush()) {
                        if (!has_op_capacity(1)) {
                            diag_println("Trix try-catch bypassed: operand stack full; escalating to global handler");
                            return;
                        } else {
                            *++m_op_ptr = *m_last_error_name_ptr;
                            // Record the plant: scan_procedure's unwind cleanup
                            // (scanner.inl) must preserve it across its op-stack
                            // reset when the error fired mid-proc-scan.
                            m_planted_error_objects = 1;
                        }
                    }

                    // Close any streams open by @run on the cut-back range.
                    for (auto run_ptr = m_exec_ptr; (run_count != 0) && (run_ptr > object); --run_ptr) {
                        if (run_ptr->is_operator() && run_ptr->operator_is_run()) {
                            // The stream sits at run_ptr+1.  On a SyntaxError
                            // path the stream may be logically above m_exec_ptr
                            // (popped but not yet re-pushed by execute_stream);
                            // the slot is still physically valid -- see the
                            // invariant comment in execute_stream().
                            auto stream_ptr = (run_ptr + 1);
                            auto [stream, sid] = stream_ptr->stream_value(trx);
                            if (stream->sid() == sid) {
                                stream->close(trx);
                            }
                            --run_count;
                        }
                    }

                    // Free any ExtValues on the cut-back range.
                    if (free_extvalue) {
                        maybe_free_extvalue_execstack(trx, object);
                    }

                    if (desc.unwind == UnwindAction::Stop) {
                        // @try-barrier: discard barrier + body + @call frames.
                        m_exec_ptr = (object - 1);
                    } else {
                        // UnwindAction::Replace.  Three side-effect special cases
                        // precede the generic atom-splice: @repl-barrier stashes
                        // the failing op below /error-name; @finally-barrier reads
                        // the finally-block from err and splices it above the
                        // @finally-reraise marker; @with-stream closes the stream
                        // from err.  Everything else is a straight atom swap.
                        if (object->operator_is_replbarrier()) {
                            if (!has_op_capacity(1)) {
                                diag_println("Trix repl-barrier bypassed: operand stack full; escalating to global handler");
                                return;
                            } else {
                                *(m_op_ptr + 1) = *m_op_ptr;
                                *m_op_ptr = *m_last_operator_ptr;
                                ++m_op_ptr;
                                // failing-op + /error-name: two planted objects
                                // for @repl-recover.
                                m_planted_error_objects = 2;
                                *object = Object::make_control_operator(desc.replace_with);
                                m_exec_ptr = object;
                            }
                        } else if (object->operator_is_finallybarrier()) {
                            auto finally_block = *m_err_ptr--;
                            *object = Object::make_control_operator(desc.replace_with);
                            *(object + 1) = finally_block;
                            m_exec_ptr = (object + 1);
                        } else if (object->operator_is_withstream()) {
                            auto stream_obj = *m_err_ptr--;
                            auto [stream, sid] = stream_obj.stream_value(trx);
                            if (stream->is_open(sid)) {
                                stream->close(trx);
                            }
                            *object = Object::make_control_operator(desc.replace_with);
                            m_exec_ptr = object;
                        } else {
                            *object = Object::make_control_operator(desc.replace_with);
                            m_exec_ptr = object;
                        }
                    }

                    // return to interpreter and resume execution
                    throw Exception::Error;
                }
            } else if (object->operator_is_dictpop()) {
                // Recycle only on @end-locals: other dict-pop barriers
                // don't own their frame dict's storage.
                auto popped = pop_frame_dict_if_present();
                if (popped.valid() && object->operator_is_endlocals()) {
                    Dict::recycle(trx, popped.dict, popped.offset);
                }
            } else if (object->operator_is_run()) {
                ++run_count;
            }
        } else if (object->uses_heap_cell()) {
            free_extvalue = true;
        }
    }
}

// Walks the exec stack top-to-bottom and prints a backtrace to stderr.
// Called from global_handler() on the fatal (unhandled error) path.
// Shows: the current operator (#0), source context from @run frames,
// named call frames from @call markers, try-catch boundary annotations,
// and a compact operand-stack snapshot.
void format_backtrace() {
    // Compile-time gate: -DTRIX_NO_BACKTRACE elides the entire body.  When
    // BacktraceEnabled is false, this function is an empty stub the compiler
    // can inline away at every callsite (currently just global_handler).
    if constexpr (BacktraceEnabled) {
        // Quiet mode silences stderr; skip the whole walk (exec-stack scan,
        // operator_string calls, operand-stack snapshot) -- every print below
        // would be swallowed anyway.  Saves tens to hundreds of microseconds per
        // fatal error in fuzz / sandbox hosts.
        if (!m_quiet) {
            format_backtrace_report();
        }
    }
}

// Emit the backtrace report -- VM-state header, last-error-data, the exec-stack
// frame walk, and a compact operand-stack snapshot -- to stderr.  Split from
// format_backtrace() so the quiet-mode gate reads as a single top-to-bottom
// flow; the if constexpr keeps the ~600-line body compiled out under
// !BacktraceEnabled (same empty-stub behaviour as before the split).
void format_backtrace_report() {
    if constexpr (BacktraceEnabled) {
        using namespace std::literals::string_view_literals;
        auto trx = this;

        // One-line VM-state header
        // before the frame walk.  Live reads only -- NO snapshot members, to
        // keep Trix per-instance storage minimal.  Two notes on accuracy:
        //  - sl and coro never change during try_catch_handler unwind; safe.
        //  - dict-depth and err-depth reflect POST-unwind state (try_catch_handler
        //    has by now popped err companions and @end-locals dict frames).
        //    The high-water section below shows peaks; the exec walk above
        //    shows the at-error barrier nesting via boundary annotations.
        //  - alloc-global is intentionally omitted: try_catch_handler restores
        //    it from @end-in-global's saved Boolean, so live reads are
        //    post-unwind.  The exec walk surfaces `[in-global/local boundary]`
        //    when execution was inside a ${...} / $${...} block at error time.
        auto running_coro = offset_to_nullptr<CoroutineContext>(m_running_coroutine);
        auto coro_id = (running_coro != nullptr) ? running_coro->m_id : 0u;
        auto dict_depth = static_cast<int>(m_dict_ptr - m_dict_base + 1);
        auto err_depth = static_cast<int>(m_err_ptr - m_err_base + 1);
        if (m_running_coroutine == m_main_context) {
            diag_println("  state: sl={} coro=main dict-depth={} err-depth={}", m_curr_save_level, dict_depth, err_depth);
        } else {
            diag_println("  state: sl={} coro=#{} dict-depth={} err-depth={}", m_curr_save_level, coro_id, dict_depth, err_depth);
        }

        // Last-error-data: if throw-with attached a data payload, surface a
        // compact summary before the frame walk.  Type-marker plus a brief
        // value/length descriptor -- no recursive expansion, no heap-walks.
        if ((m_last_error_data_ptr != nullptr) && !m_last_error_data_ptr->is_null()) {
            char buffer[ObjectNameBufferSize];
            static_cast<void>(trx->object_name(m_last_error_data_ptr, buffer));
            switch (+m_last_error_data_ptr->type()) {
            case +Object::Type::Boolean:
                diag_println("  last-error-data: {} {}", buffer, m_last_error_data_ptr->boolean_value() ? "true" : "false");
                break;

            case +Object::Type::Integer:
                diag_println("  last-error-data: {} {}", buffer, m_last_error_data_ptr->integer_value());
                break;

            case +Object::Type::Name:
                diag_println("  last-error-data: {} {}{}",
                             buffer,
                             m_last_error_data_ptr->is_literal() ? "/" : "\\",
                             m_last_error_data_ptr->name_sv(trx));
                break;

            case +Object::Type::String:
                // Pure header read; never sv_value to avoid faulting on stale eqref.
                diag_println("  last-error-data: {}({} bytes)", buffer, m_last_error_data_ptr->string_length());
                break;

            case +Object::Type::Array:
            case +Object::Type::Packed:
                diag_println("  last-error-data: {}[{}]", buffer, m_last_error_data_ptr->arrays_length());
                break;

            default:
                // Dict / Set / Tagged / Record / ExtValue / etc.  Object-name
                // alone tells the reader the type; richer rendering belongs to
                // the content-preview / verbose-mode paths.
                diag_println("  last-error-data: {}", buffer);
                break;
            }
        }

        // Nested-context summary.  Bottom-up exec walk that collapses
        // the barrier chain into one line, outermost-first, with the innermost
        // @call's file:line appended.  Lets a cold-reader answer "what was
        // wrapping the failure?" without scrolling through the per-frame walk.
        static constexpr int MaxContexts = 6;
        std::string_view contexts[MaxContexts];
        int context_count = 0;
        Object *inner_call_loc = nullptr;
        for (auto obj = m_exec_base; obj <= m_exec_ptr; ++obj) {
            if (obj->is_operator()) {
                if (obj->operator_is_call()) {
                    auto loc_slot = (obj - 2);
                    if ((loc_slot >= m_exec_base) && loc_slot->is_sourceloc()) {
                        inner_call_loc = loc_slot;  // last one wins == innermost call
                    }
                } else {
                    // Table-driven inside-label lookup via op_descriptor.inl.  Atoms
                    // without an inside_label (default-empty) are skipped.
                    auto label = op_inside_label(obj->m_operator);
                    if (!label.empty() && (context_count < MaxContexts)) {
                        contexts[context_count++] = label;
                    }
                }
            }
        }
        if ((context_count > 0) || (inner_call_loc != nullptr)) {
            diag_print("  inside:");
            for (int i = 0; i < context_count; ++i) {
                if (i == 0) {
                    diag_print(" {}", contexts[i]);
                } else {
                    diag_print(" / {}", contexts[i]);
                }
            }
            if (inner_call_loc != nullptr) {
                auto sid = inner_call_loc->m_source_sid;
                auto line = inner_call_loc->m_source_line;
                std::string_view filename = "<unknown>"sv;
                if (sid == Stream::STRING_SID) {
                    filename = "<string>"sv;
                } else {
                    for (auto s = m_stream_inuse_list; s != nullptr; s = s->next_stream(trx)) {
                        if (s->m_sid == sid) {
                            filename = s->source_location(trx).source;
                            break;
                        }
                    }
                }
                diag_print(" @ {}:{}", filename, line);
            }
            diag_println("");
        }

        // Frame 0: the operator that raised the error (or the last operator that
        // ran before a non-operator error, e.g. in name lookup or proc dispatch).
        auto opstr = m_last_operator_ptr->operator_string(trx);
        diag_println("  #0: '{}'", opstr);

        // Walk exec stack top-to-bottom: collect @call names, @run source
        // contexts, and @try-catch boundary annotations.
        // @run: the stream being scanned sits one slot above the @run marker
        // and holds the current filename/line/col via source_location().
        // @call: companion literal Name one slot below gives the called name.
        //
        // err_scan tracks the err-stack position adjacent to each barrier we
        // encounter, starting from m_error_err_ptr_snapshot (the at-error err
        // top).  try_catch_handler has by now popped past PopAndContinue
        // barriers, but the bytes survive below the live m_err_ptr.  The
        // companion-rich barriers read *err_scan to print
        // handler-dict effect names, in-global vs in-local distinction, etc.
        auto err_scan = m_error_err_ptr_snapshot;
        int frame = 1;
        for (auto object = m_exec_ptr; object >= m_exec_base; --object) {
            if (object->is_operator()) {
                if (object->operator_is_call()) {
                    auto companion = (object - 1);
                    if ((companion >= m_exec_base) && companion->is_name()) {
                        auto name = offset_to_ptr<Name>(companion->m_name);
                        diag_print("  #{}: {}", frame, name->sv());

                        auto loc_slot = (object - 2);
                        if ((loc_slot >= m_exec_base) && loc_slot->is_sourceloc()) {
                            auto sid = loc_slot->m_source_sid;
                            auto line = loc_slot->m_source_line;
                            auto col = loc_slot->m_source_pos;
                            std::string_view filename = "<unknown>"sv;
                            if (sid == Stream::STRING_SID) {
                                filename = "<string>"sv;
                            } else {
                                for (auto s = m_stream_inuse_list; s != nullptr; s = s->next_stream(trx)) {
                                    if (s->m_sid == sid) {
                                        filename = s->source_location(trx).source;
                                        break;
                                    }
                                }
                            }
                            diag_print(" at {}:{}:{}", filename, line, col);
                        }
                        diag_println("");
                        ++frame;
                    }
                } else if (object->operator_is_run()) {
                    // Stream sits at @run+1. On the SyntaxError path execute_stream raises
                    // the error before pushing the stream back, so the stream slot is
                    // logically above m_exec_ptr but physically valid -- same invariant as
                    // try_catch_handler(). No upper-bound check needed or correct here.
                    auto stream_slot = (object + 1);
                    if (stream_slot->is_stream()) {
                        auto [stream, _] = stream_slot->stream_value(trx);
                        auto [source, line, col] = stream->source_location(trx);
                        diag_println("      at {}:{}:{}", source, line, col);
                    }
                } else {
                    // Table-driven boundary annotation.  Returns empty
                    // for non-control atoms or atoms without a label.
                    auto label = op_bt_label(object->m_operator);
                    if (!label.empty()) {
                        // Companion-rich variants.  For barriers that
                        // own err companions, read them via err_scan (which
                        // walks the popped-but-resident err data downward).
                        if (is_control_op_atom(object->m_operator)) {
                            const auto &desc = op_descriptor_for(object->m_operator);
                            auto has_companion = (desc.err_companions > 0) && (err_scan >= m_err_base);
                            // Note: @end-in-global intentionally not enriched.
                            // ${...} and $${...} share the same closer atom and
                            // both save the PRIOR alloc-global value (not the
                            // body's value), so they are indistinguishable
                            // post-unwind.  The generic table-driven label suffices.
                            if (object->operator_is_effectbarrier() && has_companion) {
                                // Companion is the handler-dict; list its keys.
                                if (err_scan->is_dict()) {
                                    auto dict = err_scan->dict_value(trx);
                                    diag_print("      {} handles:", label);
                                    auto first = true;
                                    static constexpr length_t MAX_EFFECT_NAMES = 6;
                                    length_t shown = 0;
                                    dict->for_each(trx, [&](Object key, Object) {
                                        if ((shown < MAX_EFFECT_NAMES) && key.is_name()) {
                                            diag_print("{} /{}", first ? "" : ",", key.name_sv(trx));
                                            first = false;
                                            ++shown;
                                        }
                                    });
                                    auto total = dict->length();
                                    if (total > MAX_EFFECT_NAMES) {
                                        diag_print(" (+{} more)", total - MAX_EFFECT_NAMES);
                                    }
                                    diag_println("");
                                } else {
                                    diag_println("      {}", label);
                                }
                            } else if (object->operator_is_delimitbarrier() && has_companion) {
                                if (err_scan->is_integer()) {
                                    diag_println("      {} snapshot-depth={}", label, err_scan->integer_value());
                                } else {
                                    diag_println("      {}", label);
                                }
                            } else if (object->operator_is_handlerscope() && ((err_scan - 1) >= m_err_base)) {
                                // Layout: err[-1] = identity (lower), err[0] = op-depth (upper).
                                auto identity = (err_scan - 1);
                                auto op_depth_ok = err_scan->is_integer();
                                auto identity_ok = identity->is_continuation();
                                if (op_depth_ok && identity_ok) {
                                    diag_println("      {} id={} op-depth={}",
                                                 label,
                                                 identity->m_continuation,
                                                 err_scan->integer_value());
                                } else {
                                    diag_println("      {}", label);
                                }
                            } else {
                                diag_println("      {}", label);
                            }
                            // Advance err_scan past this barrier's companions, regardless
                            // of whether we printed rich content.
                            err_scan -= desc.err_companions;
                        } else {
                            diag_println("      {}", label);
                        }
                    }
                }
            }
        }

        // Operand-stack snapshot: top N operands with types and compact values.
        // TRIX_BT_VERBOSE uncaps the count.
        static constexpr int DefaultMaxOperands = 8;
        auto op_count = static_cast<int>(m_op_ptr - m_op_base + 1);
        auto max_operands = m_bt_verbose ? op_count : DefaultMaxOperands;
        if (op_count > 0) {
            diag_print("  operands:");
            auto show = std::min(op_count, max_operands);
            for (int i = 0; i < show; ++i) {
                auto object = (m_op_ptr - i);

                char buffer[ObjectNameBufferSize];
                static_cast<void>(trx->object_name(object, buffer));
                switch (+object->type()) {
                case +Object::Type::Boolean:
                    diag_print(" {} {}", buffer, object->boolean_value() ? "true" : "false");
                    break;

                case +Object::Type::Byte:
                    diag_print(" {} 0x{:02x}", buffer, object->byte_value());
                    break;

                case +Object::Type::Integer:
                    diag_print(" {} {}", buffer, object->integer_value());
                    break;

                case +Object::Type::UInteger:
                    diag_print(" {} {}u", buffer, object->uinteger_value());
                    break;

                case +Object::Type::Long:
                    if (object->heap_cell_valid(trx)) {
                        diag_print(" {} {}l", buffer, object->long_value(trx));
                    } else {
                        diag_print(" {} <freed-long>", buffer);
                    }
                    break;

                case +Object::Type::ULong:
                    if (object->heap_cell_valid(trx)) {
                        diag_print(" {} {}ul", buffer, object->ulong_value(trx));
                    } else {
                        diag_print(" {} <freed-ulong>", buffer);
                    }
                    break;

                case +Object::Type::Real:
                    diag_print(" {} {}", buffer, object->real_value());
                    break;

                case +Object::Type::Double:
                    if (object->heap_cell_valid(trx)) {
                        diag_print(" {} {}d", buffer, object->double_value(trx));
                    } else {
                        diag_print(" {} <freed-double>", buffer);
                    }
                    break;

                case +Object::Type::Int128:
                    if (object->heap_cell_valid(trx)) {
                        char digits[40];
                        auto r = PrintFmt::i128_to_chars(digits, digits + sizeof(digits), object->int128_value(trx), 10);
                        diag_print(" {} {}h", buffer, std::string_view(digits, static_cast<size_t>(r.ptr - digits)));
                    } else {
                        diag_print(" {} <freed-int128>", buffer);
                    }
                    break;

                case +Object::Type::UInt128:
                    if (object->heap_cell_valid(trx)) {
                        char digits[40];
                        auto r = PrintFmt::u128_to_chars(digits, digits + sizeof(digits), object->uint128_value(trx), 10);
                        diag_print(" {} {}uh", buffer, std::string_view(digits, static_cast<size_t>(r.ptr - digits)));
                    } else {
                        diag_print(" {} <freed-uint128>", buffer);
                    }
                    break;

                case +Object::Type::Name:
                    diag_print(" {} {}{}", buffer, object->is_literal() ? "/" : "\\", object->name_sv(trx));
                    break;

                case +Object::Type::String:
                    // Use string_length (pure header read) rather than sv_value so the
                    // diagnostic path never faults on a stale eqref (diag_print inside
                    // global_handler must not retrigger the same error it's reporting).
                    diag_print(" {}({} bytes)", buffer, object->string_length());
                    break;

                case +Object::Type::Array:
                case +Object::Type::Packed:
                    diag_print(" {}[{}]", buffer, object->arrays_length());
                    break;

                case +Object::Type::Operator:
                    diag_print(" '{}'", object->operator_string(trx));
                    break;

                default:
                    diag_print(" '{}'", buffer);
                    break;
                }
            }
            if (op_count > max_operands) {
                diag_print(" (+{} more)", (op_count - max_operands));
            }
            diag_println("");

            // Content preview for the first composite operand in the shown
            // top-N range -- typically the most relevant clue when the operand
            // stack shows a non-scalar near the failure.  Index is 1-based from
            // the top (matches "operand #N" in type-check error messages).
            // TRIX_BT_VERBOSE previews EVERY composite (not just first) and
            // uncaps element/char counts.
            static constexpr int DefaultMaxPreviewElems = 3;
            static constexpr int DefaultMaxPreviewChars = 30;
            for (int i = 0; i < show; ++i) {
                auto object = (m_op_ptr - i);
                auto preview_index = (i + 1);
                if (object->is_array()) {
                    auto len = static_cast<int>(object->arrays_length());
                    auto elems = object->array_objects(trx);
                    auto cap = m_bt_verbose ? len : std::min(len, DefaultMaxPreviewElems);
                    diag_print("  preview [#{}]: [", preview_index);
                    for (int j = 0; j < cap; ++j) {
                        char b[ObjectNameBufferSize];
                        static_cast<void>(trx->object_name(&elems[j], b));
                        if (j == 0) {
                            diag_print("{}", b);
                        } else {
                            diag_print(" {}", b);
                        }
                    }
                    if (cap < len) {
                        diag_print(" ...");
                    }
                    diag_println("]");
                    if (!m_bt_verbose) {
                        break;  // first composite only
                    }
                } else if (object->is_string()) {
                    auto sv = object->sv_value(trx);
                    auto sv_size = static_cast<int>(sv.size());
                    auto cap = m_bt_verbose ? sv_size : std::min(sv_size, DefaultMaxPreviewChars);
                    diag_print("  preview [#{}]: \"", preview_index);
                    for (int j = 0; j < cap; ++j) {
                        auto c = sv[static_cast<size_t>(j)];
                        if ((c >= 0x20) && (c < 0x7F)) {
                            diag_print("{:c}", c);
                        } else {
                            diag_print(".");  // non-printable byte
                        }
                    }
                    if (cap < sv_size) {
                        diag_print("...");
                    }
                    diag_println("\"");
                    if (!m_bt_verbose) {
                        break;
                    }
                }
            }
        }

        // Stack high-water marks (account for current position which may exceed tracked high-water)
        auto op_hw_ptr = (m_op_ptr > m_op_high_water_ptr) ? m_op_ptr : m_op_high_water_ptr;
        auto op_hw = static_cast<int>(op_hw_ptr - m_op_base + 1);
        auto op_max = static_cast<int>(m_op_limit - m_op_base);
        auto exec_hw = static_cast<int>(m_exec_high_water_ptr - m_exec_base + 1);
        auto exec_max = static_cast<int>(m_exec_limit - m_exec_base);
        auto dict_hw = static_cast<int>(m_dict_high_water_ptr - m_dict_base + 1);
        auto dict_max = static_cast<int>(m_dict_limit - m_dict_base);
        auto err_hw = static_cast<int>(m_err_high_water_ptr - m_err_base + 1);
        auto err_max = static_cast<int>(m_err_limit - m_err_base);
        diag_println("  high-water: op {}/{} exec {}/{} dict {}/{} error {}/{}",
                     op_hw,
                     op_max,
                     exec_hw,
                     exec_max,
                     dict_hw,
                     dict_max,
                     err_hw,
                     err_max);
    }
}

[[noreturn]] void global_handler() {
    auto trx = this;

#ifdef TRIX_DEBUGGER
    // Break-on-error halt: park the fatal error and run an inspector
    // session on the erroring coroutine's intact at-raise stacks; the
    // queued @debug-error-resume re-enters here afterwards (armed) and
    // the error completes normally -- handler dispatch on main, kill +
    // reschedule for a dying coroutine.  Only uncaught errors reach
    // global_handler, so try-based control flow never halts.  Halted
    // mode guards against errors raised by the session itself.
    if (m_debug.m_break_on_error && !m_debug.m_error_halt_armed && !m_debug.m_event_callback.is_null() &&
        (m_debug.m_mode != DebugState::Mode::Halted)) {
        debug_error_halt();
        // Resume the interpreter: the queued session runs next.
        throw Exception::Error;
    }
    m_debug.m_error_halt_armed = false;
#endif

    // If we're in a non-main coroutine, kill it and schedule the next one.
    // The uncaught error does NOT propagate to other coroutines (unless linked via supervision).
    if (m_running_coroutine != m_main_context) {
        coroutine_flush_running();
        auto ctx = offset_to_ptr<CoroutineContext>(m_running_coroutine);
        // Use the error name as exit reason (abnormal exit)
        auto error_reason = *m_last_error_name_ptr;
        coroutine_kill(ctx, error_reason);
        coroutine_schedule();
        // coroutine_schedule loaded the next coroutine's stacks into Trix members.
        // Resume the interpreter by throwing Error (the interpreter loop will catch it).
        throw Exception::Error;
    }

    if (m_in_global_handler) {
        diag_println("Trix recursive error during global_handler");
        // Same contract as default_handler_op: an abnormal exit must
        // carry the Error enum as the process exit code.
        m_exit_code = +m_last_error;
        throw Exception::Exit;
    }
    m_in_global_handler = true;

    auto errname = *m_last_error_name_ptr;
    auto value = m_handlersdict->get(trx, errname);
    // UserError: user-thrown Name may not be pre-registered in handlersdict;
    // fall back to the /user-error entry (default-handler).
    if ((value == nullptr) && (m_last_error == Error::UserError)) {
        auto fallback_obj = error_name(Error::UserError);
        value = m_handlersdict->get(trx, &fallback_obj);
    }
    if ((value == nullptr) || value->is_literal() || value->ignores_execute()) {
        auto cause = (value == nullptr) ? "missing" : "invalid";
        auto errstr = errname.name_value(trx)->sv();
        auto opstr = m_last_operator_ptr->operator_string(trx);

        diag_println("Trix {} {} handler '{}'", cause, errstr, opstr);
        format_backtrace();
        // return to interpreter and exit (abnormal: exit code = Error enum)
        m_exit_code = +m_last_error;
        throw Exception::Exit;
    } else {
        // print backtrace while exec stack is still intact
        format_backtrace();

        // Lazily allocate arrays for preserving stacks (allocated once, reused on subsequent errors)
        if (m_ostack_ptr->arrays_length() == 0) {
            auto op_capacity = static_cast<length_t>(m_op_limit - m_op_base);
            auto dict_capacity = static_cast<length_t>(m_dict_limit - m_dict_base);
            auto exec_capacity = static_cast<length_t>(m_exec_limit - m_exec_base);
            auto n = ((op_capacity + dict_capacity + exec_capacity) * sizeof(Object));
            auto remaining = vm_remaining<Object>();
            if (remaining < n) {
                auto errstr = errname.name_value(trx)->sv();
                auto opstr = m_last_operator_ptr->operator_string(trx);
                diag_println("Trix insufficient VM for {} handler '{}'", errstr, opstr);
                // return to interpreter and exit (abnormal: exit code = Error enum)
                m_exit_code = +m_last_error;
                throw Exception::Exit;
            }

            *m_ostack_ptr = Object::make_empty_array(trx, op_capacity, Object::LiteralAttrib, Object::ReadOnlyAccess);
            *m_dstack_ptr = Object::make_empty_array(trx, dict_capacity, Object::LiteralAttrib, Object::ReadOnlyAccess);
            *m_estack_ptr = Object::make_empty_array(trx, exec_capacity, Object::LiteralAttrib, Object::ReadOnlyAccess);
        }

        // preserve stacks
        *m_ostack_ptr = save_stack(*m_ostack_ptr, m_op_base, m_op_ptr);
        *m_dstack_ptr = save_stack(*m_dstack_ptr, m_dict_base, m_dict_ptr);
        *m_estack_ptr = save_stack(*m_estack_ptr, m_exec_base, m_exec_ptr);

        // Clear name bindings for all dicts above the three permanent dicts.
        // dict_base[0] = systemdict, dict_base[1] = protocoldict, dict_base[2] = userdict.
        auto dict_bottom = (m_dict_base + PermanentDictCount - 1);
        for (auto ptr = m_dict_ptr; ptr > dict_bottom; --ptr) {
            ptr->dict_value(trx)->clear_name_bindings(trx);
        }

        // reset stacks; cap dict_ptr at dict_bottom but never advance past actual top
        m_op_ptr = (m_op_base - 1);
        m_dict_ptr = std::min(m_dict_ptr, dict_bottom);
        m_exec_ptr = (m_exec_base - 1);
        m_err_ptr = (m_err_base - 1);

        // push parameters
        *++m_op_ptr = *m_dstack_ptr;
        *++m_op_ptr = *m_estack_ptr;
        *++m_op_ptr = *m_ostack_ptr;
        *++m_op_ptr = *m_last_error_msg_ptr;
        *++m_op_ptr = *m_last_operator_ptr;
        *++m_op_ptr = *m_last_error_name_ptr;

        // call handler; @handler-done sentinel clears m_in_global_handler after it
        // completes.  The Quit floor below it restores the termination sentinel
        // that the stack reset above discarded (the startup bootstrap pushes
        // [Quit, @run, stream] -- see Stream::init): a custom handlersdict proc
        // that returns normally then drains to Quit and exits cleanly (exit code
        // 0, the documented contract) instead of parking the interpreter forever
        // in wait_for_exec_ready on an empty exec stack.  The stock
        // default-handler throws Exception::Exit itself and never reaches it.
        require_exec_capacity(3);
        *++m_exec_ptr = Object::make_operator(SystemName::Quit);
        *++m_exec_ptr = Object::make_control_operator(SystemName::atHandlerDone);
        *++m_exec_ptr = *value;

        // return to interpreter and resume execution
        throw Exception::Error;
    }
}

[[noreturn]] void error(Error err, Object error_msg) {
    if (!error_msg.is_string()) {
        diag_println("Trix internal error: error(Error, Object) called with non-string message");
        std::abort();
    }

    // During VM initialization (before Dict::init completes), the error
    // infrastructure (errordict pointers, error name table, error stack)
    // has not been set up.  Bypass the full error path and throw directly.
    if (!m_error_init_complete) {
        m_last_error = err;
        diag_println("Trix {}: error during VM initialization (VM heap too small?)", error_sv(err));
        throw Exception::Error;
    }

    // Cancel any in-progress multi-step VM allocation so partially-allocated
    // memory is not committed by a subsequent vm_end_alloc call.
    m_vm_alloc_active = nullptr;

    // Reset the GC-root stack (vm_heap.inl).  The operator that raised the error
    // may have registered in-flight temporary roots; they are abandoned along
    // with the operator, so drop them here -- the earliest point on the throw
    // path, before try_catch_handler() / global_handler() run.  Those handlers
    // (and the handler atoms they splice) can restore the VM and then allocate,
    // so a stale root left on the stack would be walked by a vm_global_gc against
    // freed memory.  This must live in error(), not the interpreter-loop catch:
    // error() invokes the handlers itself, so the exception does not reach the
    // catch until after they have already run.
    m_gc_roots_ptr = m_gc_roots_base - 1;

    // Free the running coroutine's scratch arena if dirty (find-all/find-n
    // interrupted by error).  Fail errors are normal control flow in find-all --
    // the scratch holds accumulated results that must survive Fail/restore cycles.
    // Runs for whichever coroutine raised the error; m_scratch_* point at that
    // coroutine's per-coroutine arena.  Resetting it here is safe for a dying
    // non-main coroutine because coroutine_flush_running re-saves the reset
    // m_scratch_ptr before coroutine_kill walks the arena.
    if ((m_scratch_ptr != m_scratch_base) && (err != Error::Fail)) {
        scratch_free<Object>();
    }

    m_last_error = err;
    // UserError carries an arbitrary user-thrown Name pre-written to
    // *m_last_error_name_ptr by throw_op / throw_with_op; leave it intact.
    if (err != Error::UserError) {
        *m_last_error_name_ptr = error_name(err);
    }
    *m_last_error_msg_ptr = error_msg;
    *m_last_error_data_ptr = root_object(RootObject::PendingErrorData);
    set_root_object(RootObject::PendingErrorData, Object::make_null());

    // Snapshot the err-stack top so format_backtrace can read barrier
    // companions even after try_catch_handler unwinds them.  Pops decrement
    // the live m_err_ptr but the bytes survive; format_backtrace walks
    // downward from this snapshot.
    m_error_err_ptr_snapshot = m_err_ptr;

    // Fresh error: no objects planted yet for THIS unwind.  (A stale count
    // from an error whose unwind never crossed a scanner cleanup is cleared
    // by the interpreter's Exception::Error catch; this reset covers errors
    // raised before that catch ran, e.g. a cascade inside a handler atom.)
    m_planted_error_objects = 0;

    // VMFull is always printed to stderr; try_catch_handler() is still called so
    // a { ... } try-catch around the exhausting op can catch and handle it.
    if (err == Error::VMFull) {
        auto trx = this;
        auto opstr = m_last_operator_ptr->operator_string(trx);
        auto msgstr = m_last_error_msg_ptr->sv_value(trx);

        diag_println("Trix vm-full '{}': {}", opstr, msgstr);
    }
    try_catch_handler();
    global_handler();
}
// check_chattr / verify_chattr moved to scanner_tables.inl as consteval --
// the character-class tables are now verified at COMPILE time (static_assert
// at the bottom of trix.h), not at debug startup.

#include "ops_debugger.inl"
public:
// Host-kernel API: bounded engine RNG draw -- the SAME PCG32 stream the
// rand-* operators consume.  Dedicated host binaries (tetrix.cpp,
// chip8.cpp) call this from native kernels so seeded runs stay
// bit-identical with the Trix-side reference implementations; the
// lockstep self-tests depend on that equivalence.
uint32_t rand_bounded_uint32(uint32_t bound) {
    return m_pcg32.next_uint32(bound);
}
private:
// rand-seed: ulong :- --
// Seeds the PCG32 random number generator.
// throws: opstack-underflow, type-check
static void pcg_seed_op(Trix *trx) {
    trx->verify_operands(VerifyULong);

    auto ul = trx->m_op_ptr->ulong_value(trx);
    trx->m_pcg32.seed(ul);
    trx->m_op_ptr->free_extvalue(trx);
    --trx->m_op_ptr;
}

// rand-uinteger: :- uint
// Generates a random unsigned integer.
// throws: opstack-overflow
static void pcg_uinteger_op(Trix *trx) {
    trx->require_op_capacity(1);

    auto ui = trx->m_pcg32.next_uint32();
    *++trx->m_op_ptr = Object::make_uinteger(ui);
}

// rand-bounded-uinteger: uint :- uint
// Generates a random unsigned integer in [0, bound).
// throws: opstack-underflow, range-check, type-check
static void pcg_bounded_uinteger_op(Trix *trx) {
    trx->verify_operands(VerifyUInteger);

    auto bound = trx->m_op_ptr->uinteger_value();
    if (bound == 0) {
        trx->error(Error::RangeCheck, "rand-bounded-uinteger: bound must be > 0");
    } else {
        auto ui = trx->m_pcg32.next_uint32(bound);
        trx->m_op_ptr->update_uinteger(ui);
    }
}

// rand-ulong: :- ulong
// Generates a random unsigned long.
// throws: vm-full, opstack-overflow
static void pcg_ulong_op(Trix *trx) {
    trx->require_op_capacity(1);

    auto ul = trx->m_pcg32.next_uint64();
    *++trx->m_op_ptr = Object::make_ulong(trx, ul);
}

// rand-bounded-ulong: ulong :- ulong
// Generates a random unsigned long in [0, bound).
// throws: opstack-underflow, range-check, type-check
static void pcg_bounded_ulong_op(Trix *trx) {
    trx->verify_operands(VerifyULong);

    auto bound = trx->m_op_ptr->ulong_value(trx);
    if (bound == 0) {
        trx->error(Error::RangeCheck, "rand-bounded-ulong: bound must be > 0");
    } else {
        auto ul = trx->m_pcg32.next_uint64(bound);
        trx->m_op_ptr->update_ulong(trx, ul);
    }
}

// rand-int128: :- int128
// Generates a random signed 128-bit integer (full range, including negative).
// throws: vm-full, opstack-overflow
static void pcg_int128_op(Trix *trx) {
    trx->require_op_capacity(1);

    auto u = trx->m_pcg32.next_uint128();
    *++trx->m_op_ptr = Object::make_int128(trx, static_cast<int128_t>(u));
}

// rand-uint128: :- uint128
// Generates a random unsigned 128-bit integer.
// throws: vm-full, opstack-overflow
static void pcg_uint128_op(Trix *trx) {
    trx->require_op_capacity(1);

    auto u = trx->m_pcg32.next_uint128();
    *++trx->m_op_ptr = Object::make_uint128(trx, u);
}

// rand-bounded-uint128: uint128 :- uint128
// Generates a random unsigned 128-bit integer in [0, bound).
// throws: opstack-underflow, range-check, type-check
static void pcg_bounded_uint128_op(Trix *trx) {
    trx->verify_operands(VerifyUInt128);

    auto bound = trx->m_op_ptr->uint128_value(trx);
    if (bound == 0) {
        trx->error(Error::RangeCheck, "rand-bounded-uint128: bound must be > 0");
    } else {
        auto u = trx->m_pcg32.next_uint128(bound);
        trx->m_op_ptr->update_uint128(trx, u);
    }
}

// rand-real: :- num
// Generates a random real in [0, 1).
// throws: opstack-overflow
static void pcg_real_op(Trix *trx) {
    trx->require_op_capacity(1);

    auto f = trx->m_pcg32.next_float();
    *++trx->m_op_ptr = Object::make_real(f);
}

// rand-double: :- num
// Generates a random double in [0, 1).
// throws: vm-full, opstack-overflow
static void pcg_double_op(Trix *trx) {
    trx->require_op_capacity(1);

    auto d = trx->m_pcg32.next_double();
    *++trx->m_op_ptr = Object::make_double(trx, d);
}
