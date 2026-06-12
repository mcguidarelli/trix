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

//===--- Lazy Sequence Operators ---===//
//
// Implements lazy (demand-driven) sequences for memory-efficient processing
// of potentially infinite data.  Based on:
//
//   Haskell lazy lists: the default evaluation strategy where list elements
//   are computed only when demanded.  `[1..]` is an infinite list that
//   consumes no memory until elements are forced.
//
//   Clojure lazy sequences (lazy-seq, map, filter, take, etc.): a practical
//   lazy sequence library built on thunks, with chunked realization for
//   performance and composable transformers.
//
//   SRFI-41 (Scheme streams): explicitly lazy sequences using delay/force
//   (thunks) for the tail, with a rich combinator library.
//
// The specific design follows the SRFI-41 / Clojure model: a lazy sequence
// is a cons cell [head, thunk-for-tail] where the tail is computed on
// demand.  This gives O(1) memory for pipeline-style processing of
// arbitrarily large (or infinite) data.
//
// --- Core concepts for maintainers ---
//
// REPRESENTATION
//   A lazy sequence is one of:
//     - null: the empty sequence (lazy-nil).
//     - A 2-element array [head, tail-thunk]: head is the current value,
//       tail-thunk is a Thunk (ops_higher.inl) that, when forced, produces
//       the next lazy sequence (another [head, tail-thunk] or null).
//
//   This is a classic cons-cell representation.  The thunk wrapper on the
//   tail is what makes it lazy: the rest of the sequence does not exist
//   until someone calls lazy-tail (which forces the thunk).
//
//   No new VM type is needed -- lazy sequences are built from existing
//   Array and Thunk types.  The `lazy-seq?` predicate checks: is it null,
//   or is it a 2-element array whose second element is a thunk?
//
// CONSTRUCTORS (sources)
//   lazy-nil         -- empty sequence
//   lazy-cons        -- prepend a value to a lazy tail
//   lazy-seq         -- convert an eager array to a lazy sequence
//   lazy-from        -- infinite sequence: n, n+1, n+2, ...
//   lazy-range       -- bounded sequence: start to stop by step
//   lazy-repeat      -- infinite repetition of a value
//   lazy-repeat-n    -- repeat a value N times
//   lazy-iterate     -- seed, f(seed), f(f(seed)), ...
//   lazy-cycle       -- infinite cycle through an array's elements
//   lazy-unfold      -- generate from a state + step function
//
// TRANSFORMERS (lazy, produce new lazy sequences)
//   lazy-map         -- apply proc to each element
//   lazy-filter      -- keep elements matching predicate
//   lazy-filter-not  -- keep elements NOT matching predicate
//   lazy-filter-map  -- map + filter nulls (like Rust's filter_map)
//   lazy-flat-map    -- map then flatten one level
//   lazy-flatten     -- flatten nested lazy sequences
//   lazy-map-indexed -- map with index argument
//   lazy-scan        -- running fold (like Haskell's scanl)
//   lazy-take        -- first N elements
//   lazy-drop        -- skip first N elements
//   lazy-take-while  -- take while predicate holds
//   lazy-drop-while  -- skip while predicate holds
//   lazy-dedupe      -- remove consecutive duplicates
//   lazy-intersperse -- insert separator between elements
//   lazy-step-by     -- take every Nth element
//   lazy-zip         -- pair elements from two sequences
//   lazy-zip-with    -- combine elements with a proc
//   lazy-chain       -- concatenate two sequences
//   lazy-interleave  -- alternate elements from two sequences
//   lazy-enumerate   -- pair each element with its index
//   lazy-chunked     -- group into arrays of N
//   lazy-windowed    -- sliding window of size N
//   lazy-pairwise    -- sliding window of size 2
//
// CONSUMERS (eager, force the sequence)
//   lazy-to-array    -- realize entire sequence into an array
//   lazy-fold        -- reduce to a single value (foldl)
//   lazy-for-each    -- side-effect per element
//   lazy-any         -- true if any element matches predicate
//   lazy-all         -- true if all elements match predicate
//   lazy-find        -- first element matching predicate
//   lazy-find-index  -- index of first match
//   lazy-nth         -- element at index N
//   lazy-count       -- count elements
//   lazy-sum         -- sum all elements
//
// TRAMPOLINE PATTERN
//   Many lazy operations need to run a user proc (e.g., the predicate in
//   lazy-filter) and then continue building the lazy sequence based on
//   the result.  Since Trix cannot call a proc and wait for its return
//   (procs execute via the exec stack, not function calls), these
//   operations use a trampoline:
//     1. Push a continuation control operator (@lazy-filter-test, etc.)
//        and any saved state onto the exec stack.
//     2. Push the user proc onto the exec stack (runs first).
//     3. Return from the operator.  The interpreter runs the proc.
//     4. When the proc completes, the control operator fires, reads
//        the result from the operand stack, and continues.
//   This pattern appears throughout the file and is the reason for the
//   large number of @-prefixed control operators.
//
// MEMORY BEHAVIOR
//   Lazy sequences consume O(1) memory per pipeline stage regardless of
//   sequence length.  Each step produces one cons cell and discards the
//   previous one (the thunk is forced, producing the next cell, and the
//   old cell becomes garbage eligible for save/restore reclamation).
//   However, holding a reference to the head of a sequence while
//   traversing it prevents reclamation (the entire realized prefix is
//   retained).
//

// lazy-seq?: any :- bool
// throws: opstack-underflow
static void is_lazy_seq_op(Trix *trx) {
    trx->require_op_count(1);

    auto top_ptr = trx->m_op_ptr;
    auto is_lazy = top_ptr->is_lazy_seq(trx);
    if (!is_lazy) {
        top_ptr->maybe_free_extvalue(trx);
    }
    *top_ptr = Object::make_boolean(is_lazy);
}

// lazy-empty?: lazy :- bool
// throws: opstack-underflow, type-check
static void lazy_empty_op(Trix *trx) {
    trx->verify_operands(VerifyLazySeq);

    auto top_ptr = trx->m_op_ptr;
    *top_ptr = Object::make_boolean(top_ptr->is_null());
}

// lazy-head: lazy :- val
// throws: opstack-underflow, range-check, type-check, vm-full
static void lazy_head_op(Trix *trx) {
    trx->verify_operands(VerifyLazySeqNonempty);

    auto top_ptr = trx->m_op_ptr;
    *top_ptr = top_ptr->array_objects(trx)[Object::LazyHeadIndex].make_clone(trx);
}

// lazy-tail: lazy :- lazy
// Gets element [1] (tail thunk) and forces it.
// throws: execstack-overflow, opstack-underflow, range-check, type-check, undefined-result, vm-full
static void lazy_tail_op(Trix *trx) {
    trx->verify_operands(VerifyLazySeqNonempty);

    auto top_ptr = trx->m_op_ptr;
    *top_ptr = top_ptr->array_objects(trx)[Object::LazyTailThunkIndex].make_clone(trx);
    force_op(trx);
}

// lazy-nil: -- null
// throws: opstack-overflow
static void lazy_nil_op(Trix *trx) {
    trx->require_op_capacity(1);

    *++trx->m_op_ptr = Object::make_null();
}

// lazy-cons: val tail -- lazy
// tail may be null, a lazy-seq (array), or a thunk.
// throws: opstack-underflow, type-check, vm-full
static void lazy_cons_op(Trix *trx) {
    trx->require_op_count(2);

    auto tail_ptr = trx->m_op_ptr;
    auto val_ptr = (tail_ptr - 1);
    auto tail_obj = *tail_ptr;
    auto val_obj = *val_ptr;

    Object tail_thunk;
    if (tail_obj.is_thunk()) {
        tail_thunk = tail_obj;
    } else if (tail_obj.is_null()) {
        // Wrap null in a pre-evaluated thunk
        tail_thunk = Object::make_pre_evaluated_thunk(trx);
    } else if (tail_obj.is_lazy_seq_nonempty(trx)) {
        // Wrap a genuine lazy-seq (2-element array whose [1] is a thunk) in a
        // pre-evaluated thunk.  A bare 2-element array whose [1] is NOT a thunk
        // falls through to the TypeCheck below, surfacing the error at the cons
        // site instead of as a confusing deferred failure when the tail is forced.
        tail_thunk = Object::make_pre_evaluated_thunk(trx, tail_obj);
    } else {
        trx->error(Error::TypeCheck, "lazy-cons: tail must be null, lazy-seq, or thunk");
    }

    --trx->m_op_ptr;
    *trx->m_op_ptr = Object::make_lazy(trx, val_obj, tail_thunk);
}

// The lazy thunk-chain builders root their in-flight global handles on the
// GC-root stack (vm_heap.inl: require_gc_root_capacity + raw `*++m_gc_roots_ptr`
// pushes + a rolling `*r`, cleared at the operator's tail).  make_curry_pair /
// make_compose_pair / make_lazy_thunk / make_lazy route through vm_alloc_dispatch_n
// -> gvm_alloc when m_curr_alloc_global is set (a ${...} / set-global context),
// and gvm_alloc can fire vm_global_gc mid-build on the VMFull retry path;
// vm_global_gc walks the op/exec/err/dict/scratch/gc-root stacks but NOT C locals,
// so a freshly built chain link held only in a C local across the NEXT allocation
// would be swept and the published lazy-seq would point at freed VM.

// lazy-from: n -- lazy
// Infinite sequence: n, n+1, n+2, ...
// throws: limit-check, opstack-underflow, type-check, vm-full
static void lazy_from_op(Trix *trx) {
    trx->verify_operands(VerifyIntegers);

    // Tail thunk proc: curry(n+1, op(LazyFrom))
    auto n_obj = *trx->m_op_ptr;
    auto next_obj = n_obj.make_clone(trx);
    switch (+next_obj.type()) {
    case +Object::Type::Integer:
        next_obj.update_integer(static_cast<integer_t>(static_cast<uinteger_t>(next_obj.integer_value()) + 1));
        break;

    case +Object::Type::UInteger:
        next_obj.update_uinteger(next_obj.uinteger_value() + 1);
        break;

    case +Object::Type::Long:
        next_obj.update_long(trx, static_cast<long_t>(static_cast<ulong_t>(next_obj.long_value(trx)) + 1));
        break;

    case +Object::Type::ULong:
        next_obj.update_ulong(trx, next_obj.ulong_value(trx) + 1);
        break;

    case +Object::Type::Byte:
        next_obj.update_byte(static_cast<vm_t>(next_obj.byte_value() + 1));
        break;

    case +Object::Type::Int128:
        next_obj.update_int128(trx, static_cast<int128_t>(static_cast<uint128_t>(next_obj.int128_value(trx)) + 1));
        break;

    case +Object::Type::UInt128:
        next_obj.update_uint128(trx, next_obj.uint128_value(trx) + 1);
        break;

    default:
        std::unreachable();
    }

    // Node: make_lazy(n, thunk(curry(n+1, op(LazyFrom)))).  next_obj is a fresh
    // clone reachable from no root; root it on the GC-root stack across the chain
    // allocs and roll the chain through *r.  n_obj stays rooted via its operand
    // slot (*m_op_ptr, not overwritten until the node is published).
    trx->require_gc_root_capacity(1);

    *++trx->m_gc_roots_ptr = next_obj;
    auto *r = trx->m_gc_roots_ptr;
    *r = Object::make_curry_pair(trx, *r, Object::make_operator(SystemName::LazyFrom));
    *r = Object::make_lazy_thunk(trx, *r);
    *trx->m_op_ptr = Object::make_lazy(trx, n_obj, *r);

    trx->reset_gc_root(1);  // clear operator-scoped roots
}

// lazy-repeat: val -- lazy
// Infinite repetition of val.
// throws: limit-check, opstack-underflow, vm-full
static void lazy_repeat_op(Trix *trx) {
    trx->require_op_count(1);

    auto val_obj = *trx->m_op_ptr;
    // Node: make_lazy(val, thunk(curry(val_clone, op(LazyRepeat)))).  val_clone is
    // a fresh clone reachable from no root; root it on the GC-root stack across the
    // chain allocs.  val_obj stays rooted via its operand slot.
    trx->require_gc_root_capacity(1);

    *++trx->m_gc_roots_ptr = val_obj.make_clone(trx);
    auto *r = trx->m_gc_roots_ptr;
    *r = Object::make_curry_pair(trx, *r, Object::make_operator(SystemName::LazyRepeat));
    *r = Object::make_lazy_thunk(trx, *r);
    *trx->m_op_ptr = Object::make_lazy(trx, val_obj, *r);

    trx->reset_gc_root(1);  // clear operator-scoped roots
}

// lazy-repeat-n: val n -- lazy
// Finite: n copies of val.
// throws: limit-check, opstack-underflow, type-check, vm-full
static void lazy_repeat_n_op(Trix *trx) {
    trx->verify_operands(VerifyInteger, VerifyAny);

    auto n_ptr = trx->m_op_ptr;
    auto val_ptr = (n_ptr - 1);

    auto n = n_ptr->integer_value();
    if (n <= 0) {
        val_ptr->maybe_free_extvalue(trx);
        --trx->m_op_ptr;
        *trx->m_op_ptr = Object::make_null();
    } else {
        auto val_obj = *val_ptr;
        if (n == 1) {
            // Single element, tail is null.  tail_thunk is a fresh node reachable
            // from no root; root it across make_lazy.  val_obj stays rooted via its
            // operand slot (at m_op_ptr after the drop below).
            trx->require_gc_root_capacity(1);

            *++trx->m_gc_roots_ptr = Object::make_pre_evaluated_thunk(trx);
            auto *thunk = trx->m_gc_roots_ptr;
            --trx->m_op_ptr;
            *trx->m_op_ptr = Object::make_lazy(trx, val_obj, *thunk);

            trx->reset_gc_root(1);  // clear operator-scoped roots
        } else {
            // Tail thunk proc: thunk(curry(val_clone, curry(n-1, op(LazyRepeatN)))).
            // val_clone is a fresh clone reachable from no root; root it and the
            // rolling chain on the GC-root stack.  val_obj stays rooted via its
            // operand slot.
            trx->require_gc_root_capacity(2);  // val_clone + rolling chain

            *++trx->m_gc_roots_ptr = val_obj.make_clone(trx);
            auto *vc = trx->m_gc_roots_ptr;
            *++trx->m_gc_roots_ptr = Object::make_curry_pair(
                    trx, Object::make_integer(n - 1), Object::make_operator(SystemName::LazyRepeatN));  // inner
            auto *r = trx->m_gc_roots_ptr;
            *r = Object::make_curry_pair(trx, *vc, *r);  // tail_proc = curry(val_clone, inner)
            *r = Object::make_lazy_thunk(trx, *r);       // tail_thunk
            --trx->m_op_ptr;
            *trx->m_op_ptr = Object::make_lazy(trx, val_obj, *r);

            trx->reset_gc_root(2);  // clear operator-scoped roots
        }
    }
}

// lazy-map implementation: no operand verification (used by both user-facing and internal paths)
static void lazy_map_impl(Trix *trx) {
    auto proc_obj = *trx->m_op_ptr;
    auto lazy_obj = *(trx->m_op_ptr - 1);
    if (lazy_obj.is_null()) {
        --trx->m_op_ptr;
    } else {
        trx->require_exec_capacity(2);

        auto [head_obj, source_tail_obj] = lazy_uncons(trx, lazy_obj);

        // Build tail thunk: thunk(curry(proc, curry(source_tail, compose(Force,
        // compose(Exch, @lazy-map-impl))))).  head_obj / proc_clone / source_tail_obj
        // are fresh values reachable from no root; root them on the GC-root stack
        // across the chain allocs and roll the compose/curry chain through *r.
        // proc_obj / lazy_obj stay rooted via their operand slots.
        trx->require_gc_root_capacity(4);  // head, proc_clone, source_tail, rolling chain

        *++trx->m_gc_roots_ptr = head_obj;
        auto *head = trx->m_gc_roots_ptr;
        *++trx->m_gc_roots_ptr = proc_obj.make_clone(trx);
        auto *carried = trx->m_gc_roots_ptr;
        *++trx->m_gc_roots_ptr = source_tail_obj;
        auto *st = trx->m_gc_roots_ptr;
        *++trx->m_gc_roots_ptr = Object::make_compose_pair(
                trx, Object::make_operator(SystemName::Exch), Object::make_control_operator(SystemName::atLazyMapImpl));
        auto *r = trx->m_gc_roots_ptr;
        *r = Object::make_compose_pair(trx, Object::make_operator(SystemName::Force), *r);
        *r = Object::make_curry_pair(trx, *st, *r);       // inner = curry(source_tail, force_chain)
        *r = Object::make_curry_pair(trx, *carried, *r);  // tail_proc = curry(proc_clone, inner)
        *r = Object::make_lazy_thunk(trx, *r);            // tail_thunk

        // Push @lazy-map-build + tail_thunk on exec stack, then apply proc to head
        *++trx->m_exec_ptr = *r;
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atLazyMapBuild);

        --trx->m_op_ptr;
        *trx->m_op_ptr = *head;

        trx->reset_gc_root(4);  // clear operator-scoped roots
        execute_lazy_proc(trx, proc_obj);
    }
}

// lazy-map: lazy proc -- lazy
// throws: execstack-overflow, limit-check, opstack-underflow, type-check, vm-full
static void lazy_map_op(Trix *trx) {
    trx->verify_operands(VerifyCallable, VerifyLazySeq);
    lazy_map_impl(trx);
}

// @lazy-map-impl: internal recursive entry point (callable arrives literal from curry)
static void at_lazy_map_impl_op(Trix *trx) {
    trx->require_op_count(2);
    lazy_map_impl(trx);
}

// @lazy-map-build: collect mapped head, build [mapped_head, tail_thunk]
// Exec stack: [tail_thunk]  Op stack: [..., mapped_head]
static void at_lazy_map_build_op(Trix *trx) {
    trx->require_op_count(1);

    auto mapped_head_obj = *trx->m_op_ptr;
    auto tail_thunk = *trx->m_exec_ptr;  // read without popping: stays rooted on exec across make_lazy

    *trx->m_op_ptr = Object::make_lazy(trx, mapped_head_obj, tail_thunk);
    --trx->m_exec_ptr;
}

