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
//                                                                            //
//     https://www.apache.org/licenses/LICENSE-2.0                            //
//                                                                            //
//===----------------------------------------------------------------------===//

//===--- Exec-Stack Operator Descriptor Table ---===//
//
// Single source of truth for "what does this exec-stack control operator do
// during a walk."  Consumed by format_backtrace() (boundary annotations),
// perform_op() / capture_op() (continuation capture err-scan tracking), and
// try_catch_handler() (error unwind decisions).  Replacing four parallel
// `operator_is_X()` predicate chains with table lookups eliminates the err-
// scan drift bug class -- a single descriptor table is the one source of
// truth, so perform_op and capture_op cannot disagree about which ops push
// err-stack companions (e.g. @end-in-global).
//
// PERFORMANCE: roughly neutral.  Walkers it consumes fire on millisecond-
// scale events (fatal error, perform, capture, unwind) -- NOT in the
// interpreter's hot dispatch loop, which is untouched.  Cold-cache fetch
// may be marginally slower than predicate short-circuit; once-warm L1
// makes it a wash.  Justification is structural-fix-of-bug-class, not perf.

// Coarse-grained category of an exec-stack operator.  Drives walker
// dispatch in format_backtrace + the unwind/capture machinery.
enum struct OpKind : uint8_t {
    // Not an exec-stack control op (or not yet catalogued).  Default for
    // every row -- only at* atoms with active walker meaning get a non-None
    // entry.  Rows of None are correct for non-control operators (StandardOp
    // dispatched into via execute_object) and for at* atoms that don't
    // currently surface during a walk (e.g. @lazy-* iterator internals).
    None,

    // @call -- frame-bearer.  Companions on exec at object[-1] (Name) and
    // object[-2] (SourceLoc).  Increments the frame counter in
    // format_backtrace.  Not a barrier; capture/unwind walk straight past.
    Call,

    // @run -- stream-bearer.  Companion on exec at object[+1] (Stream).
    // format_backtrace reads it for the live scanner position.  Not a
    // barrier.
    Run,

    // Bounds an unwind / capture region.  Subclassed by unwind action +
    // companion counts.
    Barrier,

    // Paired closer for an opener (e.g. @end-locals closes @begin-locals
    // by popping the dict-stack frame; @end-in-global closes ${...} by
    // popping the err-stack saved-Boolean).  Walkers treat closers like
    // barriers for stack-tracking purposes.
    Closer,

    // Mid-exec control marker -- not a true unwind boundary but carries
    // an exec-stack companion (saved-depth Integer at object[-1] for
    // @try-rollback / @try-result-complete / @*-for-all).  Capture walks
    // emit a rewrite entry for these.
    Helper,
};

// What try_catch_handler does when it encounters this op while unwinding.
// Distinct from OpKind because two Barriers can have very different unwind
// behavior (e.g. @try-barrier stops the walk; @finally-barrier pops its
// companion and keeps searching).
enum struct UnwindAction : uint8_t {
    // Walker skips past this op.  Used for non-unwind-participating ops
    // (e.g. @loop, @repeat, @time, @dip -- their semantics are independent
    // of error handling).
    None,

    // Unwind stops here.  The barrier marks the "caught by try" point;
    // the walker drops everything above this slot.  Used by @try-barrier.
    Stop,

    // Unwind replaces this marker with a different op then resumes
    // execution at the replacement (e.g. @try-catch-barrier ->
    // @catch-error; @repl-barrier -> @repl-recover; @cell-eval ->
    // @cell-eval-fail).  The specific replacement target is NOT in this
    // descriptor (it's per-atom and lives in the unwind code).
    Replace,

    // Unwind pops the op's err-stack companion(s) and continues searching
    // upward.  Used by @finally-barrier, @with-stream, @delimit-barrier,
    // @effect-barrier, @handler-scope, @end-in-global.
    PopAndContinue,
};

