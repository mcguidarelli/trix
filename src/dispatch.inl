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

//===--- Consteval-built dispatch tables ---===//
//
// The row lists below are the SPECIFICATION for the three dispatch tables:
// sysvariable name strings, wellknown name strings, and sysoperator
// Operator{function, name} entries.  Each build_* function places every row
// at its enum index during constant evaluation, and verify_dispatch_tables()
// -- static_assert'ed right after class Trix closes (trix.h) -- proves
// row-count == slot-count with no slot left empty: by pigeonhole, every enum
// index is covered exactly once (no holes, no duplicates).  A row pointing
// outside its table fails the build on its own (an out-of-bounds std::array
// access is not a constant expression).
//
// TRIX_DEBUGGER / TRIX_HEAP_TRACKING rows sit inside the same #ifdef blocks
// as their SystemName enum entries (enums.inl), so the tables stay dense and
// the verification holds in every build configuration.
//
// These tables replaced three switches totalling ~3,500 lines; the 1015-case
// sysoperator_value switch needed GCC's switch-lowering case cap raised
// (--param=switch-lower-slow-alg-max-cases) just to compile warning-free.
//
// Debugging a static_assert failure: verify_dispatch_tables() checks each
// row list in source order (sysvariable, wellknown, sysoperator) -- replace
// the single trix.h static_assert with per-table probes to bisect, or diff
// the row edit that triggered the failure against enums.inl.

struct SysVariableRow {
    SystemName m_name;
    std::string_view m_sv;
};

struct WellKnownRow {
    WellKnownName m_name;
    std::string_view m_sv;
};

struct SysOperatorRow {
    SystemName m_name;
    operator_func_t m_func;
    std::string_view m_sv;
};

[[nodiscard]] static consteval std::span<const SysVariableRow> sysvariable_rows() {
    using namespace std::literals::string_view_literals;

    static constexpr SysVariableRow rows[] = {
            {         SystemName::False,           "false"sv},
            {          SystemName::True,            "true"sv},
            {          SystemName::Null,            "null"sv},
            {           SystemName::Inf,             "inf"sv},
            {          SystemName::NaNR,             "nan"sv},
            {    SystemName::InfRSuffix,           "inf#r"sv},
            {    SystemName::NaNRSuffix,           "nan#r"sv},
            {          SystemName::InfD,           "inf#d"sv},
            {          SystemName::NaND,           "nan#d"sv},
            {         SystemName::StdIn,           "stdin"sv},
            {       SystemName::StdEdit,         "stdedit"sv},
            {        SystemName::StdOut,          "stdout"sv},
            {        SystemName::StdErr,          "stderr"sv},
            {      SystemName::IsSigned,       "is-signed"sv},
            {      SystemName::IsIEC559,       "is-iec559"sv},
            {SystemName::TinynessBefore, "tinyness-before"sv},
            {        SystemName::Digits,          "digits"sv},
            {      SystemName::Digits10,        "digits10"sv},
            {   SystemName::MaxDigits10,    "max-digits10"sv},
            {         SystemName::Radix,           "radix"sv},
            {   SystemName::MinExponent,    "min-exponent"sv},
            { SystemName::MinExponent10,  "min-exponent10"sv},
            {   SystemName::MaxExponent,    "max-exponent"sv},
            { SystemName::MaxExponent10,  "max-exponent10"sv},
            {        SystemName::Lowest,          "lowest"sv},
            {       SystemName::Epsilon,         "epsilon"sv},
            {      SystemName::Infinity,        "infinity"sv},
            {    SystemName::RoundError,     "round-error"sv},
            {      SystemName::QuietNaN,       "quiet-NaN"sv},
            {  SystemName::SignalingNaN,   "signaling-NaN"sv},
            {     SystemName::DenormMin,      "denorm-min"sv},
            {             SystemName::E,               "e"sv},
            {         SystemName::Log2e,           "log2e"sv},
            {        SystemName::Log10e,          "log10e"sv},
            {            SystemName::Pi,              "pi"sv},
            {         SystemName::InvPi,          "inv-pi"sv},
            {     SystemName::InvSqrtPi,     "inv-sqrt-pi"sv},
            {           SystemName::Ln2,             "ln2"sv},
            {          SystemName::Ln10,            "ln10"sv},
            {         SystemName::Sqrt2,           "sqrt2"sv},
            {         SystemName::Sqrt3,           "sqrt3"sv},
            {      SystemName::InvSqrt3,      "inv-sqrt-3"sv},
            {        SystemName::EGamma,          "egamma"sv},
            {           SystemName::Phi,             "phi"sv},
            {     SystemName::FP_Normal,       "fp-normal"sv},
            {  SystemName::FP_SubNormal,    "fp-subnormal"sv},
            {       SystemName::FP_Zero,         "fp-zero"sv},
            {   SystemName::FP_Infinite,     "fp-infinite"sv},
            {        SystemName::FP_NaN,          "fp-nan"sv},
            {      SystemName::FP_Other,        "fp-other"sv},
            {  SystemName::FE_DivByZero,  "fe-div-by-zero"sv},
            {    SystemName::FE_Inexact,      "fe-inexact"sv},
            {    SystemName::FE_Invalid,      "fe-invalid"sv},
            {   SystemName::FE_Overflow,     "fe-overflow"sv},
            {  SystemName::FE_Underflow,    "fe-underflow"sv},
            { SystemName::FE_All_Except,   "fe-all-except"sv},
            {   SystemName::FE_Downward,     "fe-downward"sv},
            {  SystemName::FE_ToNearest,   "fe-to-nearest"sv},
            { SystemName::FE_TowardZero,  "fe-toward-zero"sv},
            {     SystemName::FE_Upward,       "fe-upward"sv},
            {      SystemName::FE_Other,        "fe-other"sv},
            {    SystemName::FE_Default,      "fe-default"sv},
            {       SystemName::Numbers,         "numbers"sv},
            {SystemName::Classification,  "classification"sv},
            {   SystemName::Environment,     "environment"sv},
            {     SystemName::ErrorDict,       "errordict"sv},
            {  SystemName::HandlersDict,    "handlersdict"sv},
            {    SystemName::SystemDict,      "systemdict"sv},
            {      SystemName::UserDict,        "userdict"sv},
            {  SystemName::Addr_Invalid,    "addr-invalid"sv},
            {  SystemName::Addr_NullPtr,       "addr-null"sv},
            { SystemName::Addr_ReadOnly,  "addr-read-only"sv},
            {SystemName::Addr_ReadWrite, "addr-read-write"sv},
    };
    return rows;
}

[[nodiscard]] static consteval std::span<const WellKnownRow> wellknown_rows() {
    using namespace std::literals::string_view_literals;

    static constexpr WellKnownRow rows[] = {
            {         WellKnownName::Default,          "default"sv},
            {              WellKnownName::Ok,               "ok"sv},
            {             WellKnownName::Err,              "err"sv},
            {        WellKnownName::Sleeping,         "sleeping"sv},
            {           WellKnownName::Ready,            "ready"sv},
            {         WellKnownName::Running,          "running"sv},
            {            WellKnownName::Dead,             "dead"sv},
            {       WellKnownName::Suspended,        "suspended"sv},
            {          WellKnownName::Normal,           "normal"sv},
            {         WellKnownName::NoError,         "no-error"sv},
            {          WellKnownName::Killed,           "killed"sv},
            {        WellKnownName::Shutdown,         "shutdown"sv},
            {            WellKnownName::Down,             "down"sv},
            {            WellKnownName::Exit,             "exit"sv},
            {          WellKnownName::RefKey,              "ref"sv},
            {        WellKnownName::ActorKey,            "actor"sv},
            {       WellKnownName::ReasonKey,           "reason"sv},
            {            WellKnownName::LVar,             "lvar"sv},
            {      WellKnownName::StartChild,      "start-child"sv},
            {  WellKnownName::TerminateChild,  "terminate-child"sv},
            {    WellKnownName::RestartChild,    "restart-child"sv},
            {         WellKnownName::SupStop,             "stop"sv},
            {         WellKnownName::GenCall,         "gen-call"sv},
            {         WellKnownName::GenCast,         "gen-cast"sv},
            {         WellKnownName::GenStop,         "gen-stop"sv},
            {           WellKnownName::Reply,            "reply"sv},
            {         WellKnownName::NoReply,          "noreply"sv},
            {            WellKnownName::Init,             "init"sv},
            {      WellKnownName::HandleCall,      "handle-call"sv},
            {      WellKnownName::HandleCast,      "handle-cast"sv},
            {      WellKnownName::HandleInfo,      "handle-info"sv},
            {       WellKnownName::Terminate,        "terminate"sv},
            {           WellKnownName::XfMap,           "xf-map"sv},
            {        WellKnownName::XfFilter,        "xf-filter"sv},
            {          WellKnownName::XfTake,          "xf-take"sv},
            {          WellKnownName::XfDrop,          "xf-drop"sv},
            {          WellKnownName::XfScan,          "xf-scan"sv},
            {       WellKnownName::XfFlatten,       "xf-flatten"sv},
            {      WellKnownName::XfDistinct,      "xf-distinct"sv},
            {      WellKnownName::IKeyStatus,           "status"sv},
            {WellKnownName::IKeyMailboxCount,    "mailbox-count"sv},
            {  WellKnownName::IKeyMailboxCap, "mailbox-capacity"sv},
            {   WellKnownName::IKeyHasJoiner,       "has-joiner"sv},
            {    WellKnownName::IKeyTrapExit,        "trap-exit"sv},
            {WellKnownName::IKeyMonitorCount,    "monitor-count"sv},
            {        WellKnownName::IKeyName,             "name"sv},
            {       WellKnownName::IKeyCount,            "count"sv},
            {    WellKnownName::IKeyCapacity,         "capacity"sv},
            {      WellKnownName::IKeyClosed,           "closed"sv},
            {       WellKnownName::IKeyError,            "error"sv},
            { WellKnownName::IKeyHasProducer,     "has-producer"sv},
            { WellKnownName::IKeyHasConsumer,     "has-consumer"sv},
            {    WellKnownName::IKeyStrategy,         "strategy"sv},
            {   WellKnownName::IKeyIntensity,        "intensity"sv},
            {      WellKnownName::IKeyPeriod,           "period"sv},
            {  WellKnownName::IKeyChildCount,      "child-count"sv},
            { WellKnownName::IKeyActiveCount,     "active-count"sv},
            { WellKnownName::IKeyMaxChildren,     "max-children"sv},
            {WellKnownName::IKeyRestartCount,    "restart-count"sv},
            {        WellKnownName::IKeyType,             "type"sv},
            {      WellKnownName::IKeySource,           "source"sv},
            {        WellKnownName::IKeyProc,             "proc"sv},
            {           WellKnownName::IKeyN,                "n"sv},
            {        WellKnownName::IKeyInit,             "init"sv},
            {     WellKnownName::IKeySources,          "sources"sv},
            {             WellKnownName::Add,              "add"sv},
            {             WellKnownName::Sub,              "sub"sv},
            {             WellKnownName::Mul,              "mul"sv},
            {             WellKnownName::Div,              "div"sv},
            {             WellKnownName::Mod,              "mod"sv},
            {             WellKnownName::Pow,              "pow"sv},
            {              WellKnownName::Or,               "or"sv},
            {             WellKnownName::Xor,              "xor"sv},
            {             WellKnownName::And,              "and"sv},
            {              WellKnownName::Eq,               "eq"sv},
            {              WellKnownName::Ne,               "ne"sv},
            {              WellKnownName::Lt,               "lt"sv},
            {              WellKnownName::Le,               "le"sv},
            {              WellKnownName::Gt,               "gt"sv},
            {              WellKnownName::Ge,               "ge"sv},
            {       WellKnownName::ShiftLeft,       "shift-left"sv},
            {      WellKnownName::ShiftRight,      "shift-right"sv},
            {             WellKnownName::Neg,              "neg"sv},
            {             WellKnownName::Not,              "not"sv},
            {         WellKnownName::Promote,          "promote"sv},
            {             WellKnownName::Rot,              "rot"sv},
            {          WellKnownName::Select,           "select"sv},
            {             WellKnownName::Get,              "get"sv},
    };
    return rows;
}

