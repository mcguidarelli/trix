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

//===--- Transducer Operators ---===//
//
// Composable transformations that can be applied to any sequence type.
// Each transducer step is a Tagged value (payload /tag-name tag).
// Composed transducers are arrays of steps.
//
// The application operators (into, lazy-into, pipe-into) interpret the
// transducer spec and push the corresponding target-specific operations
// onto the exec stack, reusing existing operators for each target type.
//
//   xf-map       proc -- xf           Mapping step
//   xf-filter    proc -- xf           Filtering step
//   xf-take      n -- xf              Take first N
//   xf-drop      n -- xf              Drop first N
//   xf-scan      init proc -- xf      Running accumulation
//   xf-flatten   -- xf                Flatten one level
//   xf-distinct  -- xf                Remove consecutive duplicates
//   xf-compose   xf1 xf2 -- xf       Compose two transducers
//   into         array xf -- array'   Apply to eager array
//   lazy-into    lazy xf -- lazy'     Apply to lazy sequence
//   pipe-into    pipe xf -- pipe'     Apply to pipeline stage
//   xf-reduce    coll init xf rf -- result  Full transduce with reducer

// Transducer step constructors: create tagged values.

// xf-map: proc -- xf
// throws: opstack-underflow, type-check, vm-full
static void xf_map_op(Trix *trx) {
    trx->verify_operands(VerifyProc);

    *trx->m_op_ptr = Object::make_tagged(trx, trx->wellknown_name(WellKnownName::XfMap), *trx->m_op_ptr);
}

// xf-filter: proc -- xf
// throws: opstack-underflow, type-check, vm-full
static void xf_filter_op(Trix *trx) {
    trx->verify_operands(VerifyProc);

    *trx->m_op_ptr = Object::make_tagged(trx, trx->wellknown_name(WellKnownName::XfFilter), *trx->m_op_ptr);
}

// xf-take: n -- xf
// throws: opstack-underflow, type-check, vm-full
static void xf_take_op(Trix *trx) {
    trx->verify_operands(VerifyIntegers);

    *trx->m_op_ptr = Object::make_tagged(trx, trx->wellknown_name(WellKnownName::XfTake), *trx->m_op_ptr);
}

// xf-drop: n -- xf
// throws: opstack-underflow, type-check, vm-full
static void xf_drop_op(Trix *trx) {
    trx->verify_operands(VerifyIntegers);

    *trx->m_op_ptr = Object::make_tagged(trx, trx->wellknown_name(WellKnownName::XfDrop), *trx->m_op_ptr);
}

static constexpr int XFInitIndex = 0;
static constexpr int XFProcIndex = 1;

// xf-scan: init proc -- xf
// throws: opstack-underflow, type-check, vm-full
static void xf_scan_op(Trix *trx) {
    trx->verify_operands(VerifyProc, VerifyAny);

    // Payload is a 2-element array [init, proc].  Allocate region-aware so the
    // payload tracks make_tagged's region (a local payload under a global Tagged
    // dangles after restore), then root it across make_tagged's own allocation.
    auto [elem_ptr, array_offset] = trx->vm_alloc_dispatch_n<Object>(2, Trix::ChunkKind::Array);
    elem_ptr[XFInitIndex] = trx->m_op_ptr[-1];
    elem_ptr[XFProcIndex] = trx->m_op_ptr[0];
    auto payload_obj = Object::make_array(array_offset, 2);

    --trx->m_op_ptr;
    *trx->m_op_ptr = payload_obj;
    *trx->m_op_ptr = Object::make_tagged(trx, trx->wellknown_name(WellKnownName::XfScan), payload_obj);
}

// xf-flatten: -- xf
// throws: opstack-overflow, vm-full
static void xf_flatten_op(Trix *trx) {
    trx->require_op_capacity(1);

    *++trx->m_op_ptr = Object::make_tagged(trx, trx->wellknown_name(WellKnownName::XfFlatten), Object::make_null());
}

// xf-distinct: -- xf
// throws: opstack-overflow, vm-full
static void xf_distinct_op(Trix *trx) {
    trx->require_op_capacity(1);

    *++trx->m_op_ptr = Object::make_tagged(trx, trx->wellknown_name(WellKnownName::XfDistinct), Object::make_null());
}