// One row per at* control-op atom in [FIRST_CONTROL_OP..LAST_CONTROL_OP],
// indexed by (atom - OP_DESCRIPTOR_FIRST) -- see OP_DESCRIPTOR_FIRST below.
// Row size: 16-byte header (packed flags/enums + replace_with + pad) +
// 2 * 16-byte string_view = 48 bytes; OP_DESCRIPTOR_COUNT * 48 ~ 7.4 KiB
// constexpr data.  Fits in L1 once warm.
//
// The two `needs_*_rewrite` flags exist because capture_op / perform_op
// build rewrite-offset arrays for ops whose op-depth needs adjustment on
// continuation resume.  err-companion-rewrite ops have an op-depth Integer
// on the err stack (delimit-barrier, handler-scope); exec-companion-rewrite
// ops have a saved-depth Integer on the exec stack (try-rollback,
// try-result-complete, the six for-all variants, and the logic-programming
// barrier family).  These flags drive uniform handling in those walkers.
//
// Field-ordering note: do NOT use bit-fields here.  Plain bool/uint8_t
// only; the compiler handles natural alignment.  Bit-fields cause
// cross-platform surprises and confuse constexpr aggregate init.
struct OpDescriptor {
    OpKind kind;
    UnwindAction unwind;
    uint8_t err_companions;   // err-stack entries this op owns
    uint8_t dict_companions;  // dict-stack entries this op owns
    uint8_t exec_companions;  // exec-stack companions below this op
    bool needs_op_rewrite;    // contributes to ContinuationContext::m_op_rewrite_count
    bool needs_exec_rewrite;  // contributes to ContinuationContext::m_exec_rewrite_count
    // implicit 1-byte pad for SystemName 2-byte alignment
    SystemName replace_with;  // for UnwindAction::Replace barriers: atom to splice in (else: ignored)
    // implicit 6-byte pad for std::string_view 8-byte alignment
    std::string_view bt_label;      // format_backtrace per-frame annotation; empty = none
    std::string_view inside_label;  // format_backtrace "inside:" summary token; empty = skip
};

static_assert(sizeof(OpDescriptor) == 16 + 2 * sizeof(std::string_view), "OpDescriptor packing changed unexpectedly");

// Convenience: a sentinel "no replacement" value for the replace_with
// descriptor field on rows where unwind != Replace.  Picked at the very
// start of the SystemName enum (FIRST_VARIABLE = False) so any accidental
// dereference produces a recognizably-wrong atom rather than a valid one.
static constexpr SystemName NoReplacement{SystemName::FIRST_VARIABLE};

// Sized for just the at* control-operator range [FIRST_CONTROL_OP..
// LAST_CONTROL_OP] rather than the full SYSTEMNAME_COUNT (a dense table over
// every atom would be ~50 KiB) -- only at* atoms can appear on the exec stack
// as control markers, so the rest would be wasted None rows.  Index by
// subtracting OP_DESCRIPTOR_FIRST from the operator's name_index_t.
static constexpr name_index_t OP_DESCRIPTOR_FIRST{+SystemName::FIRST_CONTROL_OP};
static constexpr name_index_t OP_DESCRIPTOR_LAST{+SystemName::LAST_CONTROL_OP};
static constexpr name_index_t OP_DESCRIPTOR_COUNT{OP_DESCRIPTOR_LAST - OP_DESCRIPTOR_FIRST + 1};