// lazy-filter implementation
static void lazy_filter_impl(Trix *trx) {
    auto pred_obj = *trx->m_op_ptr;
    auto lazy_obj = *(trx->m_op_ptr - 1);

    if (lazy_obj.is_null()) {
        --trx->m_op_ptr;
    } else {
        trx->require_exec_capacity(4);

        auto [head_obj, source_tail_obj] = lazy_uncons(trx, lazy_obj);

        // Push @lazy-filter-test + saved state on exec stack
        // State: [source_tail, pred, head]
        *++trx->m_exec_ptr = source_tail_obj;
        *++trx->m_exec_ptr = pred_obj;
        // head_obj is a fresh clone (reachable from no root); root it across its
        // own clone allocation (the clone lands on the GC-walked exec stack) and
        // until it is placed on the operand stack below.
        trx->gc_root_push_oneoff(head_obj);
        *++trx->m_exec_ptr = head_obj.make_clone(trx);
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atLazyFilterTest);

        // Push head and execute pred
        --trx->m_op_ptr;
        *trx->m_op_ptr = head_obj;

        trx->gc_root_pop_oneoff();
        execute_lazy_proc(trx, pred_obj.make_clone(trx));
    }
}

// lazy-filter: lazy pred -- lazy
// Eagerly searches for first matching element.
// throws: execstack-overflow, limit-check, opstack-underflow, type-check, vm-full
static void lazy_filter_op(Trix *trx) {
    trx->verify_operands(VerifyCallable, VerifyLazySeq);
    lazy_filter_impl(trx);
}

// @lazy-filter-impl: internal recursive entry point
static void at_lazy_filter_impl_op(Trix *trx) {
    trx->require_op_count(2);
    lazy_filter_impl(trx);
}

// @lazy-filter-test: check pred result. If true, build node. If false, try next.
// Exec stack: [source_tail, pred, head]  Op stack: [..., bool_result]
static void at_lazy_filter_test_op(Trix *trx) {
    trx->verify_operands(VerifyBoolean);

    auto result = trx->m_op_ptr->boolean_value();

    // Read the saved state WITHOUT popping: head/pred/source_tail stay rooted on
    // the exec stack while the result branch allocates its tail-thunk chain (a
    // gvm_alloc in ${...} can fire vm_global_gc, which does not walk popped-off
    // slots).  Pop only once the chain holds them or they are freed/forwarded.
    auto head_obj = *trx->m_exec_ptr;
    auto pred_obj = *(trx->m_exec_ptr - 1);
    auto source_tail_obj = *(trx->m_exec_ptr - 2);

    if (result) {
        // Match found: build node make_lazy(head, thunk(curry(pred, curry(source_tail,
        // compose(Force, compose(Exch, @lazy-filter-impl)))))).  head / pred /
        // source_tail stay rooted on the exec stack (popped only after); roll the
        // compose/curry chain through *r.
        trx->require_gc_root_capacity(1);

        *++trx->m_gc_roots_ptr = Object::make_compose_pair(
                trx, Object::make_operator(SystemName::Exch), Object::make_control_operator(SystemName::atLazyFilterImpl));
        auto *r = trx->m_gc_roots_ptr;
        *r = Object::make_compose_pair(trx, Object::make_operator(SystemName::Force), *r);
        *r = Object::make_curry_pair(trx, source_tail_obj, *r);  // inner = curry(source_tail, force_chain)
        *r = Object::make_curry_pair(trx, pred_obj, *r);         // tail_proc = curry(pred, inner)
        *r = Object::make_lazy_thunk(trx, *r);                   // tail_thunk
        *trx->m_op_ptr = Object::make_lazy(trx, head_obj, *r);
        trx->m_exec_ptr -= 3;

        trx->reset_gc_root(1);  // clear operator-scoped roots
    } else {
        trx->m_exec_ptr -= 3;
        trx->require_exec_capacity(2);

        // No match: force source_tail and try next element
        head_obj.maybe_free_extvalue(trx);

        // Push @lazy-filter-resume + pred on exec stack, force source_tail
        *++trx->m_exec_ptr = pred_obj;
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atLazyFilterResume);

        *trx->m_op_ptr = source_tail_obj;
        force_op(trx);
    }
}

// @lazy-filter-resume: after forcing source tail, restart search.
// Exec stack: [pred]  Op stack: [..., forced_result]
static void at_lazy_filter_resume_op(Trix *trx) {
    trx->require_op_count(1);

    auto forced_obj = *trx->m_op_ptr;
    auto pred_obj = *trx->m_exec_ptr--;

    if (!forced_obj.is_null()) {
        // Push pred and forced lazy-seq, call lazy_filter_op
        trx->require_op_capacity(1);

        *++trx->m_op_ptr = pred_obj;
        lazy_filter_impl(trx);
    }
}

// lazy-filter-not: lazy pred -- lazy
// throws: execstack-overflow, limit-check, opstack-underflow, type-check, vm-full
static void lazy_filter_not_op(Trix *trx) {
    trx->verify_operands(VerifyCallable, VerifyLazySeq);

    auto pred_obj = *trx->m_op_ptr;
    auto lazy_obj = *(trx->m_op_ptr - 1);

    if (lazy_obj.is_null()) {
        --trx->m_op_ptr;
    } else {
        // Build negated pred: compose(pred, { not })
        *trx->m_op_ptr = Object::make_compose_pair(trx, pred_obj, Object::make_operator(SystemName::Not));
        lazy_filter_impl(trx);
    }
}

// lazy-take: lazy n -- lazy
// throws: limit-check, opstack-underflow, type-check, vm-full
static void lazy_take_op(Trix *trx) {
    trx->verify_operands(VerifyInteger, VerifyLazySeq);

    auto n_ptr = trx->m_op_ptr;
    auto lazy_ptr = (n_ptr - 1);

    auto n = n_ptr->integer_value();
    if ((n <= 0) || lazy_ptr->is_null()) {
        --trx->m_op_ptr;
        *trx->m_op_ptr = Object::make_null();
    } else {
        auto [head_obj, source_tail_obj] = lazy_uncons(trx, *lazy_ptr);
        // head_obj is a fresh clone reachable from no root; root it across the
        // tail-thunk build and make_lazy.  source_tail stays rooted via *lazy_ptr;
        // roll the (n>1) continue-proc chain through *r.
        trx->require_gc_root_capacity(2);  // head + rolling tail-thunk/chain

        *++trx->m_gc_roots_ptr = head_obj;
        auto *head = trx->m_gc_roots_ptr;
        *++trx->m_gc_roots_ptr = Object::make_null();  // rolling slot (null-init: safe to walk)
        auto *r = trx->m_gc_roots_ptr;
        if (n == 1) {
            // Last element: tail is pre-evaluated null thunk
            *r = Object::make_pre_evaluated_thunk(trx);
        } else {
            // Tail thunk: thunk(curry(n-1, curry(source_tail, compose(Force, compose(Exch, LazyTake)))))
            *r = Object::make_compose_pair(
                    trx, Object::make_operator(SystemName::Exch), Object::make_operator(SystemName::LazyTake));
            *r = Object::make_compose_pair(trx, Object::make_operator(SystemName::Force), *r);
            *r = Object::make_curry_pair(trx, source_tail_obj, *r);              // inner = curry(source_tail, force_chain)
            *r = Object::make_curry_pair(trx, Object::make_integer(n - 1), *r);  // tail_proc = curry(n-1, inner)
            *r = Object::make_lazy_thunk(trx, *r);                               // tail_thunk
        }
        --trx->m_op_ptr;
        *trx->m_op_ptr = Object::make_lazy(trx, *head, *r);

        trx->reset_gc_root(2);  // clear operator-scoped roots
    }
}

// lazy-drop: lazy n -- lazy
// Eagerly forces n thunks, returns remaining lazy-seq.
// throws: execstack-overflow, opstack-underflow, type-check, undefined-result, vm-full
static void lazy_drop_op(Trix *trx) {
    trx->verify_operands(VerifyInteger, VerifyAny);

    auto n_ptr = trx->m_op_ptr;
    auto n = n_ptr->integer_value();
    if (n <= 0) {
        // Drop nothing: return lazy-seq as-is
        --trx->m_op_ptr;
    } else {
        auto lazy_ptr = (n_ptr - 1);
        auto lazy_obj = *lazy_ptr;

        if (lazy_obj.is_null()) {
            --trx->m_op_ptr;
        } else {
            trx->verify_operands(VerifyInteger, VerifyLazySeqNonempty);

            // Get tail thunk and start the drop loop
            auto source_tail_obj = lazy_obj.array_objects(trx)[Object::LazyTailThunkIndex].make_clone(trx);

            if (n == 1) {
                // Drop 1: just force the tail
            } else {
                // Set up loop: push @lazy-drop-step + counter on exec stack, force tail
                trx->require_exec_capacity(2);

                *++trx->m_exec_ptr = Object::make_integer(n - 1);
                *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atLazyDropStep);
            }
            --trx->m_op_ptr;
            *trx->m_op_ptr = source_tail_obj;
            force_op(trx);
        }
    }
}

// @lazy-drop-step: after forcing, check result and continue dropping.
// Exec stack: [n]  Op stack: [..., forced_result]
static void at_lazy_drop_step_op(Trix *trx) {
    trx->verify_operands(VerifyLazySeq);

    auto forced_obj = *trx->m_op_ptr;

    if (forced_obj.is_null()) {
        // Source exhausted
        --trx->m_exec_ptr;
    } else {
        auto n_ptr = trx->m_exec_ptr;
        auto n = n_ptr->integer_value();

        if (n <= 1) {
            // Done dropping: force last tail
            --trx->m_exec_ptr;
        } else {
            // Continue dropping
            trx->require_exec_capacity(1);

            n_ptr->update_integer(n - 1);
            *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atLazyDropStep);
        }
        auto elem_data = forced_obj.array_objects(trx);
        *trx->m_op_ptr = elem_data[Object::LazyTailThunkIndex].make_clone(trx);
        force_op(trx);
    }
}

// lazy-take-while implementation
static void lazy_take_while_impl(Trix *trx) {
    auto pred_obj = *trx->m_op_ptr;
    auto lazy_obj = *(trx->m_op_ptr - 1);

    if (lazy_obj.is_null()) {
        --trx->m_op_ptr;
    } else {
        trx->require_exec_capacity(4);

        auto [head_obj, source_tail_obj] = lazy_uncons(trx, lazy_obj);

        // Test pred on head; @lazy-take-while-test handles result
        *++trx->m_exec_ptr = source_tail_obj;
        *++trx->m_exec_ptr = pred_obj;
        // head_obj is a fresh clone (reachable from no root); root it across its
        // own clone allocation (the clone lands on the GC-walked exec stack) and
        // until it is placed on the operand stack below.
        trx->gc_root_push_oneoff(head_obj);
        *++trx->m_exec_ptr = head_obj.make_clone(trx);
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atLazyTakeWhileTest);

        --trx->m_op_ptr;
        *trx->m_op_ptr = head_obj;

        trx->gc_root_pop_oneoff();  // clear operator-scoped roots
        execute_lazy_proc(trx, pred_obj.make_clone(trx));
    }
}

// lazy-take-while: lazy pred -- lazy
// throws: execstack-overflow, limit-check, opstack-underflow, type-check, vm-full
static void lazy_take_while_op(Trix *trx) {
    trx->verify_operands(VerifyCallable, VerifyLazySeq);
    lazy_take_while_impl(trx);
}

// @lazy-take-while-impl: internal recursive entry point
static void at_lazy_take_while_impl_op(Trix *trx) {
    trx->require_op_count(2);
    lazy_take_while_impl(trx);
}

// @lazy-take-while-test: check pred result.
// Exec stack: [source_tail, pred, head]  Op stack: [..., bool]
static void at_lazy_take_while_test_op(Trix *trx) {
    trx->verify_operands(VerifyBoolean);

    auto result = trx->m_op_ptr->boolean_value();

    // Read saved state WITHOUT popping so head/pred/source_tail stay rooted on
    // the exec stack across the build branch's allocations (see at_lazy_filter_test).
    auto head_obj = *trx->m_exec_ptr;
    auto pred_obj = *(trx->m_exec_ptr - 1);
    auto source_tail_obj = *(trx->m_exec_ptr - 2);

    if (!result) {
        // Pred false: end of sequence
        trx->m_exec_ptr -= 3;
        head_obj.maybe_free_extvalue(trx);
        source_tail_obj.maybe_free_extvalue(trx);
        *trx->m_op_ptr = Object::make_null();
    } else {
        // Build node make_lazy(head, thunk(curry(pred, curry(source_tail,
        // compose(Force, compose(Exch, @lazy-take-while-impl)))))).  head / pred /
        // source_tail stay rooted on the exec stack; roll the chain through *r.
        trx->require_gc_root_capacity(1);

        *++trx->m_gc_roots_ptr = Object::make_compose_pair(
                trx, Object::make_operator(SystemName::Exch), Object::make_control_operator(SystemName::atLazyTakeWhileImpl));
        auto *r = trx->m_gc_roots_ptr;
        *r = Object::make_compose_pair(trx, Object::make_operator(SystemName::Force), *r);
        *r = Object::make_curry_pair(trx, source_tail_obj, *r);  // inner = curry(source_tail, force_chain)
        *r = Object::make_curry_pair(trx, pred_obj, *r);         // tail_proc = curry(pred, inner)
        *r = Object::make_lazy_thunk(trx, *r);                   // tail_thunk
        *trx->m_op_ptr = Object::make_lazy(trx, head_obj, *r);
        trx->m_exec_ptr -= 3;

        trx->reset_gc_root(1);  // clear operator-scoped roots
    }
}

// lazy-drop-while: lazy pred -- lazy
// Eagerly drops elements while pred is true.
// throws: execstack-overflow, limit-check, opstack-underflow, type-check, vm-full
static void lazy_drop_while_op(Trix *trx) {
    trx->verify_operands(VerifyCallable, VerifyLazySeq);

    auto pred_obj = *trx->m_op_ptr;
    auto lazy_obj = *(trx->m_op_ptr - 1);

    if (lazy_obj.is_null()) {
        --trx->m_op_ptr;
    } else {
        trx->require_exec_capacity(4);

        auto [head_obj, source_tail_obj] = lazy_uncons(trx, lazy_obj);

        // Test pred on head
        *++trx->m_exec_ptr = source_tail_obj;
        *++trx->m_exec_ptr = pred_obj;
        // head_obj is a fresh uncons clone reachable from no root; root it across its
        // own re-clone (allocates for an ExtValue element -> GC under ${...}) and
        // until it lands on the operand stack below.  Mirror lazy_filter_impl /
        // lazy_take_while_impl.
        trx->gc_root_push_oneoff(head_obj);
        *++trx->m_exec_ptr = head_obj.make_clone(trx);
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atLazyDropWhileTest);

        --trx->m_op_ptr;
        *trx->m_op_ptr = head_obj;

        trx->gc_root_pop_oneoff();
        execute_lazy_proc(trx, pred_obj.make_clone(trx));
    }
}

// @lazy-drop-while-test: check pred. If true, continue dropping. If false, yield remaining.
// Exec stack: [source_tail, pred, head]  Op stack: [..., bool]
static void at_lazy_drop_while_test_op(Trix *trx) {
    trx->verify_operands(VerifyBoolean);

    auto result = trx->m_op_ptr->boolean_value();

    auto head_obj = *trx->m_exec_ptr--;
    auto pred_obj = *trx->m_exec_ptr--;
    auto source_tail_obj = *trx->m_exec_ptr--;

    if (result) {
        // Pred true: drop this element, force source_tail, try next
        trx->require_exec_capacity(2);

        head_obj.maybe_free_extvalue(trx);
        *++trx->m_exec_ptr = pred_obj;
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atLazyDropWhileResume);

        *trx->m_op_ptr = source_tail_obj;
        force_op(trx);
    } else {
        // Pred false: return [head, source_tail] as the lazy-seq.  Both
        // head_obj and source_tail_obj were just popped off the exec stack
        // (unrooted C++ locals); make_lazy allocates the node via
        // vm_alloc_dispatch_n, which under ${...} can fire a synchronous GC
        // that would sweep the global tail thunk before make_lazy stores it.
        // Root both across the alloc.
        trx->require_gc_root_capacity(2);

        *++trx->m_gc_roots_ptr = head_obj;
        *++trx->m_gc_roots_ptr = source_tail_obj;
        *trx->m_op_ptr = Object::make_lazy(trx, head_obj, source_tail_obj);

        trx->reset_gc_root(2);
    }
}

// @lazy-drop-while-resume: after forcing, restart drop-while search.
// Exec stack: [pred]  Op stack: [..., forced_result]
static void at_lazy_drop_while_resume_op(Trix *trx) {
    trx->require_op_count(1);

    auto forced_obj = *trx->m_op_ptr;
    auto pred_obj = *trx->m_exec_ptr--;

    if (forced_obj.is_null()) {
        *trx->m_op_ptr = Object::make_null();
    } else {
        // Restart lazy-drop-while with the forced lazy-seq
        trx->require_op_capacity(1);

        *++trx->m_op_ptr = pred_obj;
        lazy_drop_while_op(trx);
    }
}

// lazy-seq: array -- lazy
// Convert eager array to lazy sequence.
// throws: limit-check, opstack-underflow, type-check, vm-full
static void lazy_seq_op(Trix *trx) {
    trx->verify_operands(VerifyArray);

    auto arr_obj = *trx->m_op_ptr;
    auto length = arr_obj.arrays_length();
    if (length == 0) {
        *trx->m_op_ptr = Object::make_null();
    } else {
        // Build the lazy-seq right-to-left on the GC-root stack.  Each make_clone /
        // make_pre_evaluated_thunk / make_lazy allocates via vm_alloc_dispatch_n,
        // which under ${...} can fire a synchronous vm_global_gc, so both the
        // accumulator chain AND each freshly-cloned element stay rooted across the
        // allocs.  arr_obj stays rooted via its operand slot -- elem_data points
        // into its payload, so it must NOT be overwritten until the loop is done.
        // seq_root morphs in place each iteration: old-seq -> tail-thunk -> new node.
        auto elem_data = arr_obj.array_objects(trx);
        trx->require_gc_root_capacity(2);

        *++trx->m_gc_roots_ptr = Object::make_null();
        auto *seq_root = trx->m_gc_roots_ptr;
        *++trx->m_gc_roots_ptr = Object::make_null();
        auto *elem_root = trx->m_gc_roots_ptr;
        for (integer_t i = static_cast<integer_t>(length) - 1; i >= 0; --i) {
            *elem_root = elem_data[i].make_clone(trx);
            *seq_root = Object::make_pre_evaluated_thunk(trx, *seq_root);  // wrap current seq as tail thunk
            *seq_root = Object::make_lazy(trx, *elem_root, *seq_root);     // node = [elem, tail_thunk]
        }
        *trx->m_op_ptr = *seq_root;

        trx->reset_gc_root(2);  // clear operator-scoped roots
    }
}