[[nodiscard]] static consteval std::span<const SysOperatorRow> sysoperator_rows() {
    using namespace std::literals::string_view_literals;

    // Narrow aliases for the templated type predicates: the full
    // is_type_pred_op<&Object::is_xxx> spelling is the widest entry in the
    // function column and drives clang-format's alignment for every row of
    // the table.  Same pointer values -- table content is unchanged.
    // throws: opstack-underflow
    constexpr auto is_address_op{is_type_pred_op<&Object::is_address>};
    // throws: opstack-underflow
    constexpr auto is_array_op{is_type_pred_op<&Object::is_array>};
    // throws: opstack-underflow
    constexpr auto is_boolean_op{is_type_pred_op<&Object::is_boolean>};
    // throws: opstack-underflow
    constexpr auto is_byte_op{is_type_pred_op<&Object::is_byte>};
    // throws: opstack-underflow
    constexpr auto is_continuation_op{is_type_pred_op<&Object::is_continuation>};
    // throws: opstack-underflow
    constexpr auto is_coroutine_op{is_type_pred_op<&Object::is_coroutine>};
    // throws: opstack-underflow
    constexpr auto is_curry_op{is_type_pred_op<&Object::is_curry>};
    // throws: opstack-underflow
    constexpr auto is_dict_op{is_type_pred_op<&Object::is_dict>};
    // throws: opstack-underflow
    constexpr auto is_double_op{is_type_pred_op<&Object::is_double>};
    // throws: opstack-underflow
    constexpr auto is_floating_point_op{is_type_pred_op<&Object::is_floating_point>};
    // throws: opstack-underflow
    constexpr auto is_int128_op{is_type_pred_op<&Object::is_int128>};
    // throws: opstack-underflow
    constexpr auto is_integer_op{is_type_pred_op<&Object::is_integer>};
    // throws: opstack-underflow
    constexpr auto is_long_op{is_type_pred_op<&Object::is_long>};
    // throws: opstack-underflow
    constexpr auto is_mark_op{is_type_pred_op<&Object::is_mark>};
    // throws: opstack-underflow
    constexpr auto is_name_op{is_type_pred_op<&Object::is_name>};
    // throws: opstack-underflow
    constexpr auto is_null_op{is_type_pred_op<&Object::is_null>};
    // throws: opstack-underflow
    constexpr auto is_operator_op{is_type_pred_op<&Object::is_operator>};
    // throws: opstack-underflow
    constexpr auto is_packed_op{is_type_pred_op<&Object::is_packed>};
    // throws: opstack-underflow
    constexpr auto is_pipe_buffer_op{is_type_pred_op<&Object::is_pipe_buffer>};
    // throws: opstack-underflow
    constexpr auto is_real_op{is_type_pred_op<&Object::is_real>};
    // throws: opstack-underflow
    constexpr auto is_record_op{is_type_pred_op<&Object::is_record>};
    // throws: opstack-underflow
    constexpr auto is_set_op{is_type_pred_op<&Object::is_set>};
    // throws: opstack-underflow
    constexpr auto is_signed_integral_op{is_type_pred_op<&Object::is_signed_integral>};
    // throws: opstack-underflow
    constexpr auto is_stream_op{is_type_pred_op<&Object::is_stream>};
    // throws: opstack-underflow
    constexpr auto is_string_op{is_type_pred_op<&Object::is_string>};
    // throws: opstack-underflow
    constexpr auto is_tagged_op{is_type_pred_op<&Object::is_tagged>};
    // throws: opstack-underflow
    constexpr auto is_thunk_op{is_type_pred_op<&Object::is_thunk>};
    // throws: opstack-underflow
    constexpr auto is_uint128_op{is_type_pred_op<&Object::is_uint128>};
    // throws: opstack-underflow
    constexpr auto is_uinteger_op{is_type_pred_op<&Object::is_uinteger>};
    // throws: opstack-underflow
    constexpr auto is_ulong_op{is_type_pred_op<&Object::is_ulong>};
    // throws: opstack-underflow
    constexpr auto is_unsigned_integral_op{is_type_pred_op<&Object::is_unsigned_integral>};

    static constexpr SysOperatorRow rows[] = {
            {                       SystemName::atRun,                            at_run_op,                            "@run"sv},
            {                      SystemName::atStop,                           at_stop_op,                           "@stop"sv},
            {                SystemName::atTryBarrier,                     at_trybarrier_op,                    "@try-barrier"sv},
            {           SystemName::atTryCatchBarrier,                at_trycatchbarrier_op,              "@try-catch-barrier"sv},
            {                SystemName::atCatchError,                     at_catcherror_op,                    "@catch-error"sv},
            {            SystemName::atFinallyBarrier,                 at_finallybarrier_op,                "@finally-barrier"sv},
            {            SystemName::atFinallyReraise,                at_finally_reraise_op,                "@finally-reraise"sv},
            {                SystemName::atWithStream,                     at_withstream_op,                    "@with-stream"sv},
            {                      SystemName::atLoop,                           at_loop_op,                           "@loop"sv},
            {            SystemName::atUIntegerRepeat,                at_uinteger_repeat_op,                "@uinteger-repeat"sv},
            {               SystemName::atULongRepeat,                   at_ulong_repeat_op,                   "@ulong-repeat"sv},
            {               SystemName::atArrayForAll,                   at_array_forall_op,                  "@array-for-all"sv},
            {              SystemName::atPackedForAll,                  at_packed_forall_op,                 "@packed-for-all"sv},
            {              SystemName::atStringForAll,                  at_string_forall_op,                 "@string-for-all"sv},
            {                SystemName::atDictForAll,                    at_dict_forall_op,                   "@dict-for-all"sv},
            {                   SystemName::atDictMap,                       at_dict_map_op,                       "@dict-map"sv},
            {                SystemName::atDictFilter,                    at_dict_filter_op,                    "@dict-filter"sv},
            {                 SystemName::atSetFilter,                     at_set_filter_op,                     "@set-filter"sv},
            {                   SystemName::atByteFor,                       at_byte_for_op,                       "@byte-for"sv},
            {                SystemName::atIntegerFor,                    at_integer_for_op,                    "@integer-for"sv},
            {               SystemName::atUIntegerFor,                   at_uinteger_for_op,                   "@uinteger-for"sv},
            {                   SystemName::atLongFor,                       at_long_for_op,                       "@long-for"sv},
            {                  SystemName::atULongFor,                      at_ulong_for_op,                      "@ulong-for"sv},
            {                SystemName::atAddressFor,                    at_address_for_op,                    "@address-for"sv},
            {                   SystemName::atRealFor,                       at_real_for_op,                       "@real-for"sv},
            {                 SystemName::atDoubleFor,                     at_double_for_op,                     "@double-for"sv},
            {                      SystemName::atCall,                           at_call_op,                           "@call"sv},
            {                       SystemName::atDip,                            at_dip_op,                            "@dip"sv},
            {                  SystemName::atArrayMap,                      at_array_map_op,                      "@array-map"sv},
            {           SystemName::atArrayMapIndexed,              at_array_map_indexed_op,              "@array-map-indexed"sv},
            {                    SystemName::atUpdate,                         at_update_op,                         "@update"sv},
            {             SystemName::atUpdatePersist,                 at_update_persist_op,                 "@update-persist"sv},
            {               SystemName::atArrayFilter,                   at_array_filter_op,                   "@array-filter"sv},
            {                 SystemName::atArrayFind,                     at_array_find_op,                     "@array-find"sv},
            {                  SystemName::atArrayAny,                      at_array_any_op,                      "@array-any"sv},
            {                  SystemName::atArrayAll,                      at_array_all_op,                      "@array-all"sv},
            {                SystemName::atArrayCount,                    at_array_count_op,                    "@array-count"sv},
            {            SystemName::atArrayPartition,                at_array_partition_op,                "@array-partition"sv},
            {                SystemName::atArrayMinBy,                    at_array_minby_op,                   "@array-min-by"sv},
            {                SystemName::atArrayMaxBy,                    at_array_maxby_op,                   "@array-max-by"sv},
            {               SystemName::atArraySortBy,                   at_array_sortby_op,                  "@array-sort-by"sv},
            {              SystemName::atArrayGroupBy,                  at_array_groupby_op,                 "@array-group-by"sv},
            {                 SystemName::atArrayScan,                     at_array_scan_op,                     "@array-scan"sv},
            {            SystemName::atArrayTakeWhile,                at_array_takewhile_op,               "@array-take-while"sv},
            {            SystemName::atArrayDropWhile,                at_array_dropwhile_op,               "@array-drop-while"sv},
            {                 SystemName::atEndLocals,                     at_end_locals_op,                     "@end-locals"sv},
            {                     SystemName::atWhile,                          at_while_op,                          "@while"sv},
            {                   SystemName::atDoWhile,                        at_dowhile_op,                       "@do-while"sv},
            {                      SystemName::atTime,                           at_time_op,                           "@time"sv},
            {               SystemName::atHandlerDone,                   at_handler_done_op,                   "@handler-done"sv},
            {               SystemName::atReplBarrier,                    at_replbarrier_op,                   "@repl-barrier"sv},
            {               SystemName::atReplRecover,                    at_replrecover_op,                   "@repl-recover"sv},
            {             SystemName::atForceComplete,                 at_force_complete_op,                 "@force-complete"sv},
            {                 SystemName::atForceFail,                     at_force_fail_op,                     "@force-fail"sv},
            {              SystemName::atLazyMapBuild,                 at_lazy_map_build_op,                 "@lazy-map-build"sv},
            {            SystemName::atLazyFilterTest,               at_lazy_filter_test_op,               "@lazy-filter-test"sv},
            {          SystemName::atLazyFilterResume,             at_lazy_filter_resume_op,             "@lazy-filter-resume"sv},
            {         SystemName::atLazyFilterMapTest,           at_lazy_filter_map_test_op,           "@lazy-filter-map-test"sv},
            {       SystemName::atLazyFilterMapResume,         at_lazy_filter_map_resume_op,         "@lazy-filter-map-resume"sv},
            {          SystemName::atLazyFlatMapBuild,            at_lazy_flat_map_build_op,            "@lazy-flat-map-build"sv},
            {         SystemName::atLazyFlatMapResume,           at_lazy_flat_map_resume_op,           "@lazy-flat-map-resume"sv},
            {          SystemName::atLazyIterateBuild,             at_lazy_iterate_build_op,             "@lazy-iterate-build"sv},
            {           SystemName::atLazyUnfoldBuild,              at_lazy_unfold_build_op,              "@lazy-unfold-build"sv},
            {             SystemName::atLazyScanBuild,                at_lazy_scan_build_op,                "@lazy-scan-build"sv},
            {       SystemName::atLazyMapIndexedBuild,         at_lazy_map_indexed_build_op,         "@lazy-map-indexed-build"sv},
            {         SystemName::atLazyTakeWhileTest,           at_lazy_take_while_test_op,           "@lazy-take-while-test"sv},
            {         SystemName::atLazyDropWhileTest,           at_lazy_drop_while_test_op,           "@lazy-drop-while-test"sv},
            {       SystemName::atLazyDropWhileResume,         at_lazy_drop_while_resume_op,         "@lazy-drop-while-resume"sv},
            {              SystemName::atLazyDropStep,                 at_lazy_drop_step_op,                 "@lazy-drop-step"sv},
            {          SystemName::atLazyZipWithBuild,            at_lazy_zip_with_build_op,            "@lazy-zip-with-build"sv},
            {        SystemName::atLazyChunkedCollect,           at_lazy_chunked_collect_op,           "@lazy-chunked-collect"sv},
            {              SystemName::atLazyFoldStep,                 at_lazy_fold_step_op,                 "@lazy-fold-step"sv},
            {           SystemName::atLazyForEachStep,             at_lazy_for_each_step_op,             "@lazy-for-each-step"sv},
            {               SystemName::atLazyAnyStep,                  at_lazy_any_step_op,                  "@lazy-any-step"sv},
            {               SystemName::atLazyAllStep,                  at_lazy_all_step_op,                  "@lazy-all-step"sv},
            {              SystemName::atLazyFindStep,                 at_lazy_find_step_op,                 "@lazy-find-step"sv},
            {         SystemName::atLazyFindIndexStep,           at_lazy_find_index_step_op,           "@lazy-find-index-step"sv},
            {           SystemName::atLazyToArrayStep,             at_lazy_to_array_step_op,             "@lazy-to-array-step"sv},
            {             SystemName::atLazyCountStep,                at_lazy_count_step_op,                "@lazy-count-step"sv},
            {               SystemName::atLazyNthHead,                  at_lazy_nth_head_op,                  "@lazy-nth-head"sv},
            {               SystemName::atLazyMapImpl,                  at_lazy_map_impl_op,                  "@lazy-map-impl"sv},
            {            SystemName::atLazyFilterImpl,               at_lazy_filter_impl_op,               "@lazy-filter-impl"sv},
            {         SystemName::atLazyTakeWhileImpl,           at_lazy_take_while_impl_op,           "@lazy-take-while-impl"sv},
            {            SystemName::atLazyUnfoldImpl,               at_lazy_unfold_impl_op,               "@lazy-unfold-impl"sv},
            {         SystemName::atLazyFilterMapImpl,           at_lazy_filter_map_impl_op,           "@lazy-filter-map-impl"sv},
            {           SystemName::atLazyFlatMapImpl,             at_lazy_flat_map_impl_op,             "@lazy-flat-map-impl"sv},
            {           SystemName::atLazyZipWithImpl,             at_lazy_zip_with_impl_op,             "@lazy-zip-with-impl"sv},
            {                 SystemName::atEndModule,                     at_end_module_op,                     "@end-module"sv},
            {               SystemName::atCheckModule,                   at_check_module_op,                   "@check-module"sv},
            {         SystemName::atTagUpdateComplete,            at_tag_update_complete_op,            "@tag-update-complete"sv},
            {         SystemName::atTryResultComplete,            at_try_result_complete_op,            "@try-result-complete"sv},
            {               SystemName::atTryRollback,                   at_try_rollback_op,                   "@try-rollback"sv},
            {                SystemName::atRecordCtor,                    at_record_ctor_op,                    "@record-ctor"sv},
            {              SystemName::atRecordForAll,                  at_record_forall_op,                 "@record-for-all"sv},
            {    SystemName::atRecordMapFieldComplete,      at_record_map_field_complete_op,      "@record-map-field-complete"sv},
            {             SystemName::atRecordMapStep,                at_record_map_step_op,                "@record-map-step"sv},
            {       SystemName::atCoroutineAwaitCheck,          at_coroutine_await_check_op,          "@coroutine-await-check"sv},
            {         SystemName::atCoroutineComplete,             at_coroutine_complete_op,             "@coroutine-complete"sv},
            {        SystemName::atCoroutineJoinCheck,           at_coroutine_join_check_op,           "@coroutine-join-check"sv},
            {     SystemName::atCoroutineWaitAllCheck,       at_coroutine_wait_all_check_op,       "@coroutine-wait-all-check"sv},
            {              SystemName::atPipePutRetry,                 at_pipe_put_retry_op,                 "@pipe-put-retry"sv},
            {              SystemName::atPipeGetRetry,                 at_pipe_get_retry_op,                 "@pipe-get-retry"sv},
            {              SystemName::atReadKeyRetry,                 at_read_key_retry_op,                 "@read-key-retry"sv},
            {       SystemName::atReadKeyTimeoutRetry,         at_read_key_timeout_retry_op,         "@read-key-timeout-retry"sv},
            {                 SystemName::atPipeStage,                     at_pipe_stage_op,                     "@pipe-stage"sv},
            {               SystemName::atPipeCleanup,                   at_pipe_cleanup_op,                   "@pipe-cleanup"sv},
            {            SystemName::atActorSendRetry,               at_actor_send_retry_op,               "@actor-send-retry"sv},
            {            SystemName::atActorRecvCheck,               at_actor_recv_check_op,               "@actor-recv-check"sv},
            {     SystemName::atActorRecvTimeoutCheck,       at_actor_recv_timeout_check_op,       "@actor-recv-timeout-check"sv},
            {       SystemName::atActorRecvMatchCheck,         at_actor_recv_match_check_op,         "@actor-recv-match-check"sv},
            {SystemName::atActorRecvMatchTimeoutCheck, at_actor_recv_match_timeout_check_op, "@actor-recv-match-timeout-check"sv},
            {           SystemName::atSupervisorCheck,               at_supervisor_check_op,               "@supervisor-check"sv},
            {            SystemName::atSupervisorInit,                at_supervisor_init_op,                "@supervisor-init"sv},
            {             SystemName::atChoiceBarrier,                 at_choice_barrier_op,                 "@choice-barrier"sv},
            {                SystemName::atChoiceFail,                    at_choice_fail_op,                    "@choice-fail"sv},
            {                  SystemName::atCellEval,                      at_cell_eval_op,                      "@cell-eval"sv},
            {              SystemName::atCellEvalFail,                 at_cell_eval_fail_op,                 "@cell-eval-fail"sv},
            {                  SystemName::atBatchEnd,                      at_batch_end_op,                      "@batch-end"sv},
            {                 SystemName::atBatchFail,                     at_batch_fail_op,                     "@batch-fail"sv},
            {                 SystemName::atBatchFire,                     at_batch_fire_op,                     "@batch-fire"sv},
            {             SystemName::atBatchFireFail,                at_batch_fire_fail_op,                "@batch-fire-fail"sv},
            {            SystemName::atCellUpdateDone,               at_cell_update_done_op,               "@cell-update-done"sv},
            {          SystemName::atCellValidateInit,             at_cell_validate_init_op,             "@cell-validate-init"sv},
            {           SystemName::atCellValidateSet,              at_cell_validate_set_op,              "@cell-validate-set"sv},
            {                     SystemName::Scanner,                              noop_op,                         "scanner"sv},
            {                 SystemName::Interpreter,                              noop_op,                     "interpreter"sv},
            {               SystemName::ArrayFromMark,                     arrayfrommark_op,                 "array-from-mark"sv},
            {       SystemName::ReadOnlyArrayFromMark,             readonlyarrayfrommark_op,        "readonly-array-from-mark"sv},
            {             SystemName::EqArrayFromMark,                   eqarrayfrommark_op,                "=array-from-mark"sv},
            {     SystemName::ReadOnlyEqArrayFromMark,           readonlyeqarrayfrommark_op,       "readonly-=array-from-mark"sv},
            {                SystemName::DictFromMark,                      dictfrommark_op,                  "dict-from-mark"sv},
            {        SystemName::ReadOnlyDictFromMark,              readonlydictfrommark_op,         "readonly-dict-from-mark"sv},
            {              SystemName::EqDictFromMark,                    eqdictfrommark_op,                 "=dict-from-mark"sv},
            {      SystemName::ReadOnlyEqDictFromMark,            readonlyeqdictfrommark_op,        "readonly-=dict-from-mark"sv},
            {                 SystemName::SetFromMark,                       setfrommark_op,                   "set-from-mark"sv},
            {         SystemName::ReadOnlySetFromMark,               readonlysetfrommark_op,          "readonly-set-from-mark"sv},
            {               SystemName::EqSetFromMark,                     eqsetfrommark_op,                  "=set-from-mark"sv},
            {       SystemName::ReadOnlyEqSetFromMark,             readonlyeqsetfrommark_op,         "readonly-=set-from-mark"sv},
            {                    SystemName::SingleEq,                         single_eq_op,                               "="sv},
            {                    SystemName::DoubleEq,                         double_eq_op,                              "=="sv},
            {                    SystemName::EqString,                          eqstring_op,                         "=string"sv},
            {                     SystemName::EqArray,                           eqarray_op,                          "=array"sv},
            {                      SystemName::EqDict,                            eqdict_op,                           "=dict"sv},
            {              SystemName::DefaultHandler,                   default_handler_op,                 "default-handler"sv},
            {                        SystemName::NoOp,                              noop_op,                           "no-op"sv},
            {                         SystemName::Abs,                               abs_op,                             "abs"sv},
            {                         SystemName::Add,                               add_op,                             "add"sv},
#ifdef TRIX_HEAP_TRACKING
            {                  SystemName::AllocStats,                       alloc_stats_op,                     "alloc-stats"sv},
#endif
            {                   SystemName::ArrayLoad,                        array_load_op,                      "array-load"sv},
            {                         SystemName::All,                               all_op,                             "all"sv},
            {                         SystemName::And,                               and_op,                             "and"sv},
            {                         SystemName::Any,                               any_op,                             "any"sv},
            {                      SystemName::Append,                            append_op,                          "append"sv},
            {               SystemName::AssocLaguerre,                    assoc_laguerre_op,                  "assoc-laguerre"sv},
            {               SystemName::AssocLegendre,                    assoc_legendre_op,                  "assoc-legendre"sv},
            {                       SystemName::Array,                             array_op,                           "array"sv},
            {                  SystemName::ArrayStore,                       array_store_op,                     "array-store"sv},
            {           SystemName::ArrayStorePersist,               array_store_persist_op,             "array-store-persist"sv},
            {                        SystemName::ATan,                              atan_op,                            "atan"sv},
            {                       SystemName::Begin,                             begin_op,                           "begin"sv},
            {                 SystemName::BeginLocals,                      begin_locals_op,                    "begin-locals"sv},
            {                   SystemName::Backtrace,                         backtrace_op,                       "backtrace"sv},
            {                       SystemName::Batch,                             batch_op,                           "batch"sv},
            {                 SystemName::BetweenPred,                      between_pred_op,                        "between?"sv},
            {                          SystemName::Bi,                                bi_op,                              "bi"sv},
            {                        SystemName::Bind,                              bind_op,                            "bind"sv},
            {                SystemName::BindIntoDict,                    bind_into_dict_op,                  "bind-into-dict"sv},
            {                  SystemName::BindLocals,                       bind_locals_op,                     "bind-locals"sv},
            {              SystemName::BytesAvailable,                    bytesavailable_op,                 "bytes-available"sv},
            {                  SystemName::Capitalize,                        capitalize_op,                      "capitalize"sv},
            {                        SystemName::Case,                              case_op,                            "case"sv},
            {                        SystemName::Ceil,                              ceil_op,                            "ceil"sv},
            {                        SystemName::Cell,                              cell_op,                            "cell"sv},
            {                    SystemName::CellDeps,                         cell_deps_op,                       "cell-deps"sv},
            {                   SystemName::CellDirty,                        cell_dirty_op,                     "cell-dirty?"sv},
            {                 SystemName::CellDispose,                      cell_dispose_op,                    "cell-dispose"sv},
            {                 SystemName::CellCombine,                      cell_combine_op,                    "cell-combine"sv},
            {                     SystemName::CellGet,                          cell_get_op,                        "cell-get"sv},
            {                     SystemName::CellMap,                          cell_map_op,                        "cell-map"sv},
            {                    SystemName::CellName,                         cell_name_op,                       "cell-name"sv},
            {                   SystemName::CellRdeps,                        cell_rdeps_op,                      "cell-rdeps"sv},
            {                     SystemName::CellSet,                          cell_set_op,                        "cell-set"sv},
            {                 SystemName::CellSetName,                     cell_set_name_op,                   "cell-set-name"sv},
            {                  SystemName::CellUpdate,                       cell_update_op,                     "cell-update"sv},
            {               SystemName::CellTransduce,                    cell_transduce_op,                  "cell-transduce"sv},
            {               SystemName::CellValidated,                    cell_validated_op,                  "cell-validated"sv},
            {                   SystemName::CellValue,                        cell_value_op,                      "cell-value"sv},
            {                SystemName::CellWatchers,                     cell_watchers_op,                   "cell-watchers"sv},
            {                       SystemName::Clamp,                             clamp_op,                           "clamp"sv},
            {                       SystemName::Clear,                             clear_op,                           "clear"sv},
#ifdef TRIX_HEAP_TRACKING
            {             SystemName::ClearAllocStats,                 clear_alloc_stats_op,               "clear-alloc-stats"sv},
#endif
            {              SystemName::ClearDictStack,                    cleardictstack_op,                 "clear-dictstack"sv},
            {             SystemName::ClearInterrupts,                   clearinterrupts_op,                "clear-interrupts"sv},
            {                 SystemName::ClearToMark,                     clear_to_mark_op,                   "clear-to-mark"sv},
            {                 SystemName::ClearObject,                       clearobject_op,                    "clear-object"sv},
            {                       SystemName::Clock,                             clock_op,                           "clock"sv},
            {                 SystemName::CloseStream,                       closestream_op,                    "close-stream"sv},
            {             SystemName::CommandLineArgs,                   commandlineargs_op,               "command-line-args"sv},
            {                        SystemName::Copy,                              copy_op,                            "copy"sv},
            {              SystemName::CoroutineAwait,                   coroutine_await_op,                 "coroutine-await"sv},
            {               SystemName::CoroutineJoin,                    coroutine_join_op,                  "coroutine-join"sv},
            {               SystemName::CoroutineById,                   coroutine_by_id_op,                 "coroutine-by-id"sv},
            {                 SystemName::CoroutineId,                      coroutine_id_op,                    "coroutine-id"sv},
            {            SystemName::CoroutineKillAll,                coroutine_kill_all_op,              "coroutine-kill-all"sv},
            {          SystemName::CoroutineLastError,              coroutine_last_error_op,            "coroutine-last-error"sv},
            {           SystemName::CoroutinePriority,                coroutine_priority_op,              "coroutine-priority"sv},
            {            SystemName::CoroutineQuantum,                 coroutine_quantum_op,               "coroutine-quantum"sv},
            {            SystemName::CoroutineRelease,                 coroutine_release_op,               "coroutine-release"sv},
            {             SystemName::CoroutineResume,                            resume_op,                "coroutine-resume"sv},
            {               SystemName::CoroutineSelf,                    coroutine_self_op,                  "coroutine-self"sv},
            {             SystemName::CoroutineStatus,                  coroutine_status_op,                "coroutine-status"sv},
            {            SystemName::CoroutineSuspend,                           suspend_op,               "coroutine-suspend"sv},
            {            SystemName::CoroutineWaitAll,                coroutine_wait_all_op,              "coroutine-wait-all"sv},
            {                         SystemName::Cos,                               cos_op,                             "cos"sv},
            {                         SystemName::Tan,                               tan_op,                             "tan"sv},
            {                       SystemName::Count,                             count_op,                           "count"sv},
            {                     SystemName::CountIf,                           countif_op,                        "count-if"sv},
            {              SystemName::CountSubstring,                   count_substring_op,                 "count-substring"sv},
            {              SystemName::CountDictStack,                    countdictstack_op,                 "count-dictstack"sv},
            {                 SystemName::CountToMark,                     count_to_mark_op,                   "count-to-mark"sv},
            {                 SystemName::CurrentDict,                       currentdict_op,                    "current-dict"sv},
            {               SystemName::CurrentStream,                     currentstream_op,                  "current-stream"sv},
            {                       SystemName::Curry,                             curry_op,                           "curry"sv},
            {                 SystemName::MakeLiteral,                      make_literal_op,                    "make-literal"sv},
            {                    SystemName::MakeDate,                         make_date_op,                       "make-date"sv},
            {                 SystemName::MakeInstant,                      make_instant_op,                    "make-instant"sv},
            {            SystemName::MakeInstantLocal,                make_instant_local_op,              "make-instant-local"sv},
            {               SystemName::InstantToDate,                   instant_to_date_op,                 "instant-to-date"sv},
            {          SystemName::InstantToDateLocal,             instant_to_date_local_op,           "instant-to-date-local"sv},
            {              SystemName::MakeExecutable,                   make_executable_op,                 "make-executable"sv},
            {                      SystemName::ToName,                           to_name_op,                         "to-name"sv},
            {                    SystemName::ToNumber,                         to_number_op,                       "to-number"sv},
            {                    SystemName::ToString,                         to_string_op,                       "to-string"sv},
            {                 SystemName::DateAddDays,                     date_add_days_op,                   "date-add-days"sv},
            {               SystemName::DateAddMonths,                   date_add_months_op,                 "date-add-months"sv},
            {                SystemName::DateAddYears,                    date_add_years_op,                  "date-add-years"sv},
            {                     SystemName::DateDay,                          date_day_op,                        "date-day"sv},
            {                SystemName::DateDiffDays,                    date_diff_days_op,                  "date-diff-days"sv},
            {                   SystemName::DateMonth,                        date_month_op,                      "date-month"sv},
            {                 SystemName::DateWeekday,                      date_weekday_op,                    "date-weekday"sv},
            {                    SystemName::DateYear,                         date_year_op,                       "date-year"sv},
            {                   SystemName::DayOfYear,                       day_of_year_op,                     "day-of-year"sv},
            {                 SystemName::DaysInMonth,                     days_in_month_op,                   "days-in-month"sv},
            {                         SystemName::Def,                               def_op,                             "def"sv},
            {                  SystemName::DefPersist,                       def_persist_op,                     "def-persist"sv},
            {                     SystemName::Deflate,                           deflate_op,                         "deflate"sv},
            {                SystemName::DeflateLevel,                     deflate_level_op,                   "deflate-level"sv},
            {               SystemName::DeflateStream,                    deflate_stream_op,                  "deflate-stream"sv},
            {          SystemName::DeflateStreamLevel,              deflate_stream_level_op,            "deflate-stream-level"sv},
            {                   SystemName::Demonitor,                         demonitor_op,                       "demonitor"sv},
            {                      SystemName::DeepEq,                            deepeq_op,                         "deep-eq"sv},
            {                      SystemName::DeepNe,                            deepne_op,                         "deep-ne"sv},
            {            SystemName::DegreesToRadians,                degrees_to_radians_op,              "degrees-to-radians"sv},
            {                  SystemName::DeleteFile,                        deletefile_op,                     "delete-file"sv},
            {                        SystemName::Dict,                              dict_op,                            "dict"sv},
            {                 SystemName::DynamicDict,                      dynamic_dict_op,                    "dynamic-dict"sv},
            {                   SystemName::DictStack,                         dictstack_op,                       "dictstack"sv},
            {                         SystemName::Dip,                               dip_op,                             "dip"sv},
            {           SystemName::DisableInterrupts,                 disableinterrupts_op,              "disable-interrupts"sv},
            {                         SystemName::Div,                               div_op,                             "div"sv},
            {                     SystemName::DoWhile,                           dowhile_op,                        "do-while"sv},
            {                  SystemName::DotProduct,                        dotproduct_op,                     "dot-product"sv},
            {                        SystemName::Drop,                              drop_op,                            "drop"sv},
            {                   SystemName::DropWhile,                         dropwhile_op,                      "drop-while"sv},
            {                         SystemName::Dup,                               dup_op,                             "dup"sv},
            {                        SystemName::DupN,                              dupn_op,                           "dup-n"sv},
            {            SystemName::EnableInterrupts,                  enableinterrupts_op,               "enable-interrupts"sv},
            {                         SystemName::End,                               end_op,                             "end"sv},
            {                    SystemName::EndsWith,                          endswith_op,                      "ends-with?"sv},
            {                   SystemName::Enumerate,                         enumerate_op,                       "enumerate"sv},
            {                   SystemName::EpochTime,                         epochtime_op,                      "epoch-time"sv},
            {                          SystemName::Eq,                                eq_op,                              "eq"sv},
            {                        SystemName::Exch,                              exch_op,                            "exch"sv},
            {                        SystemName::Exec,                              exec_op,                            "exec"sv},
            {                       SystemName::ExecN,                             execn_op,                          "exec-n"sv},
            {              SystemName::ExecutablePath,                    executablepath_op,                 "executable-path"sv},
            {                        SystemName::Exit,                              exit_op,                            "exit"sv},
            {                         SystemName::Exp,                               exp_op,                             "exp"sv},
            {                      SystemName::Stream,                            stream_op,                          "stream"sv},
            {              SystemName::StreamPosition,                    streamposition_op,                 "stream-position"sv},
            {                     SystemName::Finally,                           finally_op,                         "finally"sv},
            {                       SystemName::Floor,                             floor_op,                           "floor"sv},
            {                       SystemName::Flush,                             flush_op,                           "flush"sv},
            {                 SystemName::FlushStream,                       flushstream_op,                    "flush-stream"sv},
            {                         SystemName::For,                               for_op,                             "for"sv},
            {                      SystemName::ForAll,                            forall_op,                         "for-all"sv},
            {                       SystemName::Force,                             force_op,                           "force"sv},
            {                         SystemName::GCD,                               gcd_op,                             "gcd"sv},
            {                          SystemName::Ge,                                ge_op,                              "ge"sv},
            {                         SystemName::Get,                               get_op,                             "get"sv},
            {                  SystemName::GetDefault,                        getdefault_op,                     "get-default"sv},
            {                 SystemName::GetInterval,                       getinterval_op,                    "get-interval"sv},
            {                     SystemName::GroupBy,                           groupby_op,                        "group-by"sv},
            {                      SystemName::GetCwd,                            getcwd_op,                          "getcwd"sv},
            {                      SystemName::GetEnv,                            getenv_op,                          "getenv"sv},
            {                      SystemName::GetPid,                            getpid_op,                          "getpid"sv},
            {                          SystemName::Gt,                                gt_op,                              "gt"sv},
            {                          SystemName::If,                                if_op,                              "if"sv},
            {                      SystemName::IfElse,                            ifelse_op,                         "if-else"sv},
            {                       SystemName::Index,                             index_op,                           "index"sv},
            {                     SystemName::IndexOf,                           indexof_op,                        "index-of"sv},
            {                     SystemName::Inflate,                           inflate_op,                         "inflate"sv},
            {               SystemName::InflateStream,                    inflate_stream_op,                  "inflate-stream"sv},
            {                  SystemName::InstantDay,                       instant_day_op,                     "instant-day"sv},
            {             SystemName::InstantDayLocal,                 instant_day_local_op,               "instant-day-local"sv},
            {                 SystemName::InstantHour,                      instant_hour_op,                    "instant-hour"sv},
            {            SystemName::InstantHourLocal,                instant_hour_local_op,              "instant-hour-local"sv},
            {          SystemName::InstantMillisecond,               instant_millisecond_op,             "instant-millisecond"sv},
            {               SystemName::InstantMinute,                    instant_minute_op,                  "instant-minute"sv},
            {          SystemName::InstantMinuteLocal,              instant_minute_local_op,            "instant-minute-local"sv},
            {                SystemName::InstantMonth,                     instant_month_op,                   "instant-month"sv},
            {           SystemName::InstantMonthLocal,               instant_month_local_op,             "instant-month-local"sv},
            {               SystemName::InstantSecond,                    instant_second_op,                  "instant-second"sv},
            {          SystemName::InstantSecondLocal,              instant_second_local_op,            "instant-second-local"sv},
            {              SystemName::InstantWeekday,                   instant_weekday_op,                 "instant-weekday"sv},
            {         SystemName::InstantWeekdayLocal,             instant_weekday_local_op,           "instant-weekday-local"sv},
            {                 SystemName::InstantYear,                      instant_year_op,                    "instant-year"sv},
            {            SystemName::InstantYearLocal,                instant_year_local_op,              "instant-year-local"sv},
            {                 SystemName::IntegerSqrt,                             isqrt_op,                           "isqrt"sv},
            {           SystemName::InterruptsEnabled,                 interruptsenabled_op,             "interrupts-enabled?"sv},
            {           SystemName::InterruptsPending,                 interruptspending_op,              "interrupts-pending"sv},
            {                   SystemName::Interpose,                         interpose_op,                       "interpose"sv},
            {                        SystemName::Join,                              join_op,                            "join"sv},
            {                 SystemName::L0Interrupt,                         interrupt_op,                    "l0-interrupt"sv},
            {                 SystemName::L1Interrupt,                         interrupt_op,                    "l1-interrupt"sv},
            {                 SystemName::L2Interrupt,                         interrupt_op,                    "l2-interrupt"sv},
            {                        SystemName::Kill,                              kill_op,                  "coroutine-kill"sv},
            {                    SystemName::KahanSum,                          kahansum_op,                       "kahan-sum"sv},
            {                       SystemName::Known,                             known_op,                          "known?"sv},
            {                    SystemName::KnownGet,                          knownget_op,                       "known-get"sv},
            {                        SystemName::Keep,                              keep_op,                            "keep"sv},
            {                        SystemName::Keys,                              keys_op,                            "keys"sv},
            {                    SystemName::Laguerre,                          laguerre_op,                        "laguerre"sv},
            {                      SystemName::Launch,                            launch_op,                "coroutine-launch"sv},
            {                    SystemName::Legendre,                          legendre_op,                        "legendre"sv},
            {                         SystemName::LCM,                               lcm_op,                             "lcm"sv},
            {                          SystemName::Le,                                le_op,                              "le"sv},
            {                   SystemName::ShiftLeft,                        shift_left_op,                      "shift-left"sv},
            {                  SystemName::RotateLeft,                       rotate_left_op,                     "rotate-left"sv},
            {                      SystemName::Length,                            length_op,                          "length"sv},
            {                        SystemName::Lerp,                              lerp_op,                            "lerp"sv},
            {                        SystemName::Load,                              load_op,                            "load"sv},
            {                    SystemName::LocalDef,                         local_def_op,                       "local-def"sv},
            {                         SystemName::Log,                               log_op,                             "log"sv},
            {                        SystemName::Loop,                              loop_op,                            "loop"sv},
            {                   SystemName::Lowercase,                         lowercase_op,                       "lowercase"sv},
            {                          SystemName::Lt,                                lt_op,                              "lt"sv},
            {                        SystemName::Link,                              link_op,                            "link"sv},
            {                         SystemName::Map,                               map_op,                             "map"sv},
            {                     SystemName::MapDict,                           mapdict_op,                        "map-dict"sv},
            {                  SystemName::MapIndexed,                        mapindexed_op,                     "map-indexed"sv},
            {                        SystemName::Mark,                              mark_op,                            "mark"sv},
            {                         SystemName::Max,                               max_op,                             "max"sv},
            {                       SystemName::MaxBy,                             maxby_op,                          "max-by"sv},
            {                       SystemName::MaxOf,                             maxof_op,                          "max-of"sv},
            {                   SystemName::MaxLength,                         maxlength_op,                      "max-length"sv},
            {                       SystemName::Merge,                             merge_op,                           "merge"sv},
            {                    SystemName::MidPoint,                          midpoint_op,                        "midpoint"sv},
            {                         SystemName::Min,                               min_op,                             "min"sv},
            {                       SystemName::MinBy,                             minby_op,                          "min-by"sv},
            {                       SystemName::MinOf,                             minof_op,                          "min-of"sv},
            {                       SystemName::Mkdir,                             mkdir_op,                           "mkdir"sv},
            {                     SystemName::Monitor,                           monitor_op,                         "monitor"sv},
            {                         SystemName::Mod,                               mod_op,                             "mod"sv},
            {                         SystemName::Mul,                               mul_op,                             "mul"sv},
            {                          SystemName::Ne,                                ne_op,                              "ne"sv},
            {                         SystemName::Neg,                               neg_op,                             "neg"sv},
            {                     SystemName::NewLine,                           newline_op,                              "nl"sv},
            {                         SystemName::Not,                               not_op,                             "not"sv},
            {                         SystemName::Now,                               now_op,                             "now"sv},
            {                     SystemName::OpStack,                          op_stack_op,                         "opstack"sv},
            {                          SystemName::Or,                                or_op,                              "or"sv},
            {                        SystemName::Over,                              over_op,                            "over"sv},
            {                    SystemName::Override,                          override_op,                        "override"sv},
            {                      SystemName::Packed,                            packed_op,                          "packed"sv},
            {                        SystemName::Pack,                              pack_op,                            "pack"sv},
            {                    SystemName::PackSize,                          packsize_op,                       "pack-size"sv},
            {                     SystemName::PadLeft,                           padleft_op,                        "pad-left"sv},
            {                    SystemName::PadRight,                          padright_op,                       "pad-right"sv},
            {                   SystemName::Partition,                         partition_op,                       "partition"sv},
            {                   SystemName::PipeBatch,                        pipe_batch_op,                      "pipe-batch"sv},
            {                  SystemName::PipeBuffer,                       pipe_buffer_op,                     "pipe-buffer"sv},
            {                   SystemName::PipeClose,                        pipe_close_op,                      "pipe-close"sv},
            {                 SystemName::PipeCollect,                      pipe_collect_op,                    "pipe-collect"sv},
            {                SystemName::PipeDistinct,                     pipe_distinct_op,                   "pipe-distinct"sv},
            {                    SystemName::PipeDrop,                         pipe_drop_op,                       "pipe-drop"sv},
            {              SystemName::PipeErrorClose,                  pipe_error_close_op,                "pipe-error-close"sv},
            {                  SystemName::PipeFilter,                       pipe_filter_op,                     "pipe-filter"sv},
            {                 SystemName::PipeFlatMap,                     pipe_flat_map_op,                   "pipe-flat-map"sv},
            {                     SystemName::PipeGet,                          pipe_get_op,                        "pipe-get"sv},
            {                     SystemName::PipeMap,                          pipe_map_op,                        "pipe-map"sv},
            {                   SystemName::PipeMerge,                        pipe_merge_op,                      "pipe-merge"sv},
            {                     SystemName::PipePut,                          pipe_put_op,                        "pipe-put"sv},
            {                  SystemName::PipeReduce,                       pipe_reduce_op,                     "pipe-reduce"sv},
            {                     SystemName::PipeRun,                          pipe_run_op,                        "pipe-run"sv},
            {                    SystemName::PipeScan,                         pipe_scan_op,                       "pipe-scan"sv},
            {                  SystemName::PipeStatus,                       pipe_status_op,                     "pipe-status"sv},
            {                    SystemName::PipeTake,                         pipe_take_op,                       "pipe-take"sv},
            {                     SystemName::PipeTap,                          pipe_tap_op,                        "pipe-tap"sv},
            {                  SystemName::PipeWindow,                       pipe_window_op,                     "pipe-window"sv},
            {                     SystemName::PipeZip,                          pipe_zip_op,                        "pipe-zip"sv},
            {                         SystemName::Pop,                               pop_op,                             "pop"sv},
            {                        SystemName::PopN,                              popn_op,                           "pop-n"sv},
            {                       SystemName::Print,                             print_op,                           "print"sv},
            {                     SystemName::Promote,                           promote_op,                         "promote"sv},
            {                     SystemName::Product,                           product_op,                         "product"sv},
            {                         SystemName::Put,                               put_op,                             "put"sv},
            {                 SystemName::PutInterval,                       putinterval_op,                    "put-interval"sv},
            {                  SystemName::PutPersist,                       put_persist_op,                     "put-persist"sv},
            {                 SystemName::QueryStatus,                      query_status_op,                    "query-status"sv},
            {                        SystemName::Quit,                              quit_op,                            "quit"sv},
            {                       SystemName::Range,                             range_op,                           "range"sv},
            {                   SystemName::RangeFrom,                        range_from_op,                      "range-from"sv},
            {            SystemName::RadiansToDegrees,                radians_to_degrees_op,              "radians-to-degrees"sv},
            {                     SystemName::RawMode,                           rawmode_op,                        "raw-mode"sv},
            {                        SystemName::Read,                              read_op,                            "read"sv},
            {                 SystemName::ReadKeyByte,                       readkeybyte_op,                   "read-key-byte"sv},
            {          SystemName::ReadKeyByteTimeout,               readkeybyte_timeout_op,           "read-key-byte-timeout"sv},
            {                     SystemName::ReadAll,                           readall_op,                        "read-all"sv},
            {               SystemName::ReadHexString,                     readhexstring_op,                 "read-hex-string"sv},
            {                    SystemName::ReadLine,                          readline_op,                       "read-line"sv},
            {                SystemName::MakeReadOnly,                     make_readonly_op,                   "make-readonly"sv},
            {                  SystemName::ReadString,                        readstring_op,                     "read-string"sv},
            {                SystemName::RecordTypeOp,                       record_type_op,                     "record-type"sv},
            {                  SystemName::MakeRecord,                       make_record_op,                          "record"sv},
            {                SystemName::RecordSchema,                     record_schema_op,                   "record-schema"sv},
            {                SystemName::RecordFields,                     record_fields_op,                   "record-fields"sv},
            {                SystemName::RecordUpdate,                     record_update_op,                   "record-update"sv},
            {              SystemName::RecordMapField,                  record_map_field_op,                "record-map-field"sv},
            {                   SystemName::RecordMap,                        record_map_op,                      "record-map"sv},
            {                 SystemName::RecordMerge,                      record_merge_op,                    "record-merge"sv},
            {                SystemName::RecordSelect,                     record_select_op,                   "record-select"sv},
            {                SystemName::RecordToDict,                    record_to_dict_op,                  "record-to-dict"sv},
            {              SystemName::RecordFromDict,                  record_from_dict_op,                "record-from-dict"sv},
            {                   SystemName::RecordZip,                        record_zip_op,                      "record-zip"sv},
            {               SystemName::RecordGroupBy,                   record_group_by_op,                 "record-group-by"sv},
            {                SystemName::RecordValues,                     record_values_op,                   "record-values"sv},
            {                 SystemName::RecordKnown,                      record_known_op,                    "record-known"sv},
            {                      SystemName::Reduce,                            reduce_op,                          "reduce"sv},
            {                SystemName::RegexFindAll,                      regexfindall_op,                  "regex-find-all"sv},
            {                  SystemName::RegexMatch,                        regexmatch_op,                     "regex-match"sv},
            {                SystemName::RegexReplace,                      regexreplace_op,                   "regex-replace"sv},
            {                 SystemName::RegexSearch,                       regexsearch_op,                    "regex-search"sv},
            {                  SystemName::RegexSplit,                        regexsplit_op,                     "regex-split"sv},
            {                SystemName::RemovePrefix,                      removeprefix_op,                   "remove-prefix"sv},
            {                SystemName::RemoveSuffix,                      removesuffix_op,                   "remove-suffix"sv},
            {                  SystemName::RenameFile,                        renamefile_op,                     "rename-file"sv},
            {                      SystemName::Repeat,                            repeat_op,                          "repeat"sv},
            {                 SystemName::ResetStream,                       resetstream_op,                    "reset-stream"sv},
            {                     SystemName::Restore,                           restore_op,                         "restore"sv},
            {                     SystemName::Reverse,                           reverse_op,                         "reverse"sv},
            {                 SystemName::RiemannZeta,                      riemann_zeta_op,                    "riemann-zeta"sv},
            {                  SystemName::ShiftRight,                       shift_right_op,                     "shift-right"sv},
            {                 SystemName::RotateRight,                      rotate_right_op,                    "rotate-right"sv},
            {                        SystemName::Roll,                              roll_op,                            "roll"sv},
            {                         SystemName::Rot,                               rot_op,                             "rot"sv},
            {                      SystemName::RevRot,                            revrot_op,                         "rev-rot"sv},
            {                       SystemName::Round,                             round_op,                           "round"sv},
            {                   SystemName::RoundEven,                         roundeven_op,                      "round-even"sv},
            {                     SystemName::Require,                           require_op,                         "require"sv},
            {                         SystemName::Run,                               run_op,                             "run"sv},
            {                 SystemName::RecoverSave,                      recover_save_op,                    "recover-save"sv},
            {                        SystemName::Save,                              save_op,                            "save"sv},
            {                  SystemName::SaveLevelQ,                      save_level_q_op,                     "save-level?"sv},
            {                        SystemName::Scan,                              scan_op,                            "scan"sv},
            {                SystemName::ScratchClear,                     scratch_clear_op,                   "scratch-clear"sv},
            {              SystemName::ScratchCollect,                   scratch_collect_op,                 "scratch-collect"sv},
            {                 SystemName::ScratchPush,                      scratch_push_op,                    "scratch-push"sv},
            {                      SystemName::Search,                            search_op,                          "search"sv},
            {           SystemName::SetStreamPosition,                 setstreamposition_op,             "set-stream-position"sv},
            {                      SystemName::SetEnv,                            setenv_op,                          "setenv"sv},
            {                       SystemName::Shell,                             shell_op,                           "shell"sv},
            {                         SystemName::Sin,                               sin_op,                             "sin"sv},
            {                        SystemName::Sqrt,                              sqrt_op,                            "sqrt"sv},
            {                   SystemName::StepRange,                         steprange_op,                      "step-range"sv},
            {                       SystemName::Stack,                             stack_op,                           "stack"sv},
            {                  SystemName::StackProbe,                       stack_probe_op,                     "stack-probe"sv},
            {                      SystemName::PStack,                        printstack_op,                     "print-stack"sv},
            {                  SystemName::StartsWith,                        startswith_op,                    "starts-with?"sv},
            {                      SystemName::Status,                            status_op,                          "status"sv},
            {               SystemName::IsInteractive,                       interactive_op,                    "interactive?"sv},
            {                    SystemName::ShortAnd,                         short_and_op,                            "and?"sv},
            {                     SystemName::ShortOr,                          short_or_op,                             "or?"sv},
            {            SystemName::MakeMemoryStream,                make_memory_stream_op,              "make-memory-stream"sv},
            {                   SystemName::SetGlobal,                        set_global_op,                      "set-global"sv},
            {               SystemName::CurrentGlobal,                    current_global_op,                  "current-global"sv},
            {                  SystemName::atInGlobal,                         in_global_op,                      "@in-global"sv},
            {                   SystemName::atInLocal,                          in_local_op,                       "@in-local"sv},
            {               SystemName::atEndInGlobal,                  at_end_in_global_op,                  "@end-in-global"sv},
#ifdef TRIX_DEBUGGER
            {          SystemName::atDebugErrorResume,             at_debug_error_resume_op,             "@debug-error-resume"sv},
#endif
            {            SystemName::MakeStringStream,                make_string_stream_op,              "make-string-stream"sv},
            {             SystemName::GetStringStream,                 get_string_stream_op,               "get-string-stream"sv},
            {           SystemName::ClearStringStream,               clear_string_stream_op,             "clear-string-stream"sv},
            {                   SystemName::SetStdout,                        set_stdout_op,                      "set-stdout"sv},
            {                  SystemName::MakeScreen,                       make_screen_op,                     "make-screen"sv},
            {                  SystemName::ScreenCols,                       screen_cols_op,                     "screen-cols"sv},
            {                  SystemName::ScreenRows,                       screen_rows_op,                     "screen-rows"sv},
            {                 SystemName::ScreenClear,                      screen_clear_op,                    "screen-clear"sv},
            {                SystemName::ScreenResize,                     screen_resize_op,                   "screen-resize"sv},
            {               SystemName::ScreenPutCell,                   screen_put_cell_op,                 "screen-put-cell"sv},
            {             SystemName::ScreenPutString,                 screen_put_string_op,               "screen-put-string"sv},
            {              SystemName::ScreenFillRect,                  screen_fill_rect_op,                "screen-fill-rect"sv},
            {                SystemName::ScreenRender,                     screen_render_op,                   "screen-render"sv},
            {              SystemName::ScreenRenderTo,                  screen_render_to_op,                "screen-render-to"sv},
            {               SystemName::ScreenGetCell,                   screen_get_cell_op,                 "screen-get-cell"sv},
            {         SystemName::ScreenPutUtf8String,            screen_put_utf8_string_op,          "screen-put-utf8-string"sv},
            {                  SystemName::ScreenBlit,                       screen_blit_op,                     "screen-blit"sv},
            {            SystemName::ScreenParkCursor,                screen_park_cursor_op,              "screen-park-cursor"sv},
            {                    SystemName::IsScreen,                         is_screen_op,                         "screen?"sv},
            {                  SystemName::HandleKind,                       handle_kind_op,                     "handle-kind"sv},
            {                        SystemName::Stop,                              stop_op,                            "stop"sv},
            {                  SystemName::Supervisor,                        supervisor_op,                      "supervisor"sv},
            {     SystemName::SupervisorCountChildren,         supervisor_count_children_op,       "supervisor-count-children"sv},
            {          SystemName::SupervisorGetChild,              supervisor_get_child_op,            "supervisor-get-child"sv},
            {      SystemName::SupervisorRestartChild,          supervisor_restart_child_op,        "supervisor-restart-child"sv},
            {              SystemName::SupervisorSpec,                   supervisor_spec_op,                 "supervisor-spec"sv},
            {        SystemName::SupervisorStartChild,            supervisor_start_child_op,          "supervisor-start-child"sv},
            {              SystemName::SupervisorStop,                   supervisor_stop_op,                 "supervisor-stop"sv},
            {    SystemName::SupervisorTerminateChild,        supervisor_terminate_child_op,      "supervisor-terminate-child"sv},
            {     SystemName::SupervisorWhichChildren,         supervisor_which_children_op,       "supervisor-which-children"sv},
            {                SystemName::TerminalSize,                      terminalsize_op,                   "terminal-size"sv},
            {                         SystemName::Try,                               try_op,                             "try"sv},
            {                    SystemName::TryCatch,                          trycatch_op,                       "try-catch"sv},
            {                   SystemName::TryResult,                        try_result_op,                      "try-result"sv},
            {                       SystemName::Throw,                             throw_op,                           "throw"sv},
            {                   SystemName::ThrowWith,                        throw_with_op,                      "throw-with"sv},
            {                     SystemName::ReThrow,                           rethrow_op,                         "rethrow"sv},
            {                   SystemName::LastError,                        last_error_op,                      "last-error"sv},
            {               SystemName::LastErrorData,                   last_error_data_op,                 "last-error-data"sv},
            {            SystemName::LastErrorMessage,                last_error_message_op,              "last-error-message"sv},
            {                     SystemName::Stopped,                           stopped_op,                         "stopped"sv},
            {                       SystemName::Store,                             store_op,                           "store"sv},
            {                      SystemName::String,                            string_op,                          "string"sv},
            {               SystemName::StringIndexOf,                     stringindexof_op,                 "string-index-of"sv},
            {                         SystemName::Sub,                               sub_op,                             "sub"sv},
            {                         SystemName::Sum,                               sum_op,                             "sum"sv},
            {                      SystemName::SwapAt,                            swapat_op,                         "swap-at"sv},
            {                      SystemName::System,                            system_op,                          "system"sv},
            {                       SystemName::Token,                             token_op,                           "token"sv},
            {                  SystemName::TotalOrder,                        totalorder_op,                    "total-order?"sv},
            {               SystemName::TotalOrderMag,                     totalordermag_op,                "total-order-mag?"sv},
            {                       SystemName::Trunc,                             trunc_op,                           "trunc"sv},
            {                        SystemName::Type,                              type_op,                            "type"sv},
            {                    SystemName::TypeCase,                          typecase_op,                       "type-case"sv},
            {                       SystemName::Undef,                             undef_op,                           "undef"sv},
            {                SystemName::UndefPersist,                     undef_persist_op,                   "undef-persist"sv},
            {                         SystemName::Ulp,                               ulp_op,                             "ulp"sv},
            {                    SystemName::UlpEqual,                          ulpequal_op,                      "ulp-equal?"sv},
            {                      SystemName::Unique,                            unique_op,                          "unique"sv},
            {                      SystemName::Dedupe,                            dedupe_op,                          "dedupe"sv},
            {                      SystemName::Update,                            update_op,                          "update"sv},
            {               SystemName::UpdatePersist,                    update_persist_op,                  "update-persist"sv},
            {                     SystemName::Uncurry,                           uncurry_op,                         "uncurry"sv},
            {                      SystemName::Unpack,                            unpack_op,                          "unpack"sv},
            {                      SystemName::Unlink,                            unlink_op,                          "unlink"sv},
            {                     SystemName::Unwatch,                           unwatch_op,                         "unwatch"sv},
            {                   SystemName::Uppercase,                         uppercase_op,                       "uppercase"sv},
            {                      SystemName::Values,                            values_op,                          "values"sv},
            {                      SystemName::VMSize,                            vmsize_op,                         "vm-size"sv},
            {                  SystemName::VmGlobalGc,                      vm_global_gc_op,                    "vm-global-gc"sv},
            {             SystemName::VmGlobalGcProbe,                vm_global_gc_probe_op,              "vm-global-gc-probe"sv},
#ifdef TRIX_DEBUGGER
            {                  SystemName::VmGcStress,                      vm_gc_stress_op,                    "vm-gc-stress"sv},
            {                  SystemName::VmGcPoison,                      vm_gc_poison_op,                    "vm-gc-poison"sv},
#endif
            {                SystemName::VmGlobalInfo,                    vm_global_info_op,                  "vm-global-info"sv},
#ifdef TRIX_HEAP_TRACKING
            {              SystemName::VmHeapSnapshot,                  vm_heap_snapshot_op,                "vm-heap-snapshot"sv},
            {                  SystemName::VmHeapDiff,                      vm_heap_diff_op,                    "vm-heap-diff"sv},
#endif
            {                       SystemName::Watch,                             watch_op,                           "watch"sv},
            {                   SystemName::IsRawMode,                         israwmode_op,                       "raw-mode?"sv},
            {                   SystemName::KeyReadyQ,                          keyready_op,                      "key-ready?"sv},
            {                  SystemName::IsReadable,                       is_readable_op,                       "readable?"sv},
            {                  SystemName::IsWritable,                       is_writable_op,                       "writable?"sv},
            {                       SystemName::Where,                             where_op,                           "where"sv},
            {                       SystemName::While,                             while_op,                           "while"sv},
            {                  SystemName::WithStream,                        withstream_op,                     "with-stream"sv},
            {                       SystemName::Write,                             write_op,                           "write"sv},
            {                 SystemName::WriteString,                       writestring_op,                    "write-string"sv},
            {                         SystemName::Zip,                               zip_op,                             "zip"sv},
            {                SystemName::IsExecutable,                     is_executable_op,                     "executable?"sv},
            {                SystemName::HasSingleBit,                    has_single_bit_op,                     "single-bit?"sv},
            {                     SystemName::BitCeil,                          bit_ceil_op,                        "bit-ceil"sv},
            {                    SystemName::BitClear,                          bitclear_op,                       "bit-clear"sv},
            {                    SystemName::BitFloor,                         bit_floor_op,                       "bit-floor"sv},
            {                      SystemName::BitSet,                            bitset_op,                         "bit-set"sv},
            {                     SystemName::BitTest,                           bittest_op,                            "bit?"sv},
            {                   SystemName::BitToggle,                         bittoggle_op,                      "bit-toggle"sv},
            {                         SystemName::Xor,                               xor_op,                             "xor"sv},
            {                      SystemName::Assert,                            assert_op,                          "assert"sv},
            {                        SystemName::Cast,                              cast_op,                            "cast"sv},
            {                       SystemName::Chdir,                             chdir_op,                           "chdir"sv},
            {                       SystemName::Chmod,                             chmod_op,                           "chmod"sv},
            {                      SystemName::Coerce,                            coerce_op,                          "coerce"sv},
            {                  SystemName::CookedMode,                        cookedmode_op,                     "cooked-mode"sv},
            {                 SystemName::Reinterpret,                       reinterpret_op,                     "reinterpret"sv},
            {                    SystemName::PrintFmt,                          printfmt_op,                       "print-fmt"sv},
            {                   SystemName::FPrintFmt,                         fprintfmt_op,                      "fprint-fmt"sv},
            {                   SystemName::SPrintFmt,                         sprintfmt_op,                      "sprint-fmt"sv},
            {                   SystemName::APrintFmt,                         aprintfmt_op,                      "aprint-fmt"sv},
            {                  SystemName::AFPrintFmt,                        afprintfmt_op,                     "afprint-fmt"sv},
            {                  SystemName::ASPrintFmt,                        asprintfmt_op,                     "asprint-fmt"sv},
            {                    SystemName::SScanFmt,                          sscanfmt_op,                       "sscan-fmt"sv},
            {                    SystemName::FScanFmt,                          fscanfmt_op,                       "fscan-fmt"sv},
            {                    SystemName::RandSeed,                          pcg_seed_op,                       "rand-seed"sv},
            {                SystemName::RandUInteger,                      pcg_uinteger_op,                   "rand-uinteger"sv},
            {         SystemName::RandBoundedUInteger,              pcg_bounded_uinteger_op,           "rand-bounded-uinteger"sv},
            {                   SystemName::RandULong,                         pcg_ulong_op,                      "rand-ulong"sv},
            {            SystemName::RandBoundedULong,                 pcg_bounded_ulong_op,              "rand-bounded-ulong"sv},
            {                  SystemName::RandInt128,                        pcg_int128_op,                     "rand-int128"sv},
            {                 SystemName::RandUInt128,                       pcg_uint128_op,                    "rand-uint128"sv},
            {          SystemName::RandBoundedUInt128,               pcg_bounded_uint128_op,            "rand-bounded-uint128"sv},
            {                    SystemName::RandReal,                          pcg_real_op,                       "rand-real"sv},
            {                  SystemName::RandDouble,                        pcg_double_op,                     "rand-double"sv},
            {                    SystemName::SnapShot,                          snapshot_op,                       "snap-shot"sv},
            {                        SystemName::Thaw,                              thaw_op,                            "thaw"sv},
#ifdef TRIX_DEBUGGER
            {                  SystemName::Breakpoint,                        breakpoint_op,                      "breakpoint"sv},
            {                   SystemName::DebugStep,                        debug_step_op,                      "debug-step"sv},
            {               SystemName::DebugStepOver,                   debug_step_over_op,                 "debug-step-over"sv},
            {                SystemName::DebugStepOut,                    debug_step_out_op,                  "debug-step-out"sv},
            {               SystemName::DebugContinue,                    debug_continue_op,                  "debug-continue"sv},
            {                  SystemName::DebugBreak,                       debug_break_op,                     "debug-break"sv},
            {                SystemName::DebugUnbreak,                     debug_unbreak_op,                   "debug-unbreak"sv},
            {           SystemName::DebugBreakOnError,              debug_break_on_error_op,            "debug-break-on-error"sv},
            {              SystemName::DebugCallDepth,                  debug_call_depth_op,                "debug-call-depth"sv},
            {                     SystemName::DebugPc,                          debug_pc_op,                        "debug-pc"sv},
            {               SystemName::DebugPcSource,                   debug_pc_source_op,                 "debug-pc-source"sv},
            {                SystemName::DebugOnEvent,                    debug_on_event_op,                  "debug-on-event"sv},
            {             SystemName::OpStackSnapshot,                 op_stack_snapshot_op,               "op-stack-snapshot"sv},
            {           SystemName::ExecStackSnapshot,               exec_stack_snapshot_op,             "exec-stack-snapshot"sv},
            {           SystemName::DictStackSnapshot,               dict_stack_snapshot_op,             "dict-stack-snapshot"sv},
            {            SystemName::ErrStackSnapshot,                err_stack_snapshot_op,              "err-stack-snapshot"sv},
            {                  SystemName::ProcDisasm,                       proc_disasm_op,                     "proc-disasm"sv},
            {                SystemName::FormatObject,                     format_object_op,                   "format-object"sv},
            {            SystemName::DebugBreakpoints,                 debug_breakpoints_op,               "debug-breakpoints"sv},
            {                  SystemName::StreamName,                       stream_name_op,                     "stream-name"sv},
            {             SystemName::FrameSourceLocs,                 frame_source_locs_op,               "frame-source-locs"sv},
            {                 SystemName::DebugBpHits,                     debug_bp_hits_op,                   "debug-bp-hits"sv},
#endif
            {                         SystemName::Die,                               die_op,                   "coroutine-die"sv},
            {                        SystemName::Peek,                              peek_op,                            "peek"sv},
            {                        SystemName::Poke,                              poke_op,                            "poke"sv},
            {                       SystemName::Alloc,                             alloc_op,                           "alloc"sv},
            {                        SystemName::Free,                              free_op,                            "free"sv},
            {                   SystemName::DwarfOpen,                        dwarf_open_op,                      "dwarf-open"sv},
            {                   SystemName::PeekBytes,                        peek_bytes_op,                      "peek-bytes"sv},
            {                SystemName::Leb128Decode,                     leb128_decode_op,                   "leb128-decode"sv},
            {              SystemName::ModuleLoadBias,                  module_load_bias_op,                "module-load-bias"sv},
            {                SystemName::DwarfReadDie,                    dwarf_read_die_op,                  "dwarf-read-die"sv},
            {                 SystemName::DwarfMunmap,                      dwarf_munmap_op,                    "dwarf-munmap"sv},
            {           SystemName::ModuleLoadBiasFor,              module_load_bias_for_op,            "module-load-bias-for"sv},
            {             SystemName::DwarfLineLookup,                 dwarf_line_lookup_op,               "dwarf-line-lookup"sv},
            {                  SystemName::ZipLongest,                       zip_longest_op,                     "zip-longest"sv},
            {                   SystemName::Intersect,                         intersect_op,                       "intersect"sv},
            {                       SystemName::Union,                             union_op,                           "union"sv},
            {                  SystemName::Difference,                        difference_op,                      "difference"sv},
            {         SystemName::ObjectToBinaryToken,            object_to_binary_token_op,                 "to-binary-token"sv},
            {                        SystemName::ACos,                              acos_op,                            "acos"sv},
            {                        SystemName::ASin,                              asin_op,                            "asin"sv},
            {                       SystemName::ASinh,                             asinh_op,                           "asinh"sv},
            {                       SystemName::ACosh,                             acosh_op,                           "acosh"sv},
            {              SystemName::ActorBroadcast,                   actor_broadcast_op,                 "actor-broadcast"sv},
            {                  SystemName::ActorFlush,                       actor_flush_op,                     "actor-flush"sv},
            {                   SystemName::ActorName,                        actor_name_op,                      "actor-name"sv},
            {        SystemName::ActorMailboxCapacity,            actor_mailbox_capacity_op,          "actor-mailbox-capacity"sv},
            {           SystemName::ActorMailboxCount,               actor_mailbox_count_op,             "actor-mailbox-count"sv},
            {       SystemName::ActorMailboxEmptyPred,          actor_mailbox_empty_pred_op,            "actor-mailbox-empty?"sv},
            {                   SystemName::ActorRecv,                        actor_recv_op,                      "actor-recv"sv},
            {              SystemName::ActorRecvMatch,                  actor_recv_match_op,                "actor-recv-match"sv},
            {       SystemName::ActorRecvMatchTimeout,          actor_recv_match_timeout_op,        "actor-recv-match-timeout"sv},
            {            SystemName::ActorRecvTimeout,                actor_recv_timeout_op,              "actor-recv-timeout"sv},
            {                   SystemName::ActorSelf,                        actor_self_op,                      "actor-self"sv},
            {                   SystemName::ActorSend,                        actor_send_op,                      "actor-send"sv},
            {                  SystemName::ActorSpawn,                       actor_spawn_op,                     "actor-spawn"sv},
            {                SystemName::ActorSetName,                    actor_set_name_op,                  "actor-set-name"sv},
            {                 SystemName::ActorStatus,                      actor_status_op,                    "actor-status"sv},
            {                SystemName::ActorTrySend,                    actor_try_send_op,                  "actor-try-send"sv},
            {          SystemName::ActorSpawnCapacity,              actor_spawn_capacity_op,            "actor-spawn-capacity"sv},
            {                   SystemName::ActorExit,                        actor_exit_op,                      "actor-exit"sv},
            {               SystemName::ActorTrapExit,                   actor_trap_exit_op,                 "actor-trap-exit"sv},
            {                       SystemName::ATan2,                             atan2_op,                           "atan2"sv},
            {                       SystemName::ATanh,                             atanh_op,                           "atanh"sv},
            {                        SystemName::Beta,                              beta_op,                            "beta"sv},
            {                    SystemName::BitWidth,                         bit_width_op,                       "bit-width"sv},
            {                    SystemName::ByteSwap,                          byteswap_op,                       "byte-swap"sv},
            {                   SystemName::CharAlnum,                         charalnum_op,                          "alnum?"sv},
            {                   SystemName::CharAlpha,                         charalpha_op,                          "alpha?"sv},
            {                   SystemName::CharDigit,                         chardigit_op,                          "digit?"sv},
            {                SystemName::CharHexDigit,                      charhexdigit_op,                      "hex-digit?"sv},
            {                   SystemName::CharLower,                         charlower_op,                          "lower?"sv},
            {               SystemName::CharPrintable,                     charprintable_op,                      "printable?"sv},
            {                   SystemName::CharSpace,                         charspace_op,                          "space?"sv},
            {                   SystemName::CharUpper,                         charupper_op,                          "upper?"sv},
            {                       SystemName::Chars,                             chars_op,                           "chars"sv},
            {             SystemName::StringFromBytes,                 string_from_bytes_op,               "string-from-bytes"sv},
            {                       SystemName::Chunk,                             chunk_op,                           "chunk"sv},
            {                        SystemName::Cbrt,                              cbrt_op,                            "cbrt"sv},
            {                 SystemName::CompEllint1,                     comp_ellint_1_op,                   "comp-ellint-1"sv},
            {                 SystemName::CompEllint2,                     comp_ellint_2_op,                   "comp-ellint-2"sv},
            {                 SystemName::CompEllint3,                     comp_ellint_3_op,                   "comp-ellint-3"sv},
            {                     SystemName::Compose,                           compose_op,                         "compose"sv},
            {                    SystemName::Computed,                          computed_op,                   "cell-computed"sv},
            {                      SystemName::Concat,                            concat_op,                          "concat"sv},
            {                    SystemName::Contains,                          contains_op,                       "contains?"sv},
            {                    SystemName::CopySign,                          copysign_op,                       "copy-sign"sv},
            {                        SystemName::Cosh,                              cosh_op,                            "cosh"sv},
            {                   SystemName::CountLOne,                        countl_one_op,                      "countl-one"sv},
            {                  SystemName::CountLZero,                       countl_zero_op,                     "countl-zero"sv},
            {                   SystemName::CountROne,                        countr_one_op,                      "countr-one"sv},
            {                  SystemName::CountRZero,                       countr_zero_op,                     "countr-zero"sv},
            {                     SystemName::Adler32,                           adler32_op,                         "adler32"sv},
            {               SystemName::Adler32Stream,                    adler32_stream_op,                  "adler32-stream"sv},
            {                       SystemName::Crc32,                             crc32_op,                           "crc32"sv},
            {                 SystemName::Crc32Stream,                      crc32_stream_op,                    "crc32-stream"sv},
            {                  SystemName::CylBesselI,                      cyl_bessel_i_op,                    "cyl-bessel-i"sv},
            {                  SystemName::CylBesselJ,                      cyl_bessel_j_op,                    "cyl-bessel-j"sv},
            {                  SystemName::CylBesselK,                      cyl_bessel_k_op,                    "cyl-bessel-k"sv},
            {                  SystemName::CylNeumann,                       cyl_neumann_op,                     "cyl-neumann"sv},
            {                     SystemName::Ellint1,                          ellint_1_op,                        "ellint-1"sv},
            {                     SystemName::Ellint2,                          ellint_2_op,                        "ellint-2"sv},
            {                     SystemName::Ellint3,                          ellint_3_op,                        "ellint-3"sv},
            {                         SystemName::Erf,                               erf_op,                             "erf"sv},
            {                        SystemName::Erfc,                              erfc_op,                            "erfc"sv},
            {                        SystemName::Exp2,                              exp2_op,                            "exp2"sv},
            {                       SystemName::Expm1,                             expm1_op,                           "expm1"sv},
            {                      SystemName::Expint,                            expint_op,                          "expint"sv},
            {                   SystemName::Factorial,                         factorial_op,                       "factorial"sv},
            {                        SystemName::FDim,                              fdim_op,                            "fdim"sv},
            {               SystemName::FeClearExcept,                     feclearexcept_op,                 "fe-clear-except"sv},
            {                    SystemName::FeGetEnv,                          fegetenv_op,                      "fe-get-env"sv},
            {                  SystemName::FeGetRound,                        fegetround_op,                    "fe-get-round"sv},
            {                SystemName::FeHoldExcept,                      feholdexcept_op,                  "fe-hold-except"sv},
            {               SystemName::FeRaiseExcept,                     feraiseexcept_op,                 "fe-raise-except"sv},
            {                    SystemName::FeSetEnv,                          fesetenv_op,                      "fe-set-env"sv},
            {                  SystemName::FeSetRound,                        fesetround_op,                    "fe-set-round"sv},
            {                SystemName::FeTestExcept,                      fetestexcept_op,                  "fe-test-except"sv},
            {                 SystemName::FeUpdateEnv,                       feupdateenv_op,                   "fe-update-env"sv},
            {                  SystemName::FileExists,                        fileexists_op,                    "file-exists?"sv},
            {                    SystemName::FileSize,                          filesize_op,                       "file-size"sv},
            {                    SystemName::FileStat,                          filestat_op,                       "file-stat"sv},
            {                      SystemName::Filter,                            filter_op,                          "filter"sv},
            {                  SystemName::FilterDict,                        filterdict_op,                     "filter-dict"sv},
            {                     SystemName::FlatMap,                           flatmap_op,                        "flat-map"sv},
            {                     SystemName::Flatten,                           flatten_op,                         "flatten"sv},
            {                  SystemName::Fletcher32,                        fletcher32_op,                      "fletcher32"sv},
            {            SystemName::Fletcher32Stream,                 fletcher32_stream_op,               "fletcher32-stream"sv},
            {              SystemName::FilenameForAll,                    filenameforall_op,                "filename-for-all"sv},
            {                        SystemName::Find,                              find_op,                            "find"sv},
            {                         SystemName::FMA,                               fma_op,                             "fma"sv},
            {                        SystemName::FMax,                              fmax_op,                            "fmax"sv},
            {                     SystemName::FMaxMag,                           fmaxmag_op,                        "fmax-mag"sv},
            {                        SystemName::FMin,                              fmin_op,                            "fmin"sv},
            {                     SystemName::FMinMag,                           fminmag_op,                        "fmin-mag"sv},
            {                        SystemName::FMod,                              fmod_op,                            "fmod"sv},
            {                  SystemName::FPClassify,                        fpclassify_op,                     "fp-classify"sv},
            {                       SystemName::Frexp,                             frexp_op,                           "frexp"sv},
            {                 SystemName::Frequencies,                       frequencies_op,                     "frequencies"sv},
            {                       SystemName::Hypot,                             hypot_op,                           "hypot"sv},
            {                     SystemName::Hermite,                           hermite_op,                         "hermite"sv},
            {                       SystemName::ILogB,                             ilogb_op,                           "ilogb"sv},
            {                     SystemName::IsActor,                          is_actor_op,                        "is-actor"sv},
            {                   SystemName::IsAddress,                        is_address_op,                      "is-address"sv},
            {                 SystemName::IsCoroutine,                      is_coroutine_op,                    "is-coroutine"sv},
            {                     SystemName::IsArray,                          is_array_op,                        "is-array"sv},
            {                   SystemName::IsBoolean,                        is_boolean_op,                      "is-boolean"sv},
            {                      SystemName::IsByte,                           is_byte_op,                         "is-byte"sv},
            {                      SystemName::IsCell,                           is_cell_op,                         "is-cell"sv},
            {                     SystemName::IsCurry,                          is_curry_op,                        "is-curry"sv},
            {                      SystemName::IsDict,                           is_dict_op,                         "is-dict"sv},
            {                    SystemName::IsDouble,                         is_double_op,                       "is-double"sv},
            {                    SystemName::IsFinite,                          isfinite_op,                         "finite?"sv},
            {                     SystemName::IsFloat,                 is_floating_point_op,                        "is-float"sv},
            {                   SystemName::IsGreater,                         isgreater_op,                        "greater?"sv},
            {              SystemName::IsGreaterEqual,                    isgreaterequal_op,                  "greater-equal?"sv},
            {                    SystemName::Hostname,                          hostname_op,                        "hostname"sv},
            {               SystemName::IsHomogeneous,                     ishomogeneous_op,                    "homogeneous?"sv},
            {                       SystemName::IsInf,                             isinf_op,                            "inf?"sv},
            {                   SystemName::IsInteger,                        is_integer_op,                      "is-integer"sv},
            {                      SystemName::IsLess,                            isless_op,                           "less?"sv},
            {                 SystemName::IsLessEqual,                       islessequal_op,                     "less-equal?"sv},
            {               SystemName::IsLessGreater,                     islessgreater_op,                   "less-greater?"sv},
            {                  SystemName::IsLeapYear,                      is_leap_year_op,                      "leap-year?"sv},
            {                      SystemName::IsLong,                           is_long_op,                         "is-long"sv},
            {                      SystemName::IsMark,                           is_mark_op,                         "is-mark"sv},
            {                      SystemName::IsName,                           is_name_op,                         "is-name"sv},
            {                       SystemName::IsNaN,                             isnan_op,                            "nan?"sv},
            {                    SystemName::IsNormal,                          isnormal_op,                         "normal?"sv},
            {                      SystemName::IsNull,                           is_null_op,                         "is-null"sv},
            {                    SystemName::IsNumber,                         is_number_op,                       "is-number"sv},
            {                  SystemName::IsOperator,                       is_operator_op,                     "is-operator"sv},
            {                    SystemName::IsPacked,                         is_packed_op,                       "is-packed"sv},
            {                SystemName::IsPipeBuffer,                    is_pipe_buffer_op,                   "is-pipebuffer"sv},
            {                SystemName::IsSupervisor,                     is_supervisor_op,                   "is-supervisor"sv},
            {                   SystemName::PrimePred,                        prime_pred_op,                          "prime?"sv},
            {                      SystemName::IsReal,                           is_real_op,                         "is-real"sv},
            {                    SystemName::IsRecord,                         is_record_op,                       "is-record"sv},
            {                       SystemName::IsSet,                            is_set_op,                          "is-set"sv},
            {                SystemName::IsSignedType,                is_signed_integral_op,                       "is-signed"sv},
            {                    SystemName::IsStream,                         is_stream_op,                       "is-stream"sv},
            {                    SystemName::IsString,                         is_string_op,                       "is-string"sv},
            {                    SystemName::IsTagged,                         is_tagged_op,                       "is-tagged"sv},
            {                     SystemName::IsThunk,                          is_thunk_op,                        "is-thunk"sv},
            {                  SystemName::IsUInteger,                       is_uinteger_op,                     "is-uinteger"sv},
            {                     SystemName::IsULong,                          is_ulong_op,                        "is-ulong"sv},
            {                    SystemName::IsInt128,                         is_int128_op,                       "is-int128"sv},
            {                   SystemName::IsUInt128,                        is_uint128_op,                      "is-uint128"sv},
            {                  SystemName::IsUnsigned,              is_unsigned_integral_op,                     "is-unsigned"sv},
            {                 SystemName::IsUnordered,                       isunordered_op,                      "unordered?"sv},
            {                       SystemName::LDExp,                             ldexp_op,                           "ldexp"sv},
            {                      SystemName::LGamma,                            lgamma_op,                          "lgamma"sv},
            {                       SystemName::Log10,                             log10_op,                           "log10"sv},
            {                       SystemName::Log1p,                             log1p_op,                           "log1p"sv},
            {                        SystemName::Log2,                              log2_op,                            "log2"sv},
            {                        SystemName::LogB,                              logb_op,                            "logb"sv},
            {                        SystemName::ModF,                              modf_op,                            "modf"sv},
            {                  SystemName::NanPayload,                        nanpayload_op,                     "nan-payload"sv},
            {                         SystemName::NCR,                               ncr_op,                             "nCr"sv},
            {              SystemName::NanWithPayload,                    nanwithpayload_op,                "nan-with-payload"sv},
            {                   SystemName::NearbyInt,                         nearbyint_op,                      "nearby-int"sv},
            {                   SystemName::NextAfter,                         nextafter_op,                      "next-after"sv},
            {                  SystemName::NextToward,                        nexttoward_op,                     "next-toward"sv},
            {                         SystemName::Nip,                               nip_op,                             "nip"sv},
            {                    SystemName::PopCount,                          popcount_op,                       "pop-count"sv},
            {                         SystemName::Pow,                               pow_op,                             "pow"sv},
            {                      SystemName::PowMod,                            powmod_op,                         "pow-mod"sv},
            {                     SystemName::QuotRem,                           quotrem_op,                        "quot-rem"sv},
            {                   SystemName::Remainder,                         remainder_op,                       "remainder"sv},
            {                      SystemName::RemQuo,                            remquo_op,                         "rem-quo"sv},
            {                SystemName::RepeatString,                      repeatstring_op,                   "repeat-string"sv},
            {                     SystemName::Replace,                           replace_op,                         "replace"sv},
            {                        SystemName::Rint,                              rint_op,                            "rint"sv},
            {                       SystemName::Rmdir,                             rmdir_op,                           "rmdir"sv},
            {                      SystemName::ScalBN,                            scalbn_op,                          "scalbn"sv},
            {                        SystemName::Sign,                              sign_op,                            "sign"sv},
            {                     SystemName::SignBit,                           signbit_op,                        "sign-bit"sv},
            {                        SystemName::Sinh,                              sinh_op,                            "sinh"sv},
            {                       SystemName::Sleep,                             sleep_op,                 "coroutine-sleep"sv},
            {               SystemName::SlidingWindow,                    sliding_window_op,                  "sliding-window"sv},
            {                        SystemName::Sort,                              sort_op,                            "sort"sv},
            {                      SystemName::SortBy,                            sortby_op,                         "sort-by"sv},
            {                   SystemName::SphBessel,                        sph_bessel_op,                      "sph-bessel"sv},
            {                 SystemName::SphLegendre,                      sph_legendre_op,                    "sph-legendre"sv},
            {                  SystemName::SphNeumann,                       sph_neumann_op,                     "sph-neumann"sv},
            {                       SystemName::Split,                             split_op,                           "split"sv},
            {                   SystemName::SpawnLink,                        spawn_link_op,                      "spawn-link"sv},
            {                SystemName::SpawnMonitor,                     spawn_monitor_op,                   "spawn-monitor"sv},
            {                        SystemName::Take,                              take_op,                            "take"sv},
            {                   SystemName::TakeWhile,                         takewhile_op,                      "take-while"sv},
            {                        SystemName::Tanh,                              tanh_op,                            "tanh"sv},
            {                         SystemName::Tag,                               tag_op,                             "tag"sv},
            {                       SystemName::Untag,                             untag_op,                           "untag"sv},
            {                     SystemName::TagName,                          tag_name_op,                        "tag-name"sv},
            {                    SystemName::TagValue,                         tag_value_op,                       "tag-value"sv},
            {                     SystemName::TagPred,                          tag_pred_op,                            "tag?"sv},
            {                    SystemName::TagMatch,                         tag_match_op,                       "tag-match"sv},
            {                   SystemName::TagUpdate,                        tag_update_op,                      "tag-update"sv},
            {                  SystemName::TagValueOr,                      tag_value_or_op,                    "tag-value-or"sv},
            {                     SystemName::TagBind,                          tag_bind_op,                        "tag-bind"sv},
            {                      SystemName::TGamma,                            tgamma_op,                          "tgamma"sv},
            {                       SystemName::Thunk,                             thunk_op,                           "thunk"sv},
            {             SystemName::ThunkEvaluatedQ,                   thunk_evaluated_op,                "thunk-evaluated?"sv},
            {                  SystemName::ThunkReset,                       thunk_reset_op,                     "thunk-reset"sv},
            {                        SystemName::Time,                              time_op,                            "time"sv},
            {                        SystemName::Trim,                              trim_op,                            "trim"sv},
            {                    SystemName::TrimLeft,                          trimleft_op,                       "trim-left"sv},
            {                   SystemName::TrimRight,                         trimright_op,                      "trim-right"sv},
            {                        SystemName::Tuck,                              tuck_op,                            "tuck"sv},
            {                   SystemName::IsLazySeq,                       is_lazy_seq_op,                       "lazy-seq?"sv},
            {                  SystemName::LazyEmptyQ,                        lazy_empty_op,                     "lazy-empty?"sv},
            {                    SystemName::LazyHead,                         lazy_head_op,                       "lazy-head"sv},
            {                    SystemName::LazyTail,                         lazy_tail_op,                       "lazy-tail"sv},
            {                     SystemName::LazyNil,                          lazy_nil_op,                        "lazy-nil"sv},
            {                    SystemName::LazyCons,                         lazy_cons_op,                       "lazy-cons"sv},
            {                     SystemName::LazySeq,                          lazy_seq_op,                        "lazy-seq"sv},
            {                    SystemName::LazyFrom,                         lazy_from_op,                       "lazy-from"sv},
            {                   SystemName::LazyRange,                        lazy_range_op,                      "lazy-range"sv},
            {                  SystemName::LazyRepeat,                       lazy_repeat_op,                     "lazy-repeat"sv},
            {                 SystemName::LazyRepeatN,                     lazy_repeat_n_op,                   "lazy-repeat-n"sv},
            {                 SystemName::LazyIterate,                      lazy_iterate_op,                    "lazy-iterate"sv},
            {                   SystemName::LazyCycle,                        lazy_cycle_op,                      "lazy-cycle"sv},
            {                  SystemName::LazyUnfold,                       lazy_unfold_op,                     "lazy-unfold"sv},
            {                     SystemName::LazyMap,                          lazy_map_op,                        "lazy-map"sv},
            {                  SystemName::LazyFilter,                       lazy_filter_op,                     "lazy-filter"sv},
            {               SystemName::LazyFilterNot,                   lazy_filter_not_op,                 "lazy-filter-not"sv},
            {               SystemName::LazyFilterMap,                   lazy_filter_map_op,                 "lazy-filter-map"sv},
            {                 SystemName::LazyFlatMap,                     lazy_flat_map_op,                   "lazy-flat-map"sv},
            {                 SystemName::LazyFlatten,                      lazy_flatten_op,                    "lazy-flatten"sv},
            {              SystemName::LazyMapIndexed,                  lazy_map_indexed_op,                "lazy-map-indexed"sv},
            {                    SystemName::LazyScan,                         lazy_scan_op,                       "lazy-scan"sv},
            {                    SystemName::LazyTake,                         lazy_take_op,                       "lazy-take"sv},
            {                    SystemName::LazyDrop,                         lazy_drop_op,                       "lazy-drop"sv},
            {               SystemName::LazyTakeWhile,                   lazy_take_while_op,                 "lazy-take-while"sv},
            {               SystemName::LazyDropWhile,                   lazy_drop_while_op,                 "lazy-drop-while"sv},
            {                  SystemName::LazyDedupe,                       lazy_dedupe_op,                     "lazy-dedupe"sv},
            {             SystemName::LazyIntersperse,                  lazy_intersperse_op,                "lazy-intersperse"sv},
            {                  SystemName::LazyStepBy,                      lazy_step_by_op,                    "lazy-step-by"sv},
            {                     SystemName::LazyZip,                          lazy_zip_op,                        "lazy-zip"sv},
            {                 SystemName::LazyZipWith,                     lazy_zip_with_op,                   "lazy-zip-with"sv},
            {                   SystemName::LazyChain,                        lazy_chain_op,                      "lazy-chain"sv},
            {              SystemName::LazyInterleave,                   lazy_interleave_op,                 "lazy-interleave"sv},
            {               SystemName::LazyEnumerate,                    lazy_enumerate_op,                  "lazy-enumerate"sv},
            {                 SystemName::LazyChunked,                      lazy_chunked_op,                    "lazy-chunked"sv},
            {                SystemName::LazyWindowed,                     lazy_windowed_op,                   "lazy-windowed"sv},
            {                SystemName::LazyPairwise,                     lazy_pairwise_op,                   "lazy-pairwise"sv},
            {                 SystemName::LazyToArray,                     lazy_to_array_op,                   "lazy-to-array"sv},
            {                    SystemName::LazyFold,                         lazy_fold_op,                       "lazy-fold"sv},
            {                 SystemName::LazyForEach,                     lazy_for_each_op,                   "lazy-for-each"sv},
            {                     SystemName::LazyAny,                          lazy_any_op,                        "lazy-any"sv},
            {                     SystemName::LazyAll,                          lazy_all_op,                        "lazy-all"sv},
            {                    SystemName::LazyFind,                         lazy_find_op,                       "lazy-find"sv},
            {               SystemName::LazyFindIndex,                   lazy_find_index_op,                 "lazy-find-index"sv},
            {                     SystemName::LazyNth,                          lazy_nth_op,                        "lazy-nth"sv},
            {                   SystemName::LazyCount,                        lazy_count_op,                      "lazy-count"sv},
            {                     SystemName::LazySum,                          lazy_sum_op,                        "lazy-sum"sv},
            {                      SystemName::Module,                            module_op,                          "module"sv},
            {                  SystemName::ModuleDict,                       module_dict_op,                     "module-dict"sv},
            {                  SystemName::ModulePred,                       module_pred_op,                         "module?"sv},
            {                         SystemName::Use,                               use_op,                             "use"sv},
            {                      SystemName::Import,                            import_op,                          "import"sv},
            {               SystemName::RequireModule,                    require_module_op,                  "require-module"sv},
            {                       SystemName::SetOp,                               set_op,                             "set"sv},
            {                      SystemName::SetAdd,                           set_add_op,                         "set-add"sv},
            {               SystemName::SetAddPersist,                   set_add_persist_op,                 "set-add-persist"sv},
            {                   SystemName::SetRemove,                        set_remove_op,                      "set-remove"sv},
            {            SystemName::SetRemovePersist,                set_remove_persist_op,              "set-remove-persist"sv},
            {               SystemName::SetMemberPred,                   set_member_pred_op,                     "set-member?"sv},
            {                    SystemName::SetUnion,                         set_union_op,                       "set-union"sv},
            {             SystemName::SetIntersection,                  set_intersection_op,                "set-intersection"sv},
            {               SystemName::SetDifference,                    set_difference_op,                  "set-difference"sv},
            {               SystemName::SetSubsetPred,                   set_subset_pred_op,                         "subset?"sv},
            {      SystemName::SetSymmetricDifference,          set_symmetric_difference_op,            "symmetric-difference"sv},
            {             SystemName::SetDisjointPred,                 set_disjoint_pred_op,                       "disjoint?"sv},
            {                   SystemName::SetFilter,                        set_filter_op,                      "set-filter"sv},
            {                  SystemName::SetMembers,                       set_members_op,                         "members"sv},
            {                SystemName::SetFromArray,                    set_from_array_op,                  "set-from-array"sv},
            {            SystemName::AddressStatePred,                address_state_pred_op,                  "address-state?"sv},
            {                      SystemName::Select,                            select_op,                          "select"sv},
            {                      SystemName::Choice,                            choice_op,                          "choice"sv},
            {                    SystemName::CopyTerm,                         copy_term_op,                       "copy-term"sv},
            {                         SystemName::Cut,                               cut_op,                             "cut"sv},
            {        SystemName::atChoiceCountBarrier,           at_choice_count_barrier_op,           "@choice-count-barrier"sv},
            {           SystemName::atChoiceCountFail,              at_choice_count_fail_op,              "@choice-count-fail"sv},
            {            SystemName::atFindAllBarrier,               at_find_all_barrier_op,               "@find-all-barrier"sv},
            {               SystemName::atFindAllFail,                  at_find_all_fail_op,                  "@find-all-fail"sv},
            {              SystemName::atFindNBarrier,                 at_find_n_barrier_op,                 "@find-n-barrier"sv},
            {                 SystemName::atFindNFail,                    at_find_n_fail_op,                    "@find-n-fail"sv},
            {    SystemName::atForEachSolutionBarrier,      at_for_each_solution_barrier_op,      "@for-each-solution-barrier"sv},
            {       SystemName::atForEachSolutionFail,         at_for_each_solution_fail_op,         "@for-each-solution-fail"sv},
            {          SystemName::atAggregateBarrier,              at_aggregate_barrier_op,              "@aggregate-barrier"sv},
            {             SystemName::atAggregateFail,                 at_aggregate_fail_op,                 "@aggregate-fail"sv},
            {    SystemName::atAggregateReduceBarrier,       at_aggregate_reduce_barrier_op,       "@aggregate-reduce-barrier"sv},
            {       SystemName::atAggregateReduceFail,          at_aggregate_reduce_fail_op,          "@aggregate-reduce-fail"sv},
            {                SystemName::atNafBarrier,                    at_naf_barrier_op,                    "@naf-barrier"sv},
            {                   SystemName::atNafFail,                       at_naf_fail_op,                       "@naf-fail"sv},
            {               SystemName::atOnceBarrier,                   at_once_barrier_op,                   "@once-barrier"sv},
            {                  SystemName::atOnceFail,                      at_once_fail_op,                      "@once-fail"sv},
            {         SystemName::atUnifyMatchBarrier,            at_unify_match_barrier_op,            "@unify-match-barrier"sv},
            {            SystemName::atUnifyMatchFail,               at_unify_match_fail_op,               "@unify-match-fail"sv},
            {                   SystemName::Aggregate,                         aggregate_op,                       "aggregate"sv},
            {                 SystemName::ChoiceCount,                      choice_count_op,                    "choice-count"sv},
            {                       SystemName::Deref,                             deref_op,                           "deref"sv},
            {                        SystemName::Fail,                              fail_op,                            "fail"sv},
            {                     SystemName::FindAll,                          find_all_op,                        "find-all"sv},
            {                       SystemName::FindN,                            find_n_op,                          "find-n"sv},
            {             SystemName::ForEachSolution,                 for_each_solution_op,               "for-each-solution"sv},
            {                     SystemName::IsBound,                          is_bound_op,                          "bound?"sv},
            {                  SystemName::IsLogicVar,                      is_logic_var_op,                    "is-logic-var"sv},
            {                    SystemName::LogicVar,                         logic_var_op,                       "logic-var"sv},
            {                    SystemName::LvarName,                         lvar_name_op,                       "lvar-name"sv},
            {                         SystemName::Naf,                               naf_op,                             "naf"sv},
            {                    SystemName::NamedVar,                         named_var_op,                       "named-var"sv},
            {                        SystemName::Once,                              once_op,                            "once"sv},
            {                       SystemName::Guard,                             guard_op,                           "guard"sv},
            {                       SystemName::Unify,                             unify_op,                           "unify"sv},
            {                  SystemName::UnifyMatch,                       unify_match_op,                     "unify-match"sv},
            {                 SystemName::DefProtocol,                      def_protocol_op,                    "def-protocol"sv},
            {                   SystemName::DefMethod,                        def_method_op,                      "def-method"sv},
            {            SystemName::DefDefaultMethod,                def_default_method_op,              "def-default-method"sv},
            {              SystemName::ExtendProtocol,                   extend_protocol_op,                 "extend-protocol"sv},
            {           SystemName::ProtocolSatisfies,                protocol_satisfies_op,             "protocol-satisfies?"sv},
            {             SystemName::ProtocolMethods,                  protocol_methods_op,                "protocol-methods"sv},
            {                         SystemName::Let,                               let_op,                             "let"sv},
            {                 SystemName::Destructure,                       destructure_op,                     "destructure"sv},
            {                       SystemName::Match,                             match_op,                           "match"sv},
            {                    SystemName::MatchAll,                         match_all_op,                       "match-all"sv},
            {                        SystemName::When,                              when_op,                            "when"sv},
            {                        SystemName::Cond,                              cond_op,                            "cond"sv},
            {                       SystemName::XfMap,                            xf_map_op,                          "xf-map"sv},
            {                    SystemName::XfFilter,                         xf_filter_op,                       "xf-filter"sv},
            {                      SystemName::XfTake,                           xf_take_op,                         "xf-take"sv},
            {                      SystemName::XfDrop,                           xf_drop_op,                         "xf-drop"sv},
            {                      SystemName::XfScan,                           xf_scan_op,                         "xf-scan"sv},
            {                   SystemName::XfFlatten,                        xf_flatten_op,                      "xf-flatten"sv},
            {                  SystemName::XfDistinct,                       xf_distinct_op,                     "xf-distinct"sv},
            {                   SystemName::XfCompose,                        xf_compose_op,                      "xf-compose"sv},
            {                        SystemName::Into,                              into_op,                            "into"sv},
            {                    SystemName::LazyInto,                         lazy_into_op,                       "lazy-into"sv},
            {                    SystemName::PipeInto,                         pipe_into_op,                       "pipe-into"sv},
            {                    SystemName::XfReduce,                         xf_reduce_op,                       "xf-reduce"sv},
            {                   SystemName::GenServer,                        gen_server_op,                      "gen-server"sv},
            {                     SystemName::GenCall,                          gen_call_op,                        "gen-call"sv},
            {              SystemName::GenCallTimeout,                  gen_call_timeout_op,                "gen-call-timeout"sv},
            {                     SystemName::GenCast,                          gen_cast_op,                        "gen-cast"sv},
            {                     SystemName::GenStop,                          gen_stop_op,                        "gen-stop"sv},
            {                    SystemName::GenReply,                         gen_reply_op,                       "gen-reply"sv},
            {             SystemName::atGenServerInit,                at_gen_server_init_op,                "@gen-server-init"sv},
            {             SystemName::atGenServerRecv,                at_gen_server_recv_op,                "@gen-server-recv"sv},
            {         SystemName::atGenServerCallDone,           at_gen_server_call_done_op,           "@gen-server-call-done"sv},
            {         SystemName::atGenServerCastDone,           at_gen_server_cast_done_op,           "@gen-server-cast-done"sv},
            {        SystemName::atGenCallTimeoutDone,          at_gen_call_timeout_done_op,          "@gen-call-timeout-done"sv},
            {              SystemName::ClosureCapture,                   closure_capture_op,                 "closure-capture"sv},
            {             SystemName::ClosureWithDict,                 closure_with_dict_op,               "closure-with-dict"sv},
            {                  SystemName::ClosureEnv,                       closure_env_op,                     "closure-env"sv},
            {                SystemName::Precondition,                      precondition_op,                    "precondition"sv},
            {               SystemName::Postcondition,                     postcondition_op,                   "postcondition"sv},
            {         SystemName::PostconditionVerify,              postcondition_verify_op,            "postcondition-verify"sv},
            {               SystemName::atEnsureCheck,                   at_ensure_check_op,                   "@ensure-check"sv},
            {                SystemName::atClosureEnd,                    at_closure_end_op,                    "@closure-end"sv},
            {              SystemName::atMatchAllTest,                 at_match_all_test_op,                 "@match-all-test"sv},
            {                 SystemName::atMatchTest,                     at_match_test_op,                     "@match-test"sv},
            {                 SystemName::atSetForAll,                     at_set_forall_op,                    "@set-for-all"sv},
            {                     SystemName::Delimit,                           delimit_op,                         "delimit"sv},
            {                     SystemName::Capture,                           capture_op,                         "capture"sv},
            {                   SystemName::AbortExec,                        abort_exec_op,                      "abort-exec"sv},
            {              SystemName::IsContinuation,                   is_continuation_op,                 "is-continuation"sv},
            {            SystemName::atDelimitBarrier,                at_delimit_barrier_op,                "@delimit-barrier"sv},
            {                SystemName::HandleEffect,                     handle_effect_op,                   "handle-effect"sv},
            {                     SystemName::Perform,                           perform_op,                         "perform"sv},
            {             SystemName::atEffectBarrier,                 at_effect_barrier_op,                 "@effect-barrier"sv},
            {              SystemName::atHandlerScope,                  at_handler_scope_op,                  "@handler-scope"sv},
    };
    return rows;
}