// Constexpr-built array.  Every entry defaults to OpKind::None / no
// companions / no label.  Only at* atoms with active walker meaning are
// populated below.
//
// Row field-order (positional):
//   kind, unwind, err_companions, dict_companions, exec_companions,
//   needs_op_rewrite, needs_exec_rewrite, replace_with, bt_label
//
// When adding a new SystemName atom that appears on the exec stack during
// a walk, add a corresponding row here.  The static_assert below catches
// size mismatches; walker code consults sm_op_descriptors[atom] uniformly.
static constexpr auto sm_op_descriptors = []() {
    std::array<OpDescriptor, OP_DESCRIPTOR_COUNT> t{};

    // --- Frame-bearers ---
    t[+SystemName::atCall - OP_DESCRIPTOR_FIRST] = {
            OpKind::Call, UnwindAction::None, 0, 0, 2, false, false, NoReplacement, "", ""};  // [SourceLoc][Name][@call]
    t[+SystemName::atRun - OP_DESCRIPTOR_FIRST] = {
            OpKind::Run, UnwindAction::None, 0, 0, 0, false, false, NoReplacement, "", ""};  // stream at exec[+1]

    // --- Try / finally / with-stream family ---
    // @try-barrier: unwind cuts back the exec stack here (no replacement).
    t[+SystemName::atTryBarrier - OP_DESCRIPTOR_FIRST] = {
            OpKind::Barrier, UnwindAction::Stop, 0, 0, 0, false, false, NoReplacement, "[try boundary]", ""};
    // @try-catch-barrier: unwind replaces with @catch-error (ErrorPushOp side-effect handled by Object flags).
    t[+SystemName::atTryCatchBarrier - OP_DESCRIPTOR_FIRST] = {
            OpKind::Barrier, UnwindAction::Replace, 1, 0, 0, false, false, SystemName::atCatchError, "[try-catch boundary]", ""};
    // @finally-barrier: side-effect Replace -- pops finally-block from err, replaces with @finally-reraise.
    t[+SystemName::atFinallyBarrier - OP_DESCRIPTOR_FIRST] = {
            OpKind::Barrier, UnwindAction::Replace, 1, 0, 0, false, false, SystemName::atFinallyReraise, "[finally boundary]", ""};
    // @with-stream: side-effect Replace -- closes stream from err, replaces with @finally-reraise.
    t[+SystemName::atWithStream - OP_DESCRIPTOR_FIRST] = {OpKind::Barrier,
                                                          UnwindAction::Replace,
                                                          1,
                                                          0,
                                                          0,
                                                          false,
                                                          false,
                                                          SystemName::atFinallyReraise,
                                                          "[with-stream boundary]",
                                                          ""};

    // --- Continuation / effect family (pop companions, keep searching for catcher) ---
    t[+SystemName::atDelimitBarrier - OP_DESCRIPTOR_FIRST] = {
            OpKind::Barrier, UnwindAction::PopAndContinue, 1, 0, 0, true, false, NoReplacement, "[delimit boundary]", ""};
    t[+SystemName::atEffectBarrier - OP_DESCRIPTOR_FIRST] = {
            OpKind::Barrier, UnwindAction::PopAndContinue, 1, 0, 0, false, false, NoReplacement, "[effect boundary]", ""};
    t[+SystemName::atHandlerScope - OP_DESCRIPTOR_FIRST] = {
            OpKind::Barrier, UnwindAction::PopAndContinue, 2, 0, 0, true, false, NoReplacement, "[handler-scope boundary]", ""};

    // --- Global / local alloc scope ---
    t[+SystemName::atEndInGlobal - OP_DESCRIPTOR_FIRST] = {
            OpKind::Closer, UnwindAction::PopAndContinue, 1, 0, 0, false, false, NoReplacement, "[in-global/local boundary]", ""};

    // --- Locals frame ---
    t[+SystemName::atEndLocals - OP_DESCRIPTOR_FIRST] = {
            OpKind::Closer, UnwindAction::None, 0, 1, 0, false, false, NoReplacement, "[locals boundary]", ""};

    // --- Loop / repeat / do-while ---
    t[+SystemName::atLoop - OP_DESCRIPTOR_FIRST] = {
            OpKind::Barrier, UnwindAction::None, 0, 0, 0, false, false, NoReplacement, "[loop boundary]", ""};
    t[+SystemName::atDoWhile - OP_DESCRIPTOR_FIRST] = {
            OpKind::Barrier, UnwindAction::None, 0, 0, 0, false, false, NoReplacement, "[do-while boundary]", ""};
    t[+SystemName::atUIntegerRepeat - OP_DESCRIPTOR_FIRST] = {
            OpKind::Barrier, UnwindAction::None, 0, 0, 0, false, false, NoReplacement, "[repeat boundary]", ""};
    t[+SystemName::atULongRepeat - OP_DESCRIPTOR_FIRST] = {
            OpKind::Barrier, UnwindAction::None, 0, 0, 0, false, false, NoReplacement, "[repeat boundary]", ""};

    // --- For-all family (exec[-1] = saved-depth Integer; needs exec rewrite on capture) ---
    constexpr OpDescriptor forall_row = {
            OpKind::Helper, UnwindAction::None, 0, 0, 1, false, true, NoReplacement, "[for-all boundary]", ""};
    t[+SystemName::atArrayForAll - OP_DESCRIPTOR_FIRST] = forall_row;
    t[+SystemName::atPackedForAll - OP_DESCRIPTOR_FIRST] = forall_row;
    t[+SystemName::atStringForAll - OP_DESCRIPTOR_FIRST] = forall_row;
    t[+SystemName::atDictForAll - OP_DESCRIPTOR_FIRST] = forall_row;
    t[+SystemName::atSetForAll - OP_DESCRIPTOR_FIRST] = forall_row;
    t[+SystemName::atRecordForAll - OP_DESCRIPTOR_FIRST] = forall_row;

    // --- Logic-programming barriers ---
    // Family with op_depth at exec[-2] + save_obj at exec[-1]; needs exec rewrite for op-depth.
    // All Replace under unwind: substitute the matching @*-fail atom.
    t[+SystemName::atNafBarrier - OP_DESCRIPTOR_FIRST] = {
            OpKind::Barrier, UnwindAction::Replace, 0, 0, 2, false, true, SystemName::atNafFail, "[naf boundary]", ""};
    t[+SystemName::atChoiceCountBarrier - OP_DESCRIPTOR_FIRST] = {OpKind::Barrier,
                                                                  UnwindAction::Replace,
                                                                  0,
                                                                  0,
                                                                  2,
                                                                  false,
                                                                  true,
                                                                  SystemName::atChoiceCountFail,
                                                                  "[choice-count boundary]",
                                                                  ""};
    t[+SystemName::atForEachSolutionBarrier - OP_DESCRIPTOR_FIRST] = {OpKind::Barrier,
                                                                      UnwindAction::Replace,
                                                                      0,
                                                                      0,
                                                                      2,
                                                                      false,
                                                                      true,
                                                                      SystemName::atForEachSolutionFail,
                                                                      "[for-each-solution boundary]",
                                                                      ""};
    t[+SystemName::atAggregateBarrier - OP_DESCRIPTOR_FIRST] = {
            OpKind::Barrier, UnwindAction::Replace, 0, 0, 2, false, true, SystemName::atAggregateFail, "[aggregate boundary]", ""};
    t[+SystemName::atAggregateReduceBarrier - OP_DESCRIPTOR_FIRST] = {OpKind::Barrier,
                                                                      UnwindAction::Replace,
                                                                      0,
                                                                      0,
                                                                      2,
                                                                      false,
                                                                      true,
                                                                      SystemName::atAggregateReduceFail,
                                                                      "[aggregate-reduce boundary]",
                                                                      ""};
    // Logic barriers WITHOUT rewrite tracking (no op-depth companion to relocate).
    t[+SystemName::atChoiceBarrier - OP_DESCRIPTOR_FIRST] = {
            OpKind::Barrier, UnwindAction::Replace, 0, 0, 0, false, false, SystemName::atChoiceFail, "[choice boundary]", ""};
    t[+SystemName::atOnceBarrier - OP_DESCRIPTOR_FIRST] = {
            OpKind::Barrier, UnwindAction::Replace, 0, 0, 0, false, false, SystemName::atOnceFail, "[once boundary]", ""};
    t[+SystemName::atFindNBarrier - OP_DESCRIPTOR_FIRST] = {
            OpKind::Barrier, UnwindAction::Replace, 0, 0, 0, false, false, SystemName::atFindNFail, "[find-n boundary]", ""};
    t[+SystemName::atUnifyMatchBarrier - OP_DESCRIPTOR_FIRST] = {OpKind::Barrier,
                                                                 UnwindAction::Replace,
                                                                 0,
                                                                 0,
                                                                 0,
                                                                 false,
                                                                 false,
                                                                 SystemName::atUnifyMatchFail,
                                                                 "[unify-match boundary]",
                                                                 ""};
    t[+SystemName::atFindAllBarrier - OP_DESCRIPTOR_FIRST] = {
            OpKind::Barrier, UnwindAction::Replace, 0, 0, 0, false, false, SystemName::atFindAllFail, "[find-all boundary]", ""};

    // --- Reactive cells + batch ---
    t[+SystemName::atCellEval - OP_DESCRIPTOR_FIRST] = {
            OpKind::Barrier, UnwindAction::Replace, 0, 0, 0, false, false, SystemName::atCellEvalFail, "[cell-eval boundary]", ""};
    t[+SystemName::atBatchEnd - OP_DESCRIPTOR_FIRST] = {
            OpKind::Barrier, UnwindAction::Replace, 0, 0, 0, false, false, SystemName::atBatchFail, "[batch boundary]", ""};
    t[+SystemName::atBatchFire - OP_DESCRIPTOR_FIRST] = {OpKind::Barrier,
                                                         UnwindAction::Replace,
                                                         0,
                                                         0,
                                                         0,
                                                         false,
                                                         false,
                                                         SystemName::atBatchFireFail,
                                                         "[batch-fire boundary]",
                                                         ""};

    // --- Force / thunk ---
    t[+SystemName::atForceComplete - OP_DESCRIPTOR_FIRST] = {
            OpKind::Barrier, UnwindAction::Replace, 0, 0, 0, false, false, SystemName::atForceFail, "[force boundary]", ""};

    // --- REPL: side-effect Replace (stash failing op below /error-name) ---
    t[+SystemName::atReplBarrier - OP_DESCRIPTOR_FIRST] = {
            OpKind::Barrier, UnwindAction::Replace, 0, 0, 0, false, false, SystemName::atReplRecover, "", ""};

    // --- Try-result / try-rollback / tag-update helpers (exec[-1] = saved-depth; needs exec rewrite) ---
    constexpr OpDescriptor exec_helper_row = {OpKind::Helper, UnwindAction::None, 0, 0, 1, false, true, NoReplacement, "", ""};
    t[+SystemName::atTryRollback - OP_DESCRIPTOR_FIRST] = exec_helper_row;
    t[+SystemName::atTryResultComplete - OP_DESCRIPTOR_FIRST] = exec_helper_row;
    t[+SystemName::atTagUpdateComplete - OP_DESCRIPTOR_FIRST] = exec_helper_row;

    // --- Misc barriers / closers without companions ---
    t[+SystemName::atStop - OP_DESCRIPTOR_FIRST] = {
            OpKind::Barrier, UnwindAction::None, 0, 0, 0, false, false, NoReplacement, "[stop boundary]", ""};
    t[+SystemName::atDip - OP_DESCRIPTOR_FIRST] = {
            OpKind::Barrier, UnwindAction::None, 0, 0, 0, false, false, NoReplacement, "[dip boundary]", ""};
    t[+SystemName::atTime - OP_DESCRIPTOR_FIRST] = {
            OpKind::Barrier, UnwindAction::None, 0, 0, 0, false, false, NoReplacement, "[time boundary]", ""};
    t[+SystemName::atEndModule - OP_DESCRIPTOR_FIRST] = {
            OpKind::Closer, UnwindAction::None, 0, 0, 0, false, false, NoReplacement, "", ""};
    t[+SystemName::atClosureEnd - OP_DESCRIPTOR_FIRST] = {
            OpKind::Closer, UnwindAction::None, 0, 0, 0, false, false, NoReplacement, "", ""};

    // "inside:" short labels.  Rows without an inside_label here
    // are skipped by the inside-summary builder (default `""`).  Only the
    // contexts that meaningfully wrap user code show up -- loops/repeats/
    // for-all/dip/time, locals frames, global-alloc scopes, delimit,
    // effect, handler-scope.  Try/finally/with-stream Replace barriers
    // would have caught the error, so they never appear in a fatal trace
    // and don't need inside_labels.
    t[+SystemName::atLoop - OP_DESCRIPTOR_FIRST].inside_label = "loop";
    t[+SystemName::atDoWhile - OP_DESCRIPTOR_FIRST].inside_label = "do-while";
    t[+SystemName::atUIntegerRepeat - OP_DESCRIPTOR_FIRST].inside_label = "repeat";
    t[+SystemName::atULongRepeat - OP_DESCRIPTOR_FIRST].inside_label = "repeat";
    t[+SystemName::atArrayForAll - OP_DESCRIPTOR_FIRST].inside_label = "for-all";
    t[+SystemName::atPackedForAll - OP_DESCRIPTOR_FIRST].inside_label = "for-all";
    t[+SystemName::atStringForAll - OP_DESCRIPTOR_FIRST].inside_label = "for-all";
    t[+SystemName::atDictForAll - OP_DESCRIPTOR_FIRST].inside_label = "for-all";
    t[+SystemName::atSetForAll - OP_DESCRIPTOR_FIRST].inside_label = "for-all";
    t[+SystemName::atRecordForAll - OP_DESCRIPTOR_FIRST].inside_label = "for-all";
    t[+SystemName::atDip - OP_DESCRIPTOR_FIRST].inside_label = "dip";
    t[+SystemName::atTime - OP_DESCRIPTOR_FIRST].inside_label = "time";
    t[+SystemName::atEndLocals - OP_DESCRIPTOR_FIRST].inside_label = "|locals|";
    t[+SystemName::atEndInGlobal - OP_DESCRIPTOR_FIRST].inside_label = "in-global/local";
    t[+SystemName::atDelimitBarrier - OP_DESCRIPTOR_FIRST].inside_label = "delimit";
    t[+SystemName::atEffectBarrier - OP_DESCRIPTOR_FIRST].inside_label = "effect";
    t[+SystemName::atHandlerScope - OP_DESCRIPTOR_FIRST].inside_label = "handler-scope";

    return t;
}();