// lazy-range: start stop step -- lazy
// Finite sequence: [start, stop) with step.
// throws: limit-check, opstack-underflow, range-check, type-check, vm-full
static void lazy_range_op(Trix *trx) {
    trx->verify_operands(VerifyInteger, VerifyInteger, VerifyInteger);

    auto step_ptr = trx->m_op_ptr;
    auto stop_ptr = (step_ptr - 1);
    auto start_ptr = (step_ptr - 2);

    auto step = step_ptr->integer_value();
    if (step == 0) {
        trx->error(Error::RangeCheck, "lazy-range: step must not be zero");
    } else {
        auto start = start_ptr->integer_value();
        auto stop = stop_ptr->integer_value();

        trx->m_op_ptr -= 2;

        // Check if range is empty
        if (((step > 0) && (start >= stop)) || ((step < 0) && (start <= stop))) {
            *trx->m_op_ptr = Object::make_null();
        } else {
            // Tail thunk: make_lazy(start, thunk(curry(start+step, curry(stop,
            // curry(step, op(LazyRange)))))).  All chain values are non-heap
            // integers/operators; only the in-flight chain links need rooting, so
            // roll them through *r.
            auto sum = static_cast<uinteger_t>(start) + static_cast<uinteger_t>(step);
            trx->require_gc_root_capacity(1);

            *++trx->m_gc_roots_ptr =
                    Object::make_curry_pair(trx, Object::make_integer(step), Object::make_operator(SystemName::LazyRange));
            auto *r = trx->m_gc_roots_ptr;
            *r = Object::make_curry_pair(trx, Object::make_integer(stop), *r);                         // inner1
            *r = Object::make_curry_pair(trx, Object::make_integer(static_cast<integer_t>(sum)), *r);  // tail_proc
            *r = Object::make_lazy_thunk(trx, *r);                                                     // tail_thunk
            *trx->m_op_ptr = Object::make_lazy(trx, Object::make_integer(start), *r);

            trx->reset_gc_root(1);  // clear operator-scoped roots
        }
    }
}

// lazy-iterate implementation
static void lazy_iterate_impl(Trix *trx) {
    auto proc_obj = *trx->m_op_ptr;
    auto seed_obj = *(trx->m_op_ptr - 1);

    // Tail thunk: make_lazy(seed, thunk(curry(proc_clone, curry(seed_clone,
    // op(@lazy-iterate-build))))).  seed_clone / proc_clone are fresh clones
    // reachable from no root; root them on the GC-root stack across the chain
    // allocs and roll the chain through *r.  seed_obj / proc_obj stay rooted via
    // their operand slots.
    trx->require_gc_root_capacity(3);  // seed_clone, proc_clone, rolling chain

    *++trx->m_gc_roots_ptr = seed_obj.make_clone(trx);
    auto *sc = trx->m_gc_roots_ptr;
    *++trx->m_gc_roots_ptr = proc_obj.make_clone(trx);
    auto *pc = trx->m_gc_roots_ptr;
    *++trx->m_gc_roots_ptr =
            Object::make_curry_pair(trx, *sc, Object::make_control_operator(SystemName::atLazyIterateBuild));  // inner
    auto *r = trx->m_gc_roots_ptr;
    *r = Object::make_curry_pair(trx, *pc, *r);  // tail_proc = curry(proc_clone, inner)
    *r = Object::make_lazy_thunk(trx, *r);       // tail_thunk

    --trx->m_op_ptr;
    *trx->m_op_ptr = Object::make_lazy(trx, seed_obj, *r);

    trx->reset_gc_root(3);  // clear operator-scoped roots
}

// lazy-iterate: seed proc -- lazy
// Infinite: seed, f(seed), f(f(seed)), ...
// throws: limit-check, opstack-underflow, type-check, vm-full
static void lazy_iterate_op(Trix *trx) {
    trx->verify_operands(VerifyCallable, VerifyAny);
    lazy_iterate_impl(trx);
}

// @lazy-iterate-build: two-phase control op for lazy-iterate tail thunk.
// Phase 1 (from curry chain): Op = [..., proc, seed], exec top != mark
//   Apply proc to seed, push continuation for Phase 2.
// Phase 2 (after proc): Op = [..., new_seed], exec top = mark
//   Collect new_seed, push proc, call lazy_iterate_op.
static void at_lazy_iterate_build_op(Trix *trx) {
    if (trx->m_exec_ptr->is_mark()) {
        // Phase 2: after proc(seed) completed
        trx->require_op_count(1);

        trx->m_exec_ptr--;  // pop mark
        auto proc_obj = *trx->m_exec_ptr--;

        // Call lazy_iterate_impl(new_seed, proc) -- proc is literal from curry
        *++trx->m_op_ptr = proc_obj;
        lazy_iterate_impl(trx);
    } else {
        // Phase 1: from curry chain
        trx->require_op_count(2);
        trx->require_exec_capacity(3);

        auto seed_obj = *trx->m_op_ptr--;
        auto proc_obj = *trx->m_op_ptr;

        // Save proc on exec stack with mark + continuation
        *++trx->m_exec_ptr = proc_obj.make_clone(trx);
        *++trx->m_exec_ptr = Object::make_mark();
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atLazyIterateBuild);

        // Push seed and execute proc
        *trx->m_op_ptr = seed_obj;
        execute_lazy_proc(trx, proc_obj);
    }
}

// lazy-cycle: array -- lazy
// Infinite cycling through array elements.
// Eagerly builds one pass of the array as a lazy-seq chain, with the last
// element's tail thunk calling lazy-cycle(arr) again to restart the cycle.
// throws: limit-check, opstack-underflow, type-check, vm-full
static void lazy_cycle_op(Trix *trx) {
    trx->verify_operands(VerifyArray);

    auto arr_obj = *trx->m_op_ptr;
    auto length = arr_obj.arrays_length();

    if (length == 0) {
        *trx->m_op_ptr = Object::make_null();
    } else {
        // Same GC-rooting discipline as lazy_seq_op: the restart thunk, the
        // accumulator chain, and each freshly-cloned element all allocate via
        // vm_alloc_dispatch_n (GC-capable under ${...}) and must stay rooted.
        // arr_obj remains rooted at its operand slot (elem_data points into its
        // payload -- do not overwrite it until the loop is done); the accumulator
        // and current element get GC-root slots.  seq_root morphs in place.
        auto elem_data = arr_obj.array_objects(trx);
        trx->require_gc_root_capacity(2);

        *++trx->m_gc_roots_ptr = Object::make_null();  // accumulator (rolling)
        auto *seq_root = trx->m_gc_roots_ptr;
        *++trx->m_gc_roots_ptr = elem_data[length - 1].make_clone(trx);  // last element
        auto *elem_root = trx->m_gc_roots_ptr;

        // Last element's tail is the restart thunk: thunk(curry(arr, op(LazyCycle)))
        // -- forces to lazy-cycle(arr) again.  arr is an array, so make_clone is a
        // no-op alias of the rooted arr_obj.
        *seq_root = Object::make_curry_pair(
                trx, arr_obj.make_clone(trx), Object::make_operator(SystemName::LazyCycle));  // restart_proc
        *seq_root = Object::make_lazy_thunk(trx, *seq_root);                                  // restart_thunk
        *seq_root = Object::make_lazy(trx, *elem_root, *seq_root);                            // last node

        // Build remaining elements from last-but-one to first.
        for (integer_t i = static_cast<integer_t>(length) - 2; i >= 0; --i) {
            *elem_root = elem_data[i].make_clone(trx);
            *seq_root = Object::make_pre_evaluated_thunk(trx, *seq_root);  // wrap current seq as tail thunk
            *seq_root = Object::make_lazy(trx, *elem_root, *seq_root);     // node = [elem, tail_thunk]
        }

        *trx->m_op_ptr = *seq_root;

        trx->reset_gc_root(2);  // clear operator-scoped roots
    }
}

// lazy-unfold implementation
static void lazy_unfold_impl(Trix *trx) {
    trx->require_exec_capacity(2);

    auto proc_obj = *trx->m_op_ptr;
    auto seed_obj = *(trx->m_op_ptr - 1);

    // Apply proc to seed; @lazy-unfold-build handles result
    *++trx->m_exec_ptr = proc_obj;
    *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atLazyUnfoldBuild);

    --trx->m_op_ptr;
    *trx->m_op_ptr = seed_obj;
    execute_lazy_proc(trx, proc_obj.make_clone(trx));
}

// lazy-unfold: seed proc -- lazy
// General generator. proc: seed -> [val, new_seed] or null to stop.
// throws: execstack-overflow, opstack-underflow, type-check, vm-full
static void lazy_unfold_op(Trix *trx) {
    trx->verify_operands(VerifyCallable, VerifyAny);
    lazy_unfold_impl(trx);
}

// @lazy-unfold-impl: internal recursive entry point
static void at_lazy_unfold_impl_op(Trix *trx) {
    trx->require_op_count(2);
    lazy_unfold_impl(trx);
}

// @lazy-unfold-build: after proc(seed), unpack result and build node.
// Exec stack: [proc]  Op stack: [..., result]
// result is [val, new_seed] or null.
static void at_lazy_unfold_build_op(Trix *trx) {
    trx->require_op_count(1);

    auto result_obj = *trx->m_op_ptr;
    auto proc_obj = *trx->m_exec_ptr--;

    if (result_obj.is_null()) {
        *trx->m_op_ptr = Object::make_null();
    } else if (!result_obj.is_array() || (result_obj.arrays_length() != 2)) {
        trx->error(Error::TypeCheck, "lazy-unfold: proc must return [val, new-seed] or null");
    } else {
        // Build node make_lazy(val, thunk(curry(new_seed, curry(proc,
        // op(@lazy-unfold-impl))))).  proc_obj was popped off the exec stack (now
        // unrooted); val (head) and new_seed are fresh clones.  Root all three on
        // the GC-root stack across the clone + curry allocs and roll the chain
        // through *r.  result_obj stays rooted via its operand slot, keeping
        // elem_data valid (the GC is non-moving).
        auto elem_data = result_obj.array_objects(trx);
        trx->require_gc_root_capacity(4);  // proc, val(head), new_seed, rolling chain

        *++trx->m_gc_roots_ptr = proc_obj;
        auto *proc = trx->m_gc_roots_ptr;
        *++trx->m_gc_roots_ptr = elem_data[0].make_clone(trx);  // val (head)
        auto *val = trx->m_gc_roots_ptr;
        *++trx->m_gc_roots_ptr = elem_data[1].make_clone(trx);  // new_seed
        auto *seed = trx->m_gc_roots_ptr;
        *++trx->m_gc_roots_ptr =
                Object::make_curry_pair(trx, *proc, Object::make_control_operator(SystemName::atLazyUnfoldImpl));  // inner
        auto *r = trx->m_gc_roots_ptr;
        *r = Object::make_curry_pair(trx, *seed, *r);  // tail_proc = curry(new_seed, inner)
        *r = Object::make_lazy_thunk(trx, *r);         // tail_thunk
        *trx->m_op_ptr = Object::make_lazy(trx, *val, *r);

        trx->reset_gc_root(4);  // clear operator-scoped roots
    }
}

// lazy-filter-map implementation
static void lazy_filter_map_impl(Trix *trx) {
    auto proc_obj = *trx->m_op_ptr;
    auto lazy_obj = *(trx->m_op_ptr - 1);

    if (lazy_obj.is_null()) {
        --trx->m_op_ptr;
        *trx->m_op_ptr = Object::make_null();
    } else {
        trx->require_exec_capacity(3);

        auto [head_obj, source_tail_obj] = lazy_uncons(trx, lazy_obj);

        // Apply proc to head; @lazy-filter-map-test checks result
        *++trx->m_exec_ptr = source_tail_obj;
        *++trx->m_exec_ptr = proc_obj;
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atLazyFilterMapTest);

        --trx->m_op_ptr;
        *trx->m_op_ptr = head_obj;
        execute_lazy_proc(trx, proc_obj.make_clone(trx));
    }
}

// lazy-filter-map: lazy proc -- lazy
// Map + filter: proc returns null to skip, value to keep.
// throws: execstack-overflow, opstack-underflow, type-check, vm-full
static void lazy_filter_map_op(Trix *trx) {
    trx->verify_operands(VerifyCallable, VerifyLazySeq);
    lazy_filter_map_impl(trx);
}

// @lazy-filter-map-impl: internal recursive entry point
static void at_lazy_filter_map_impl_op(Trix *trx) {
    trx->require_op_count(2);
    lazy_filter_map_impl(trx);
}

// @lazy-filter-map-test: check proc result. null=skip, value=keep.
// Exec stack: [source_tail, proc]  Op stack: [..., result]
static void at_lazy_filter_map_test_op(Trix *trx) {
    trx->require_op_count(1);

    auto result_obj = *trx->m_op_ptr;
    auto proc_obj = *trx->m_exec_ptr--;
    auto source_tail_obj = *trx->m_exec_ptr--;

    if (result_obj.is_null()) {
        // Skip: force source_tail and try next
        trx->require_exec_capacity(2);

        *++trx->m_exec_ptr = proc_obj;
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atLazyFilterMapResume);

        *trx->m_op_ptr = source_tail_obj;
        force_op(trx);
    } else {
        // Keep: build node make_lazy(result, thunk(curry(proc, curry(source_tail,
        // compose(Force, compose(Exch, @lazy-filter-map-impl)))))).  proc_obj +
        // source_tail_obj were popped off the exec stack (now unrooted); root them
        // on the GC-root stack and roll the chain through *r.  result_obj (the head)
        // stays rooted via its operand slot.
        trx->require_gc_root_capacity(3);  // proc, source_tail, rolling chain

        *++trx->m_gc_roots_ptr = proc_obj;
        auto *proc = trx->m_gc_roots_ptr;
        *++trx->m_gc_roots_ptr = source_tail_obj;
        auto *st = trx->m_gc_roots_ptr;
        *++trx->m_gc_roots_ptr = Object::make_compose_pair(
                trx, Object::make_operator(SystemName::Exch), Object::make_control_operator(SystemName::atLazyFilterMapImpl));
        auto *r = trx->m_gc_roots_ptr;
        *r = Object::make_compose_pair(trx, Object::make_operator(SystemName::Force), *r);
        *r = Object::make_curry_pair(trx, *st, *r);    // inner = curry(source_tail, force_chain)
        *r = Object::make_curry_pair(trx, *proc, *r);  // tail_proc = curry(proc, inner)
        *r = Object::make_lazy_thunk(trx, *r);         // tail_thunk
        *trx->m_op_ptr = Object::make_lazy(trx, result_obj, *r);

        trx->reset_gc_root(3);  // clear operator-scoped roots
    }
}

// @lazy-filter-map-resume: after forcing source tail, restart filter-map.
// Exec stack: [proc]  Op stack: [..., forced_result]
static void at_lazy_filter_map_resume_op(Trix *trx) {
    trx->require_op_count(1);

    auto forced_obj = *trx->m_op_ptr;
    auto proc_obj = *trx->m_exec_ptr--;

    if (!forced_obj.is_null()) {
        trx->require_op_capacity(1);

        *++trx->m_op_ptr = proc_obj;
        lazy_filter_map_impl(trx);
    }
}

// lazy-flat-map implementation
static void lazy_flat_map_impl(Trix *trx) {
    auto proc_obj = *trx->m_op_ptr;
    auto lazy_obj = *(trx->m_op_ptr - 1);

    if (lazy_obj.is_null()) {
        --trx->m_op_ptr;
        *trx->m_op_ptr = Object::make_null();
    } else {
        trx->require_exec_capacity(3);

        auto [head_obj, source_tail_obj] = lazy_uncons(trx, lazy_obj);

        // Apply proc to head -> inner lazy-seq
        // @lazy-flat-map-build chains inner with remaining
        *++trx->m_exec_ptr = source_tail_obj;
        *++trx->m_exec_ptr = proc_obj;
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atLazyFlatMapBuild);

        --trx->m_op_ptr;
        *trx->m_op_ptr = head_obj;
        execute_lazy_proc(trx, proc_obj.make_clone(trx));
    }
}

// lazy-flat-map: lazy proc -- lazy
// Map to lazy-seqs, concatenate results.
// throws: execstack-overflow, opstack-underflow, type-check, vm-full
static void lazy_flat_map_op(Trix *trx) {
    trx->verify_operands(VerifyCallable, VerifyLazySeq);
    lazy_flat_map_impl(trx);
}

// @lazy-flat-map-impl: internal recursive entry point
static void at_lazy_flat_map_impl_op(Trix *trx) {
    trx->require_op_count(2);
    lazy_flat_map_impl(trx);
}