[[nodiscard]] static consteval std::array<std::string_view, SYSVARIABLE_COUNT> build_sysvariable_table() {
    std::array<std::string_view, SYSVARIABLE_COUNT> table{};
    for (auto row : sysvariable_rows()) {
        table[static_cast<size_t>(+row.m_name)] = row.m_sv;
    }
    return table;
}

[[nodiscard]] static consteval std::array<std::string_view, WELLKNOWN_COUNT> build_wellknown_table() {
    std::array<std::string_view, WELLKNOWN_COUNT> table{};
    for (auto row : wellknown_rows()) {
        table[static_cast<size_t>(+row.m_name)] = row.m_sv;
    }
    return table;
}

[[nodiscard]] static consteval std::array<Operator, SYSOPERATOR_COUNT> build_sysoperator_table() {
    std::array<Operator, SYSOPERATOR_COUNT> table{};
    for (auto row : sysoperator_rows()) {
        table[static_cast<size_t>(+row.m_name) - SYSVARIABLE_COUNT] = Operator{row.m_func, row.m_sv};
    }
    return table;
}

// Build-discriminating fingerprint of the system-operator table: FNV-1a over
// every operator NAME, in dispatch-row listing order, with a 0xFF separator
// after each name (so {"ab","c"} stays distinct from {"a","bc"}).  The value
// diverges between build configurations whose operator sets differ (debug-only
// ops are #ifdef'd out of the row list), so a snap-shot can carry it and thaw
// can reject a cross-build image instead of silently dispatching the wrong
// operator.  It is also sensitive to a pure re-listing of the rows -- harmless:
// that only forces a re-snap-shot, exactly like a format-version bump.
//
// constexpr, not consteval: the only callers (snap-shot write, thaw validate)
// live in ops_snapshot.inl, which is #included BEFORE dispatch.inl, so an
// immediate call would be a use-before-definition; both call sites are rare
// I/O paths where folding it (opt) or recomputing it (debug) is free.  Iterates
// the sysoperator_rows() span (16 bytes over a static-storage array) rather
// than build_sysoperator_table() (a ~24 KB by-value array) to stay within the
// -Wstack-usage budget when evaluated at run time.
[[nodiscard]] static constexpr uint32_t compute_operator_table_signature() {
    uint32_t hash = 0x811C9DC5U;  // FNV-1a 32-bit offset basis
    for (auto row : sysoperator_rows()) {
        for (char name_char : row.m_sv) {
            hash = (hash ^ static_cast<uint32_t>(static_cast<uint8_t>(name_char))) * 0x01000193U;
        }
        hash = (hash ^ uint32_t{0xFF}) * 0x01000193U;
    }
    return hash;
}