static_assert(sm_op_descriptors.size() == OP_DESCRIPTOR_COUNT,
              "OpDescriptor table size does not match FIRST_CONTROL_OP..LAST_CONTROL_OP range");

// Compile-time sanity checks: lock in the err/dict/exec companion counts
// that the walker code (currently in ops_effect.inl / ops_continuation.inl /
// ops_system.inl) depends on.  If a table row is edited to disagree with
// what a walker reads, the build breaks at this assert -- not at fuzz time
// or at customer-trace time.

// Frame-bearers
static_assert(sm_op_descriptors[+SystemName::atCall - OP_DESCRIPTOR_FIRST].kind == OpKind::Call);
static_assert(sm_op_descriptors[+SystemName::atCall - OP_DESCRIPTOR_FIRST].exec_companions == 2);  // [SourceLoc][Name][@call]
static_assert(sm_op_descriptors[+SystemName::atRun - OP_DESCRIPTOR_FIRST].kind == OpKind::Run);

// Try / finally / with-stream
static_assert(sm_op_descriptors[+SystemName::atTryBarrier - OP_DESCRIPTOR_FIRST].unwind == UnwindAction::Stop);
static_assert(sm_op_descriptors[+SystemName::atTryBarrier - OP_DESCRIPTOR_FIRST].err_companions == 0);
static_assert(sm_op_descriptors[+SystemName::atTryCatchBarrier - OP_DESCRIPTOR_FIRST].unwind == UnwindAction::Replace);
static_assert(sm_op_descriptors[+SystemName::atTryCatchBarrier - OP_DESCRIPTOR_FIRST].err_companions == 1);
static_assert(sm_op_descriptors[+SystemName::atFinallyBarrier - OP_DESCRIPTOR_FIRST].err_companions == 1);
static_assert(sm_op_descriptors[+SystemName::atWithStream - OP_DESCRIPTOR_FIRST].err_companions == 1);

