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

//===--- Per-Coroutine Scratch Arena Operators ---===//
//
// Thin wrappers over Trix::scratch_push / scratch_free / scratch_collect.
// Each running coroutine owns a private bump-and-collect arena carved
// from the back of its stack block; size is configurable via the
// --scratch-depth CLI flag (default 128 Objects = 1 KB).
//
// Surface (3 ops):
//   scratch-push    val --        push val into running coroutine's scratch
//   scratch-collect -- array      drain to fresh array, reset arena
//   scratch-clear   --            reset arena, freeing any ExtValue contents
//
// These ops are the public surface for the per-coroutine arena.  Internal
// C++ callers can invoke the Trix::scratch_push<Object> templates directly
// rather than dispatching through these ops; the ops stay public for any
// user code that wants a per-coroutine bump arena.

// scratch-push: val --
// Push val into the running coroutine's scratch arena.  Raises
// /vm-full when the arena is exhausted.
// throws: opstack-underflow, vm-full
static void scratch_push_op(Trix *trx) {
    trx->verify_operands(VerifyAny);
    auto val_obj = *trx->m_op_ptr--;
    trx->template scratch_push<Object>(val_obj);
}

// scratch-collect: -- array
// Drain the running coroutine's scratch arena into a freshly-allocated
// Array (in local VM, at the caller's save level).  Order is preserved
// (push order = result order).  Resets the arena to idle.
// throws: opstack-overflow, vm-full
static void scratch_collect_op(Trix *trx) {
    auto arr_obj = trx->template scratch_collect<Object>();
    trx->require_op_capacity(1);
    *++trx->m_op_ptr = arr_obj;
}

// scratch-clear: --
// Reset the running coroutine's scratch arena, freeing any ExtValue
// contents.  No allocation, no return value.
// throws: (none)
static void scratch_clear_op(Trix *trx) {
    trx->template scratch_free<Object>();
}