// Count of USER-FACING operators: dispatch rows whose name is not `@`-prefixed.
// The `@`-names are internal operators (reachable through systemdict but not
// meant to be typed directly), so they are excluded from the figure the docs
// advertise and the banner prints.  Build-specific -- debug-only ops are
// #ifdef'd out of the row list -- so an optimized build reports fewer than a
// debug build.  constexpr (cold-path callers: the banner and --about) for the
// same include-order reason as compute_operator_table_signature.
[[nodiscard]] static constexpr name_index_t user_facing_operator_count() {
    name_index_t count{0};
    for (auto row : sysoperator_rows()) {
        if (!row.m_sv.empty() && (row.m_sv.front() != '@')) {
            ++count;
        }
    }
    return count;
}
public:
// Completeness proof for all three dispatch tables, static_assert'ed in
// trix.h right after class Trix closes.  Row-count == slot-count plus
// no-empty-slot proves (pigeonhole) that every enum index has exactly one
// row in this build configuration.
[[nodiscard]] static consteval bool verify_dispatch_tables() {
    if (sysvariable_rows().size() != SYSVARIABLE_COUNT) {
        return false;
    } else if (wellknown_rows().size() != WELLKNOWN_COUNT) {
        return false;
    } else if (sysoperator_rows().size() != SYSOPERATOR_COUNT) {
        return false;
    } else {
        for (auto sv : build_sysvariable_table()) {
            if (sv.empty()) {
                return false;
            }
        }
        for (auto sv : build_wellknown_table()) {
            if (sv.empty()) {
                return false;
            }
        }
        // Only m_sv is checked: GCC will not constant-fold a function pointer
        // compared against nullptr.  An unfilled slot has BOTH fields empty and
        // every row supplies its m_func non-optionally, so the empty-sv check
        // alone detects holes.
        for (auto op : build_sysoperator_table()) {
            if (op.m_sv.empty()) {
                return false;
            }
        }
        return true;
    }
}
private:
[[nodiscard]] static std::string_view sysvariable_sv(name_index_t sysvariable_index) {
    static constexpr auto table{build_sysvariable_table()};

    assert((sysvariable_index <= +SystemName::LAST_VARIABLE) && "sysvariable_sv: unknown sysvariable_index");

    return table[sysvariable_index];
}

[[nodiscard]] static std::string_view wellknown_sv(name_index_t wellknown_index) {
    static constexpr auto table{build_wellknown_table()};

    assert((wellknown_index < WELLKNOWN_COUNT) && "wellknown_sv: unknown wellknown_index");

    return table[wellknown_index];
}

[[nodiscard]] static Operator sysoperator_value(name_index_t sysoperator_index) {
    static constexpr auto table{build_sysoperator_table()};

    assert((sysoperator_index >= +SystemName::FIRST_CONTROL_OP) && (sysoperator_index < SYSTEMNAME_COUNT) &&
           "sysoperator_value: unknown sysoperator_index");

    return table[static_cast<size_t>(sysoperator_index) - SYSVARIABLE_COUNT];
}

[[nodiscard]] static Operator sysname_value(name_index_t sysname_index) {
    if ((sysname_index >= +SystemName::FIRST_VARIABLE) && (sysname_index <= +SystemName::LAST_VARIABLE)) {
        return Operator{nullptr, sysvariable_sv(sysname_index)};
    } else {
        return sysoperator_value(sysname_index);
    }
}