// Continuation / effect family -- matches perform_op + capture_op walks
static_assert(sm_op_descriptors[+SystemName::atDelimitBarrier - OP_DESCRIPTOR_FIRST].err_companions == 1);
static_assert(sm_op_descriptors[+SystemName::atEffectBarrier - OP_DESCRIPTOR_FIRST].err_companions == 1);
static_assert(sm_op_descriptors[+SystemName::atHandlerScope - OP_DESCRIPTOR_FIRST].err_companions == 2);
static_assert(sm_op_descriptors[+SystemName::atEndInGlobal - OP_DESCRIPTOR_FIRST].err_companions == 1);
static_assert(sm_op_descriptors[+SystemName::atEndLocals - OP_DESCRIPTOR_FIRST].dict_companions == 1);

// For-all family + try-rollback helpers: saved-depth Integer at exec[-1]
static_assert(sm_op_descriptors[+SystemName::atArrayForAll - OP_DESCRIPTOR_FIRST].exec_companions == 1);
static_assert(sm_op_descriptors[+SystemName::atRecordForAll - OP_DESCRIPTOR_FIRST].exec_companions == 1);
static_assert(sm_op_descriptors[+SystemName::atTryRollback - OP_DESCRIPTOR_FIRST].exec_companions == 1);
static_assert(sm_op_descriptors[+SystemName::atTryResultComplete - OP_DESCRIPTOR_FIRST].exec_companions == 1);