// xf-compose: xf1 xf2 -- xf
// Compose two transducers.  A single step (Tagged) becomes a 1-element array.
// Two arrays are concatenated.
// throws: limit-check, opstack-underflow, type-check, unsupported, vm-full
static void xf_compose_op(Trix *trx) {
    trx->verify_operands(VerifyTransducer, VerifyTransducer);

    auto xf2_obj = trx->m_op_ptr[0];
    auto xf1_obj = trx->m_op_ptr[-1];

    // Normalize to arrays
    auto xf1_is_array = xf1_obj.is_array();
    auto xf2_is_array = xf2_obj.is_array();

    auto len1 = xf1_is_array ? xf1_obj.arrays_length() : static_cast<length_t>(1);
    auto len2 = xf2_is_array ? xf2_obj.arrays_length() : static_cast<length_t>(1);
    if ((len1 + len2) > MaxLength) {
        trx->error(Error::LimitCheck, "xf-compose: composed transducer exceeds maximum length");
    } else {
        auto total = static_cast<length_t>(len1 + len2);

        // Region-aware result so the composed transducer survives save/restore
        // under ${...} (mirrors xf-scan's region-aware payload).  No GC-rooting is
        // needed across the fill: transducer steps are Tagged objects, and a
        // Tagged uses neither an ExtValue nor a WideValue, so make_clone is a
        // shallow handle copy that never allocates -- no vm_global_gc can fire
        // between this allocation and the push onto the op stack.
        auto [elem_ptr, array_offset] = trx->vm_alloc_dispatch_n<Object>(total, ChunkKind::Array);

        if (xf1_is_array) {
            auto src1 = xf1_obj.array_objects(trx);
            for (length_t i = 0; i < len1; ++i) {
                elem_ptr[i] = src1[i].make_clone(trx);
            }
        } else {
            elem_ptr[0] = xf1_obj.make_clone(trx);
        }

        if (xf2_is_array) {
            auto src2 = xf2_obj.array_objects(trx);
            for (length_t i = 0; i < len2; ++i) {
                elem_ptr[len1 + i] = src2[i].make_clone(trx);
            }
        } else {
            elem_ptr[len1] = xf2_obj.make_clone(trx);
        }

        --trx->m_op_ptr;
        *trx->m_op_ptr = Object::make_array(array_offset, total, Object::LiteralAttrib, Object::ReadOnlyAccess);
    }
}

// xf_get_steps: normalize a transducer to an array of steps.
// If Tagged (single step): returns pointer to the single object, length 1.
// If Array (composed): returns array elements, length N.
// Otherwise (not Tagged or Array): raises Error::TypeCheck.
static std::pair<Object *, length_t> xf_get_steps(Trix *trx, Object *xf_ptr) {
    if (xf_ptr->is_tagged()) {
        return std::pair{xf_ptr, static_cast<length_t>(1)};
    } else if (xf_ptr->is_array()) {
        return xf_ptr->array_value(trx);
    } else {
        trx->error(Error::TypeCheck, "transducer must be a tagged step or array of steps");
    }
}

enum struct XfTarget { Array, Lazy, Pipe };