// @lazy-flat-map-build: chain inner lazy-seq with rest of outer.
// Exec stack: [source_tail, proc]  Op stack: [..., inner_lazy]
static void at_lazy_flat_map_build_op(Trix *trx) {
    trx->require_op_count(1);

    auto inner = *trx->m_op_ptr;
    auto proc_obj = *trx->m_exec_ptr--;
    auto source_tail = *trx->m_exec_ptr--;

    // Build outer_rest_thunk: thunk(curry(proc, curry(source_tail, compose(Force,
    // compose(Exch, @lazy-flat-map-impl)))))  -- forces source_tail and re-runs
    // lazy-flat-map.  proc_obj + source_tail were popped off the exec stack (now
    // unrooted); root them on the GC-root stack and roll the chain through *ort,
    // which then HOLDS outer_rest_thunk for the second build below.  `inner` stays
    // rooted via its operand slot.
    trx->require_gc_root_capacity(3);  // proc, source_tail, rolling chain -> outer_rest_thunk

    *++trx->m_gc_roots_ptr = proc_obj;
    auto *proc = trx->m_gc_roots_ptr;
    *++trx->m_gc_roots_ptr = source_tail;
    auto *st = trx->m_gc_roots_ptr;
    *++trx->m_gc_roots_ptr = Object::make_compose_pair(
            trx, Object::make_operator(SystemName::Exch), Object::make_control_operator(SystemName::atLazyFlatMapImpl));
    auto *ort = trx->m_gc_roots_ptr;
    *ort = Object::make_compose_pair(trx, Object::make_operator(SystemName::Force), *ort);
    *ort = Object::make_curry_pair(trx, *st, *ort);    // inner_curry = curry(source_tail, force_chain)
    *ort = Object::make_curry_pair(trx, *proc, *ort);  // outer_proc = curry(proc, inner_curry)
    *ort = Object::make_lazy_thunk(trx, *ort);         // outer_rest_thunk

    if (inner.is_null()) {
        // Inner is empty: skip to outer rest
        *trx->m_op_ptr = *ort;

        trx->reset_gc_root(3);  // clear operator-scoped roots
        force_op(trx);
    } else {
        verify_lazy_seq_nonempty(trx, inner);

        // Node: make_lazy(inner_head, thunk(curry(outer_rest_thunk, curry(inner_tail,
        // @lazy-flat-map-resume)))).  inner_head / inner_tail are fresh clones;
        // outer_rest_thunk is already rooted in *ort.  Root the clones and roll the
        // chain through *r.  `inner` stays rooted via its operand slot (elem
        // accessors point into it; GC is non-moving).
        trx->require_gc_root_capacity_more(3);  // 2nd guard: 3 MORE on top of the 3 above

        auto inner_elems = inner.array_objects(trx);
        *++trx->m_gc_roots_ptr = inner_elems[Object::LazyHeadIndex].make_clone(trx);  // inner_head
        auto *ih = trx->m_gc_roots_ptr;
        *++trx->m_gc_roots_ptr = inner_elems[Object::LazyTailThunkIndex].make_clone(trx);  // inner_tail
        auto *it = trx->m_gc_roots_ptr;
        *++trx->m_gc_roots_ptr =
                Object::make_curry_pair(trx, *it, Object::make_control_operator(SystemName::atLazyFlatMapResume));  // resume_inner
        auto *r = trx->m_gc_roots_ptr;
        *r = Object::make_curry_pair(trx, *ort, *r);  // resume_proc = curry(outer_rest_thunk, resume_inner)
        *r = Object::make_lazy_thunk(trx, *r);        // tail thunk
        *trx->m_op_ptr = Object::make_lazy(trx, *ih, *r);

        trx->reset_gc_root(6);  // clear operator-scoped roots
    }
}

// @lazy-flat-map-resume: two-phase control op.
// Phase 1 (from curry chain, exec top not null sentinel):
//   Op = [outer_rest_thunk, inner_tail]. Force inner_tail, save state.
// Phase 2 (after force, null sentinel on exec top):
//   Op = [forced_inner]. If null, force outer_rest. If non-null, build resume node.
static void at_lazy_flat_map_resume_op(Trix *trx) {
    if (trx->m_exec_ptr->is_null()) {
        trx->require_op_count(1);

        // Phase 2: after forcing inner_tail
        trx->m_exec_ptr--;  // pop null sentinel
        auto outer_rest_thunk = *trx->m_exec_ptr--;

        auto forced_inner = *trx->m_op_ptr;

        if (forced_inner.is_null()) {
            // Inner exhausted: switch to outer rest
            *trx->m_op_ptr = outer_rest_thunk;
            force_op(trx);
        } else {
            verify_lazy_seq_nonempty(trx, forced_inner);

            // Build resume node make_lazy(inner_head, thunk(curry(outer_rest_thunk,
            // curry(inner_tail_tail, @lazy-flat-map-resume)))).  outer_rest_thunk was
            // popped off the exec stack (now unrooted); inner_head / inner_tail_tail
            // are fresh clones.  Root all three on the GC-root stack and roll the
            // chain through *r.  forced_inner stays rooted via its operand slot
            // (elem accessors point into it; GC is non-moving).
            trx->require_gc_root_capacity(4);  // outer_rest_thunk, inner_head, inner_tail_tail, rolling

            *++trx->m_gc_roots_ptr = outer_rest_thunk;
            auto *ort = trx->m_gc_roots_ptr;
            auto elem_data = forced_inner.array_objects(trx);
            *++trx->m_gc_roots_ptr = elem_data[Object::LazyHeadIndex].make_clone(trx);  // inner_head
            auto *ih = trx->m_gc_roots_ptr;
            *++trx->m_gc_roots_ptr = elem_data[Object::LazyTailThunkIndex].make_clone(trx);  // inner_tail_tail
            auto *it = trx->m_gc_roots_ptr;
            *++trx->m_gc_roots_ptr = Object::make_curry_pair(
                    trx, *it, Object::make_control_operator(SystemName::atLazyFlatMapResume));  // resume_inner
            auto *r = trx->m_gc_roots_ptr;
            *r = Object::make_curry_pair(trx, *ort, *r);  // resume_proc = curry(outer_rest_thunk, resume_inner)
            *r = Object::make_lazy_thunk(trx, *r);        // tail thunk
            *trx->m_op_ptr = Object::make_lazy(trx, *ih, *r);

            trx->reset_gc_root(4);  // clear operator-scoped roots
        }
    } else {
        // Phase 1: from curry chain. Op = [outer_rest_thunk, inner_tail]
        trx->require_op_count(2);
        trx->require_exec_capacity(3);

        auto inner_tail = *trx->m_op_ptr--;
        auto outer_rest_thunk = *trx->m_op_ptr;

        // Save state, push continuation, force inner_tail
        *++trx->m_exec_ptr = outer_rest_thunk;
        *++trx->m_exec_ptr = Object::make_null();  // Phase 2 sentinel
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atLazyFlatMapResume);

        *trx->m_op_ptr = inner_tail;

        force_op(trx);
    }
}

// lazy-flatten: lazy -- lazy
// Flatten lazy-seq of lazy-seqs.
// throws: execstack-overflow, opstack-overflow, opstack-underflow, type-check, vm-full
static void lazy_flatten_op(Trix *trx) {
    trx->verify_operands(VerifyLazySeq);
    auto lazy_obj = *trx->m_op_ptr;

    if (lazy_obj.is_null()) {
        // already null on stack
    } else {
        trx->require_op_capacity(1);

        // flat-map with identity: each element is already a lazy-seq
        // Reuse: lazy-flat-map with a no-op proc (identity)
        // Push identity proc: { } (empty executable array = no-op, leaves value on stack).
        // Allocate region-aware so the identity tracks the flat-map chain's region
        // (it gets cloned into globally-allocated curries inside ${...}).
        auto [_, noop_offset] = trx->vm_alloc_dispatch_n<Object>(0, Trix::ChunkKind::Array);
        auto identity = Object::make_array(noop_offset, 0, Object::ExecutableAttrib);
        *++trx->m_op_ptr = identity;
        lazy_flat_map_op(trx);
    }
}

// lazy-map-indexed: lazy proc -- lazy
// proc receives (element index), returns mapped value.
// throws: execstack-overflow, limit-check, opstack-overflow, opstack-underflow, type-check, vm-full
static void lazy_map_indexed_op(Trix *trx) {
    trx->verify_operands(VerifyCallable, VerifyLazySeq);

    auto proc_obj = *trx->m_op_ptr;
    auto lazy_obj = *(trx->m_op_ptr - 1);

    if (lazy_obj.is_null()) {
        --trx->m_op_ptr;
        *trx->m_op_ptr = Object::make_null();
    } else {
        trx->m_op_ptr -= 2;  // clean up caller's op stack
        lazy_map_indexed_internal(trx, lazy_obj, proc_obj, 0);
    }
}

// Internal: set up async proc application for map-indexed.
// Caller must clean op stack before calling.
static void lazy_map_indexed_internal(Trix *trx, Object lazy, Object proc, integer_t index) {
    verify_lazy_seq_nonempty(trx, lazy);
    trx->require_op_capacity(2);
    trx->require_exec_capacity(5);

    // lazy + proc arrive as already-popped locals from both callers
    // (lazy_map_indexed_op pops at -= 2; @lazy-map-indexed-build Phase C pops them
    // off exec).  Root them across lazy_uncons, whose ExtValue head-clone fires a
    // GC under ${...} that would otherwise sweep the source seq / proc.
    trx->require_gc_root_capacity(2);

    *++trx->m_gc_roots_ptr = lazy;
    *++trx->m_gc_roots_ptr = proc;
    auto [head_obj, source_tail_obj] = lazy_uncons(trx, lazy);

    trx->reset_gc_root(2);

    // Exec: [source_tail, proc, index, mark, @build]
    // After proc: @build sees mark sentinel -> Phase 2
    *++trx->m_exec_ptr = source_tail_obj;
    *++trx->m_exec_ptr = proc;
    *++trx->m_exec_ptr = Object::make_integer(index);
    *++trx->m_exec_ptr = Object::make_mark();
    *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atLazyMapIndexedBuild);

    *++trx->m_op_ptr = head_obj;
    *++trx->m_op_ptr = Object::make_integer(index);
    execute_lazy_proc(trx, proc.make_clone(trx));
}

// @lazy-map-indexed-build: three-phase control op.
// Phase 1 (from curry chain, exec top neither mark nor null):
//   Op = [proc, next_index, source_tail]. Force source_tail, save state.
// Phase C (after force, null sentinel on exec top):
//   Op = [forced]. If null -> return null. Else call lazy_map_indexed_internal.
// Phase 2 (after proc, mark sentinel on exec top):
//   Op = [mapped_head]. Build node with tail thunk.
static void at_lazy_map_indexed_build_op(Trix *trx) {
    if (trx->m_exec_ptr->is_mark()) {
        // Phase 2: after proc(head, index). mapped_head on op stack.
        trx->require_op_count(1);

        auto mapped_head_obj = *trx->m_op_ptr;
        trx->m_exec_ptr--;  // pop mark
        auto index = trx->m_exec_ptr->integer_value();
        trx->m_exec_ptr--;
        auto proc_obj = *trx->m_exec_ptr--;
        auto source_tail = *trx->m_exec_ptr--;

        // Build node make_lazy(mapped_head, thunk(curry(proc, curry(int(index+1),
        // curry(source_tail, @lazy-map-indexed-build))))).  proc_obj + source_tail
        // were popped off the exec stack (now unrooted); root them on the GC-root
        // stack and roll the chain through *r.  mapped_head_obj stays rooted via its
        // operand slot.
        auto next_index = index + 1;
        trx->require_gc_root_capacity(3);  // proc, source_tail, rolling chain

        *++trx->m_gc_roots_ptr = proc_obj;
        auto *proc = trx->m_gc_roots_ptr;
        *++trx->m_gc_roots_ptr = source_tail;
        auto *st = trx->m_gc_roots_ptr;
        *++trx->m_gc_roots_ptr =
                Object::make_curry_pair(trx, *st, Object::make_control_operator(SystemName::atLazyMapIndexedBuild));  // inner2
        auto *r = trx->m_gc_roots_ptr;
        *r = Object::make_curry_pair(trx, Object::make_integer(next_index), *r);  // inner1
        *r = Object::make_curry_pair(trx, *proc, *r);                             // tail_proc
        *r = Object::make_lazy_thunk(trx, *r);                                    // tail_thunk
        *trx->m_op_ptr = Object::make_lazy(trx, mapped_head_obj, *r);

        trx->reset_gc_root(3);  // clear operator-scoped roots
    } else if (trx->m_exec_ptr->is_null()) {
        // Phase C: after forcing source_tail.
        trx->require_op_count(1);

        trx->m_exec_ptr--;  // pop null sentinel
        auto next_index = trx->m_exec_ptr->integer_value();
        trx->m_exec_ptr--;
        auto proc_obj = *trx->m_exec_ptr--;

        auto forced_obj = *trx->m_op_ptr--;

        if (forced_obj.is_null()) {
            trx->require_op_capacity(1);

            *++trx->m_op_ptr = Object::make_null();
        } else {
            // Continue with forced lazy-seq and carried index
            lazy_map_indexed_internal(trx, forced_obj, proc_obj, next_index);
        }
    } else {
        // Phase 1: from curry chain. Op = [proc, next_index, source_tail]
        trx->require_op_count(3);
        trx->require_exec_capacity(4);

        auto source_tail = *trx->m_op_ptr--;
        if (!trx->m_op_ptr->is_integer()) {
            trx->error(Error::TypeCheck, "lazy-map-indexed: expected integer index on operand stack");
        } else {
            auto next_index = trx->m_op_ptr->integer_value();
            trx->m_op_ptr--;
            auto proc_obj = *trx->m_op_ptr;

            // Save state, push continuation, force source_tail
            *++trx->m_exec_ptr = proc_obj;
            *++trx->m_exec_ptr = Object::make_integer(next_index);
            *++trx->m_exec_ptr = Object::make_null();  // Phase C sentinel
            *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atLazyMapIndexedBuild);

            *trx->m_op_ptr = source_tail;
            force_op(trx);
        }
    }
}

// lazy-scan: lazy init proc -- lazy
// Running fold: emit each accumulator state.
// throws: limit-check, opstack-underflow, type-check, vm-full
static void lazy_scan_op(Trix *trx) {
    // Validate proc callability up front like every sibling (lazy-fold etc.):
    // an unverified non-callable proc is otherwise pushed as data deep inside the
    // forced tail thunk, producing corrupt scan results far from the call site.
    // VerifyLazySeq admits null, so the is_null branch below stays valid.
    trx->verify_operands(VerifyCallable, VerifyAny, VerifyLazySeq);

    auto proc_obj = *trx->m_op_ptr;
    auto init_obj = *(trx->m_op_ptr - 1);
    auto lazy_obj = *(trx->m_op_ptr - 2);

    // First element is init. Tail produces proc(init, head), proc(proc(init,head), next), ...
    if (lazy_obj.is_null()) {
        // Just emit init as single-element lazy seq.  Root the tail thunk before
        // make_lazy allocates over it (make_lazy stores its args only after the
        // alloc that may GC).  init_obj stays rooted via its operand slot.
        trx->require_gc_root_capacity(1);

        *++trx->m_gc_roots_ptr = Object::make_pre_evaluated_thunk(trx);
        auto *r = trx->m_gc_roots_ptr;
        *r = Object::make_lazy(trx, init_obj, *r);
        trx->m_op_ptr -= 2;
        *trx->m_op_ptr = *r;

        trx->reset_gc_root(1);  // clear operator-scoped roots
    } else {
        verify_lazy_seq_nonempty(trx, lazy_obj);

        // Build node make_lazy(init, thunk(curry(proc, curry(init, curry(source,
        // @lazy-scan-build))))).  When forced, @scan-build Phase 1 receives
        // [proc, acc, source], forces source, extracts head, applies proc(acc,
        // head), builds next node.  init_clone / proc_clone are fresh clones; root
        // them on the GC-root stack and roll the chain through *r.  lazy_obj /
        // init_obj stay rooted via their operand slots.
        trx->require_gc_root_capacity(3);  // init_clone, proc_clone, rolling chain

        *++trx->m_gc_roots_ptr = init_obj.make_clone(trx);
        auto *ic = trx->m_gc_roots_ptr;
        *++trx->m_gc_roots_ptr = proc_obj.make_clone(trx);
        auto *pc = trx->m_gc_roots_ptr;
        *++trx->m_gc_roots_ptr =
                Object::make_curry_pair(trx, lazy_obj, Object::make_control_operator(SystemName::atLazyScanBuild));  // inner2
        auto *r = trx->m_gc_roots_ptr;
        *r = Object::make_curry_pair(trx, *ic, *r);  // inner1 = curry(init_clone, inner2)
        *r = Object::make_curry_pair(trx, *pc, *r);  // tail_proc = curry(proc_clone, inner1)
        *r = Object::make_lazy_thunk(trx, *r);       // tail_thunk
        *r = Object::make_lazy(trx, init_obj, *r);   // node
        trx->m_op_ptr -= 2;
        *trx->m_op_ptr = *r;

        trx->reset_gc_root(3);  // clear operator-scoped roots
    }
}