// Logic-programming family: [op_depth][save_obj][<barrier>] layout, 2 exec companions
static_assert(sm_op_descriptors[+SystemName::atNafBarrier - OP_DESCRIPTOR_FIRST].exec_companions == 2);
static_assert(sm_op_descriptors[+SystemName::atChoiceCountBarrier - OP_DESCRIPTOR_FIRST].exec_companions == 2);
static_assert(sm_op_descriptors[+SystemName::atForEachSolutionBarrier - OP_DESCRIPTOR_FIRST].exec_companions == 2);
static_assert(sm_op_descriptors[+SystemName::atAggregateBarrier - OP_DESCRIPTOR_FIRST].exec_companions == 2);
static_assert(sm_op_descriptors[+SystemName::atAggregateReduceBarrier - OP_DESCRIPTOR_FIRST].exec_companions == 2);

// Rewrite-flag invariants: which ops need op-depth offset rewriting in
// captured continuations.  Must match perform_op + capture_op walks.
static_assert(sm_op_descriptors[+SystemName::atDelimitBarrier - OP_DESCRIPTOR_FIRST].needs_op_rewrite);
static_assert(sm_op_descriptors[+SystemName::atHandlerScope - OP_DESCRIPTOR_FIRST].needs_op_rewrite);
static_assert(!sm_op_descriptors[+SystemName::atEffectBarrier - OP_DESCRIPTOR_FIRST].needs_op_rewrite);
static_assert(!sm_op_descriptors[+SystemName::atTryCatchBarrier - OP_DESCRIPTOR_FIRST].needs_op_rewrite);
static_assert(sm_op_descriptors[+SystemName::atTryRollback - OP_DESCRIPTOR_FIRST].needs_exec_rewrite);
static_assert(sm_op_descriptors[+SystemName::atTryResultComplete - OP_DESCRIPTOR_FIRST].needs_exec_rewrite);
static_assert(sm_op_descriptors[+SystemName::atArrayForAll - OP_DESCRIPTOR_FIRST].needs_exec_rewrite);
static_assert(sm_op_descriptors[+SystemName::atRecordForAll - OP_DESCRIPTOR_FIRST].needs_exec_rewrite);
static_assert(sm_op_descriptors[+SystemName::atNafBarrier - OP_DESCRIPTOR_FIRST].needs_exec_rewrite);
static_assert(sm_op_descriptors[+SystemName::atAggregateBarrier - OP_DESCRIPTOR_FIRST].needs_exec_rewrite);
static_assert(!sm_op_descriptors[+SystemName::atTryBarrier - OP_DESCRIPTOR_FIRST].needs_exec_rewrite);