// xf_push_steps_for_target: push transducer steps onto the exec stack as
// target-specific operations.  Steps are pushed in reverse (LIFO) so they
// execute in the correct order.  A step's tag-value, when present, is pushed as
// a literal argument (e.g. xf-map's proc, xf-take's count).
//
// For a step like xf-map { 3 mul }:
//   Push the `map` operator (exec)
//   Push the proc `{ 3 mul }` as literal (exec -> lands on op stack)
//
// For a step like xf-flatten (no argument):
//   Push the `flatten` operator (exec)
static void xf_push_steps_for_target(Trix *trx, Object *steps_ptr, length_t step_count, XfTarget target) {
    // Push in reverse order (last step pushed first = executed last)
    for (length_t i = step_count; i > 0; --i) {
        auto step_ptr = &steps_ptr[i - 1];
        if (step_ptr->is_tagged()) {
            auto pair = step_ptr->tagged_storage(trx);
            auto tag_name_obj = pair[Object::TaggedNameIndex];
            auto payload_obj = pair[Object::TaggedValueIndex];

            // Dispatch on tag name to find target operator
            SystemName op{};
            auto has_arg = true;
            auto is_scan = false;

            if (tag_name_obj.equal(trx, trx->wellknown_name(WellKnownName::XfMap))) {
                if (target == XfTarget::Array) {
                    op = SystemName::Map;
                } else if (target == XfTarget::Lazy) {
                    op = SystemName::LazyMap;
                } else {
                    op = SystemName::PipeMap;
                }
            } else if (tag_name_obj.equal(trx, trx->wellknown_name(WellKnownName::XfFilter))) {
                if (target == XfTarget::Array) {
                    op = SystemName::Filter;
                } else if (target == XfTarget::Lazy) {
                    op = SystemName::LazyFilter;
                } else {
                    op = SystemName::PipeFilter;
                }
            } else if (tag_name_obj.equal(trx, trx->wellknown_name(WellKnownName::XfTake))) {
                if (target == XfTarget::Array) {
                    op = SystemName::Take;
                } else if (target == XfTarget::Lazy) {
                    op = SystemName::LazyTake;
                } else {
                    trx->error(Error::Unsupported, "xf-take not supported for pipeline target");
                }
            } else if (tag_name_obj.equal(trx, trx->wellknown_name(WellKnownName::XfDrop))) {
                if (target == XfTarget::Array) {
                    op = SystemName::Drop;
                } else if (target == XfTarget::Lazy) {
                    op = SystemName::LazyDrop;
                } else {
                    trx->error(Error::Unsupported, "xf-drop not supported for pipeline target");
                }
            } else if (tag_name_obj.equal(trx, trx->wellknown_name(WellKnownName::XfScan))) {
                if (target == XfTarget::Array) {
                    op = SystemName::Scan;
                } else if (target == XfTarget::Lazy) {
                    op = SystemName::LazyScan;
                } else {
                    trx->error(Error::Unsupported, "xf-scan not supported for pipeline target");
                }
                has_arg = false;
                is_scan = true;
            } else if (tag_name_obj.equal(trx, trx->wellknown_name(WellKnownName::XfFlatten))) {
                if (target == XfTarget::Array) {
                    op = SystemName::Flatten;
                } else if (target == XfTarget::Lazy) {
                    op = SystemName::LazyFlatten;
                } else {
                    trx->error(Error::Unsupported, "xf-flatten not supported for pipeline target");
                }
                has_arg = false;
            } else if (tag_name_obj.equal(trx, trx->wellknown_name(WellKnownName::XfDistinct))) {
                if (target == XfTarget::Array) {
                    op = SystemName::Dedupe;
                } else if (target == XfTarget::Lazy) {
                    op = SystemName::LazyDedupe;
                } else {
                    trx->error(Error::Unsupported, "xf-distinct not supported for pipeline target");
                }
                has_arg = false;
            } else {
                auto sv = tag_name_obj.name_sv(trx);
                trx->error(Error::TypeCheck, "unknown transducer step tag: /{}", sv);
            }

            if (has_arg) {
                auto arg_obj = payload_obj.make_clone(trx);
                auto arg_is_proc = (arg_obj.is_sequence() && arg_obj.is_executable());
                arg_obj.set_literal();

                if (arg_is_proc) {
                    // Proc arguments need make-executable after landing on op stack
                    trx->require_exec_capacity(3);

                    *++trx->m_exec_ptr = Object::make_operator(op);
                    *++trx->m_exec_ptr = Object::make_operator(SystemName::MakeExecutable);
                    *++trx->m_exec_ptr = arg_obj;
                } else {
                    // Scalar arguments (integers, etc.) just need literal push
                    trx->require_exec_capacity(2);

                    *++trx->m_exec_ptr = Object::make_operator(op);
                    *++trx->m_exec_ptr = arg_obj;
                }
            } else if (is_scan) {
                trx->require_exec_capacity(4);

                // The step tag-name is user-forgeable via `tag`, so the payload
                // shape is not guaranteed; array_objects() only asserts is_array()
                // (compiled out under NDEBUG -> OOB read on a forged payload).
                if (!payload_obj.is_array() || (payload_obj.arrays_length() != 2)) {
                    trx->error(Error::TypeCheck, "xf-scan step payload must be a 2-element [init, proc] array");
                } else {
                    // Payload is [init, proc] -- push both as arguments
                    auto scan_args_ptr = payload_obj.array_objects(trx);
                    auto scan_init_obj = scan_args_ptr[XFInitIndex];
                    auto scan_proc_obj = scan_args_ptr[XFProcIndex];

                    // scan/reduce: push init (literal), make-executable, proc (literal), operator
                    auto proc_arg_obj = scan_proc_obj.make_clone(trx);
                    proc_arg_obj.set_literal();

                    auto init_arg_obj = scan_init_obj.make_clone(trx);
                    init_arg_obj.set_literal();

                    *++trx->m_exec_ptr = Object::make_operator(op);
                    *++trx->m_exec_ptr = Object::make_operator(SystemName::MakeExecutable);
                    *++trx->m_exec_ptr = proc_arg_obj;
                    *++trx->m_exec_ptr = init_arg_obj;
                }
            } else {
                trx->require_exec_capacity(1);

                *++trx->m_exec_ptr = Object::make_operator(op);
            }
        } else {
            trx->error(Error::TypeCheck, "transducer step must be a tagged value");
        }
    }
}