// @lazy-scan-build: three-phase control op for lazy-scan tail thunks.
// Phase 1 (from curry chain, exec top neither mark nor null):
//   Op = [proc, acc, source]. Force source, push Phase C.
// Phase C (after force, exec top = null sentinel):
//   Op = [forced]. If null -> return null. Else extract head, apply proc(acc, head), push Phase 2.
// Phase 2 (after proc, exec top = mark):
//   Op = [new_acc]. Build [new_acc, next_tail_thunk] node and return.
static void at_lazy_scan_build_op(Trix *trx) {
    if (trx->m_exec_ptr->is_mark()) {
        trx->require_op_count(1);

        // Phase 2: after proc(acc, head). new_acc on op stack.
        // Build [new_acc, continuation_thunk] node and return.
        auto new_acc = *trx->m_op_ptr;
        trx->m_exec_ptr--;  // pop mark
        auto proc_obj = *trx->m_exec_ptr--;
        auto source_tail = *trx->m_exec_ptr--;

        // Build node make_lazy(new_acc, thunk(curry(proc, curry(new_acc_clone,
        // curry(source_tail, @lazy-scan-build))))).  proc_obj + source_tail were
        // popped off the exec stack (now unrooted); new_acc_clone is a fresh clone.
        // Root all three on the GC-root stack and roll the chain through *r.  new_acc
        // stays rooted via its operand slot.
        trx->require_gc_root_capacity(4);  // proc, source_tail, new_acc_clone, rolling chain

        *++trx->m_gc_roots_ptr = proc_obj;
        auto *proc = trx->m_gc_roots_ptr;
        *++trx->m_gc_roots_ptr = source_tail;
        auto *st = trx->m_gc_roots_ptr;
        *++trx->m_gc_roots_ptr = new_acc.make_clone(trx);
        auto *nac = trx->m_gc_roots_ptr;
        *++trx->m_gc_roots_ptr =
                Object::make_curry_pair(trx, *st, Object::make_control_operator(SystemName::atLazyScanBuild));  // inner2
        auto *r = trx->m_gc_roots_ptr;
        *r = Object::make_curry_pair(trx, *nac, *r);   // inner1 = curry(new_acc_clone, inner2)
        *r = Object::make_curry_pair(trx, *proc, *r);  // tail_proc = curry(proc, inner1)
        *r = Object::make_lazy_thunk(trx, *r);         // tail_thunk
        *trx->m_op_ptr = Object::make_lazy(trx, new_acc, *r);

        trx->reset_gc_root(4);  // clear operator-scoped roots
    } else if (trx->m_exec_ptr->is_null()) {
        // Phase C: after force. forced result on op stack.
        trx->require_op_count(1);

        auto forced_obj = *trx->m_op_ptr;
        trx->m_exec_ptr--;  // pop null sentinel
        auto proc_obj = *trx->m_exec_ptr--;
        auto acc = *trx->m_exec_ptr--;

        if (forced_obj.is_null()) {
            // Source exhausted. Return null (no more scan elements).
            *trx->m_op_ptr = Object::make_null();
        } else {
            verify_lazy_seq_nonempty(trx, forced_obj);
            trx->require_op_capacity(1);
            trx->require_exec_capacity(4);

            // proc_obj + acc were popped off the exec stack; root them across
            // lazy_uncons -- its ExtValue head-clone fires a GC under ${...} that
            // would otherwise sweep them.  forced_obj stays rooted via its operand
            // slot.
            trx->require_gc_root_capacity(2);

            *++trx->m_gc_roots_ptr = proc_obj;
            *++trx->m_gc_roots_ptr = acc;
            auto [head_obj, source_tail_obj] = lazy_uncons(trx, forced_obj);

            trx->reset_gc_root(2);

            // Apply proc(acc, head), Phase 2 continuation
            *++trx->m_exec_ptr = source_tail_obj;
            *++trx->m_exec_ptr = proc_obj.make_clone(trx);
            *++trx->m_exec_ptr = Object::make_mark();
            *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atLazyScanBuild);

            *trx->m_op_ptr = acc;
            *++trx->m_op_ptr = head_obj;
            execute_lazy_proc(trx, proc_obj);
        }
    } else {
        // Phase 1: from curry chain. Op = [proc, acc, source].
        // Force source, push Phase C continuation.
        trx->require_op_count(3);
        trx->require_exec_capacity(4);

        auto source = *trx->m_op_ptr--;
        auto acc = *trx->m_op_ptr--;
        auto proc_obj = *trx->m_op_ptr;

        // Save acc and proc on exec, force source
        *++trx->m_exec_ptr = acc;
        *++trx->m_exec_ptr = proc_obj;
        *++trx->m_exec_ptr = Object::make_null();  // Phase C sentinel
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atLazyScanBuild);

        *trx->m_op_ptr = source;
        force_op(trx);
    }
}

// lazy-dedupe: lazy -- lazy
// Remove consecutive duplicates. Tail thunk uses lazy-drop-while(curry(head, eq))
// to skip duplicates, then applies lazy-dedupe recursively.
// throws: limit-check, opstack-underflow, type-check, vm-full
static void lazy_dedupe_op(Trix *trx) {
    trx->verify_operands(VerifyLazySeq);

    auto lazy_obj = *trx->m_op_ptr;
    if (!lazy_obj.is_null()) {
        auto [head_obj, source_tail_obj] = lazy_uncons(trx, lazy_obj);

        // Tail thunk: curry(eq_pred, curry(source_tail, compose(Force, compose(Exch,
        // compose(LazyDropWhile, LazyDedupe))))) with eq_pred = curry(head_clone, Eq),
        // head node make_lazy(head, thunk).  head_obj / source_tail_obj are fresh
        // clones from lazy_uncons (reachable from no root), and the chain links are
        // in-flight global nodes; register each on the GC-root stack and roll the
        // compose/curry chain through *r.  The head clone is stored in its slot
        // BEFORE make_curry_pair allocates over it (make_curry stores its args only
        // after the alloc that may GC).  lazy_obj stays rooted via its operand slot.
        // The stack is cleared at the tail (see require_gc_root_capacity).
        trx->require_gc_root_capacity(4);  // head, source_tail, eq_pred, rolling chain

        *++trx->m_gc_roots_ptr = head_obj;
        auto *head = trx->m_gc_roots_ptr;
        *++trx->m_gc_roots_ptr = source_tail_obj;
        auto *st = trx->m_gc_roots_ptr;
        *++trx->m_gc_roots_ptr = head_obj.make_clone(trx);  // rooted before make_curry allocates
        auto *eq = trx->m_gc_roots_ptr;
        *eq = Object::make_curry_pair(trx, *eq, Object::make_operator(SystemName::Eq));  // eq_pred
        *++trx->m_gc_roots_ptr = Object::make_compose_pair(
                trx, Object::make_operator(SystemName::LazyDropWhile), Object::make_operator(SystemName::LazyDedupe));
        auto *r = trx->m_gc_roots_ptr;
        *r = Object::make_compose_pair(trx, Object::make_operator(SystemName::Exch), *r);
        *r = Object::make_compose_pair(trx, Object::make_operator(SystemName::Force), *r);
        *r = Object::make_curry_pair(trx, *st, *r);  // inner = curry(source_tail, force_chain)
        *r = Object::make_curry_pair(trx, *eq, *r);  // tail_proc = curry(eq_pred, inner)
        *r = Object::make_lazy_thunk(trx, *r);       // tail_thunk
        *trx->m_op_ptr = Object::make_lazy(trx, *head, *r);

        trx->reset_gc_root(4);  // clear operator-scoped roots
    }
}

// lazy-intersperse: lazy val -- lazy
// Insert val between elements.
// throws: execstack-overflow, limit-check, opstack-overflow, opstack-underflow, type-check, undefined-result, vm-full
static void lazy_intersperse_op(Trix *trx) {
    trx->verify_operands(VerifyAny, VerifyLazySeq);

    auto val_obj = *trx->m_op_ptr;
    auto lazy_obj = *(trx->m_op_ptr - 1);

    if (lazy_obj.is_null()) {
        --trx->m_op_ptr;
    } else {
        auto [head_obj, source_tail_obj] = lazy_uncons(trx, lazy_obj);

        // Build tail: force source_tail synchronously if pre-evaluated, else force async and retry.
        // Intersperse cannot use compose chains because it needs a conditional (null check)
        // between forcing and building the [val, interspersed-rest] two-level node.
        auto tail_storage = source_tail_obj.thunk_storage(trx);

        if (tail_storage[Object::ThunkStorageState].integer_value() == Object::ThunkEvaluated) {
            // Sync: check cached result.  head_obj is a fresh clone reachable from
            // no root; root it on the GC-root stack across the node construction.
            // val_obj / lazy_obj stay rooted at their operand slots (val_obj is
            // re-cloned, forced_obj reads through lazy_obj).  The chain rolls
            // through *r; clones land in *tmp before make_curry / make_lazy allocs
            // over them (those store their args only after the GC-capable alloc).
            auto forced_obj = tail_storage[Object::ThunkStorageResult];
            trx->require_gc_root_capacity(3);  // head, rolling chain, clone temp

            *++trx->m_gc_roots_ptr = head_obj;
            auto *head = trx->m_gc_roots_ptr;
            *++trx->m_gc_roots_ptr = Object::make_null();  // rolling chain
            auto *r = trx->m_gc_roots_ptr;
            if (forced_obj.is_null()) {
                // No more elements: tail is null (no trailing val)
                *r = Object::make_pre_evaluated_thunk(trx);
                *r = Object::make_lazy(trx, *head, *r);
                --trx->m_op_ptr;  // drop val_obj operand
                *trx->m_op_ptr = *r;

                trx->reset_gc_root(2);  // head + rolling chain
            } else {
                // Source has more elements: build [head, thunk([val, thunk(intersperse(forced, val))])].
                // Inner thunk proc: curry(val, curry(forced, compose(Exch, LazyIntersperse)))
                // -- when forced: push val, push forced, Exch -> [forced, val], LazyIntersperse.
                *++trx->m_gc_roots_ptr = Object::make_null();  // reusable clone temp
                auto *tmp = trx->m_gc_roots_ptr;
                *r = Object::make_compose_pair(
                        trx, Object::make_operator(SystemName::Exch), Object::make_operator(SystemName::LazyIntersperse));
                *tmp = forced_obj.make_clone(trx);
                *r = Object::make_curry_pair(trx, *tmp, *r);  // inner_c = curry(forced, compose)
                *tmp = val_obj.make_clone(trx);
                *r = Object::make_curry_pair(trx, *tmp, *r);  // inner_proc = curry(val, inner_c)
                *r = Object::make_lazy_thunk(trx, *r);        // inner_thunk
                *tmp = val_obj.make_clone(trx);
                *r = Object::make_lazy(trx, *tmp, *r);           // val_node = [val, inner_thunk]
                *r = Object::make_pre_evaluated_thunk(trx, *r);  // tail_thunk
                *r = Object::make_lazy(trx, *head, *r);          // node
                --trx->m_op_ptr;                                 // drop val_obj operand
                *trx->m_op_ptr = *r;

                trx->reset_gc_root(3);  // head + rolling chain + clone temp
            }
        } else {
            // Async: force source_tail first, then retry lazy-intersperse
            trx->require_op_capacity(1);
            trx->require_exec_capacity(2);

            head_obj.maybe_free_extvalue(trx);
            *trx->m_op_ptr = val_obj;
            *(trx->m_op_ptr - 1) = lazy_obj;
            *++trx->m_exec_ptr = Object::make_operator(SystemName::LazyIntersperse);
            *++trx->m_exec_ptr = Object::make_operator(SystemName::Pop);

            *++trx->m_op_ptr = source_tail_obj;
            force_op(trx);
        }
    }
}

// lazy-step-by: lazy n -- lazy
// Every nth element. Tail thunk: force tail, drop n-1, then step-by again.
// throws: limit-check, opstack-underflow, range-check, type-check, vm-full
static void lazy_step_by_op(Trix *trx) {
    trx->verify_operands(VerifyInteger, VerifyLazySeq);

    auto n_ptr = trx->m_op_ptr;
    auto n = n_ptr->integer_value();
    if (n <= 0) {
        trx->error(Error::RangeCheck, "lazy-step-by: step must be positive");
    } else {
        auto lazy_obj = *(n_ptr - 1);
        if (lazy_obj.is_null()) {
            --trx->m_op_ptr;
        } else {
            // head_obj / source_tail are fresh clones reachable from no root, and
            // the tail-proc chain is built from global nodes held across allocs;
            // root head + source_tail + a rolling accumulator `r` on the GC-root
            // stack.  lazy_obj stays rooted via its operand slot (elem_data points
            // into it; GC is non-moving).
            auto elem_data = lazy_obj.array_objects(trx);
            trx->require_gc_root_capacity(3);  // head, source_tail, rolling chain

            *++trx->m_gc_roots_ptr = elem_data[Object::LazyHeadIndex].make_clone(trx);  // head
            auto *head = trx->m_gc_roots_ptr;
            *++trx->m_gc_roots_ptr = elem_data[Object::LazyTailThunkIndex].make_clone(trx);  // source_tail
            auto *st = trx->m_gc_roots_ptr;
            *++trx->m_gc_roots_ptr = Object::make_null();  // rolling chain
            auto *r = trx->m_gc_roots_ptr;

            if (n == 1) {
                // Step of 1: just force tail and step-by again
                *r = Object::make_curry_pair(trx, Object::make_integer(1), Object::make_operator(SystemName::LazyStepBy));
                *r = Object::make_compose_pair(trx, Object::make_operator(SystemName::Force), *r);
                *r = Object::make_curry_pair(trx, *st, *r);  // tail_proc = curry(source_tail, force_step)
                *r = Object::make_lazy_thunk(trx, *r);
            } else {
                // Force tail, drop n-1, then curry(n, LazyStepBy) to repeat
                *r = Object::make_curry_pair(trx, Object::make_integer(n), Object::make_operator(SystemName::LazyStepBy));
                *r = Object::make_compose_pair(trx, Object::make_operator(SystemName::LazyDrop), *r);
                *r = Object::make_compose_pair(trx, Object::make_operator(SystemName::Exch), *r);
                *r = Object::make_compose_pair(trx, Object::make_operator(SystemName::Force), *r);
                *r = Object::make_curry_pair(trx, *st, *r);                          // inner = curry(source_tail, force_chain)
                *r = Object::make_curry_pair(trx, Object::make_integer(n - 1), *r);  // tail_proc = curry(n-1, inner)
                *r = Object::make_lazy_thunk(trx, *r);
            }

            *r = Object::make_lazy(trx, *head, *r);  // node
            --trx->m_op_ptr;                         // drop the n operand
            *trx->m_op_ptr = *r;

            trx->reset_gc_root(3);  // clear operator-scoped roots
        }
    }
}

// lazy-zip: lazy1 lazy2 -- lazy
// Pairs: [a, b] arrays; stops at shorter.
// throws: limit-check, opstack-underflow, type-check, vm-full
static void lazy_zip_op(Trix *trx) {
    trx->verify_operands(VerifyLazySeq, VerifyLazySeq);

    auto lazy2_obj = *trx->m_op_ptr;
    auto lazy1_obj = *(trx->m_op_ptr - 1);

    if (lazy1_obj.is_null() || lazy2_obj.is_null()) {
        --trx->m_op_ptr;
        *trx->m_op_ptr = Object::make_null();
    } else {
        auto elems1 = lazy1_obj.array_objects(trx);
        auto elems2 = lazy2_obj.array_objects(trx);
        auto curr_save_level = trx->m_curr_save_level;
        // Tail thunks are composites, so make_clone aliases them -- they stay
        // reachable through the rooted lazy1_obj / lazy2_obj operand slots.
        auto t1 = elems1[Object::LazyTailThunkIndex].make_clone(trx);
        auto t2 = elems2[Object::LazyTailThunkIndex].make_clone(trx);

        // Head pair [h1, h2]: allocate region-aware (lands in global VM under
        // ${...} so it survives a restore alongside the node, and auto-stamps the
        // obj_count for the GC walker), null-init, root on the GC-root stack, then
        // clone the heads directly into it so their possibly-ExtValue clones are
        // never held unrooted across a GC-capable allocation.
        trx->require_gc_root_capacity(2);  // head pair + rolling chain

        auto [pair_storage, pair_offset] = trx->vm_alloc_dispatch_n<Object>(2, Trix::ChunkKind::Array);
        pair_storage[0] = Object::make_null();
        pair_storage[1] = Object::make_null();
        *++trx->m_gc_roots_ptr = Object::make_array(pair_offset, 2);
        auto *head = trx->m_gc_roots_ptr;
        auto h1 = elems1[Object::LazyHeadIndex].make_clone(trx);
        h1.set_save_level(curr_save_level);
        pair_storage[0] = h1;
        auto h2 = elems2[Object::LazyHeadIndex].make_clone(trx);
        h2.set_save_level(curr_save_level);
        pair_storage[1] = h2;

        // Tail: curry(t2, curry(t1, compose(Force, compose(Exch, compose(Force, LazyZip))))).
        // Build bottom-up in *r; the head pair stays rooted in *head.  t1 / t2 alias
        // the rooted lazy1_obj / lazy2_obj tail spines.
        *++trx->m_gc_roots_ptr = Object::make_compose_pair(
                trx, Object::make_operator(SystemName::Force), Object::make_operator(SystemName::LazyZip));
        auto *r = trx->m_gc_roots_ptr;
        *r = Object::make_compose_pair(trx, Object::make_operator(SystemName::Exch), *r);
        *r = Object::make_compose_pair(trx, Object::make_operator(SystemName::Force), *r);
        *r = Object::make_curry_pair(trx, t1, *r);
        *r = Object::make_curry_pair(trx, t2, *r);
        *r = Object::make_lazy_thunk(trx, *r);
        *r = Object::make_lazy(trx, *head, *r);  // node

        --trx->m_op_ptr;  // drop lazy2 operand
        *trx->m_op_ptr = *r;

        trx->reset_gc_root(2);  // clear operator-scoped roots
    }
}

// lazy-zip-with implementation
static void lazy_zip_with_impl(Trix *trx) {
    auto proc_obj = *trx->m_op_ptr;
    auto lazy2_obj = *(trx->m_op_ptr - 1);
    auto lazy1_obj = *(trx->m_op_ptr - 2);

    if (lazy1_obj.is_null() || lazy2_obj.is_null()) {
        trx->m_op_ptr -= 2;
        *trx->m_op_ptr = Object::make_null();
    } else {
        trx->require_exec_capacity(4);

        auto elems1 = lazy1_obj.array_objects(trx);
        auto elems2 = lazy2_obj.array_objects(trx);
        // h1 / h2 may be ExtValue clones; the second clone's alloc can fire a GC
        // under ${...} and sweep the first while it is an unrooted C local.  Land
        // both in gc-root slots as they are created.  t1 / t2 are tail thunks
        // (composite -> make_clone is a no-op alias) that stay reachable via
        // lazy1_obj / lazy2_obj on the operand stack, so they need no rooting.
        trx->require_gc_root_capacity(2);

        *++trx->m_gc_roots_ptr = elems1[Object::LazyHeadIndex].make_clone(trx);
        auto *h1 = trx->m_gc_roots_ptr;
        *++trx->m_gc_roots_ptr = elems2[Object::LazyHeadIndex].make_clone(trx);
        auto *h2 = trx->m_gc_roots_ptr;
        auto t1 = elems1[Object::LazyTailThunkIndex].make_clone(trx);
        auto t2 = elems2[Object::LazyTailThunkIndex].make_clone(trx);

        // Apply proc(h1, h2), @lazy-zip-with-build collects result
        *++trx->m_exec_ptr = t1;
        *++trx->m_exec_ptr = t2;
        *++trx->m_exec_ptr = proc_obj;
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atLazyZipWithBuild);

        trx->m_op_ptr -= 3;
        *++trx->m_op_ptr = *h1;
        *++trx->m_op_ptr = *h2;

        trx->reset_gc_root(2);
        execute_lazy_proc(trx, proc_obj.make_clone(trx));
    }
}

// lazy-zip-with: lazy1 lazy2 proc -- lazy
// Apply proc to pairs.
// throws: execstack-overflow, limit-check, opstack-underflow, type-check, vm-full
static void lazy_zip_with_op(Trix *trx) {
    trx->verify_operands(VerifyCallable, VerifyLazySeq, VerifyLazySeq);
    lazy_zip_with_impl(trx);
}

// @lazy-zip-with-impl: internal recursive entry point
static void at_lazy_zip_with_impl_op(Trix *trx) {
    trx->require_op_count(3);
    lazy_zip_with_impl(trx);
}