// Unwind-action + replace_with invariants: must match try_catch_handler.
static_assert(sm_op_descriptors[+SystemName::atTryBarrier - OP_DESCRIPTOR_FIRST].unwind == UnwindAction::Stop);
static_assert(sm_op_descriptors[+SystemName::atTryCatchBarrier - OP_DESCRIPTOR_FIRST].replace_with == SystemName::atCatchError);
static_assert(sm_op_descriptors[+SystemName::atFinallyBarrier - OP_DESCRIPTOR_FIRST].replace_with == SystemName::atFinallyReraise);
static_assert(sm_op_descriptors[+SystemName::atWithStream - OP_DESCRIPTOR_FIRST].replace_with == SystemName::atFinallyReraise);
static_assert(sm_op_descriptors[+SystemName::atChoiceBarrier - OP_DESCRIPTOR_FIRST].replace_with == SystemName::atChoiceFail);
static_assert(sm_op_descriptors[+SystemName::atCellEval - OP_DESCRIPTOR_FIRST].replace_with == SystemName::atCellEvalFail);
static_assert(sm_op_descriptors[+SystemName::atBatchEnd - OP_DESCRIPTOR_FIRST].replace_with == SystemName::atBatchFail);
static_assert(sm_op_descriptors[+SystemName::atFindAllBarrier - OP_DESCRIPTOR_FIRST].replace_with == SystemName::atFindAllFail);
static_assert(sm_op_descriptors[+SystemName::atReplBarrier - OP_DESCRIPTOR_FIRST].replace_with == SystemName::atReplRecover);
static_assert(sm_op_descriptors[+SystemName::atDelimitBarrier - OP_DESCRIPTOR_FIRST].unwind == UnwindAction::PopAndContinue);
static_assert(sm_op_descriptors[+SystemName::atEffectBarrier - OP_DESCRIPTOR_FIRST].unwind == UnwindAction::PopAndContinue);
static_assert(sm_op_descriptors[+SystemName::atHandlerScope - OP_DESCRIPTOR_FIRST].unwind == UnwindAction::PopAndContinue);
static_assert(sm_op_descriptors[+SystemName::atEndInGlobal - OP_DESCRIPTOR_FIRST].unwind == UnwindAction::PopAndContinue);