// into: array xf -- array'
// Apply transducer to eager array.
// throws: execstack-overflow, opstack-underflow, type-check, unsupported, vm-full
static void into_op(Trix *trx) {
    trx->verify_operands(VerifyTransducer, VerifyArray);

    auto [steps_ptr, step_count] = xf_get_steps(trx, trx->m_op_ptr);

    // Pop xf, leave array on stack
    --trx->m_op_ptr;

    xf_push_steps_for_target(trx, steps_ptr, step_count, XfTarget::Array);
}

// lazy-into: lazy xf -- lazy'
// Apply transducer to lazy sequence.
// throws: execstack-overflow, opstack-underflow, type-check, unsupported, vm-full
static void lazy_into_op(Trix *trx) {
    trx->verify_operands(VerifyTransducer, VerifyLazySeq);

    auto [steps_ptr, step_count] = xf_get_steps(trx, trx->m_op_ptr);

    --trx->m_op_ptr;

    xf_push_steps_for_target(trx, steps_ptr, step_count, XfTarget::Lazy);
}

// pipe-into: pipe xf -- pipe'
// Apply transducer as pipeline stage.
// throws: execstack-overflow, opstack-underflow, type-check, unsupported, vm-full
static void pipe_into_op(Trix *trx) {
    trx->verify_operands(VerifyTransducer, VerifyPipeBuffer);

    auto [steps_ptr, step_count] = xf_get_steps(trx, trx->m_op_ptr);

    --trx->m_op_ptr;

    xf_push_steps_for_target(trx, steps_ptr, step_count, XfTarget::Pipe);
}

// xf-reduce: coll init xf rf -- result
// Full transduce: apply transducer to collection, then reduce with rf.
// Equivalent to: coll xf into init rf reduce
// throws: execstack-overflow, opstack-underflow, type-check, unsupported, vm-full
static void xf_reduce_op(Trix *trx) {
    trx->verify_operands(VerifyProc, VerifyTransducer, VerifyAny, VerifyAny);
    trx->require_exec_capacity(4);

    auto rf_obj = trx->m_op_ptr[0];
    auto xf_obj = trx->m_op_ptr[-1];
    auto init_obj = trx->m_op_ptr[-2];

    auto [steps_ptr, step_count] = xf_get_steps(trx, &xf_obj);

    // Pop rf, xf, init; leave coll on stack
    trx->m_op_ptr -= 3;

    // Push: reduce (last), make-executable, rf (literal), init (literal), then xf steps
    auto rf_lit_obj = rf_obj;
    rf_lit_obj.set_literal();

    auto init_lit_obj = init_obj;
    init_lit_obj.set_literal();

    *++trx->m_exec_ptr = Object::make_operator(SystemName::Reduce);
    *++trx->m_exec_ptr = Object::make_operator(SystemName::MakeExecutable);
    *++trx->m_exec_ptr = rf_lit_obj;
    *++trx->m_exec_ptr = init_lit_obj;

    // Push xf steps (these execute first, before reduce)
    xf_push_steps_for_target(trx, steps_ptr, step_count, XfTarget::Array);
}