// @lazy-zip-with-build: collect mapped result, build node.
// Exec stack: [t1, t2, proc]  Op stack: [..., result]
static void at_lazy_zip_with_build_op(Trix *trx) {
    trx->require_op_count(1);

    auto result_obj = *trx->m_op_ptr;
    auto proc_obj = *trx->m_exec_ptr--;
    auto t2 = *trx->m_exec_ptr--;
    auto t1 = *trx->m_exec_ptr--;

    // Tail thunk: curry(proc, curry(t2, curry(t1, Force >> Exch >> Force >> Rot >> LazyZipWith))).
    // When forced: push proc, t2, t1; force t1 -> f1; exch -> [proc, f1, t2];
    // force t2 -> f2; rot -> [f1, f2, proc]; lazy-zip-with.  proc_obj/t2/t1 were
    // popped off the exec stack (now unrooted); root them on the GC-root stack and
    // roll the chain through *r.  result_obj stays rooted via its operand slot.
    trx->require_gc_root_capacity(4);  // proc, t2, t1, rolling chain

    *++trx->m_gc_roots_ptr = proc_obj;
    auto *proc = trx->m_gc_roots_ptr;
    *++trx->m_gc_roots_ptr = t2;
    auto *t2r = trx->m_gc_roots_ptr;
    *++trx->m_gc_roots_ptr = t1;
    auto *t1r = trx->m_gc_roots_ptr;
    *++trx->m_gc_roots_ptr = Object::make_compose_pair(
            trx, Object::make_operator(SystemName::Rot), Object::make_control_operator(SystemName::atLazyZipWithImpl));
    auto *r = trx->m_gc_roots_ptr;
    *r = Object::make_compose_pair(trx, Object::make_operator(SystemName::Force), *r);
    *r = Object::make_compose_pair(trx, Object::make_operator(SystemName::Exch), *r);
    *r = Object::make_compose_pair(trx, Object::make_operator(SystemName::Force), *r);
    *r = Object::make_curry_pair(trx, *t1r, *r);   // inner2 = curry(t1, ...)
    *r = Object::make_curry_pair(trx, *t2r, *r);   // inner1 = curry(t2, inner2)
    *r = Object::make_curry_pair(trx, *proc, *r);  // tail_proc = curry(proc, inner1)
    *r = Object::make_lazy_thunk(trx, *r);
    *trx->m_op_ptr = Object::make_lazy(trx, result_obj, *r);

    trx->reset_gc_root(4);  // clear operator-scoped roots
}

// lazy-chain: lazy1 lazy2 -- lazy
// Concatenate: all of lazy1 then all of lazy2.
// throws: limit-check, opstack-underflow, type-check, vm-full
static void lazy_chain_op(Trix *trx) {
    trx->verify_operands(VerifyLazySeq, VerifyLazySeq);
    auto lazy2_obj = *trx->m_op_ptr;
    auto lazy1_obj = *(trx->m_op_ptr - 1);

    if (lazy1_obj.is_null()) {
        // lazy1 exhausted: return lazy2
        --trx->m_op_ptr;
        *trx->m_op_ptr = lazy2_obj;
    } else if (lazy2_obj.is_null()) {
        // lazy2 empty: return lazy1
        --trx->m_op_ptr;
        *trx->m_op_ptr = lazy1_obj;
    } else {
        // Tail thunk: force t1, chain with lazy2 --
        // curry(lazy2, curry(t1, compose(Force, compose(Exch, LazyChain)))).
        // head_obj / t1 are fresh clones reachable from no root; root them plus a
        // rolling accumulator `r` on the GC-root stack across the chain allocs.
        // lazy1_obj / lazy2_obj stay rooted via their operand slots.
        auto elem_data = lazy1_obj.array_objects(trx);
        trx->require_gc_root_capacity(3);  // head, t1, rolling chain

        *++trx->m_gc_roots_ptr = elem_data[Object::LazyHeadIndex].make_clone(trx);  // head
        auto *head = trx->m_gc_roots_ptr;
        *++trx->m_gc_roots_ptr = elem_data[Object::LazyTailThunkIndex].make_clone(trx);  // t1
        auto *t1r = trx->m_gc_roots_ptr;
        *++trx->m_gc_roots_ptr = Object::make_compose_pair(
                trx, Object::make_operator(SystemName::Exch), Object::make_operator(SystemName::LazyChain));
        auto *r = trx->m_gc_roots_ptr;
        *r = Object::make_compose_pair(trx, Object::make_operator(SystemName::Force), *r);
        *r = Object::make_curry_pair(trx, *t1r, *r);       // inner = curry(t1, force_chain)
        *r = Object::make_curry_pair(trx, lazy2_obj, *r);  // tail_proc = curry(lazy2, inner)
        *r = Object::make_lazy_thunk(trx, *r);
        *r = Object::make_lazy(trx, *head, *r);  // node
        --trx->m_op_ptr;                         // drop lazy2 operand
        *trx->m_op_ptr = *r;

        trx->reset_gc_root(3);  // clear operator-scoped roots
    }
}

// lazy-interleave: lazy1 lazy2 -- lazy
// Alternate: a1, b1, a2, b2, ...
// throws: limit-check, opstack-underflow, type-check, vm-full
static void lazy_interleave_op(Trix *trx) {
    trx->verify_operands(VerifyLazySeq, VerifyLazySeq);
    auto lazy2_obj = *trx->m_op_ptr;
    auto lazy1_obj = *(trx->m_op_ptr - 1);

    if (lazy1_obj.is_null()) {
        --trx->m_op_ptr;
        *trx->m_op_ptr = lazy2_obj;
    } else {
        // Tail thunk: interleave(lazy2, t1_forced) -- swap order each step --
        // curry(lazy2, curry(t1, compose(Force, LazyInterleave))).  When forced:
        // push lazy2, push t1; force t1 -> f_t1; LazyInterleave pops f_t1 as lazy2
        // and lazy2 as lazy1 (lazy2 elements come first, then f_t1 alternating).
        // head_obj / t1 are fresh clones reachable from no root; root them plus a
        // rolling accumulator `r` on the GC-root stack across the chain allocs.
        // lazy1_obj / lazy2_obj stay rooted via their operand slots.
        auto elem_data = lazy1_obj.array_objects(trx);
        trx->require_gc_root_capacity(3);  // head, t1, rolling chain

        *++trx->m_gc_roots_ptr = elem_data[Object::LazyHeadIndex].make_clone(trx);  // head
        auto *head = trx->m_gc_roots_ptr;
        *++trx->m_gc_roots_ptr = elem_data[Object::LazyTailThunkIndex].make_clone(trx);  // t1
        auto *t1r = trx->m_gc_roots_ptr;
        *++trx->m_gc_roots_ptr = Object::make_compose_pair(
                trx, Object::make_operator(SystemName::Force), Object::make_operator(SystemName::LazyInterleave));
        auto *r = trx->m_gc_roots_ptr;
        *r = Object::make_curry_pair(trx, *t1r, *r);       // inner = curry(t1, force_il)
        *r = Object::make_curry_pair(trx, lazy2_obj, *r);  // tail_proc = curry(lazy2, inner)
        *r = Object::make_lazy_thunk(trx, *r);
        *r = Object::make_lazy(trx, *head, *r);  // node
        --trx->m_op_ptr;                         // drop lazy2 operand
        *trx->m_op_ptr = *r;

        trx->reset_gc_root(3);  // clear operator-scoped roots
    }
}

// lazy-enumerate: lazy -- lazy
// Pairs: [index, element] arrays.
// throws: limit-check, opstack-overflow, opstack-underflow, type-check, vm-full
static void lazy_enumerate_op(Trix *trx) {
    trx->verify_operands(VerifyLazySeq);
    auto lazy_obj = *trx->m_op_ptr;

    if (lazy_obj.is_null()) {
        // already null on stack
    } else {
        // Implement as lazy-zip(lazy-from(0), lazy)
        // Build lazy-from(0) on op stack, then zip
        trx->require_op_capacity(1);

        *++trx->m_op_ptr = Object::make_integer(0);
        lazy_from_op(trx);

        // op = [lazy, indices_lazy]. Swap for zip(indices, lazy).
        auto indices = *trx->m_op_ptr;
        *trx->m_op_ptr = *(trx->m_op_ptr - 1);
        *(trx->m_op_ptr - 1) = indices;
        lazy_zip_op(trx);
    }
}

// lazy-chunked: lazy n -- lazy
// Groups of n elements (as arrays).
// throws: execstack-overflow, limit-check, opstack-underflow, range-check, type-check, undefined-result, vm-full
static void lazy_chunked_op(Trix *trx) {
    trx->verify_operands(VerifyInteger, VerifyLazySeq);

    auto n_ptr = trx->m_op_ptr;
    auto lazy_ptr = (n_ptr - 1);
    auto n = n_ptr->integer_value();
    if (n <= 0) {
        trx->error(Error::RangeCheck, "lazy-chunked: size must be positive");
    } else if (n > MaxArrayLength) {
        trx->error(Error::LimitCheck, "lazy-chunked: size {} exceeds maximum {}", n, MaxArrayLength);
    } else {
        auto lazy_obj = *lazy_ptr;
        if (lazy_obj.is_null()) {
            --trx->m_op_ptr;
        } else {
            // Eagerly collect the head chunk.  Allocate it region-aware so it
            // lands in the same region as the lazy node (a local chunk embedded
            // in a global node dangles after restore) and auto-stamps obj_count
            // for the GC walker; null-init and root it on the GC-root stack BEFORE
            // lazy_uncons (whose head clone can fire a GC) so the chunk and the
            // head element written into it survive any collection.
            auto [dst, dst_offset] = trx->vm_alloc_dispatch_n<Object>(static_cast<length_t>(n), Trix::ChunkKind::Array);
            auto curr_save_level = trx->m_curr_save_level;
            for (integer_t i = 0; i < n; ++i) {
                dst[i] = Object::make_null(curr_save_level);
            }
            trx->require_gc_root_capacity(1);

            *++trx->m_gc_roots_ptr = Object::make_array(dst_offset, static_cast<length_t>(n));
            auto *chunk = trx->m_gc_roots_ptr;

            auto [head_obj, source_tail_obj] = lazy_uncons(trx, lazy_obj);
            head_obj.set_save_level(curr_save_level);
            dst[0] = head_obj;

            if (n == 1) {
                // Single-element chunk: tail thunk forces source_tail, chunks again.
                // Tail proc = curry(source_tail, compose(Force, curry(n, LazyChunked))).
                // Root source_tail + the rolling chain accumulator; the chunk stays
                // rooted in *chunk.
                trx->require_gc_root_capacity_more(2);  // 2nd guard: 2 MORE on top of chunk above

                *++trx->m_gc_roots_ptr = source_tail_obj;
                auto *st = trx->m_gc_roots_ptr;
                *++trx->m_gc_roots_ptr =
                        Object::make_curry_pair(trx, Object::make_integer(n), Object::make_operator(SystemName::LazyChunked));
                auto *r = trx->m_gc_roots_ptr;
                *r = Object::make_compose_pair(trx, Object::make_operator(SystemName::Force), *r);
                *r = Object::make_curry_pair(trx, *st, *r);
                *r = Object::make_lazy_thunk(trx, *r);
                *r = Object::make_lazy(trx, *chunk, *r);  // node
                --trx->m_op_ptr;                          // drop the n operand
                *trx->m_op_ptr = *r;

                trx->reset_gc_root(3);  // clear operator-scoped roots
            } else {
                // Collect more via @lazy-chunked-collect.  Move the chunk onto the
                // exec frame (a GC root for the whole collection loop), then force
                // source_tail.  Exec stack: [dst_array, write_index, n, @collect]
                trx->require_exec_capacity(4);
                *++trx->m_exec_ptr = *chunk;
                *++trx->m_exec_ptr = Object::make_integer(1);  // write_index (index 0 written)
                *++trx->m_exec_ptr = Object::make_integer(n);
                *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atLazyChunkedCollect);

                trx->reset_gc_root(1);  // chunk now rooted on the exec frame
                --trx->m_op_ptr;        // drop the n operand
                *trx->m_op_ptr = source_tail_obj;
                force_op(trx);
            }
        }
    }
}

// @lazy-chunked-collect: collect elements into chunk array.
// Exec stack: [dst_array, write_index, n]  Op stack: [..., forced_result]
static void at_lazy_chunked_collect_op(Trix *trx) {
    trx->require_op_count(1);

    auto forced_obj = *trx->m_op_ptr;
    auto n_obj = *trx->m_exec_ptr--;
    auto idx_obj = *trx->m_exec_ptr--;
    auto dst_obj = *trx->m_exec_ptr--;

    auto n = n_obj.integer_value();
    auto write_idx = idx_obj.integer_value();

    // dst_obj (the chunk-accumulator array) was popped off the exec stack and is
    // reachable from no root, yet the element/tail clones below, make_lazy and the
    // tail-proc chain can fire a global GC.  Root it on the GC-root stack across
    // the body; forced_obj stays rooted via its operand slot.  (The chunk was
    // null-init'd to n slots by lazy_chunked_op and lives in the lazy node's
    // region, so walking its full obj_count during a GC stays valid.)  The node
    // head uses the dst_obj C local (which carries the trimmed length); the slot
    // is just the GC anchor for the shared storage, so it is not dereferenced.
    trx->gc_root_push_oneoff(dst_obj);

    if (forced_obj.is_null()) {
        // Source exhausted mid-chunk: trim array and return partial chunk (or null)
        if (write_idx == 0) {
            *trx->m_op_ptr = Object::make_null();

            trx->gc_root_pop_oneoff();  // clear operator-scoped roots
        } else {
            // Return partial chunk
            dst_obj.set_array_length(static_cast<length_t>(write_idx));
            auto unused = static_cast<vm_size_t>(n - write_idx) * vm_sizeof<Object>();
            if (unused > 0) {
                auto dst_end = dst_obj.array_objects(trx) + n;
                trx->vm_trim_alloc(reinterpret_cast<vm_t *>(dst_end), unused);
            }
            // Build node with null tail (dst storage stays rooted in *dst)
            trx->require_gc_root_capacity_more(1);  // 2nd guard: 1 MORE on top of dst_obj above

            *++trx->m_gc_roots_ptr = Object::make_pre_evaluated_thunk(trx);
            auto *r = trx->m_gc_roots_ptr;
            *trx->m_op_ptr = Object::make_lazy(trx, dst_obj, *r);

            trx->reset_gc_root(2);  // clear operator-scoped roots
        }
    } else {
        verify_lazy_seq_nonempty(trx, forced_obj);

        // Clone + store the element FIRST (so it is reachable via the rooted dst),
        // THEN clone the source tail -- otherwise a GC fired by the source-tail
        // clone could sweep the fresh element clone before it lands in dst.
        auto src_elems = forced_obj.array_objects(trx);
        auto elem = src_elems[Object::LazyHeadIndex].make_clone(trx);
        elem.set_save_level(trx->m_curr_save_level);
        dst_obj.array_objects(trx)[write_idx] = elem;
        auto source_tail = src_elems[Object::LazyTailThunkIndex].make_clone(trx);

        if ((write_idx + 1) >= n) {
            // Chunk complete: build node, tail thunk continues chunking.
            // curry(source_tail, compose(Force, curry(n, LazyChunked))); root
            // source_tail + a rolling accumulator (dst storage stays rooted in *dst).
            trx->require_gc_root_capacity_more(2);  // 2nd guard: 2 MORE on top of dst_obj above

            *++trx->m_gc_roots_ptr = source_tail;
            auto *st = trx->m_gc_roots_ptr;
            *++trx->m_gc_roots_ptr =
                    Object::make_curry_pair(trx, Object::make_integer(n), Object::make_operator(SystemName::LazyChunked));
            auto *r = trx->m_gc_roots_ptr;
            *r = Object::make_compose_pair(trx, Object::make_operator(SystemName::Force), *r);
            *r = Object::make_curry_pair(trx, *st, *r);  // tail_proc = curry(source_tail, force_chunked)
            *r = Object::make_lazy_thunk(trx, *r);
            *trx->m_op_ptr = Object::make_lazy(trx, dst_obj, *r);

            trx->reset_gc_root(3);  // clear operator-scoped roots
        } else {
            // Continue collecting: move the chunk back onto the exec frame (a GC
            // root for the rest of the loop), then force source_tail.
            trx->require_exec_capacity(4);

            *++trx->m_exec_ptr = dst_obj;
            *++trx->m_exec_ptr = Object::make_integer(write_idx + 1);
            *++trx->m_exec_ptr = Object::make_integer(n);
            *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atLazyChunkedCollect);

            trx->reset_gc_root(1);  // chunk now rooted on the exec frame
            *trx->m_op_ptr = source_tail;
            force_op(trx);
        }
    }
}