// Spot-check: an at* atom we don't catalogue (atCatchError is inserted by
// unwind itself, never appears mid-walk) stays None by default.
static_assert(sm_op_descriptors[+SystemName::atCatchError - OP_DESCRIPTOR_FIRST].kind == OpKind::None);
static_assert(sm_op_descriptors[+SystemName::atCatchError - OP_DESCRIPTOR_FIRST].bt_label.empty());

//===--- ACCESSOR INTERFACE ---===//
//
// Source files OTHER THAN op_descriptor.inl must NOT access
// sm_op_descriptors[] directly.  Always go through the accessor
// functions below.  This keeps the table indexing scheme (offset by
// OP_DESCRIPTOR_FIRST), bounds checking, and any future representation
// change (e.g. perfect hashing, sparse map) confined to this file.

// Range check: is this raw operator value in the at* control-op range?
// Walkers gate ALL descriptor lookups on this first.
[[nodiscard]] static constexpr bool is_control_op_atom(int operator_value) {
    return ((operator_value >= OP_DESCRIPTOR_FIRST) && (operator_value <= OP_DESCRIPTOR_LAST));
}

// Look up the descriptor for a control-op atom by SystemName enum.
// Caller must guarantee atom is in [FIRST_CONTROL_OP, LAST_CONTROL_OP].
[[nodiscard]] static constexpr const OpDescriptor &op_descriptor_for(SystemName atom) {
    return sm_op_descriptors[+atom - OP_DESCRIPTOR_FIRST];
}

// Look up the descriptor for a control-op atom by raw operator value.
// Caller MUST have already verified is_control_op_atom(operator_value).
[[nodiscard]] static constexpr const OpDescriptor &op_descriptor_for(int operator_value) {
    return sm_op_descriptors[static_cast<size_t>(operator_value - OP_DESCRIPTOR_FIRST)];
}

// Convenience: returns the backtrace label for this operator (or empty
// view if not a control op / no label).  Single-call replacement for
// the format_backtrace boundary-annotation pattern.
[[nodiscard]] static constexpr std::string_view op_bt_label(int operator_value) {
    if (!is_control_op_atom(operator_value)) {
        return {};
    } else {
        return op_descriptor_for(operator_value).bt_label;
    }
}

// Convenience: returns the "inside:" summary label for this operator,
// or empty for non-control atoms / contexts not surfaced in the
// summary line.
[[nodiscard]] static constexpr std::string_view op_inside_label(int operator_value) {
    if (!is_control_op_atom(operator_value)) {
        return {};
    } else {
        return op_descriptor_for(operator_value).inside_label;
    }
}

// Convenience: returns 0 for non-control-op atoms; otherwise the descriptor's
// err_companions count.  Useful for err-scan tracking walks that need a
// single value per slot regardless of whether the op is in the table.
[[nodiscard]] static constexpr uint8_t op_err_companions(int operator_value) {
    return (is_control_op_atom(operator_value) ? op_descriptor_for(operator_value).err_companions : uint8_t{0});
}