// lazy-windowed: lazy n -- lazy
// Sliding windows of size n.
// throws: execstack-overflow, limit-check, opstack-underflow, range-check, type-check, vm-full
static void lazy_windowed_op(Trix *trx) {
    trx->verify_operands(VerifyInteger, VerifyLazySeq);

    auto n_ptr = trx->m_op_ptr;
    auto lazy_ptr = (n_ptr - 1);
    auto n = n_ptr->integer_value();
    if (n <= 0) {
        trx->error(Error::RangeCheck, "lazy-windowed: size must be positive");
    } else if (lazy_ptr->is_null()) {
        --trx->m_op_ptr;
    } else {
        // Strategy: head = take(n) from current position as array.
        // Tail thunk: force source_tail (position 1), call lazy-windowed(pos1, n).
        // O(n) per window step; thunk caching keeps total work O(n*m).
        //
        // First window requires async take+to-array. Use exec stack:
        // 1. Build tail thunk (synchronous)
        // 2. Save [tail_thunk, @LazyMapBuild] on exec
        // 3. Push lazy + n on op, execute compose(LazyTake, LazyToArray)
        // 4. @LazyMapBuild picks up window array + tail_thunk -> node

        // Build tail thunk: curry(source_tail, compose(Force, curry(int(n), LazyWindowed))).
        // Keep lazy + n on the op stack as roots while building the chain (the
        // source lazy is re-read below for the first-window take), and root
        // source_tail + a rolling accumulator `r` on the GC-root stack across the
        // allocs.
        auto source_tail = lazy_ptr->array_objects(trx)[Object::LazyTailThunkIndex].make_clone(trx);
        trx->require_gc_root_capacity(2);  // source_tail + rolling chain

        *++trx->m_gc_roots_ptr = source_tail;
        auto *st = trx->m_gc_roots_ptr;
        *++trx->m_gc_roots_ptr =
                Object::make_curry_pair(trx, Object::make_integer(n), Object::make_operator(SystemName::LazyWindowed));
        auto *r = trx->m_gc_roots_ptr;
        *r = Object::make_compose_pair(trx, Object::make_operator(SystemName::Force), *r);
        *r = Object::make_curry_pair(trx, *st, *r);  // tail_inner = curry(source_tail, force_wind)
        *r = Object::make_lazy_thunk(trx, *r);       // tail_thunk

        auto src_lazy = *lazy_ptr;  // capture before popping the operands
        trx->m_op_ptr -= 2;         // pop n + lazy

        // Save continuation: @LazyMapBuild will pop window_array + tail_thunk
        trx->require_exec_capacity(2);

        *++trx->m_exec_ptr = *r;  // tail_thunk -> rooted on the exec frame
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atLazyMapBuild);

        // src_lazy was just popped off the operand stack; keep it rooted (reuse the
        // rolling slot now that tail_thunk is safe on the exec frame) across the
        // make_compose_pair below -- that is an unconditional alloc that fires a GC
        // under ${...} and would otherwise sweep the source seq, leaving lazy-take a
        // reused [op op] block.
        *r = src_lazy;

        // Collect first window: lazy-take(lazy, n) then lazy-to-array
        auto take_to_array = Object::make_compose_pair(
                trx, Object::make_operator(SystemName::LazyTake), Object::make_operator(SystemName::LazyToArray));
        *++trx->m_op_ptr = src_lazy;
        *++trx->m_op_ptr = Object::make_integer(n);

        trx->reset_gc_root(2);  // clear operator-scoped roots
        execute_lazy_proc(trx, take_to_array);
    }
}

// lazy-pairwise: lazy -- lazy
// Consecutive pairs: [a, b] arrays.
// throws: execstack-overflow, limit-check, opstack-overflow, opstack-underflow, type-check, vm-full
static void lazy_pairwise_op(Trix *trx) {
    trx->verify_operands(VerifyLazySeq);
    trx->require_op_capacity(1);

    // Delegate to lazy-windowed with n=2
    *++trx->m_op_ptr = Object::make_integer(2);
    lazy_windowed_op(trx);
}

// Walk the lazy chain starting from `curr`, forcing the first unevaluated
// thunk encountered.  On finding one, pushes the resume frame and kicks off
// force_op; caller must return.  Returns false if the chain is fully evaluated
// from `curr` onward (caller finishes the materialization).
//
// Resume frame (exec stack, bottom to top):  [head, curr_hint, @step-op, Pop]
// The @step-op reads head and curr_hint after Pop discards the forced result.
// Saving head on the exec stack (rather than relying on the op-stack top
// staying intact across user proc invocations) prevents misbehaving user
// procs from corrupting the materialization target.
[[nodiscard]] static bool lazy_drive_forcing(Trix *trx, Object curr, Object head_obj, SystemName step_name) {
    while (!curr.is_null()) {
        verify_lazy_seq_nonempty(trx, curr);

        auto curr_elems = curr.array_objects(trx);
        auto tail_thunk = curr_elems[Object::LazyTailThunkIndex];
        if (!tail_thunk.is_thunk()) {
            return false;
        } else {
            auto thunk_s = tail_thunk.thunk_storage(trx);
            if (thunk_s[Object::ThunkStorageState].integer_value() == Object::ThunkEvaluated) {
                curr = thunk_s[Object::ThunkStorageResult];
            } else {
                trx->require_op_capacity(1);
                trx->require_exec_capacity(4);

                *++trx->m_exec_ptr = head_obj;
                *++trx->m_exec_ptr = curr;  // literal Array; consumed by @step-op
                *++trx->m_exec_ptr = Object::make_control_operator(step_name);
                *++trx->m_exec_ptr = Object::make_operator(SystemName::Pop);

                *++trx->m_op_ptr = tail_thunk.make_clone(trx);
                force_op(trx);
                return true;
            }
        }
    }
    return false;
}

// After all thunks in the chain are evaluated, count from head and allocate
// the result array in place of head on the op stack.
static void lazy_to_array_finish(Trix *trx, Object head_obj) {
    auto current = head_obj;
    vm_size_t count32 = 0;
    while (!current.is_null()) {
        auto elem_data = current.array_objects(trx);
        auto thunk_storage = elem_data[Object::LazyTailThunkIndex].thunk_storage(trx);
        current = thunk_storage[Object::ThunkStorageResult];
        ++count32;
    }
    if (count32 > MaxArrayLength) {
        trx->error(Error::LimitCheck, "lazy-to-array: sequence length {} exceeds maximum {}", count32, MaxArrayLength);
    } else {
        auto count = static_cast<length_t>(count32);
        auto [dst, offset] = trx->vm_alloc_dispatch_n<Object>(count, Trix::ChunkKind::Array);
        auto curr_save_level = trx->m_curr_save_level;
        // Null-init + root the result array BEFORE the clone-fill loop: each
        // make_clone allocates a global ExtValue/WideValue per element inside ${...},
        // which can fire a global GC.  The walker marks all obj_count slots (so the
        // tail must be valid Null Objects) and the array must be reachable from the
        // op stack so the clones already written survive.  We overwrite the op-stack
        // slot that held the source seq, so save the source on the exec stack to keep
        // it (which `current` walks) rooted across the loop's GCs.
        for (length_t i = 0; i < count; ++i) {
            dst[i] = Object::make_null();
        }
        trx->require_exec_capacity(1);
        *++trx->m_exec_ptr = head_obj;
        *trx->m_op_ptr = Object::make_array(offset, count);

        current = head_obj;
        for (length_t i = 0; i < count; ++i) {
            auto elem_data = current.array_objects(trx);
            auto clone = elem_data[Object::LazyHeadIndex].make_clone(trx);
            clone.set_save_level(curr_save_level);
            dst[i] = clone;
            auto thunk_storage = elem_data[Object::LazyTailThunkIndex].thunk_storage(trx);
            current = thunk_storage[Object::ThunkStorageResult];
        }
        --trx->m_exec_ptr;
    }
}

// lazy-to-array: lazy -- array
// Materialize to array (must be finite!).
// throws: execstack-overflow, limit-check, opstack-overflow, opstack-underflow, type-check, undefined-result, vm-full
static void lazy_to_array_op(Trix *trx) {
    trx->verify_operands(VerifyLazySeq);
    auto lazy_obj = *trx->m_op_ptr;

    if (lazy_obj.is_null()) {
        auto [_, offset] = trx->vm_alloc_dispatch_n<Object>(0, Trix::ChunkKind::Array);
        *trx->m_op_ptr = Object::make_array(offset, 0);
    } else {
        verify_lazy_seq_nonempty(trx, lazy_obj);

        // Drive forcing starting from the head.  If an unevaluated thunk is found,
        // force it and the @step-op will resume from that position on re-entry.
        if (!lazy_drive_forcing(trx, lazy_obj, lazy_obj, SystemName::atLazyToArrayStep)) {
            lazy_to_array_finish(trx, lazy_obj);
        }
    }
}

// @lazy-to-array-step: (internal) resumes lazy_drive_forcing from a saved
// position after a thunk has been forced.  Exec stack (below this op):
// [head, curr_hint] -- saved by lazy_drive_forcing.
static void at_lazy_to_array_step_op(Trix *trx) {
    auto curr_hint_obj = *trx->m_exec_ptr--;  // consume the resume hint
    auto head_obj = *trx->m_exec_ptr--;       // consume the saved head

    if (!lazy_drive_forcing(trx, curr_hint_obj, head_obj, SystemName::atLazyToArrayStep)) {
        // Restore head to op top before finishing: a misbehaving user proc may
        // have corrupted what was there.  require_op_count(1) guards against
        // user-proc underflow -- raises a proper error instead of writing below
        // the stack base.
        trx->require_op_count(1);
        *trx->m_op_ptr = head_obj;
        lazy_to_array_finish(trx, head_obj);
    }
}

// lazy-fold: lazy init proc -- result
// Left fold over all elements.
// throws: execstack-overflow, limit-check, opstack-underflow, type-check, vm-full
static void lazy_fold_op(Trix *trx) {
    trx->verify_operands(VerifyCallable, VerifyAny, VerifyLazySeq);
    auto proc_obj = *trx->m_op_ptr;
    auto init_obj = *(trx->m_op_ptr - 1);
    auto lazy_obj = *(trx->m_op_ptr - 2);

    trx->m_op_ptr -= 3;

    if (lazy_obj.is_null()) {
        *++trx->m_op_ptr = init_obj;
    } else {
        trx->require_exec_capacity(4);

        // lazy_obj / init_obj / proc_obj were all popped (m_op_ptr -= 3 above); root
        // them across lazy_uncons, whose ExtValue head-clone fires a GC under ${...}.
        trx->require_gc_root_capacity(3);

        *++trx->m_gc_roots_ptr = lazy_obj;
        *++trx->m_gc_roots_ptr = init_obj;
        *++trx->m_gc_roots_ptr = proc_obj;
        auto [head_obj, source_tail_obj] = lazy_uncons(trx, lazy_obj);

        trx->reset_gc_root(3);

        // Apply proc(init, head), then continue with source_tail
        // Phase A continuation: after proc, force source_tail
        *++trx->m_exec_ptr = source_tail_obj;
        *++trx->m_exec_ptr = proc_obj.make_clone(trx);
        *++trx->m_exec_ptr = Object::make_mark();
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atLazyFoldStep);

        // Push init and head, execute proc
        *++trx->m_op_ptr = init_obj;
        *++trx->m_op_ptr = head_obj;
        execute_lazy_proc(trx, proc_obj);
    }
}

// @lazy-fold-step: two-phase control op for lazy-fold.
// Phase A (after proc, exec top = mark): new_acc on op, (source_tail, proc, mark) on exec
//   Force source_tail, push Phase B continuation.
// Phase B (after force, exec top != mark): forced on op, (acc, proc) on exec
//   If null -> return acc. If not null -> apply proc(acc, head), loop.
static void at_lazy_fold_step_op(Trix *trx) {
    if (trx->m_exec_ptr->is_mark()) {
        // Phase A: after proc completed. new_acc on op stack.
        trx->require_op_count(1);
        trx->require_exec_capacity(1);

        auto new_acc = *trx->m_op_ptr;
        trx->m_exec_ptr--;  // pop mark
        auto proc_obj = *trx->m_exec_ptr--;
        auto source_tail = *trx->m_exec_ptr--;

        // Force source_tail, push Phase B continuation

        *++trx->m_exec_ptr = new_acc;
        *++trx->m_exec_ptr = proc_obj;
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atLazyFoldStep);

        *trx->m_op_ptr = source_tail;
        force_op(trx);
    } else {
        // Phase B: after force. forced result on op stack.
        trx->require_op_count(1);

        auto forced_obj = *trx->m_op_ptr;
        auto proc_obj = *trx->m_exec_ptr--;
        auto acc = *trx->m_exec_ptr--;

        if (forced_obj.is_null()) {
            *trx->m_op_ptr = acc;
        } else {
            verify_lazy_seq_nonempty(trx, forced_obj);
            trx->require_op_capacity(1);
            trx->require_exec_capacity(4);

            // proc_obj + acc were popped off the exec stack; root them across
            // lazy_uncons (ExtValue head-clone can GC under ${...}).  forced_obj
            // stays rooted via its operand slot.
            trx->require_gc_root_capacity(2);

            *++trx->m_gc_roots_ptr = proc_obj;
            *++trx->m_gc_roots_ptr = acc;
            auto [head_obj, source_tail_obj] = lazy_uncons(trx, forced_obj);

            trx->reset_gc_root(2);

            // Apply proc(acc, head), push Phase A continuation
            *++trx->m_exec_ptr = source_tail_obj;
            *++trx->m_exec_ptr = proc_obj.make_clone(trx);
            *++trx->m_exec_ptr = Object::make_mark();
            *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atLazyFoldStep);

            *trx->m_op_ptr = acc;
            *++trx->m_op_ptr = head_obj;
            execute_lazy_proc(trx, proc_obj);
        }
    }
}

// lazy-for-each: lazy proc --
// Execute proc on each element (side effects).
// throws: execstack-overflow, limit-check, opstack-underflow, type-check, vm-full
static void lazy_for_each_op(Trix *trx) {
    trx->verify_operands(VerifyCallable, VerifyLazySeq);
    auto proc_obj = *trx->m_op_ptr;
    auto lazy_obj = *(trx->m_op_ptr - 1);
    trx->m_op_ptr -= 2;

    if (!lazy_obj.is_null()) {
        trx->require_exec_capacity(4);

        // lazy_obj + proc_obj were both popped (m_op_ptr -= 2 above); root them
        // across lazy_uncons, whose ExtValue head-clone fires a GC under ${...}.
        trx->require_gc_root_capacity(2);

        *++trx->m_gc_roots_ptr = lazy_obj;
        *++trx->m_gc_roots_ptr = proc_obj;
        auto [head_obj, source_tail_obj] = lazy_uncons(trx, lazy_obj);

        trx->reset_gc_root(2);

        // Phase A setup: after proc(head), force source_tail
        *++trx->m_exec_ptr = source_tail_obj;
        *++trx->m_exec_ptr = proc_obj.make_clone(trx);
        *++trx->m_exec_ptr = Object::make_mark();
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atLazyForEachStep);

        *++trx->m_op_ptr = head_obj;
        execute_lazy_proc(trx, proc_obj);
    }
}

// @lazy-for-each-step: two-phase control op for lazy-for-each.
// Phase A (after proc, exec top = mark): pop mark, pop saved state, force source_tail
// Phase B (after force, exec top != mark): check forced result, continue or terminate
static void at_lazy_for_each_step_op(Trix *trx) {
    if (trx->m_exec_ptr->is_mark()) {
        trx->require_op_capacity(1);

        // Phase A: after proc. User's proc manages its own stack effects.
        trx->m_exec_ptr--;  // pop mark
        auto proc_obj = *trx->m_exec_ptr--;
        auto source_tail = *trx->m_exec_ptr--;

        // Force source_tail, Phase B continuation
        *++trx->m_exec_ptr = proc_obj;
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atLazyForEachStep);

        *++trx->m_op_ptr = source_tail;
        force_op(trx);
    } else {
        // Phase B: after force. forced result on op.
        trx->require_op_count(1);
        auto forced_obj = *trx->m_op_ptr--;
        auto proc_obj = *trx->m_exec_ptr--;

        if (!forced_obj.is_null()) {
            verify_lazy_seq_nonempty(trx, forced_obj);
            trx->require_exec_capacity(4);

            // forced_obj (the seq) was popped off the operand stack and proc_obj off
            // the exec stack; root both across lazy_uncons (ExtValue head-clone can
            // GC under ${...}).
            trx->require_gc_root_capacity(2);

            *++trx->m_gc_roots_ptr = forced_obj;
            *++trx->m_gc_roots_ptr = proc_obj;
            auto [head_obj, source_tail_obj] = lazy_uncons(trx, forced_obj);

            trx->reset_gc_root(2);

            // Apply proc(head), Phase A continuation
            *++trx->m_exec_ptr = source_tail_obj;
            *++trx->m_exec_ptr = proc_obj.make_clone(trx);
            *++trx->m_exec_ptr = Object::make_mark();
            *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atLazyForEachStep);

            *++trx->m_op_ptr = head_obj;
            execute_lazy_proc(trx, proc_obj);
        }
    }
}

// lazy-any: lazy pred -- bool
// throws: execstack-overflow, opstack-underflow, type-check, vm-full
static void lazy_any_op(Trix *trx) {
    trx->verify_operands(VerifyCallable, VerifyLazySeq);

    auto pred_obj = *trx->m_op_ptr;
    auto lazy_obj = *(trx->m_op_ptr - 1);

    if (lazy_obj.is_null()) {
        --trx->m_op_ptr;
        *trx->m_op_ptr = Object::make_boolean(false);
    } else {
        trx->require_exec_capacity(4);

        auto [head_obj, source_tail_obj] = lazy_uncons(trx, lazy_obj);

        // Phase A: after pred(head), check result
        *++trx->m_exec_ptr = source_tail_obj;
        *++trx->m_exec_ptr = pred_obj.make_clone(trx);
        *++trx->m_exec_ptr = Object::make_mark();
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atLazyAnyStep);

        --trx->m_op_ptr;
        *trx->m_op_ptr = head_obj;
        execute_lazy_proc(trx, pred_obj);
    }
}

// @lazy-any-step: two-phase control op for lazy-any.
// Phase A (after pred, exec top = mark): boolean on op stack
// Phase B (after force, exec top != mark): forced result on op stack
static void at_lazy_any_step_op(Trix *trx) {
    if (trx->m_exec_ptr->is_mark()) {
        // Phase A: after pred. boolean on op stack.
        trx->verify_operands(VerifyBoolean);
        auto result = trx->m_op_ptr->boolean_value();
        --trx->m_op_ptr;
        trx->m_exec_ptr--;  // pop mark
        auto pred_obj = *trx->m_exec_ptr--;
        auto source_tail = *trx->m_exec_ptr--;

        if (result) {
            trx->require_op_capacity(1);

            *++trx->m_op_ptr = Object::make_boolean(true);
        } else {
            // Force source_tail, Phase B
            trx->require_op_capacity(1);

            *++trx->m_exec_ptr = pred_obj;
            *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atLazyAnyStep);

            *++trx->m_op_ptr = source_tail;
            force_op(trx);
        }
    } else {
        // Phase B: after force.
        trx->require_op_count(1);

        auto forced_obj = *trx->m_op_ptr--;
        auto pred_obj = *trx->m_exec_ptr--;

        if (forced_obj.is_null()) {
            trx->require_op_capacity(1);

            *++trx->m_op_ptr = Object::make_boolean(false);
        } else {
            verify_lazy_seq_nonempty(trx, forced_obj);
            trx->require_op_capacity(1);
            trx->require_exec_capacity(4);

            // forced_obj (the seq) was popped off the operand stack and pred_obj off
            // the exec stack; root both across lazy_uncons (ExtValue head-clone can
            // GC under ${...}).
            trx->require_gc_root_capacity(2);

            *++trx->m_gc_roots_ptr = forced_obj;
            *++trx->m_gc_roots_ptr = pred_obj;
            auto [head_obj, source_tail_obj] = lazy_uncons(trx, forced_obj);

            trx->reset_gc_root(2);

            *++trx->m_exec_ptr = source_tail_obj;
            *++trx->m_exec_ptr = pred_obj.make_clone(trx);
            *++trx->m_exec_ptr = Object::make_mark();
            *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atLazyAnyStep);

            *++trx->m_op_ptr = head_obj;
            execute_lazy_proc(trx, pred_obj);
        }
    }
}

// lazy-all: lazy pred -- bool
// throws: execstack-overflow, opstack-underflow, type-check, vm-full
static void lazy_all_op(Trix *trx) {
    trx->verify_operands(VerifyCallable, VerifyLazySeq);

    auto pred_obj = *trx->m_op_ptr;
    auto lazy_obj = *(trx->m_op_ptr - 1);

    if (lazy_obj.is_null()) {
        --trx->m_op_ptr;
        *trx->m_op_ptr = Object::make_boolean(true);
    } else {
        trx->require_exec_capacity(4);

        auto [head_obj, source_tail_obj] = lazy_uncons(trx, lazy_obj);

        // Phase A: after pred(head), check result
        *++trx->m_exec_ptr = source_tail_obj;
        *++trx->m_exec_ptr = pred_obj.make_clone(trx);
        *++trx->m_exec_ptr = Object::make_mark();
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atLazyAllStep);

        --trx->m_op_ptr;
        *trx->m_op_ptr = head_obj;
        execute_lazy_proc(trx, pred_obj);
    }
}

// @lazy-all-step: two-phase control op for lazy-all.
// Phase A (after pred, exec top = mark): boolean on op stack
// Phase B (after force, exec top != mark): forced result on op stack
static void at_lazy_all_step_op(Trix *trx) {
    if (trx->m_exec_ptr->is_mark()) {
        // Phase A: after pred. boolean on op stack.
        trx->verify_operands(VerifyBoolean);
        auto result = trx->m_op_ptr->boolean_value();
        --trx->m_op_ptr;
        trx->m_exec_ptr--;  // pop mark
        auto pred_obj = *trx->m_exec_ptr--;
        auto source_tail = *trx->m_exec_ptr--;

        if (!result) {
            *++trx->m_op_ptr = Object::make_boolean(false);
        } else {
            // Force source_tail, Phase B
            *++trx->m_exec_ptr = pred_obj;
            *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atLazyAllStep);

            *++trx->m_op_ptr = source_tail;
            force_op(trx);
        }
    } else {
        // Phase B: after force.
        trx->require_op_count(1);

        auto forced_obj = *trx->m_op_ptr;
        auto pred_obj = *trx->m_exec_ptr--;

        if (forced_obj.is_null()) {
            *trx->m_op_ptr = Object::make_boolean(true);
        } else {
            verify_lazy_seq_nonempty(trx, forced_obj);
            trx->require_exec_capacity(4);

            // pred_obj was popped off the exec stack; root it across lazy_uncons
            // (ExtValue head-clone can GC under ${...}).  forced_obj stays rooted via
            // its operand slot.
            trx->gc_root_push_oneoff(pred_obj);
            auto [head_obj, source_tail_obj] = lazy_uncons(trx, forced_obj);

            trx->gc_root_pop_oneoff();

            *++trx->m_exec_ptr = source_tail_obj;
            *++trx->m_exec_ptr = pred_obj.make_clone(trx);
            *++trx->m_exec_ptr = Object::make_mark();
            *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atLazyAllStep);

            *trx->m_op_ptr = head_obj;
            execute_lazy_proc(trx, pred_obj);
        }
    }
}

// lazy-find: lazy pred -- val
// throws: execstack-overflow, limit-check, opstack-underflow, range-check, type-check, vm-full
static void lazy_find_op(Trix *trx) {
    trx->verify_operands(VerifyCallable, VerifyLazySeq);

    auto pred_obj = *trx->m_op_ptr;
    auto lazy_obj = *(trx->m_op_ptr - 1);

    if (lazy_obj.is_null()) {
        trx->error(Error::RangeCheck, "lazy-find: no matching element found");
    } else {
        trx->require_exec_capacity(5);

        auto [head_obj, source_tail_obj] = lazy_uncons(trx, lazy_obj);

        // head_obj is a fresh uncons clone that is re-cloned as the saved head
        // below; root it across that re-clone (an ExtValue clone fires a GC under
        // ${...}).  lazy_obj / pred_obj stay rooted via their operand slots.
        trx->gc_root_push_oneoff(head_obj);

        // Phase A: after pred(head), check result. Save head for potential return.
        *++trx->m_exec_ptr = source_tail_obj;
        *++trx->m_exec_ptr = pred_obj.make_clone(trx);
        *++trx->m_exec_ptr = head_obj.make_clone(trx);  // saved head
        *++trx->m_exec_ptr = Object::make_mark();
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atLazyFindStep);

        --trx->m_op_ptr;
        *trx->m_op_ptr = head_obj;

        trx->gc_root_pop_oneoff();
        execute_lazy_proc(trx, pred_obj);
    }
}

// @lazy-find-step: two-phase control op for lazy-find.
// Phase A (after pred, exec top = mark): boolean on op, (source_tail, pred, head, mark) on exec
// Phase B (after force, exec top != mark): forced on op, (pred) on exec
static void at_lazy_find_step_op(Trix *trx) {
    if (trx->m_exec_ptr->is_mark()) {
        // Phase A: after pred. boolean on op.
        trx->verify_operands(VerifyBoolean);
        auto result = trx->m_op_ptr->boolean_value();
        trx->m_exec_ptr--;  // pop mark
        auto head_obj = *trx->m_exec_ptr--;
        auto pred_obj = *trx->m_exec_ptr--;
        auto source_tail = *trx->m_exec_ptr--;

        if (result) {
            *trx->m_op_ptr = head_obj;
        } else {
            head_obj.maybe_free_extvalue(trx);

            // Force source_tail, Phase B
            *++trx->m_exec_ptr = pred_obj;
            *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atLazyFindStep);

            *trx->m_op_ptr = source_tail;
            force_op(trx);
        }
    } else {
        // Phase B: after force.
        trx->require_op_count(1);

        auto forced_obj = *trx->m_op_ptr;
        auto pred_obj = *trx->m_exec_ptr--;

        if (forced_obj.is_null()) {
            trx->error(Error::RangeCheck, "lazy-find: no matching element found");
        } else {
            verify_lazy_seq_nonempty(trx, forced_obj);
            trx->require_exec_capacity(5);

            // pred_obj was popped off the exec stack; root it across lazy_uncons AND
            // the head_obj.make_clone below (both fire a GC under ${...}).  head_obj
            // is a fresh uncons clone re-cloned as the saved head, so root it too.
            // forced_obj stays rooted via its operand slot.
            trx->require_gc_root_capacity(2);

            *++trx->m_gc_roots_ptr = pred_obj;
            auto [head_obj, source_tail_obj] = lazy_uncons(trx, forced_obj);
            *++trx->m_gc_roots_ptr = head_obj;

            *++trx->m_exec_ptr = source_tail_obj;
            *++trx->m_exec_ptr = pred_obj.make_clone(trx);
            *++trx->m_exec_ptr = head_obj.make_clone(trx);
            *++trx->m_exec_ptr = Object::make_mark();
            *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atLazyFindStep);

            *trx->m_op_ptr = head_obj;

            trx->reset_gc_root(2);
            execute_lazy_proc(trx, pred_obj);
        }
    }
}

// lazy-find-index: lazy pred -- int
// throws: execstack-overflow, opstack-underflow, range-check, type-check, vm-full
static void lazy_find_index_op(Trix *trx) {
    trx->verify_operands(VerifyCallable, VerifyLazySeq);

    auto pred_obj = *trx->m_op_ptr;
    auto lazy_obj = *(trx->m_op_ptr - 1);

    if (lazy_obj.is_null()) {
        trx->error(Error::RangeCheck, "lazy-find-index: no matching element found");
    } else {
        trx->require_exec_capacity(5);

        auto [head_obj, source_tail_obj] = lazy_uncons(trx, lazy_obj);

        // Phase A: after pred(head), check result
        *++trx->m_exec_ptr = source_tail_obj;
        *++trx->m_exec_ptr = pred_obj.make_clone(trx);
        *++trx->m_exec_ptr = Object::make_integer(0);
        *++trx->m_exec_ptr = Object::make_mark();
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atLazyFindIndexStep);

        --trx->m_op_ptr;
        *trx->m_op_ptr = head_obj;
        execute_lazy_proc(trx, pred_obj);
    }
}

// @lazy-find-index-step: two-phase control op for lazy-find-index.
// Phase A (after pred, exec top = mark): boolean on op, (source_tail, pred, index, mark) on exec
// Phase B (after force, exec top != mark): forced on op, (pred, index) on exec
static void at_lazy_find_index_step_op(Trix *trx) {
    if (trx->m_exec_ptr->is_mark()) {
        // Phase A: after pred.
        trx->verify_operands(VerifyBoolean);
        auto result = trx->m_op_ptr->boolean_value();
        trx->m_exec_ptr--;  // pop mark
        auto index_obj = *trx->m_exec_ptr--;
        auto pred_obj = *trx->m_exec_ptr--;
        auto source_tail = *trx->m_exec_ptr--;

        if (result) {
            *trx->m_op_ptr = index_obj;
        } else {
            auto index = index_obj.integer_value();

            // Force source_tail, Phase B
            *++trx->m_exec_ptr = pred_obj;
            *++trx->m_exec_ptr = Object::make_integer(index + 1);
            *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atLazyFindIndexStep);

            *trx->m_op_ptr = source_tail;
            force_op(trx);
        }
    } else {
        // Phase B: after force.
        trx->require_op_count(1);

        auto forced_obj = *trx->m_op_ptr;
        auto index_obj = *trx->m_exec_ptr--;
        auto pred_obj = *trx->m_exec_ptr--;

        if (forced_obj.is_null()) {
            trx->error(Error::RangeCheck, "lazy-find-index: no matching element found");
        } else {
            verify_lazy_seq_nonempty(trx, forced_obj);
            trx->require_exec_capacity(5);

            // pred_obj was popped off the exec stack; root it across lazy_uncons
            // (ExtValue head-clone can GC under ${...}).  index_obj is an immediate;
            // forced_obj stays rooted via its operand slot.
            trx->gc_root_push_oneoff(pred_obj);
            auto [head_obj, source_tail_obj] = lazy_uncons(trx, forced_obj);

            trx->gc_root_pop_oneoff();

            *++trx->m_exec_ptr = source_tail_obj;
            *++trx->m_exec_ptr = pred_obj.make_clone(trx);
            *++trx->m_exec_ptr = index_obj;
            *++trx->m_exec_ptr = Object::make_mark();
            *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atLazyFindIndexStep);

            *trx->m_op_ptr = head_obj;
            execute_lazy_proc(trx, pred_obj);
        }
    }
}

// lazy-nth: lazy n -- val
// throws: execstack-overflow, opstack-underflow, range-check, type-check, undefined-result, vm-full
static void lazy_nth_op(Trix *trx) {
    trx->verify_operands(VerifyInteger, VerifyLazySeq);
    auto n_ptr = trx->m_op_ptr;
    auto lazy_ptr = (n_ptr - 1);

    auto n = n_ptr->integer_value();
    if (n < 0) {
        trx->error(Error::RangeCheck, "lazy-nth: index must be non-negative");
    } else {
        auto lazy_obj = *lazy_ptr;
        if (lazy_obj.is_null()) {
            trx->error(Error::RangeCheck, "lazy-nth: index out of range");
        } else if (n == 0) {
            // Just return the head
            auto elem_data = lazy_obj.array_objects(trx);
            auto head_obj = elem_data[Object::LazyHeadIndex].make_clone(trx);
            --trx->m_op_ptr;
            *trx->m_op_ptr = head_obj;
        } else {
            trx->require_exec_capacity(1);

            // Drop n elements, then get head.  Use @lazy-nth-head (not lazy-head)
            // so an index past the end reports lazy-nth's own "index out of range"
            // message instead of lazy-head's generic "lazy-seq is empty".
            *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atLazyNthHead);

            *lazy_ptr = lazy_obj;
            *n_ptr = Object::make_integer(n);
            lazy_drop_op(trx);
        }
    }
}

// @lazy-nth-head: terminal fetch for lazy-nth's n>0 path.  A null operand here
// means the requested index was past the end of the sequence -- report it with
// lazy-nth's own message rather than lazy-head's generic "lazy-seq is empty".
static void at_lazy_nth_head_op(Trix *trx) {
    auto top_ptr = trx->m_op_ptr;
    if (top_ptr->is_null()) {
        trx->error(Error::RangeCheck, "lazy-nth: index out of range");
    } else {
        trx->verify_operands(VerifyLazySeqNonempty);
        *top_ptr = top_ptr->array_objects(trx)[Object::LazyHeadIndex].make_clone(trx);
    }
}

// Walk the lazy chain from `curr`, counting evaluated nodes.  On finding the
// first unevaluated thunk, pushes resume frame [count_literal, curr_hint,
// @lazy-count-step, Pop] and kicks off force_op; caller must return after
// the call.  When fully evaluated, writes the Integer count into op stack top.
static void lazy_drive_count(Trix *trx, Object curr, integer_t count_so_far) {
    while (!curr.is_null()) {
        verify_lazy_seq_nonempty(trx, curr);

        auto elem_data = curr.array_objects(trx);
        auto thunk_s = elem_data[Object::LazyTailThunkIndex].thunk_storage(trx);
        if (thunk_s[Object::ThunkStorageState].integer_value() == Object::ThunkEvaluated) {
            ++count_so_far;
            curr = thunk_s[Object::ThunkStorageResult];
        } else {
            trx->require_op_capacity(1);
            trx->require_exec_capacity(4);

            auto unevaluated_tail = elem_data[Object::LazyTailThunkIndex].make_clone(trx);

            *++trx->m_exec_ptr = Object::make_integer(count_so_far);
            *++trx->m_exec_ptr = curr;  // literal Array; consumed by @step-op
            *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atLazyCountStep);
            *++trx->m_exec_ptr = Object::make_operator(SystemName::Pop);

            *++trx->m_op_ptr = unevaluated_tail;
            force_op(trx);
            return;
        }
    }
    // A misbehaving tail-thunk proc (forced via the async resume path) may have
    // underflowed the operand stack; guard the terminal write so it raises a
    // clean error instead of clobbering an unrelated operand or writing below the
    // stack base, matching at_lazy_to_array_step_op.
    trx->require_op_count(1);
    *trx->m_op_ptr = Object::make_integer(count_so_far);
}

// lazy-count: lazy -- int
// Synchronous fast path walks evaluated thunks; async path forces the first
// unevaluated tail and re-enters via @lazy-count-step (forced thunks are
// cached, so progress is guaranteed).
// throws: execstack-overflow, opstack-overflow, opstack-underflow, type-check, undefined-result, vm-full
static void lazy_count_op(Trix *trx) {
    trx->verify_operands(VerifyLazySeq);

    auto lazy_obj = *trx->m_op_ptr;
    if (lazy_obj.is_null()) {
        *trx->m_op_ptr = Object::make_integer(0);
    } else {
        lazy_drive_count(trx, lazy_obj, 0);
    }
}

// @lazy-count-step: (internal) resumes lazy_drive_count from a saved
// position + partial count after a thunk has been forced.  Exec stack (below
// this op): [count_literal, curr_hint].  Op stack top: original head.
static void at_lazy_count_step_op(Trix *trx) {
    auto curr_hint_obj = *trx->m_exec_ptr--;
    auto count_hint = trx->m_exec_ptr->integer_value();
    --trx->m_exec_ptr;

    lazy_drive_count(trx, curr_hint_obj, count_hint);
}

// lazy-sum: lazy -- num
// throws: execstack-overflow, limit-check, opstack-overflow, opstack-underflow, type-check, vm-full
static void lazy_sum_op(Trix *trx) {
    trx->verify_operands(VerifyLazySeq);
    auto lazy_obj = *trx->m_op_ptr;

    if (lazy_obj.is_null()) {
        *trx->m_op_ptr = Object::make_integer(0);
    } else {
        trx->require_op_capacity(2);

        // Use lazy-fold with 0 and add
        // lazy-fold(lazy, 0, { add })
        // Push 0 and add-operator as proc
        *++trx->m_op_ptr = Object::make_integer(0);
        *++trx->m_op_ptr = Object::make_operator(SystemName::Add);
        lazy_fold_op(trx);
    }
}

// ====================================================================
// Lazy Sequence Operators
// ====================================================================
// A lazy-seq is either null (empty) or a 2-element array [head, tail-thunk]
// where tail-thunk is a Thunk that, when forced, yields the next lazy-seq or null.

// Verify that obj is a non-empty lazy-seq (2-element array with thunk at [1]).
static void verify_lazy_seq_nonempty(Trix *trx, Object obj) {
    if (obj.is_null()) {
        trx->error(Error::RangeCheck, "lazy-seq is empty");
    } else if (!obj.is_array() || (obj.arrays_length() != 2)) {
        trx->error(Error::TypeCheck, "expected lazy-seq (2-element array), got {}", Object::type_sv(obj.type()));
    } else {
        auto elem_data = obj.array_objects(trx);
        if (!elem_data[Object::LazyTailThunkIndex].is_thunk()) {
            trx->error(Error::TypeCheck, "lazy-seq tail must be a thunk");
        }
    }
}

// Execute a user proc that may have been stored as literal in a curry pair.
// Restores the executable attribute before dispatching.
static void execute_lazy_proc(Trix *trx, Object proc) {
    proc.set_executable();
    trx->execute_value(proc);
}

// Decompose a non-empty lazy-seq into (head, tail) with cloning.
// Caller must verify seq is non-null and valid before calling.
[[nodiscard]] static std::pair<Object, Object> lazy_uncons(Trix *trx, Object seq) {
    auto elem_data = seq.array_objects(trx);
    return std::pair{elem_data[Object::LazyHeadIndex].make_clone(trx), elem_data[Object::LazyTailThunkIndex].make_clone(trx)};
}
