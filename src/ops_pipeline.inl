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

//===--- Pipeline Operators ---===//
//
// Implements a concurrent dataflow pipeline model built on coroutines and
// bounded buffers.  Based on concepts from:
//
//   Unix pipes (Thompson & Ritchie, 1973): linear producer/consumer chains
//   with backpressure via bounded buffers.
//
//   CSP (Communicating Sequential Processes), C.A.R. Hoare, 1978:
//   synchronous message passing between sequential processes over channels.
//
//   Go channels / Clojure core.async / Kotlin Flow: modern bounded-channel
//   abstractions with blocking put/get semantics.
//
// The specific design is closest to Go channels: fixed-capacity FIFO buffers
// with blocking producers and consumers, except that blocking is cooperative
// (coroutine yield) rather than OS-thread-based.
//
// --- Core concepts for maintainers ---
//
// PIPE BUFFERS
//   A pipe buffer is a fixed-capacity circular queue (FIFO) for passing
//   Objects between coroutines.  Created with `pipe-buffer` (e.g.,
//   `8 pipe-buffer` creates a buffer with capacity 8).
//
//   The buffer enforces single-producer / single-consumer (SPSC) at
//   runtime: the first coroutine to call `pipe-put` becomes the producer,
//   the first to call `pipe-get` becomes the consumer.  Any other
//   coroutine attempting to put or get receives an error.
//
//   When the buffer is full, `pipe-put` suspends the producer coroutine
//   until the consumer drains an item (backpressure).  When the buffer is
//   empty, `pipe-get` suspends the consumer until the producer adds an
//   item.  Suspension uses the coroutine scheduler's targeted wakeup
//   mechanism -- no polling, no busy-waiting.
//
// PIPELINE CONSTRUCTION
//   A pipeline is a chain of processing stages connected by pipe buffers.
//   Each stage runs in its own coroutine and reads from an input buffer,
//   processes items, and writes to an output buffer.
//
//   Construction (stage) operators create the coroutines and wiring
//   automatically:
//     pipe-map       -- apply a proc to each item (1:1)
//     pipe-filter    -- keep items matching a predicate
//     pipe-tap       -- side-effect per item (1:1 passthrough)
//     pipe-flat-map  -- expand each item's array/packed result into items (1:N)
//     pipe-batch     -- collect N items into arrays
//     pipe-take      -- pass first N items, then close
//     pipe-drop      -- skip first N items, pass rest
//     pipe-scan      -- running fold, emitting each intermediate accumulator
//     pipe-distinct  -- drop duplicate items via a seen-set
//     pipe-window    -- sliding window of size N
//     pipe-merge     -- interleave N sources round-robin
//     pipe-zip       -- one item from each of N sources per cycle, as an array
//     pipe-into      -- apply a transducer as a pipeline stage
//
//   Terminal operators consume the pipeline and block the calling
//   coroutine until all stages complete:
//     pipe-run       -- drive pipeline, calling a proc on each item
//     pipe-collect   -- collect all output into an array
//     pipe-reduce    -- fold output into an accumulator
//
// BACKPRESSURE
//   Backpressure is automatic and inherent in the bounded buffer design.
//   A fast producer cannot overwhelm a slow consumer because `pipe-put`
//   blocks when the buffer is full.  This bounds memory usage to the sum
//   of all buffer capacities in the pipeline, regardless of how many items
//   are processed.
//
// CLOSING AND ERROR PROPAGATION
//   `pipe-close` marks the buffer as closed.  A closed buffer continues
//   to deliver buffered items; `pipe-get` returns false (no more data)
//   only after the buffer is both closed and empty.
//
//   `pipe-error-close` closes with an error code.  The consumer receives
//   the error on the next `pipe-get` after the buffer drains, and can
//   propagate it downstream.
//
// TARGETED WAKEUP
//   When a producer or consumer blocks, it stores its coroutine offset in
//   the buffer header (m_blocked_writer / m_blocked_reader).  The other
//   side wakes it directly by setting its status to Ready -- O(1), no
//   scheduler scan.  This is the same mechanism used by actor mailboxes
//   and coroutine-join.
//
// BUFFER REPRESENTATION
//   Pipe buffers are a dedicated VM type (Type::PipeBuffer, slot 25).
//   The Object holds a vm_offset_t pointing to a PipeBufferHeader struct
//   on the VM heap, immediately followed by an Object[capacity] data array.
//   The header contains read/write indices, occupancy, capacity, two
//   completion flags (producer-closed and consumer-dead), error code, and
//   producer/consumer/blocked coroutine offsets.  A fully-dead buffer (both
//   flags set) is recycled to a free-list pool.
//
// --- Operators ---
//
//   pipe-buffer      int -- buf          Create a buffer with given capacity
//   pipe-put         val buf --          Write item (blocks if full)
//   pipe-get         buf -- val bool     Read item (blocks if empty; false=EOS)
//   pipe-close       buf --              Mark buffer as closed
//   pipe-error-close int buf --          Close with error code
//   pipe-status      buf -- dict         Read-only introspection dict
//   is-pipebuffer    any -- bool         Test if value is a pipe buffer
//   pipe-map         source proc -- desc        Stage: transform each item (1:1)
//   pipe-filter      source pred -- desc        Stage: keep matching items
//   pipe-tap         source proc -- desc        Stage: side-effect passthrough
//   pipe-flat-map    source proc -- desc        Stage: expand array/packed result (1:N)
//   pipe-batch       source n -- desc           Stage: group into arrays of N
//   pipe-take        source n -- desc           Stage: first N items
//   pipe-drop        source n -- desc           Stage: skip first N items
//   pipe-scan        source init proc -- desc   Stage: running fold (emit each acc)
//   pipe-distinct    source -- desc             Stage: drop duplicates
//   pipe-window      source n -- desc           Stage: sliding window of N
//   pipe-merge       [sources] -- desc          Stage: round-robin interleave
//   pipe-zip         [sources] -- desc          Stage: N-item tuples per cycle
//   pipe-into        pipe xf -- pipe'           Stage: apply a transducer
//   pipe-run         source cap proc --         Terminal: drive, proc per item
//   pipe-collect     source cap -- array        Terminal: collect into array
//   pipe-reduce      source cap init proc -- val  Terminal: fold into accumulator
//
// Control operators (internal, not user-visible):
//   @pipe-put-retry    Resume blocked producer
//   @pipe-get-retry    Resume blocked consumer
//   @pipe-stage        State machine for batch/take/drop/distinct/window/merge/zip stages
//   @pipe-cleanup      Error propagation: error-close downstream buffer after @try-barrier
//

//===--- PipeBufferHeader ---===//
// Fixed-capacity circular buffer for inter-coroutine communication.
// Allocated on the VM heap: header + Object[capacity] data array.
//
// Single-producer / single-consumer (SPSC) enforced at runtime.
// Targeted wakeup via coroutine scheduler (no polling).

struct PipeBufferHeader {
    vm_offset_t m_data;            // offset to Object[capacity] array
    length_t m_head;               // read index (circular); 0xFFFF when parked on free-list pool (sentinel)
    length_t m_tail;               // write index (circular)
    length_t m_count;              // current occupancy
    length_t m_capacity;           // maximum items
    bool m_closed;                 // true when producer is done (signaled via pipe-close / error-close)
    bool m_consumer_dead;          // true when consumer has observed /empty (or signaled done)
    Error m_error;                 // NoError normally; set by pipe-error-close
    vm_offset_t m_producer;        // bound on first pipe-put (nulloffset = unbound)
    vm_offset_t m_consumer;        // bound on first pipe-get (nulloffset = unbound)
    vm_offset_t m_blocked_writer;  // coroutine waiting to put (nulloffset = none)
    vm_offset_t m_blocked_reader;  // coroutine waiting to get (nulloffset = none)
};
static_assert(sizeof(PipeBufferHeader) % sizeof(Object) == 0, "PipeBufferHeader must be Object-aligned for contiguous data array");

// Sentinel stored in m_head while a pipe buffer is parked on the free-list pool.
// Detects accidental use-after-recycle (sentinel check at pipe-put/pipe-get entry).
static constexpr length_t PipeRecycleSentinel{0xFFFF};

// Round a requested pipe capacity up to the nearest pool size class.  Returns
// {class_index, class_capacity}.  Returns {-1, requested} if above the largest
// class -- such buffers bypass the pool (allocated exact, abandoned on free).
static std::pair<int, length_t> pipe_class_for_capacity(length_t requested) {
    for (size_t i = 0; i < MaxPipePoolSize; ++i) {
        if (requested <= sm_pipe_pool_classes[i]) {
            return {static_cast<int>(i), sm_pipe_pool_classes[i]};
        }
    }
    return {-1, requested};
}

// Return a fully-dead pipe buffer (both producer and consumer signaled done)
// to the per-save-level pool for reuse.  Stamps m_head with the recycle
// sentinel so stale pipe-put/pipe-get calls can detect the recycled state
// and bail out instead of corrupting memory.  Free-list link reuses
// m_blocked_writer (unused while parked).
static void pipe_recycle(Trix *trx, PipeBufferHeader *hdr, vm_offset_t hdr_offset) {
    auto [class_idx, _] = pipe_class_for_capacity(hdr->m_capacity);
    if (class_idx >= 0) {
        if (hdr->m_head == PipeRecycleSentinel) {
            trx->error(Error::InternalError, "pipe_recycle: double-recycle at offset {}", hdr_offset);
        } else {
            // Pool is [MaxPipePoolSize] (class-indexed only).  Pipe buffers live
            // in global VM (BASE-immune); pool entries don't dangle on restore,
            // so the pool needs no per-save-level bucketing.
            auto pool_idx = static_cast<size_t>(class_idx);
            hdr->m_blocked_writer = trx->m_pipe_pool[pool_idx];
            hdr->m_head = PipeRecycleSentinel;
            trx->m_pipe_pool[pool_idx] = hdr_offset;
        }
    }
}

// Check both completion flags and recycle if both sides are done.
static void pipe_maybe_free(Trix *trx, PipeBufferHeader *hdr, vm_offset_t hdr_offset) {
    if (hdr->m_closed && hdr->m_consumer_dead) {
        pipe_recycle(trx, hdr, hdr_offset);
    }
}

//===--- Helpers ---===//

// Initialize a freshly-allocated PipeBufferHeader with default values.
static void init_pipe_buffer_header(PipeBufferHeader *hdr, vm_offset_t hdr_offset, length_t capacity) {
    hdr->m_data = static_cast<vm_offset_t>(hdr_offset + sizeof(PipeBufferHeader));
    hdr->m_head = 0;
    hdr->m_tail = 0;
    hdr->m_count = 0;
    hdr->m_capacity = capacity;
    hdr->m_closed = false;
    hdr->m_consumer_dead = false;
    hdr->m_error = Error::NoError;
    hdr->m_producer = nulloffset;
    hdr->m_consumer = nulloffset;
    hdr->m_blocked_writer = nulloffset;
    hdr->m_blocked_reader = nulloffset;
}

// Allocate-or-recycle a pipe buffer: try the per-save-level class pool first;
// fall back to fresh vm_alloc.  Physical capacity is the user's request
// rounded up to the nearest size class; logical capacity (m_capacity) is the
// user's exact request.  Same logical/physical split as mailbox -- see the
// comment in sm_pipe_pool_classes for the rationale.
static std::pair<PipeBufferHeader *, vm_offset_t> pipe_alloc_buffer(Trix *trx, length_t capacity) {
    auto [class_idx, class_cap] = pipe_class_for_capacity(capacity);
    if (class_idx >= 0) {
        // Pool is class-indexed only (pipe buffers live in global VM,
        // BASE-immune).
        auto pool_idx = static_cast<size_t>(class_idx);
        auto head = trx->m_pipe_pool[pool_idx];
        if (head != nulloffset) {
            auto hdr = trx->offset_to_ptr<PipeBufferHeader>(head);
            if (hdr->m_head != PipeRecycleSentinel) {
                trx->error(Error::InternalError, "pipe_alloc_buffer: recycled pipe missing sentinel");
            } else {
                trx->m_pipe_pool[pool_idx] = hdr->m_blocked_writer;  // next-free link
                // Reset all fields; keep m_data pointing at the existing trailing Object array.
                hdr->m_head = 0;
                hdr->m_tail = 0;
                hdr->m_count = 0;
                hdr->m_capacity = capacity;  // new user's logical cap; physical buffer is class_cap >= capacity
                hdr->m_closed = false;
                hdr->m_consumer_dead = false;
                hdr->m_error = Error::NoError;
                hdr->m_producer = nulloffset;
                hdr->m_consumer = nulloffset;
                hdr->m_blocked_writer = nulloffset;
                hdr->m_blocked_reader = nulloffset;
                return {hdr, head};
            }
        }
    }
    auto physical = (class_idx >= 0) ? class_cap : capacity;
    auto total = static_cast<vm_size_t>(sizeof(PipeBufferHeader) + physical * sizeof(Object));
    // Route through global VM so pipe buffers survive save/restore
    // (mirrors the mailbox pool).
    auto [hdr, hdr_offset] = trx->gvm_alloc<PipeBufferHeader>(total, Trix::ChunkKind::PipeBuffer);
    init_pipe_buffer_header(hdr, hdr_offset, capacity);
    return {hdr, hdr_offset};
}

// Close output pipe buffer and wake any blocked reader.
static void pipe_close_and_wake_reader(Trix *trx, PipeBufferHeader *hdr) {
    hdr->m_closed = true;
    auto reader = hdr->m_blocked_reader;
    hdr->m_blocked_reader = nulloffset;
    trx->coroutine_wake(reader);
}

//===--- Buffer Primitives ---===//

// pipe-buffer: capacity -- buffer
// Allocate a PipeBufferHeader + Object[capacity] as a single contiguous
// block on the VM heap.
// throws: opstack-underflow, range-check, type-check, vm-full
static void pipe_buffer_op(Trix *trx) {
    trx->verify_operands(VerifyInteger | VerifyNotNegative | VerifyNotZero);

    auto raw_capacity = trx->m_op_ptr->integer_value();
    if (raw_capacity > std::numeric_limits<length_t>::max()) {
        trx->error(Error::RangeCheck,
                   "pipe-buffer: capacity {} exceeds maximum {}",
                   raw_capacity,
                   std::numeric_limits<length_t>::max());
    }
    auto capacity = static_cast<length_t>(raw_capacity);

    auto [hdr, hdr_offset] = pipe_alloc_buffer(trx, capacity);
    static_cast<void>(hdr);

    *trx->m_op_ptr = Object::make_pipe_buffer(hdr_offset);
}

// Internal: complete pipe-put (buffer not full).
// Operand stack: ... value buffer
static void pipe_put_complete(Trix *trx, PipeBufferHeader *hdr, Object value) {
    auto data = trx->offset_to_ptr<Object>(hdr->m_data);
    data[hdr->m_tail] = value;
    hdr->m_tail = static_cast<length_t>((hdr->m_tail + 1) % hdr->m_capacity);
    ++hdr->m_count;

    // Wake blocked reader
    auto reader = hdr->m_blocked_reader;
    hdr->m_blocked_reader = nulloffset;
    trx->coroutine_wake(reader);
}

// @pipe-put-retry: (control operator, popcount=0)
// Continuation for pipe-put when buffer was full.
// Exec stack on entry: [buffer] (companion below the popped control op)
// Operand stack: ... value
static void at_pipe_put_retry_op(Trix *trx) {
    // Companion: buffer object (popcount=0, so interpreter popped the control op,
    // leaving m_exec_ptr at the companion)
    auto buffer_obj = *trx->m_exec_ptr;
    auto hdr = buffer_obj.pipe_buffer_header(trx);

    // Clear blocked writer since we're now running
    hdr->m_blocked_writer = nulloffset;

    // Check error
    if (hdr->m_error != Error::NoError) {
        --trx->m_exec_ptr;  // pop companion
        auto err = hdr->m_error;
        trx->error(err, "pipe-put: buffer has error");
    } else if (hdr->m_count < hdr->m_capacity) {
        // Space available: complete the put
        --trx->m_exec_ptr;  // pop companion
        auto val_obj = *trx->m_op_ptr;
        --trx->m_op_ptr;
        pipe_put_complete(trx, hdr, val_obj);
    } else {
        // Still full: re-sleep
        hdr->m_blocked_writer = trx->m_running_coroutine;
        trx->require_exec_capacity(1);
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atPipePutRetry);
        trx->coroutine_flush_running();
        auto ctx = trx->offset_to_ptr<CoroutineContext>(trx->m_running_coroutine);
        ctx->m_status = CoroutineContext::Sleeping;
        ctx->m_flags |= CoroutineContext::FlagBlocked;
        ctx->m_wake_time_ns = std::numeric_limits<uint64_t>::max();
        trx->coroutine_schedule();
    }
}

// pipe-put: value buffer --
// Write value into the buffer.  If full, block until space available.
// Enforces single-producer.  Raises on closed or errored buffer.
// throws: invalid-access, opstack-underflow, type-check
static void pipe_put_op(Trix *trx) {
    trx->require_op_count(2);

    auto buffer_ptr = trx->m_op_ptr;
    if (!buffer_ptr->is_pipe_buffer()) {
        trx->error(Error::TypeCheck, "pipe-put: expected pipebuffer, got {}", Object::type_sv(buffer_ptr->type()));
    } else {
        auto hdr = buffer_ptr->pipe_buffer_header(trx);

        // Sentinel: recycled buffer.  A stale producer holding a PipeBuffer Object
        // after both sides signaled done (and pool reclaimed) hits this check.
        if (hdr->m_head == PipeRecycleSentinel) {
            trx->error(Error::InvalidAccess, "pipe-put: buffer has been recycled (both sides already done)");
        } else {
            // SPSC: bind or verify producer
            if (hdr->m_producer == nulloffset) {
                hdr->m_producer = trx->m_running_coroutine;
            } else if (hdr->m_producer != trx->m_running_coroutine) {
                trx->error(Error::InvalidAccess, "pipe-put: wrong producer coroutine");
            }

            // Check closed
            if (hdr->m_closed) {
                trx->error(Error::InvalidAccess, "pipe-put: buffer is closed");
            } else {
                // Check errored.  Leave value+buffer on the stack like the recycled/closed
                // error paths above (a surrounding try-catch then sees a uniform stack across
                // all pipe-put error cases).
                if (hdr->m_error != Error::NoError) {
                    auto err = hdr->m_error;
                    trx->error(err, "pipe-put: buffer has error");
                } else {
                    auto saved_buffer_obj = *buffer_ptr;
                    --trx->m_op_ptr;  // pop buffer

                    if (hdr->m_count < hdr->m_capacity) {
                        // Space available: complete immediately
                        auto val_obj = *trx->m_op_ptr;
                        --trx->m_op_ptr;
                        pipe_put_complete(trx, hdr, val_obj);
                    } else {
                        // Buffer full: leave value on op stack, push continuation, sleep
                        // Exec stack: [buffer, @pipe-put-retry]
                        trx->require_exec_capacity(2);
                        *++trx->m_exec_ptr = saved_buffer_obj;
                        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atPipePutRetry);

                        hdr->m_blocked_writer = trx->m_running_coroutine;
                        trx->coroutine_flush_running();
                        auto ctx = trx->offset_to_ptr<CoroutineContext>(trx->m_running_coroutine);
                        ctx->m_status = CoroutineContext::Sleeping;
                        ctx->m_flags |= CoroutineContext::FlagBlocked;
                        ctx->m_wake_time_ns = std::numeric_limits<uint64_t>::max();
                        trx->coroutine_schedule();
                    }
                }
            }
        }
    }
}

// Internal: complete pipe-get when data is available.
// Reads from head, pushes value + true onto operand stack.
// hdr must have m_count > 0.
static void pipe_get_complete(Trix *trx, PipeBufferHeader *hdr) {
    auto data = trx->offset_to_ptr<Object>(hdr->m_data);
    auto val_obj = data[hdr->m_head];
    hdr->m_head = static_cast<length_t>((hdr->m_head + 1) % hdr->m_capacity);
    --hdr->m_count;

    // Wake blocked writer
    auto writer = hdr->m_blocked_writer;
    hdr->m_blocked_writer = nulloffset;
    trx->coroutine_wake(writer);

    // Push: value true
    trx->require_op_capacity(2);
    *++trx->m_op_ptr = val_obj;
    *++trx->m_op_ptr = Object::make_boolean(true);
}

// Internal: handle pipe-get end-of-stream (closed + drained).
// Pushes false, or re-raises stored error.  Signals consumer-done: this is
// the consumer's last interaction with the buffer.  If producer also already
// dead (m_closed), triggers recycle to the free-list pool.
static void pipe_get_end(Trix *trx, PipeBufferHeader *hdr, vm_offset_t hdr_offset) {
    hdr->m_consumer_dead = true;
    if (hdr->m_error != Error::NoError) {
        auto err = hdr->m_error;
        // Error path: raise now.  Recycle still safe -- m_closed is true and both flags set.
        pipe_maybe_free(trx, hdr, hdr_offset);
        trx->error(err, "pipe-get: upstream error");
    } else {
        // Normal end-of-stream: push false, recycle if both sides done.
        trx->require_op_capacity(1);
        *++trx->m_op_ptr = Object::make_boolean(false);
        pipe_maybe_free(trx, hdr, hdr_offset);
    }
}

// @pipe-get-retry: (control operator)
// Continuation for pipe-get when buffer was empty.
// Exec stack: [buffer, @pipe-get-retry]
static void at_pipe_get_retry_op(Trix *trx) {
    auto buffer_obj = *trx->m_exec_ptr;
    auto hdr_offset = buffer_obj.pipe_buffer_offset();
    auto hdr = buffer_obj.pipe_buffer_header(trx);

    // Clear blocked reader since we're now running
    hdr->m_blocked_reader = nulloffset;

    if (hdr->m_count > 0) {
        // Data available
        --trx->m_exec_ptr;  // pop companion
        pipe_get_complete(trx, hdr);
    } else if (hdr->m_closed) {
        // Closed + drained
        --trx->m_exec_ptr;  // pop companion
        pipe_get_end(trx, hdr, hdr_offset);
    } else {
        // Still empty and not closed: re-sleep
        hdr->m_blocked_reader = trx->m_running_coroutine;
        trx->require_exec_capacity(1);
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atPipeGetRetry);
        trx->coroutine_flush_running();
        auto ctx = trx->offset_to_ptr<CoroutineContext>(trx->m_running_coroutine);
        ctx->m_status = CoroutineContext::Sleeping;
        ctx->m_flags |= CoroutineContext::FlagBlocked;
        ctx->m_wake_time_ns = std::numeric_limits<uint64_t>::max();
        trx->coroutine_schedule();
    }
}

// pipe-get: buffer -- value true  |  buffer -- false
// Read next value from the buffer.  If empty and not closed, block.
// Returns value+true for data, false for end-of-stream (closed+drained+no error).
// Re-raises stored error if closed+drained+errored.
// Enforces single-consumer.
// throws: invalid-access, opstack-underflow, type-check, <stored error>
static void pipe_get_op(Trix *trx) {
    trx->verify_operands(VerifyPipeBuffer);

    auto buffer_obj = *trx->m_op_ptr;
    --trx->m_op_ptr;  // pop buffer
    auto hdr_offset = buffer_obj.pipe_buffer_offset();
    auto hdr = buffer_obj.pipe_buffer_header(trx);

    // Sentinel: recycled buffer (both sides had already signaled done).  Treat
    // as closed+drained so stale handles read /empty instead of corrupt data.
    if (hdr->m_head == PipeRecycleSentinel) {
        trx->error(Error::InvalidAccess, "pipe-get: buffer has been recycled (both sides already done)");
    } else {
        // SPSC: bind or verify consumer
        if (hdr->m_consumer == nulloffset) {
            hdr->m_consumer = trx->m_running_coroutine;
        } else if (hdr->m_consumer != trx->m_running_coroutine) {
            trx->error(Error::InvalidAccess, "pipe-get: wrong consumer coroutine");
        }

        if (hdr->m_count > 0) {
            // Data available: complete immediately
            pipe_get_complete(trx, hdr);
        } else if (hdr->m_closed) {
            // Closed + drained
            pipe_get_end(trx, hdr, hdr_offset);
        } else {
            // Empty and not closed: push continuation, sleep
            trx->require_exec_capacity(2);
            *++trx->m_exec_ptr = buffer_obj;
            *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atPipeGetRetry);

            hdr->m_blocked_reader = trx->m_running_coroutine;
            trx->coroutine_flush_running();
            auto ctx = trx->offset_to_ptr<CoroutineContext>(trx->m_running_coroutine);
            ctx->m_status = CoroutineContext::Sleeping;
            ctx->m_flags |= CoroutineContext::FlagBlocked;
            ctx->m_wake_time_ns = std::numeric_limits<uint64_t>::max();
            trx->coroutine_schedule();
        }
    }
}

// pipe-close: buffer --
// Set the closed flag.  Wake blocked reader.  Idempotent.
// If consumer has already signaled done (got /empty earlier), recycle the
// buffer to the free-list pool now that both sides are dead.
// throws: opstack-underflow, type-check
static void pipe_close_op(Trix *trx) {
    trx->verify_operands(VerifyPipeBuffer);

    auto buffer_obj = *trx->m_op_ptr;
    auto hdr_offset = buffer_obj.pipe_buffer_offset();
    auto hdr = buffer_obj.pipe_buffer_header(trx);
    if (hdr->m_head != PipeRecycleSentinel) {
        pipe_close_and_wake_reader(trx, hdr);
        pipe_maybe_free(trx, hdr, hdr_offset);
    }
    --trx->m_op_ptr;
}

// pipe-error-close: error-code buffer --
// Set the closed flag + store error.  Wake blocked reader and writer.  Idempotent.
// Error-code is an integer (Error enum ordinal).
// throws: opstack-underflow, range-check, type-check
static void pipe_error_close_op(Trix *trx) {
    trx->require_op_count(2);

    auto buffer_ptr = trx->m_op_ptr;
    if (!buffer_ptr->is_pipe_buffer()) {
        trx->error(Error::TypeCheck, "pipe-error-close: expected pipebuffer, got {}", Object::type_sv(buffer_ptr->type()));
    } else {
        auto hdr_offset = buffer_ptr->pipe_buffer_offset();
        auto hdr = buffer_ptr->pipe_buffer_header(trx);

        // Sentinel: recycled buffer.  Silent no-op on close-of-dead-buffer matches
        // the idempotent-close semantic; nothing to wake, nothing to free again.
        if (hdr->m_head == PipeRecycleSentinel) {
            trx->m_op_ptr -= 2;
        } else {
            auto error_ptr = trx->m_op_ptr - 1;
            if (!error_ptr->is_integer()) {
                trx->error(Error::TypeCheck,
                           "pipe-error-close: expected integer error code, got {}",
                           Object::type_sv(error_ptr->type()));
            } else {
                auto error_code = error_ptr->integer_value();
                if ((error_code < 0) || (error_code >= ErrorCount)) {
                    trx->error(Error::RangeCheck, "pipe-error-close: error code {} out of range", error_code);
                } else {
                    hdr->m_closed = true;
                    hdr->m_error = static_cast<Error>(error_code);

                    // Wake both blocked reader and writer
                    auto reader = hdr->m_blocked_reader;
                    hdr->m_blocked_reader = nulloffset;
                    trx->coroutine_wake(reader);

                    auto writer = hdr->m_blocked_writer;
                    hdr->m_blocked_writer = nulloffset;
                    trx->coroutine_wake(writer);

                    pipe_maybe_free(trx, hdr, hdr_offset);

                    trx->m_op_ptr -= 2;
                }
            }
        }
    }
}

//===--- Pipeline Construction ---===//

// Helper: create a read-only pipeline descriptor dict with /type and /source.
static Object pipe_make_descriptor(Trix *trx, std::string_view type_name, Object source, length_t capacity = 3) {
    auto type_key = trx->wellknown_name(WellKnownName::IKeyType);
    auto source_key = trx->wellknown_name(WellKnownName::IKeySource);
    auto type_val = Name::make(trx, type_name);

    auto [dict, dict_offset] = Dict::create_or_recycle(trx, capacity);

    dict->put(trx, type_key, type_val);
    dict->put(trx, source_key, source);
    dict->set_readonly_access_no_save();

    auto dict_obj = Object::make_dict(dict_offset);
    dict_obj.set_readonly_access();
    return dict_obj;
}

// Helper: add /proc key to a descriptor dict.
static void pipe_add_proc(Trix *trx, Object dict_obj, Object proc) {
    auto proc_key = trx->wellknown_name(WellKnownName::IKeyProc);
    auto dict = dict_obj.dict_value(trx);
    dict->put(trx, proc_key, proc);
}

// pipe-map: source proc -- pipeline
// throws: opstack-underflow, type-check
static void pipe_map_op(Trix *trx) {
    trx->verify_operands(VerifyProc | VerifyCurry | VerifyContinuation, VerifyAny);

    auto proc_obj = *trx->m_op_ptr--;
    auto source_obj = *trx->m_op_ptr;

    auto desc = pipe_make_descriptor(trx, "pipe-map", source_obj);
    pipe_add_proc(trx, desc, proc_obj);
    *trx->m_op_ptr = desc;
}

// pipe-filter: source pred -- pipeline
// throws: opstack-underflow, type-check
static void pipe_filter_op(Trix *trx) {
    trx->verify_operands(VerifyProc | VerifyCurry | VerifyContinuation, VerifyAny);

    auto proc_obj = *trx->m_op_ptr--;
    auto source_obj = *trx->m_op_ptr;

    auto desc = pipe_make_descriptor(trx, "pipe-filter", source_obj);
    pipe_add_proc(trx, desc, proc_obj);
    *trx->m_op_ptr = desc;
}

// pipe-flat-map: source proc -- pipeline
// throws: opstack-underflow, type-check
static void pipe_flat_map_op(Trix *trx) {
    trx->verify_operands(VerifyProc | VerifyCurry | VerifyContinuation, VerifyAny);

    auto proc_obj = *trx->m_op_ptr--;
    auto source_obj = *trx->m_op_ptr;

    auto desc = pipe_make_descriptor(trx, "pipe-flat-map", source_obj);
    pipe_add_proc(trx, desc, proc_obj);
    *trx->m_op_ptr = desc;
}

// pipe-batch: source n -- pipeline
// throws: opstack-underflow, range-check, type-check
static void pipe_batch_op(Trix *trx) {
    trx->require_op_count(2);

    auto n_ptr = trx->m_op_ptr;
    if (!n_ptr->is_integer()) {
        trx->error(Error::TypeCheck, "pipe-batch: expected integer batch size, got {}", Object::type_sv(n_ptr->type()));
    } else {
        auto n = n_ptr->integer_value();
        if (n <= 0) {
            trx->error(Error::RangeCheck, "pipe-batch: batch size must be positive, got {}", n);
        } else {
            --trx->m_op_ptr;

            auto source_obj = *trx->m_op_ptr;

            auto desc = pipe_make_descriptor(trx, "pipe-batch", source_obj);
            auto dict = desc.dict_value(trx);
            auto n_key = trx->wellknown_name(WellKnownName::IKeyN);
            dict->put(trx, n_key, Object::make_integer(n));

            *trx->m_op_ptr = desc;
        }
    }
}

// pipe-tap: source proc -- pipeline
// throws: opstack-underflow, type-check
static void pipe_tap_op(Trix *trx) {
    trx->verify_operands(VerifyProc | VerifyCurry | VerifyContinuation, VerifyAny);

    auto proc_obj = *trx->m_op_ptr--;
    auto source_obj = *trx->m_op_ptr;

    auto desc = pipe_make_descriptor(trx, "pipe-tap", source_obj);
    pipe_add_proc(trx, desc, proc_obj);
    *trx->m_op_ptr = desc;
}

// pipe-take: source n -- pipeline
// throws: opstack-underflow, range-check, type-check
static void pipe_take_op(Trix *trx) {
    trx->require_op_count(2);

    auto n_ptr = trx->m_op_ptr;
    if (!n_ptr->is_integer()) {
        trx->error(Error::TypeCheck, "pipe-take: expected integer count, got {}", Object::type_sv(n_ptr->type()));
    } else {
        auto n = n_ptr->integer_value();
        if (n <= 0) {
            trx->error(Error::RangeCheck, "pipe-take: count must be positive, got {}", n);
        } else {
            --trx->m_op_ptr;

            auto source_obj = *trx->m_op_ptr;

            auto desc = pipe_make_descriptor(trx, "pipe-take", source_obj);
            auto dict = desc.dict_value(trx);
            auto n_key = trx->wellknown_name(WellKnownName::IKeyN);
            dict->put(trx, n_key, Object::make_integer(n));

            *trx->m_op_ptr = desc;
        }
    }
}

// pipe-drop: source n -- pipeline
// throws: opstack-underflow, range-check, type-check
static void pipe_drop_op(Trix *trx) {
    trx->require_op_count(2);

    auto n_ptr = trx->m_op_ptr;
    if (!n_ptr->is_integer()) {
        trx->error(Error::TypeCheck, "pipe-drop: expected integer count, got {}", Object::type_sv(n_ptr->type()));
    } else {
        auto n = n_ptr->integer_value();
        if (n <= 0) {
            trx->error(Error::RangeCheck, "pipe-drop: count must be positive, got {}", n);
        } else {
            --trx->m_op_ptr;

            auto source_obj = *trx->m_op_ptr;

            auto desc = pipe_make_descriptor(trx, "pipe-drop", source_obj);
            auto dict = desc.dict_value(trx);
            auto n_key = trx->wellknown_name(WellKnownName::IKeyN);
            dict->put(trx, n_key, Object::make_integer(n));

            *trx->m_op_ptr = desc;
        }
    }
}

// pipe-scan: source init proc -- pipeline
// throws: opstack-underflow, type-check
static void pipe_scan_op(Trix *trx) {
    trx->verify_operands(VerifyProc | VerifyCurry | VerifyContinuation, VerifyAny, VerifyAny);

    auto proc_obj = *trx->m_op_ptr--;
    auto init_obj = *trx->m_op_ptr--;
    auto source_obj = *trx->m_op_ptr;

    auto desc = pipe_make_descriptor(trx, "pipe-scan", source_obj, 4);
    pipe_add_proc(trx, desc, proc_obj);
    auto dict = desc.dict_value(trx);
    auto init_key = trx->wellknown_name(WellKnownName::IKeyInit);
    dict->put(trx, init_key, init_obj);

    *trx->m_op_ptr = desc;
}

// pipe-distinct: source -- pipeline
// throws: opstack-underflow, type-check
static void pipe_distinct_op(Trix *trx) {
    trx->require_op_count(1);
    auto source_obj = *trx->m_op_ptr;
    auto desc = pipe_make_descriptor(trx, "pipe-distinct", source_obj);
    *trx->m_op_ptr = desc;
}

// pipe-window: source n -- pipeline
// throws: opstack-underflow, range-check, type-check
static void pipe_window_op(Trix *trx) {
    trx->require_op_count(2);

    auto n_ptr = trx->m_op_ptr;
    if (!n_ptr->is_integer()) {
        trx->error(Error::TypeCheck, "pipe-window: expected integer window size, got {}", Object::type_sv(n_ptr->type()));
    } else {
        auto n = n_ptr->integer_value();
        if (n <= 0) {
            trx->error(Error::RangeCheck, "pipe-window: window size must be positive, got {}", n);
        } else {
            --trx->m_op_ptr;

            auto source_obj = *trx->m_op_ptr;

            auto desc = pipe_make_descriptor(trx, "pipe-window", source_obj);
            auto dict = desc.dict_value(trx);
            auto n_key = trx->wellknown_name(WellKnownName::IKeyN);
            dict->put(trx, n_key, Object::make_integer(n));

            *trx->m_op_ptr = desc;
        }
    }
}

// pipe-merge: [sources] -- pipeline
// Interleave items from multiple sources via round-robin.
// throws: opstack-underflow, type-check, range-check
static void pipe_merge_op(Trix *trx) {
    trx->require_op_count(1);

    auto sources_obj = *trx->m_op_ptr;
    if (!sources_obj.is_array()) {
        trx->error(Error::TypeCheck, "pipe-merge: expected array of sources");
    } else {
        auto sources_len = sources_obj.arrays_length();
        if (sources_len < 2) {
            trx->error(Error::RangeCheck, "pipe-merge: need at least 2 sources, got {}", sources_len);
        } else if (sources_len > MaxPipeStages) {
            trx->error(Error::LimitCheck, "pipe-merge: too many sources ({}, max {})", sources_len, MaxPipeStages);
        } else {
            auto type_key = trx->wellknown_name(WellKnownName::IKeyType);
            auto sources_key = trx->wellknown_name(WellKnownName::IKeySources);
            auto type_val = Name::make(trx, "pipe-merge");

            auto [dict, dict_offset] = Dict::create_or_recycle(trx, 2);

            dict->put(trx, type_key, type_val);
            dict->put(trx, sources_key, sources_obj);
            dict->set_readonly_access_no_save();

            auto dict_obj = Object::make_dict(dict_offset);
            dict_obj.set_readonly_access();
            *trx->m_op_ptr = dict_obj;
        }
    }
}

// pipe-zip: [sources] -- pipeline
// Pairwise combination: one item from each source per cycle, emitted as array.
// Stops when any source is exhausted.
// throws: opstack-underflow, type-check, range-check
static void pipe_zip_op(Trix *trx) {
    trx->require_op_count(1);

    auto sources_obj = *trx->m_op_ptr;
    if (!sources_obj.is_array()) {
        trx->error(Error::TypeCheck, "pipe-zip: expected array of sources");
    } else {
        auto sources_len = sources_obj.arrays_length();
        if (sources_len < 2) {
            trx->error(Error::RangeCheck, "pipe-zip: need at least 2 sources, got {}", sources_len);
        } else if (sources_len > MaxPipeStages) {
            trx->error(Error::LimitCheck, "pipe-zip: too many sources ({}, max {})", sources_len, MaxPipeStages);
        } else {
            auto type_key = trx->wellknown_name(WellKnownName::IKeyType);
            auto sources_key = trx->wellknown_name(WellKnownName::IKeySources);
            auto type_val = Name::make(trx, "pipe-zip");

            auto [dict, dict_offset] = Dict::create_or_recycle(trx, 2);

            dict->put(trx, type_key, type_val);
            dict->put(trx, sources_key, sources_obj);
            dict->set_readonly_access_no_save();

            auto dict_obj = Object::make_dict(dict_offset);
            dict_obj.set_readonly_access();
            *trx->m_op_ptr = dict_obj;
        }
    }
}

// pipe-status: buffer -- dict
// Returns a read-only dict with pipe buffer introspection data:
//   /count, /capacity, /closed, /error, /has-producer, /has-consumer
// throws: opstack-underflow, type-check
static void pipe_status_op(Trix *trx) {
    trx->verify_operands(VerifyPipeBuffer);

    auto buf_offset = trx->m_op_ptr->pipe_buffer_offset();
    auto hdr = trx->offset_to_ptr<PipeBufferHeader>(buf_offset);

    // Reject a recycled (free-list-parked) buffer before allocating, matching
    // pipe-put/pipe-get/pipe-close -- otherwise a stale handle reports a dead
    // buffer's last-drained fields as if live.
    if (hdr->m_head == PipeRecycleSentinel) {
        trx->error(Error::InvalidAccess, "pipe-status: buffer has been recycled (both sides already done)");
    } else {
        auto [dict, dict_offset] = Dict::create_or_recycle(trx, 6);
        hdr = trx->offset_to_ptr<PipeBufferHeader>(buf_offset);  // re-fetch after alloc

        dict->put(trx, trx->wellknown_name(WellKnownName::IKeyCount), Object::make_integer(static_cast<integer_t>(hdr->m_count)));
        dict->put(trx,
                  trx->wellknown_name(WellKnownName::IKeyCapacity),
                  Object::make_integer(static_cast<integer_t>(hdr->m_capacity)));
        dict->put(trx, trx->wellknown_name(WellKnownName::IKeyClosed), Object::make_boolean(hdr->m_closed));
        if (hdr->m_error != Error::NoError) {
            dict->put(trx, trx->wellknown_name(WellKnownName::IKeyError), trx->error_name(hdr->m_error));
        } else {
            dict->put(trx, trx->wellknown_name(WellKnownName::IKeyError), Object::make_null());
        }
        dict->put(trx, trx->wellknown_name(WellKnownName::IKeyHasProducer), Object::make_boolean(hdr->m_producer != nulloffset));
        dict->put(trx, trx->wellknown_name(WellKnownName::IKeyHasConsumer), Object::make_boolean(hdr->m_consumer != nulloffset));

        dict->set_readonly_access_no_save();
        *trx->m_op_ptr = Object::make_dict(dict_offset);
    }
}

//===--- Pipeline Execution ---===//
//
// Terminal operators (pipe-run, pipe-collect, pipe-reduce) materialize a
// pipeline descriptor chain: walk the chain to find the root source, allocate
// bounded buffers between stages, launch a coroutine per stage, and run the
// sink inline in the caller's coroutine.
//
// Each stage coroutine runs a Trix procedure built from C++ that contains
// the stage loop logic using existing operators (pipe-get, pipe-put, exec,
// for-all, lazy-head, lazy-tail, etc.).  Error containment via @try-barrier;
// error propagation via @pipe-cleanup which error-closes downstream buffers.
//
// Batch stages use the @pipe-stage C++ control operator for state management.

//===--- Stage type encoding ---===//

static constexpr int32_t PipeStageMap = 0;
static constexpr int32_t PipeStageFilter = 1;
static constexpr int32_t PipeStageFlatMap = 2;
static constexpr int32_t PipeStageBatch = 3;
static constexpr int32_t PipeStageTap = 4;
static constexpr int32_t PipeStageTake = 5;
static constexpr int32_t PipeStageDrop = 6;
static constexpr int32_t PipeStageScan = 7;
static constexpr int32_t PipeStageDistinct = 8;
static constexpr int32_t PipeStageWindow = 9;
static constexpr int32_t PipeStageMerge = 10;
static constexpr int32_t PipeStageZip = 11;

//===--- @pipe-stage phases (batch, take, and drop) ---===//

static constexpr int32_t PipeBatchGet = 0;
static constexpr int32_t PipeBatchAfterGet = 1;
static constexpr int32_t PipeBatchAfterPut = 2;
static constexpr int32_t PipeBatchFlushPut = 3;
static constexpr int32_t PipeBatchClose = 4;

static constexpr int32_t PipeTakeGet = 10;
static constexpr int32_t PipeTakeAfterGet = 11;
static constexpr int32_t PipeTakeAfterPut = 12;

static constexpr int32_t PipeDropGet = 20;
static constexpr int32_t PipeDropAfterGet = 21;
static constexpr int32_t PipeDropAfterPut = 22;

static constexpr int32_t PipeDistinctGet = 30;
static constexpr int32_t PipeDistinctAfterGet = 31;
static constexpr int32_t PipeDistinctAfterPut = 32;

static constexpr int32_t PipeWindowGet = 40;
static constexpr int32_t PipeWindowAfterGet = 41;
static constexpr int32_t PipeWindowAfterPut = 42;

static constexpr int32_t PipeMergeScan = 50;
static constexpr int32_t PipeMergeAfterGet = 51;
static constexpr int32_t PipeMergeAfterPut = 52;

static constexpr int32_t PipeZipGet = 60;
static constexpr int32_t PipeZipAfterGet = 61;
static constexpr int32_t PipeZipAfterPut = 62;

//===--- Helper: build executable Trix procedure ---===//

// Allocate a VM array of Objects and wrap as an executable Array.
static Object pipe_build_proc(Trix *trx, const Object *items, length_t count) {
    auto storage = trx->vm_alloc_n_ptr<Object>(count);
    auto offset = trx->ptr_to_offset(storage);
    auto curr_save_level = trx->m_curr_save_level;
    for (length_t i = 0; i < count; ++i) {
        storage[i] = items[i];
        storage[i].set_save_level(curr_save_level);
    }
    auto result = Object::make_array(offset, count);
    result.set_executable();
    return result;
}

// Build { output-buf pipe-close exit } proc (end-of-stream handler).
static Object pipe_build_close_exit_proc(Trix *trx, Object output_buf) {
    Object items[3];
    items[0] = output_buf;
    items[0].set_literal();
    items[1] = Object::make_operator(SystemName::PipeClose);
    items[2] = Object::make_operator(SystemName::Exit);
    return pipe_build_proc(trx, items, 3);
}

// Build { input-buf pipe-get true-proc false-proc if-else } proc (loop body).
static Object pipe_build_get_loop_body(Trix *trx, Object input_buf, Object true_proc, Object false_proc) {
    Object items[5];
    items[0] = input_buf;
    items[0].set_literal();
    items[1] = Object::make_operator(SystemName::PipeGet);
    items[2] = true_proc;
    items[3] = false_proc;
    items[4] = Object::make_operator(SystemName::IfElse);
    return pipe_build_proc(trx, items, 5);
}

// Build { body-proc loop } proc (loop wrapper).
static Object pipe_build_loop_wrapper(Trix *trx, Object body_proc) {
    Object items[2];
    items[0] = body_proc;
    items[1] = Object::make_operator(SystemName::Loop);
    return pipe_build_proc(trx, items, 2);
}

//===--- Helper: build source feeder Trix proc (array) ---===//
//
// Trix equivalent:
//   { source { output-buf pipe-put } for-all output-buf pipe-close }

static Object pipe_build_array_source_proc(Trix *trx, Object source, Object output_buf) {
    // Inner proc: { output-buf pipe-put }
    Object inner_items[2];
    inner_items[0] = output_buf;
    inner_items[0].set_literal();
    inner_items[1] = Object::make_operator(SystemName::PipePut);
    auto inner = pipe_build_proc(trx, inner_items, 2);

    // Outer proc: { source inner for-all output-buf pipe-close }
    Object outer[5];
    outer[0] = source;
    outer[0].set_literal();
    outer[1] = inner;  // executable proc (for-all needs executable)
    outer[2] = Object::make_operator(SystemName::ForAll);
    outer[3] = output_buf;
    outer[3].set_literal();
    outer[4] = Object::make_operator(SystemName::PipeClose);
    return pipe_build_proc(trx, outer, 5);
}

//===--- Helper: build source feeder Trix proc (lazy-seq) ---===//
//
// Trix equivalent:
//   lazy-node
//   { dup null eq { pop output-buf pipe-close exit } if
//     dup lazy-head output-buf pipe-put lazy-tail } loop
//
// The lazy-seq node is placed on the operand stack before the loop starts.
// Returns a proc that expects the lazy-seq node to already be on the op stack.

static Object pipe_build_lazy_source_proc(Trix *trx, Object output_buf) {
    // Exit handler: { pop output-buf pipe-close exit }
    Object exit_items[4];
    exit_items[0] = Object::make_operator(SystemName::Pop);
    exit_items[1] = output_buf;
    exit_items[1].set_literal();
    exit_items[2] = Object::make_operator(SystemName::PipeClose);
    exit_items[3] = Object::make_operator(SystemName::Exit);
    auto exit_proc = pipe_build_proc(trx, exit_items, 4);

    // Loop body: { dup null eq exit-proc if dup lazy-head output-buf pipe-put lazy-tail }
    Object body[10];
    body[0] = Object::make_operator(SystemName::Dup);
    body[1] = Object::make_null();
    body[2] = Object::make_operator(SystemName::Eq);
    body[3] = exit_proc;  // executable proc (if needs executable)
    body[4] = Object::make_operator(SystemName::If);
    body[5] = Object::make_operator(SystemName::Dup);
    body[6] = Object::make_operator(SystemName::LazyHead);
    body[7] = output_buf;
    body[7].set_literal();
    body[8] = Object::make_operator(SystemName::PipePut);
    body[9] = Object::make_operator(SystemName::LazyTail);
    auto body_proc = pipe_build_proc(trx, body, 10);

    // Outer: { body-proc loop }
    Object outer[2];
    outer[0] = body_proc;  // executable proc (loop needs executable)
    outer[1] = Object::make_operator(SystemName::Loop);
    return pipe_build_proc(trx, outer, 2);
}

//===--- Helper: build mid-stage Trix proc (map) ---===//
//
// Trix equivalent:
//   { { input-buf pipe-get
//       { user-proc exec output-buf pipe-put }
//       { output-buf pipe-close exit }
//       if-else } loop }

static Object pipe_build_map_proc(Trix *trx, Object input_buf, Object output_buf, Object user_proc) {
    // True case: { user-proc exec output-buf pipe-put }
    Object true_items[4];
    true_items[0] = user_proc;
    true_items[1] = Object::make_operator(SystemName::Exec);
    true_items[2] = output_buf;
    true_items[2].set_literal();
    true_items[3] = Object::make_operator(SystemName::PipePut);
    auto true_proc = pipe_build_proc(trx, true_items, 4);

    auto false_proc = pipe_build_close_exit_proc(trx, output_buf);
    auto body_proc = pipe_build_get_loop_body(trx, input_buf, true_proc, false_proc);
    return pipe_build_loop_wrapper(trx, body_proc);
}

//===--- Helper: build mid-stage Trix proc (filter) ---===//
//
// Trix equivalent:
//   { { input-buf pipe-get
//       { dup user-pred exec { output-buf pipe-put } { pop } if-else }
//       { output-buf pipe-close exit }
//       if-else } loop }

static Object pipe_build_filter_proc(Trix *trx, Object input_buf, Object output_buf, Object user_pred) {
    // Put sub-proc: { output-buf pipe-put }
    Object put_items[2];
    put_items[0] = output_buf;
    put_items[0].set_literal();
    put_items[1] = Object::make_operator(SystemName::PipePut);
    auto put_proc = pipe_build_proc(trx, put_items, 2);

    // Drop sub-proc: { pop }
    Object drop_items[1];
    drop_items[0] = Object::make_operator(SystemName::Pop);
    auto drop_proc = pipe_build_proc(trx, drop_items, 1);

    // True case: { dup user-pred exec put-proc drop-proc if-else }
    Object true_items[6];
    true_items[0] = Object::make_operator(SystemName::Dup);
    true_items[1] = user_pred;
    true_items[2] = Object::make_operator(SystemName::Exec);
    true_items[3] = put_proc;
    true_items[4] = drop_proc;
    true_items[5] = Object::make_operator(SystemName::IfElse);
    auto true_proc = pipe_build_proc(trx, true_items, 6);

    auto false_proc = pipe_build_close_exit_proc(trx, output_buf);
    auto body_proc = pipe_build_get_loop_body(trx, input_buf, true_proc, false_proc);
    return pipe_build_loop_wrapper(trx, body_proc);
}

//===--- Helper: build mid-stage Trix proc (flat-map) ---===//
//
// Trix equivalent:
//   { { input-buf pipe-get
//       { user-proc exec { output-buf pipe-put } for-all }
//       { output-buf pipe-close exit }
//       if-else } loop }

static Object pipe_build_flat_map_proc(Trix *trx, Object input_buf, Object output_buf, Object user_proc) {
    // Put sub-proc for for-all: { output-buf pipe-put }
    Object put_items[2];
    put_items[0] = output_buf;
    put_items[0].set_literal();
    put_items[1] = Object::make_operator(SystemName::PipePut);
    auto put_proc = pipe_build_proc(trx, put_items, 2);

    // True case: { user-proc exec put-proc for-all }
    Object true_items[4];
    true_items[0] = user_proc;
    true_items[1] = Object::make_operator(SystemName::Exec);
    true_items[2] = put_proc;
    true_items[3] = Object::make_operator(SystemName::ForAll);
    auto true_proc = pipe_build_proc(trx, true_items, 4);

    auto false_proc = pipe_build_close_exit_proc(trx, output_buf);
    auto body_proc = pipe_build_get_loop_body(trx, input_buf, true_proc, false_proc);
    return pipe_build_loop_wrapper(trx, body_proc);
}

//===--- Helper: build mid-stage Trix proc (tap) ---===//
//
// Trix equivalent:
//   { { input-buf pipe-get
//       { dup user-proc exec pop output-buf pipe-put }
//       { output-buf pipe-close exit }
//       if-else } loop }

static Object pipe_build_tap_proc(Trix *trx, Object input_buf, Object output_buf, Object user_proc) {
    // True case: { dup user-proc exec pop output-buf pipe-put }
    Object true_items[6];
    true_items[0] = Object::make_operator(SystemName::Dup);
    true_items[1] = user_proc;
    true_items[2] = Object::make_operator(SystemName::Exec);
    true_items[3] = Object::make_operator(SystemName::Pop);
    true_items[4] = output_buf;
    true_items[4].set_literal();
    true_items[5] = Object::make_operator(SystemName::PipePut);
    auto true_proc = pipe_build_proc(trx, true_items, 6);

    auto false_proc = pipe_build_close_exit_proc(trx, output_buf);
    auto body_proc = pipe_build_get_loop_body(trx, input_buf, true_proc, false_proc);
    return pipe_build_loop_wrapper(trx, body_proc);
}

//===--- Helper: build mid-stage Trix proc (scan) ---===//
//
// Trix equivalent:
//   { init
//     { input-buf pipe-get
//       { user-proc exec dup output-buf pipe-put }
//       { pop output-buf pipe-close exit }
//       if-else } loop }
//
// The accumulator sits on the operand stack.  init is pushed first, then the
// loop repeatedly: pipe-get pushes item above acc, proc folds (acc item -- new-acc),
// dup emits a copy to output, and the original stays as acc for the next iteration.
// On EOS, the residual acc is popped and the output buffer is closed.

static Object pipe_build_scan_proc(Trix *trx, Object input_buf, Object output_buf, Object user_proc, Object init) {
    // True case: { user-proc exec dup output-buf pipe-put }
    Object true_items[5];
    true_items[0] = user_proc;
    true_items[1] = Object::make_operator(SystemName::Exec);
    true_items[2] = Object::make_operator(SystemName::Dup);
    true_items[3] = output_buf;
    true_items[3].set_literal();
    true_items[4] = Object::make_operator(SystemName::PipePut);
    auto true_proc = pipe_build_proc(trx, true_items, 5);

    // False case: { pop output-buf pipe-close exit }
    Object false_items[4];
    false_items[0] = Object::make_operator(SystemName::Pop);
    false_items[1] = output_buf;
    false_items[1].set_literal();
    false_items[2] = Object::make_operator(SystemName::PipeClose);
    false_items[3] = Object::make_operator(SystemName::Exit);
    auto false_proc = pipe_build_proc(trx, false_items, 4);

    auto body_proc = pipe_build_get_loop_body(trx, input_buf, true_proc, false_proc);

    // Outer: { init body-proc loop }
    Object outer_items[3];
    outer_items[0] = init;
    outer_items[0].set_literal();
    outer_items[1] = body_proc;
    outer_items[2] = Object::make_operator(SystemName::Loop);
    return pipe_build_proc(trx, outer_items, 3);
}

//===--- @pipe-cleanup: error propagation after @try-barrier ---===//
//
// Exec stack: [output-buf] [@pipe-cleanup]
// Op stack: /error-name (from @try-barrier)
//
// If error: error-closes output buffer using trx->m_last_error.
// If no error: buffer already closed by the stage proc.

static void at_pipe_cleanup_op(Trix *trx) {
    auto output_buf_obj = *trx->m_exec_ptr;
    --trx->m_exec_ptr;  // pop companion

    // Pop error name from op stack
    auto error_name_obj = *trx->m_op_ptr;
    --trx->m_op_ptr;

    // Check if error occurred (compare name offsets for identity)
    auto no_error = trx->error_name(Error::NoError);
    if (error_name_obj.name_offset() != no_error.name_offset()) {
        // Error: error-close the output buffer
        auto hdr = output_buf_obj.pipe_buffer_header(trx);
        if (!hdr->m_closed) {
            hdr->m_closed = true;
            hdr->m_error = trx->m_last_error;
            auto reader = hdr->m_blocked_reader;
            hdr->m_blocked_reader = nulloffset;
            trx->coroutine_wake(reader);
            auto writer = hdr->m_blocked_writer;
            hdr->m_blocked_writer = nulloffset;
            trx->coroutine_wake(writer);
        }
    }
}

//===--- @pipe-stage: state machine for batch and take stages ---===//
//
// Batch exec stack: [input-buf] [output-buf] [batch-arr-offset] [batch-idx]
//                   [batch-size] [phase] [@pipe-stage]
//
// Take exec stack:  [input-buf] [output-buf] [remaining] [phase] [@pipe-stage]
//
// Dispatches on phase value: batch phases 0-4, take phases 10-12.

static void at_pipe_stage_take(Trix *trx, int32_t phase) {
    //--- Take phases (exec companions: input[-3] output[-2] remaining[-1] phase[0]) ---

    if (phase == PipeTakeGet) {
        auto input_buf = trx->m_exec_ptr[-3];
        trx->require_op_capacity(1);
        *++trx->m_op_ptr = input_buf;
        trx->m_exec_ptr[0] = Object::make_integer(PipeTakeAfterGet);
        trx->require_exec_capacity(2);
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atPipeStage);
        *++trx->m_exec_ptr = Object::make_operator(SystemName::PipeGet);
    } else if (phase == PipeTakeAfterGet) {
        auto output_buf = trx->m_exec_ptr[-2];

        auto got_data_obj = *trx->m_op_ptr;
        --trx->m_op_ptr;

        if (got_data_obj.is_boolean() && !got_data_obj.boolean_value()) {
            // End of stream before N items: close output, done
            auto out_hdr = output_buf.pipe_buffer_header(trx);
            pipe_close_and_wake_reader(trx, out_hdr);
            trx->m_exec_ptr -= 4;
        } else {
            // Got data: value is on op stack, pipe-put it to output
            trx->require_op_capacity(1);
            *++trx->m_op_ptr = output_buf;
            trx->m_exec_ptr[0] = Object::make_integer(PipeTakeAfterPut);
            trx->require_exec_capacity(2);
            *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atPipeStage);
            *++trx->m_exec_ptr = Object::make_operator(SystemName::PipePut);
        }
    } else if (phase == PipeTakeAfterPut) {
        auto remaining = trx->m_exec_ptr[-1].integer_value();
        auto output_buf = trx->m_exec_ptr[-2];

        --remaining;
        if (remaining > 0) {
            trx->m_exec_ptr[-1] = Object::make_integer(remaining);
            trx->m_exec_ptr[0] = Object::make_integer(PipeTakeGet);
            trx->require_exec_capacity(1);
            *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atPipeStage);
        } else {
            // Limit reached: close output, done
            auto out_hdr = output_buf.pipe_buffer_header(trx);
            pipe_close_and_wake_reader(trx, out_hdr);
            trx->m_exec_ptr -= 4;
        }
    }
}

static void at_pipe_stage_drop(Trix *trx, int32_t phase) {
    //--- Drop phases (exec companions: input[-3] output[-2] remaining[-1] phase[0]) ---

    if (phase == PipeDropGet) {
        auto input_buf = trx->m_exec_ptr[-3];
        trx->require_op_capacity(1);
        *++trx->m_op_ptr = input_buf;
        trx->m_exec_ptr[0] = Object::make_integer(PipeDropAfterGet);
        trx->require_exec_capacity(2);
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atPipeStage);
        *++trx->m_exec_ptr = Object::make_operator(SystemName::PipeGet);
    } else if (phase == PipeDropAfterGet) {
        auto output_buf = trx->m_exec_ptr[-2];
        auto remaining = trx->m_exec_ptr[-1].integer_value();

        auto got_data_obj = *trx->m_op_ptr;
        --trx->m_op_ptr;

        if (got_data_obj.is_boolean() && !got_data_obj.boolean_value()) {
            // End of stream: close output, done
            auto out_hdr = output_buf.pipe_buffer_header(trx);
            pipe_close_and_wake_reader(trx, out_hdr);
            trx->m_exec_ptr -= 4;
        } else if (remaining > 0) {
            // Still skipping: discard item, decrement remaining
            auto item_obj = *trx->m_op_ptr;
            item_obj.maybe_free_extvalue(trx);
            --trx->m_op_ptr;
            trx->m_exec_ptr[-1] = Object::make_integer(remaining - 1);
            trx->m_exec_ptr[0] = Object::make_integer(PipeDropGet);
            trx->require_exec_capacity(1);
            *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atPipeStage);
        } else {
            // Passthrough: pipe-put to output
            trx->require_op_capacity(1);
            *++trx->m_op_ptr = output_buf;
            trx->m_exec_ptr[0] = Object::make_integer(PipeDropAfterPut);
            trx->require_exec_capacity(2);
            *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atPipeStage);
            *++trx->m_exec_ptr = Object::make_operator(SystemName::PipePut);
        }
    } else if (phase == PipeDropAfterPut) {
        trx->m_exec_ptr[0] = Object::make_integer(PipeDropGet);
        trx->require_exec_capacity(1);
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atPipeStage);
    }
}

static void at_pipe_stage_distinct(Trix *trx, int32_t phase) {
    //--- Distinct phases (exec companions: input[-3] output[-2] set[-1] phase[0]) ---

    if (phase == PipeDistinctGet) {
        auto input_buf = trx->m_exec_ptr[-3];
        trx->require_op_capacity(1);
        *++trx->m_op_ptr = input_buf;
        trx->m_exec_ptr[0] = Object::make_integer(PipeDistinctAfterGet);
        trx->require_exec_capacity(2);
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atPipeStage);
        *++trx->m_exec_ptr = Object::make_operator(SystemName::PipeGet);
    } else if (phase == PipeDistinctAfterGet) {
        auto output_buf = trx->m_exec_ptr[-2];
        auto set_obj = trx->m_exec_ptr[-1];

        auto got_data_obj = *trx->m_op_ptr;
        --trx->m_op_ptr;

        if (got_data_obj.is_boolean() && !got_data_obj.boolean_value()) {
            // End of stream: close output, done
            auto out_hdr = output_buf.pipe_buffer_header(trx);
            pipe_close_and_wake_reader(trx, out_hdr);
            trx->m_exec_ptr -= 4;
        } else {
            // Got data: check membership in seen set
            auto item_obj = *trx->m_op_ptr;
            auto set = set_obj.set_value(trx);
            if (set->set_member(trx, item_obj)) {
                // Already seen: discard item, loop back
                item_obj.maybe_free_extvalue(trx);
                --trx->m_op_ptr;
                trx->m_exec_ptr[0] = Object::make_integer(PipeDistinctGet);
                trx->require_exec_capacity(1);
                *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atPipeStage);
            } else {
                // New: add clone to set (set takes ownership), pipe-put original to output
                auto clone = item_obj.make_clone(trx);
                set = set_obj.set_value(trx);
                set->set_put(trx, clone);
                trx->require_op_capacity(1);
                *++trx->m_op_ptr = output_buf;
                trx->m_exec_ptr[0] = Object::make_integer(PipeDistinctAfterPut);
                trx->require_exec_capacity(2);
                *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atPipeStage);
                *++trx->m_exec_ptr = Object::make_operator(SystemName::PipePut);
            }
        }
    } else if (phase == PipeDistinctAfterPut) {
        trx->m_exec_ptr[0] = Object::make_integer(PipeDistinctGet);
        trx->require_exec_capacity(1);
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atPipeStage);
    }
}

static void at_pipe_stage_window(Trix *trx, int32_t phase) {
    //--- Window phases (exec companions below phase[0]) ---
    // A 6th companion at [-6] holds an explicit filled count; without it, filled
    // would have to be inferred as write-idx until the first wrap, then full.
    // Layout: input[-6] output[-5] arr[-4] write-idx[-3] filled[-2] size[-1] phase[0]

    if (phase == PipeWindowGet) {
        auto input_buf = trx->m_exec_ptr[-6];
        trx->require_op_capacity(1);
        *++trx->m_op_ptr = input_buf;
        trx->m_exec_ptr[0] = Object::make_integer(PipeWindowAfterGet);
        trx->require_exec_capacity(2);
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atPipeStage);
        *++trx->m_exec_ptr = Object::make_operator(SystemName::PipeGet);
    } else if (phase == PipeWindowAfterGet) {
        auto output_buf = trx->m_exec_ptr[-5];
        auto arr_offset = trx->m_exec_ptr[-4].arrays_offset();
        auto write_idx = trx->m_exec_ptr[-3].integer_value();
        auto filled = trx->m_exec_ptr[-2].integer_value();
        auto window_size = trx->m_exec_ptr[-1].integer_value();

        auto got_data_obj = *trx->m_op_ptr;
        --trx->m_op_ptr;

        if (got_data_obj.is_boolean() && !got_data_obj.boolean_value()) {
            // End of stream: free window buffer ExtValues, close output, done
            {
                auto arr = trx->offset_to_ptr<Object>(arr_offset);
                for (int32_t i = 0; i < filled; ++i) {
                    arr[i].maybe_free_extvalue(trx);
                }
            }
            auto out_hdr = output_buf.pipe_buffer_header(trx);
            pipe_close_and_wake_reader(trx, out_hdr);
            trx->m_exec_ptr -= 7;
        } else {
            // Got data: store in window buffer
            auto item_obj = *trx->m_op_ptr;
            --trx->m_op_ptr;
            auto arr = trx->offset_to_ptr<Object>(arr_offset);
            if (filled >= window_size) {
                arr[write_idx].maybe_free_extvalue(trx);
            }
            arr[write_idx] = item_obj;
            arr[write_idx].set_save_level(trx->m_curr_save_level);

            auto new_write_idx = (write_idx + 1) % window_size;
            auto new_filled = (filled < window_size) ? filled + 1 : filled;

            trx->m_exec_ptr[-3] = Object::make_integer(new_write_idx);
            trx->m_exec_ptr[-2] = Object::make_integer(new_filled);

            if (new_filled >= window_size) {
                // Window full: build output array and pipe-put
                auto output = trx->vm_alloc_n_ptr<Object>(static_cast<vm_size_t>(window_size));
                auto output_offset = trx->ptr_to_offset(output);
                auto curr_save_level = trx->m_curr_save_level;
                arr = trx->offset_to_ptr<Object>(arr_offset);  // re-fetch after alloc
                for (int32_t i = 0; i < window_size; ++i) {
                    auto src_idx = (new_write_idx + i) % window_size;
                    output[i] = arr[src_idx].make_clone(trx);
                    output[i].set_save_level(curr_save_level);
                    arr = trx->offset_to_ptr<Object>(arr_offset);  // re-fetch after clone
                    output = trx->offset_to_ptr<Object>(output_offset);
                }
                auto window_arr = Object::make_array(output_offset, static_cast<length_t>(window_size));
                window_arr.set_literal();

                trx->require_op_capacity(2);
                *++trx->m_op_ptr = window_arr;
                *++trx->m_op_ptr = output_buf;
                trx->m_exec_ptr[0] = Object::make_integer(PipeWindowAfterPut);
                trx->require_exec_capacity(2);
                *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atPipeStage);
                *++trx->m_exec_ptr = Object::make_operator(SystemName::PipePut);
            } else {
                // Window not full yet: loop back to get more
                trx->m_exec_ptr[0] = Object::make_integer(PipeWindowGet);
                trx->require_exec_capacity(1);
                *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atPipeStage);
            }
        }
    } else if (phase == PipeWindowAfterPut) {
        trx->m_exec_ptr[0] = Object::make_integer(PipeWindowGet);
        trx->require_exec_capacity(1);
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atPipeStage);
    }
}

static void at_pipe_stage_merge(Trix *trx, int32_t phase) {
    //--- Merge phases (exec companions: output[-4] inputs-offset[-3] count[-2] idx[-1] phase[0]) ---

    if (phase == PipeMergeScan) {
        auto merge_output = trx->m_exec_ptr[-4];
        auto merge_inputs_offset = trx->m_exec_ptr[-3].arrays_offset();
        auto merge_count = int32_t{trx->m_exec_ptr[-2].integer_value()};
        auto merge_idx = int32_t{trx->m_exec_ptr[-1].integer_value()};

        auto merge_inputs = trx->offset_to_ptr<Object>(merge_inputs_offset);

        // Scan sources round-robin, looking for data or detecting all-closed
        int32_t checked = 0;
        while (checked < merge_count) {
            auto idx = merge_idx % merge_count;
            auto hdr = merge_inputs[idx].pipe_buffer_header(trx);

            if (hdr->m_closed && (hdr->m_count == 0)) {
                // Fully drained, skip
                ++merge_idx;
                ++checked;
            } else {
                // Commit to the first not-fully-drained source: pipe-get it.
                // If it has data (or an error) the get completes immediately;
                // if it is empty-but-open the get blocks until data arrives.
                trx->require_op_capacity(1);
                *++trx->m_op_ptr = merge_inputs[idx];
                trx->m_exec_ptr[-1] = Object::make_integer(idx);
                trx->m_exec_ptr[0] = Object::make_integer(PipeMergeAfterGet);
                trx->require_exec_capacity(2);
                *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atPipeStage);
                *++trx->m_exec_ptr = Object::make_operator(SystemName::PipeGet);
                return;
            }
        }

        // All sources fully drained: close output
        auto out_hdr = merge_output.pipe_buffer_header(trx);
        pipe_close_and_wake_reader(trx, out_hdr);
        trx->m_exec_ptr -= 5;
    } else if (phase == PipeMergeAfterGet) {
        auto merge_output = trx->m_exec_ptr[-4];
        auto merge_count = int32_t{trx->m_exec_ptr[-2].integer_value()};
        auto merge_idx = int32_t{trx->m_exec_ptr[-1].integer_value()};

        auto got_data_obj = *trx->m_op_ptr;
        --trx->m_op_ptr;

        if (got_data_obj.is_boolean() && !got_data_obj.boolean_value()) {
            // EOS from this source: advance to next, rescan
            trx->m_exec_ptr[-1] = Object::make_integer((merge_idx + 1) % merge_count);
            trx->m_exec_ptr[0] = Object::make_integer(PipeMergeScan);
            trx->require_exec_capacity(1);
            *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atPipeStage);
        } else {
            // Got data: pipe-put to output
            trx->require_op_capacity(1);
            *++trx->m_op_ptr = merge_output;
            trx->m_exec_ptr[0] = Object::make_integer(PipeMergeAfterPut);
            trx->require_exec_capacity(2);
            *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atPipeStage);
            *++trx->m_exec_ptr = Object::make_operator(SystemName::PipePut);
        }
    } else if (phase == PipeMergeAfterPut) {
        auto merge_count = int32_t{trx->m_exec_ptr[-2].integer_value()};
        auto merge_idx = int32_t{trx->m_exec_ptr[-1].integer_value()};

        // Advance round-robin to next source
        trx->m_exec_ptr[-1] = Object::make_integer((merge_idx + 1) % merge_count);
        trx->m_exec_ptr[0] = Object::make_integer(PipeMergeScan);
        trx->require_exec_capacity(1);
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atPipeStage);
    }
}

static void at_pipe_stage_zip(Trix *trx, int32_t phase) {
    //--- Zip phases (exec companions: output[-5] inputs-offset[-4] count[-3] items-offset[-2] read-idx[-1] phase[0]) ---

    if (phase == PipeZipGet) {
        auto zip_inputs_offset = trx->m_exec_ptr[-4].arrays_offset();
        auto zip_inputs = trx->offset_to_ptr<Object>(zip_inputs_offset);

        // Start reading from source 0
        trx->m_exec_ptr[-1] = Object::make_integer(0);
        trx->require_op_capacity(1);
        *++trx->m_op_ptr = zip_inputs[0];
        trx->m_exec_ptr[0] = Object::make_integer(PipeZipAfterGet);
        trx->require_exec_capacity(2);
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atPipeStage);
        *++trx->m_exec_ptr = Object::make_operator(SystemName::PipeGet);
    } else if (phase == PipeZipAfterGet) {
        auto zip_output = trx->m_exec_ptr[-5];
        auto zip_inputs_offset = trx->m_exec_ptr[-4].arrays_offset();
        auto zip_count = int32_t{trx->m_exec_ptr[-3].integer_value()};
        auto zip_items_offset = trx->m_exec_ptr[-2].arrays_offset();
        auto zip_read_idx = int32_t{trx->m_exec_ptr[-1].integer_value()};

        auto got_data_obj = *trx->m_op_ptr;
        --trx->m_op_ptr;

        if (got_data_obj.is_boolean() && !got_data_obj.boolean_value()) {
            // EOS from any source: free collected items, close output, done
            if (zip_read_idx > 0) {
                auto zip_items = trx->offset_to_ptr<Object>(zip_items_offset);
                for (int32_t i = 0; i < zip_read_idx; ++i) {
                    zip_items[i].maybe_free_extvalue(trx);
                }
            }
            auto out_hdr = zip_output.pipe_buffer_header(trx);
            pipe_close_and_wake_reader(trx, out_hdr);
            trx->m_exec_ptr -= 6;
        } else {
            // Store value in items array
            auto zip_items = trx->offset_to_ptr<Object>(zip_items_offset);
            auto val_obj = *trx->m_op_ptr;
            --trx->m_op_ptr;
            zip_items[zip_read_idx] = val_obj;
            ++zip_read_idx;

            if (zip_read_idx < zip_count) {
                // More sources to read: pipe-get from next
                auto zip_inputs = trx->offset_to_ptr<Object>(zip_inputs_offset);
                trx->m_exec_ptr[-1] = Object::make_integer(zip_read_idx);
                trx->require_op_capacity(1);
                *++trx->m_op_ptr = zip_inputs[zip_read_idx];
                trx->m_exec_ptr[0] = Object::make_integer(PipeZipAfterGet);
                trx->require_exec_capacity(2);
                *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atPipeStage);
                *++trx->m_exec_ptr = Object::make_operator(SystemName::PipeGet);
            } else {
                // All sources read: build tuple array and pipe-put
                // Re-fetch items pointer (may have moved during pipe-get)
                auto items = trx->offset_to_ptr<Object>(zip_items_offset);
                auto curr_save_level = trx->m_curr_save_level;
                auto [arr, arr_offset] = trx->vm_alloc_n<Object>(static_cast<length_t>(zip_count));
                for (int32_t i = 0; i < zip_count; ++i) {
                    arr[i] = items[i].make_clone(trx);
                    // make_clone stamps Save::BASE; match batch/window by restoring
                    // the current save level so the tuple's elements journal alike.
                    arr[i].set_save_level(curr_save_level);
                }
                // Free originals in items scratch (clones are in the tuple now)
                items = trx->offset_to_ptr<Object>(zip_items_offset);
                for (int32_t i = 0; i < zip_count; ++i) {
                    items[i].maybe_free_extvalue(trx);
                }
                auto tuple = Object::make_array(arr_offset, static_cast<length_t>(zip_count));

                trx->require_op_capacity(2);
                *++trx->m_op_ptr = tuple;
                *++trx->m_op_ptr = zip_output;
                trx->m_exec_ptr[0] = Object::make_integer(PipeZipAfterPut);
                trx->require_exec_capacity(2);
                *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atPipeStage);
                *++trx->m_exec_ptr = Object::make_operator(SystemName::PipePut);
            }
        }
    } else if (phase == PipeZipAfterPut) {
        // Go back to read next cycle
        trx->m_exec_ptr[0] = Object::make_integer(PipeZipGet);
        trx->require_exec_capacity(1);
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atPipeStage);
    }
}

static void at_pipe_stage_batch(Trix *trx, int32_t phase) {
    //--- Batch phases (exec companions: input[-5] output[-4] arr[-3] idx[-2] size[-1] phase[0]) ---

    auto batch_size = trx->m_exec_ptr[-1].integer_value();
    auto batch_idx = trx->m_exec_ptr[-2].integer_value();
    auto batch_arr_offset = trx->m_exec_ptr[-3].arrays_offset();  // companion is a literal Array
    auto output_buf = trx->m_exec_ptr[-4];
    auto input_buf = trx->m_exec_ptr[-5];

    if (phase == PipeBatchGet) {
        // Push input-buf on op stack, push pipe-get + @pipe-stage continuation
        trx->require_op_capacity(1);
        *++trx->m_op_ptr = input_buf;
        trx->m_exec_ptr[0] = Object::make_integer(PipeBatchAfterGet);
        trx->require_exec_capacity(2);
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atPipeStage);
        *++trx->m_exec_ptr = Object::make_operator(SystemName::PipeGet);

    } else if (phase == PipeBatchAfterGet) {
        // Check pipe-get result: false = end-of-stream, true = data
        auto result_obj = *trx->m_op_ptr;
        --trx->m_op_ptr;

        if (result_obj.is_boolean() && !result_obj.boolean_value()) {
            // End of stream: flush partial batch if any
            if (batch_idx > 0) {
                // Build sub-array [0..batch_idx) and pipe-put
                auto arr = trx->offset_to_ptr<Object>(batch_arr_offset);
                auto sub = trx->vm_alloc_n_ptr<Object>(static_cast<vm_size_t>(batch_idx));
                auto sub_offset = trx->ptr_to_offset(sub);
                auto curr_save_level = trx->m_curr_save_level;
                for (int32_t i = 0; i < batch_idx; ++i) {
                    sub[i] = arr[i].make_clone(trx);
                    sub[i].set_save_level(curr_save_level);
                }
                // Free originals in batch array (clones are in sub-array now)
                arr = trx->offset_to_ptr<Object>(batch_arr_offset);
                for (int32_t i = 0; i < batch_idx; ++i) {
                    arr[i].maybe_free_extvalue(trx);
                }
                auto sub_arr = Object::make_array(sub_offset, static_cast<length_t>(batch_idx));
                sub_arr.set_literal();

                trx->require_op_capacity(2);
                *++trx->m_op_ptr = sub_arr;
                *++trx->m_op_ptr = output_buf;
                trx->m_exec_ptr[0] = Object::make_integer(PipeBatchClose);
                trx->require_exec_capacity(2);
                *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atPipeStage);
                *++trx->m_exec_ptr = Object::make_operator(SystemName::PipePut);
            } else {
                // Nothing to flush: close directly
                auto out_hdr = output_buf.pipe_buffer_header(trx);
                pipe_close_and_wake_reader(trx, out_hdr);
                trx->m_exec_ptr -= 6;  // pop all companions
            }
        } else {
            // Data: pop true, accumulate value
            auto val_obj = *trx->m_op_ptr;
            --trx->m_op_ptr;

            auto arr = trx->offset_to_ptr<Object>(batch_arr_offset);
            arr[batch_idx] = val_obj;
            arr[batch_idx].set_save_level(trx->m_curr_save_level);
            ++batch_idx;
            trx->m_exec_ptr[-2] = Object::make_integer(batch_idx);

            if (batch_idx >= batch_size) {
                // Batch full: create array Object and pipe-put
                auto batch_arr = Object::make_array(batch_arr_offset, static_cast<length_t>(batch_size));
                batch_arr.set_literal();

                trx->require_op_capacity(2);
                *++trx->m_op_ptr = batch_arr;
                *++trx->m_op_ptr = output_buf;

                // Allocate the new (global, GC-walkable) batch array.  The full
                // batch_arr + output_buf are rooted on the op stack above, so the
                // gvm_alloc here cannot sweep them; the running stage's companion
                // slot roots the new array as soon as it is stored.
                auto [new_arr_obj, _] = pipe_alloc_stage_aux(trx, batch_size);
                trx->m_exec_ptr[-3] = new_arr_obj;              // companion (literal Array)
                trx->m_exec_ptr[-2] = Object::make_integer(0);  // reset idx

                trx->m_exec_ptr[0] = Object::make_integer(PipeBatchAfterPut);
                trx->require_exec_capacity(2);
                *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atPipeStage);
                *++trx->m_exec_ptr = Object::make_operator(SystemName::PipePut);
            } else {
                // More room: loop back to GET
                trx->m_exec_ptr[0] = Object::make_integer(PipeBatchGet);
                trx->require_exec_capacity(1);
                *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atPipeStage);
            }
        }
    } else if (phase == PipeBatchAfterPut) {
        // After full-batch pipe-put: loop back to GET
        trx->m_exec_ptr[0] = Object::make_integer(PipeBatchGet);
        trx->require_exec_capacity(1);
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atPipeStage);

    } else if (phase == PipeBatchFlushPut) {
        // After partial-batch pipe-put: close
        auto out_hdr = output_buf.pipe_buffer_header(trx);
        pipe_close_and_wake_reader(trx, out_hdr);
        trx->m_exec_ptr -= 6;

    } else if (phase == PipeBatchClose) {
        // Close after flush
        auto out_hdr = output_buf.pipe_buffer_header(trx);
        pipe_close_and_wake_reader(trx, out_hdr);
        trx->m_exec_ptr -= 6;
    }
}

static void at_pipe_stage_op(Trix *trx) {
    auto phase = trx->m_exec_ptr[0].integer_value();
    if (phase >= PipeZipGet) {
        at_pipe_stage_zip(trx, phase);
    } else if (phase >= PipeMergeScan) {
        at_pipe_stage_merge(trx, phase);
    } else if (phase >= PipeWindowGet) {
        at_pipe_stage_window(trx, phase);
    } else if (phase >= PipeDistinctGet) {
        at_pipe_stage_distinct(trx, phase);
    } else if (phase >= PipeDropGet) {
        at_pipe_stage_drop(trx, phase);
    } else if (phase >= PipeTakeGet) {
        at_pipe_stage_take(trx, phase);
    } else {
        at_pipe_stage_batch(trx, phase);
    }
}

//===--- Helper: create a stage coroutine with custom exec stack ---===//
//
// Common setup shared by all pipeline stage coroutines:
// allocates context + stacks, initializes all fields, sets up dict stack.
// Caller fills exec_base[0..N) with stage-specific items, then calls
// pipe_finalize_stage_context to set exec pointer and insert into scheduler.

struct PipeCoroutineSetup {
    vm_offset_t ctx_offset;
    Object *exec_base;
};

// Allocate a stage's accumulation/companion array in GLOBAL VM as a GC-walkable
// literal Array: null-init'd so the walker can scan the whole block, obj_count
// stamped, tagged Array so gc_mark_object descends into it (marking any global
// payloads the stage buffers) and so it survives a restore.  Returns the Array
// Object companion + its storage offset.  Caller must keep the stage context (and
// any earlier aux array) rooted on the op stack across this call -- it can fire
// vm_global_gc, and a not-yet-finalized stage and its siblings are not GC roots.
static std::pair<Object, vm_offset_t> pipe_alloc_stage_aux(Trix *trx, int32_t count) {
    auto [arr, offset] = trx->gvm_alloc_n<Object>(static_cast<vm_size_t>(count), Trix::ChunkKind::Array);
    trx->gvm_set_obj_count(offset, static_cast<length_t>(count));
    std::fill_n(arr, count, Object::make_null());
    auto arr_obj = Object::make_array(offset, static_cast<length_t>(count));
    arr_obj.set_literal();
    return {arr_obj, offset};
}

static PipeCoroutineSetup pipe_alloc_stage_context(Trix *trx) {
    auto [ctx, ctx_offset] = trx->coroutine_alloc_context();

    ctx->m_status = CoroutineContext::Ready;
    ctx->m_has_return = false;
    ctx->m_activation_sl = trx->m_curr_save_level;
    ctx->m_trap_exit = false;
    ctx->m_flags = 0;
    ctx->m_context_offset = ctx_offset;
    ctx->m_wake_time_ns = 0;
    ctx->m_suspend_remaining_ns = 0;
    ctx->m_ready_next = nulloffset;
    ctx->m_ready_prev = nulloffset;
    ctx->m_timer_next = nulloffset;
    ctx->m_id = trx->m_next_coroutine_id++;
    ctx->m_quantum = trx->m_default_coroutine_quantum;
    ctx->m_ops_remaining = trx->m_default_coroutine_quantum;
    ctx->m_priority = CoroutineContext::PriorityNormal;
    ctx->m_blocked_sender_next = nulloffset;
    ctx->m_last_run_time_ns = 0;
    ctx->m_joiner = nulloffset;
    ctx->m_return_value = Object::make_null();
    ctx->m_mailbox = nulloffset;
    ctx->m_monitors = nulloffset;
    ctx->m_monitoring = nulloffset;
    ctx->m_binding_table = nulloffset;
    ctx->m_exit_reason = trx->wellknown_name(WellKnownName::Normal);
    // Recycled contexts keep the dead coroutine's bytes (in-class member
    // initializers do not run on gvm_alloc storage), so these must be set
    // explicitly like coroutine_launch_common does -- the GC walker reads
    // m_last_joined_exit_reason for every registry context, and a stale
    // m_curr_alloc_global would leak into the stage's allocations.
    ctx->m_last_joined_exit_reason = trx->wellknown_name(WellKnownName::NoError);
    ctx->m_curr_alloc_global = false;
    ctx->m_last_mailbox_capacity = 0;
    ctx->m_debug_name = nulloffset;

    // Root the context across coroutine_alloc_stacks: it can fire vm_global_gc,
    // and the context is not yet linked into the registry nor rooted by the
    // caller (the caller only roots it once we return -- across its own aux-array
    // alloc), so an in-flight GC here would sweep the freshly-allocated context.
    // Push a coroutine Object referencing it (gc_mark_object marks the block);
    // m_stacks_offset stays nulloffset across the alloc so gc_walk_coroutine_context
    // skips the not-yet-initialized stack-region walk (the other GC-walked fields
    // are already set above).  Same window coroutine_launch_common closes by
    // registry-linking first -- done on the op stack here to avoid leaving a
    // half-built stage linked in the registry if the caller errors before finalize.
    ctx->m_stacks_offset = nulloffset;
    trx->require_op_capacity(1);
    *++trx->m_op_ptr = Object::make_coroutine(ctx_offset);
    auto stacks_offset = trx->coroutine_alloc_stacks();
    --trx->m_op_ptr;  // pop the temp root
    ctx->m_stacks_offset = stacks_offset;

    auto base_ptr = trx->offset_to_ptr<Object>(stacks_offset);
    auto op_base = base_ptr;
    auto exec_base = op_base + DefaultCoroutineOperandDepth;
    auto err_base = exec_base + DefaultCoroutineExecutionDepth;
    auto dict_base = err_base + DefaultCoroutineErrorDepth;
    auto scratch_base = dict_base + DefaultCoroutineDictionaryDepth;

    ctx->m_op_base = trx->ptr_to_offset(op_base);
    ctx->m_op_ptr = ctx->m_op_base;
    ctx->m_op_limit = trx->ptr_to_offset(op_base + DefaultCoroutineOperandDepth);
    ctx->m_op_high_water = trx->ptr_to_offset(op_base - 1);
    ctx->m_exec_base = trx->ptr_to_offset(exec_base);
    ctx->m_exec_limit = trx->ptr_to_offset(exec_base + DefaultCoroutineExecutionDepth);
    ctx->m_exec_high_water = trx->ptr_to_offset(exec_base - 1);
    ctx->m_err_base = trx->ptr_to_offset(err_base);
    ctx->m_err_ptr = ctx->m_err_base;
    ctx->m_err_limit = trx->ptr_to_offset(err_base + DefaultCoroutineErrorDepth);
    ctx->m_err_high_water = trx->ptr_to_offset(err_base - 1);
    ctx->m_dict_base = trx->ptr_to_offset(dict_base);
    ctx->m_dict_limit = trx->ptr_to_offset(dict_base + DefaultCoroutineDictionaryDepth);
    ctx->m_dict_high_water = trx->ptr_to_offset(dict_base - 1);
    ctx->m_scratch_base = trx->ptr_to_offset(scratch_base);
    ctx->m_scratch_ptr = ctx->m_scratch_base;
    ctx->m_scratch_limit = trx->ptr_to_offset(scratch_base + trx->m_scratch_depth);

    // Dict stack: systemdict + protocoldict + userdict
    dict_base[0] = Object::make_dict(trx->ptr_to_offset(trx->m_systemdict));
    dict_base[1] = Object::make_dict(trx->ptr_to_offset(trx->m_protocoldict));
    dict_base[2] = Object::make_dict(trx->ptr_to_offset(trx->m_userdict));
    ctx->m_dict_ptr = trx->ptr_to_offset(dict_base + PermanentDictCount);

    return {ctx_offset, exec_base};
}

// Finalize a pipeline stage coroutine: set exec pointer, clear scanner, insert into scheduler.
static void pipe_finalize_stage_context(Trix *trx, vm_offset_t ctx_offset, Object *exec_base, length_t exec_count) {
    auto ctx = trx->offset_to_ptr<CoroutineContext>(ctx_offset);
    ctx->m_exec_ptr = trx->ptr_to_offset(exec_base + exec_count);
    ctx->m_scanner_stream = nullptr;

    // Insert into registry list (after current running coroutine)
    auto running = trx->offset_to_ptr<CoroutineContext>(trx->m_running_coroutine);
    ctx->m_next = running->m_next;
    running->m_next = ctx_offset;

    // Push to ready queue for scheduling
    trx->ready_queue_push(ctx);
    trx->coroutine_count_inc();
}

static vm_offset_t pipe_create_stage_coroutine(Trix *trx, Object output_buf, Object stage_proc) {
    auto [ctx_offset, exec_base] = pipe_alloc_stage_context(trx);

    // Exec stack: [@coroutine-complete] [output-buf] [@pipe-cleanup] [@try-barrier] [stage-proc]
    length_t ei = 0;
    exec_base[ei++] = Object::make_control_operator(SystemName::atCoroutineComplete);
    exec_base[ei++] = output_buf;
    exec_base[ei++] = Object::make_control_operator(SystemName::atPipeCleanup);
    exec_base[ei++] = Object::make_control_operator(SystemName::atTryBarrier);
    exec_base[ei++] = stage_proc;

    pipe_finalize_stage_context(trx, ctx_offset, exec_base, ei);
    return ctx_offset;
}

// Variant for lazy-seq source: pre-loads lazy-seq node on operand stack.
static vm_offset_t pipe_create_lazy_source_coroutine(Trix *trx, Object output_buf, Object stage_proc, Object lazy_node) {
    auto ctx_offset = pipe_create_stage_coroutine(trx, output_buf, stage_proc);
    auto ctx = trx->offset_to_ptr<CoroutineContext>(ctx_offset);

    // Push lazy-seq node onto the coroutine's operand stack
    auto op_base = trx->offset_to_ptr<Object>(ctx->m_op_base);
    op_base[0] = lazy_node;
    ctx->m_op_ptr = trx->ptr_to_offset(op_base + 1);

    return ctx_offset;
}

// Variant for batch stage: sets up @pipe-stage control operator instead of a Trix proc.
static vm_offset_t pipe_create_batch_coroutine(Trix *trx, Object input_buf, Object output_buf, int32_t batch_size) {
    auto [ctx_offset, exec_base] = pipe_alloc_stage_context(trx);

    // Root the unlinked stage context across the global batch-array allocation
    // (pipe_alloc_stage_aux can fire vm_global_gc, and the stage is not a GC root
    // until finalize).  Empty its exec range first so a GC walk of the rooted
    // context skips the not-yet-built exec stack.
    auto ctx = trx->offset_to_ptr<CoroutineContext>(ctx_offset);
    ctx->m_exec_ptr = ctx->m_exec_base;
    trx->require_op_capacity(1);
    *++trx->m_op_ptr = Object::make_coroutine(ctx_offset);

    auto [batch_arr_obj, _] = pipe_alloc_stage_aux(trx, batch_size);

    // Exec stack: [@coroutine-complete] [output-buf] [@pipe-cleanup] [@try-barrier]
    //             [input-buf] [output-buf] [batch-arr] [0] [batch-size] [phase=0] [@pipe-stage]
    length_t ei = 0;
    exec_base[ei++] = Object::make_control_operator(SystemName::atCoroutineComplete);
    exec_base[ei++] = output_buf;
    exec_base[ei++] = Object::make_control_operator(SystemName::atPipeCleanup);
    exec_base[ei++] = Object::make_control_operator(SystemName::atTryBarrier);
    exec_base[ei++] = input_buf;
    exec_base[ei++] = output_buf;
    exec_base[ei++] = batch_arr_obj;            // companion (literal Array)
    exec_base[ei++] = Object::make_integer(0);  // batch_idx
    exec_base[ei++] = Object::make_integer(batch_size);
    exec_base[ei++] = Object::make_integer(PipeBatchGet);  // phase
    exec_base[ei++] = Object::make_control_operator(SystemName::atPipeStage);

    pipe_finalize_stage_context(trx, ctx_offset, exec_base, ei);
    --trx->m_op_ptr;  // drop the context root; the finalized stage now roots it
    return ctx_offset;
}

// Variant for take stage: sets up @pipe-stage control operator with take phases.
static vm_offset_t pipe_create_take_coroutine(Trix *trx, Object input_buf, Object output_buf, int32_t take_n) {
    auto [ctx_offset, exec_base] = pipe_alloc_stage_context(trx);

    // Exec stack: [@coroutine-complete] [output-buf] [@pipe-cleanup] [@try-barrier]
    //             [input-buf] [output-buf] [remaining] [phase=PipeTakeGet] [@pipe-stage]
    length_t ei = 0;
    exec_base[ei++] = Object::make_control_operator(SystemName::atCoroutineComplete);
    exec_base[ei++] = output_buf;
    exec_base[ei++] = Object::make_control_operator(SystemName::atPipeCleanup);
    exec_base[ei++] = Object::make_control_operator(SystemName::atTryBarrier);
    exec_base[ei++] = input_buf;
    exec_base[ei++] = output_buf;
    exec_base[ei++] = Object::make_integer(take_n);       // remaining
    exec_base[ei++] = Object::make_integer(PipeTakeGet);  // phase
    exec_base[ei++] = Object::make_control_operator(SystemName::atPipeStage);

    pipe_finalize_stage_context(trx, ctx_offset, exec_base, ei);
    return ctx_offset;
}

// Variant for drop stage: sets up @pipe-stage control operator with drop phases.
static vm_offset_t pipe_create_drop_coroutine(Trix *trx, Object input_buf, Object output_buf, int32_t drop_n) {
    auto [ctx_offset, exec_base] = pipe_alloc_stage_context(trx);

    // Exec stack: [@coroutine-complete] [output-buf] [@pipe-cleanup] [@try-barrier]
    //             [input-buf] [output-buf] [remaining] [phase=PipeDropGet] [@pipe-stage]
    length_t ei = 0;
    exec_base[ei++] = Object::make_control_operator(SystemName::atCoroutineComplete);
    exec_base[ei++] = output_buf;
    exec_base[ei++] = Object::make_control_operator(SystemName::atPipeCleanup);
    exec_base[ei++] = Object::make_control_operator(SystemName::atTryBarrier);
    exec_base[ei++] = input_buf;
    exec_base[ei++] = output_buf;
    exec_base[ei++] = Object::make_integer(drop_n);       // remaining
    exec_base[ei++] = Object::make_integer(PipeDropGet);  // phase
    exec_base[ei++] = Object::make_control_operator(SystemName::atPipeStage);

    pipe_finalize_stage_context(trx, ctx_offset, exec_base, ei);
    return ctx_offset;
}

// Variant for distinct stage: sets up @pipe-stage with seen-set companion.
static vm_offset_t pipe_create_distinct_coroutine(Trix *trx, Object input_buf, Object output_buf) {
    auto [ctx_offset, exec_base] = pipe_alloc_stage_context(trx);

    // Allocate empty dynamic set for seen tracking
    auto [set, set_offset] = Dict::create_set(trx, 8, Object::DictMode::ReadWriteDynamic);

    // Exec stack: [@coroutine-complete] [output-buf] [@pipe-cleanup] [@try-barrier]
    //             [input-buf] [output-buf] [set] [phase=PipeDistinctGet] [@pipe-stage]
    length_t ei = 0;
    exec_base[ei++] = Object::make_control_operator(SystemName::atCoroutineComplete);
    exec_base[ei++] = output_buf;
    exec_base[ei++] = Object::make_control_operator(SystemName::atPipeCleanup);
    exec_base[ei++] = Object::make_control_operator(SystemName::atTryBarrier);
    exec_base[ei++] = input_buf;
    exec_base[ei++] = output_buf;
    exec_base[ei++] = Object::make_set(set_offset);
    exec_base[ei++] = Object::make_integer(PipeDistinctGet);
    exec_base[ei++] = Object::make_control_operator(SystemName::atPipeStage);

    pipe_finalize_stage_context(trx, ctx_offset, exec_base, ei);
    return ctx_offset;
}

// Variant for window stage: sets up @pipe-stage with circular buffer.
static vm_offset_t pipe_create_window_coroutine(Trix *trx, Object input_buf, Object output_buf, int32_t window_size) {
    auto [ctx_offset, exec_base] = pipe_alloc_stage_context(trx);

    // Root the unlinked stage context across the global window-array allocation
    // (see pipe_create_batch_coroutine).
    auto ctx = trx->offset_to_ptr<CoroutineContext>(ctx_offset);
    ctx->m_exec_ptr = ctx->m_exec_base;
    trx->require_op_capacity(1);
    *++trx->m_op_ptr = Object::make_coroutine(ctx_offset);

    auto [window_arr_obj, _] = pipe_alloc_stage_aux(trx, window_size);

    // Exec stack: [@coroutine-complete] [output-buf] [@pipe-cleanup] [@try-barrier]
    //             [input-buf] [output-buf] [window-arr] [write-idx=0] [filled=0] [size] [phase] [@pipe-stage]
    length_t ei = 0;
    exec_base[ei++] = Object::make_control_operator(SystemName::atCoroutineComplete);
    exec_base[ei++] = output_buf;
    exec_base[ei++] = Object::make_control_operator(SystemName::atPipeCleanup);
    exec_base[ei++] = Object::make_control_operator(SystemName::atTryBarrier);
    exec_base[ei++] = input_buf;
    exec_base[ei++] = output_buf;
    exec_base[ei++] = window_arr_obj;           // companion (literal Array)
    exec_base[ei++] = Object::make_integer(0);  // write-idx
    exec_base[ei++] = Object::make_integer(0);  // filled
    exec_base[ei++] = Object::make_integer(window_size);
    exec_base[ei++] = Object::make_integer(PipeWindowGet);
    exec_base[ei++] = Object::make_control_operator(SystemName::atPipeStage);

    pipe_finalize_stage_context(trx, ctx_offset, exec_base, ei);
    --trx->m_op_ptr;  // drop the context root
    return ctx_offset;
}

// Variant for merge stage: round-robin across N input buffers.
// Exec stack companions (below phase[0]):
//   [-4] output-buf
//   [-3] inputs-arr-offset (pipe-buffer reuse: offset to Object[N] on VM heap)
//   [-2] source-count
//   [-1] current-idx
static vm_offset_t pipe_create_merge_coroutine(Trix *trx, Object output_buf, Object *input_bufs, int32_t source_count) {
    auto [ctx_offset, exec_base] = pipe_alloc_stage_context(trx);

    // Root the unlinked stage context across the global inputs-array allocation
    // (see pipe_create_batch_coroutine).
    auto ctx = trx->offset_to_ptr<CoroutineContext>(ctx_offset);
    ctx->m_exec_ptr = ctx->m_exec_base;
    trx->require_op_capacity(1);
    *++trx->m_op_ptr = Object::make_coroutine(ctx_offset);

    // Allocate (global, GC-walkable) array of input buffer Objects
    auto [inputs_obj, inputs_offset] = pipe_alloc_stage_aux(trx, source_count);
    auto inputs = trx->offset_to_ptr<Object>(inputs_offset);
    for (int32_t i = 0; i < source_count; ++i) {
        inputs[i] = input_bufs[i];
    }

    // Exec stack: [@coroutine-complete] [output-buf] [@pipe-cleanup] [@try-barrier]
    //             [output-buf] [inputs-arr] [count] [idx=0] [phase] [@pipe-stage]
    length_t ei = 0;
    exec_base[ei++] = Object::make_control_operator(SystemName::atCoroutineComplete);
    exec_base[ei++] = output_buf;
    exec_base[ei++] = Object::make_control_operator(SystemName::atPipeCleanup);
    exec_base[ei++] = Object::make_control_operator(SystemName::atTryBarrier);
    exec_base[ei++] = output_buf;
    exec_base[ei++] = inputs_obj;  // companion (literal Array)
    exec_base[ei++] = Object::make_integer(source_count);
    exec_base[ei++] = Object::make_integer(0);  // current-idx
    exec_base[ei++] = Object::make_integer(PipeMergeScan);
    exec_base[ei++] = Object::make_control_operator(SystemName::atPipeStage);

    pipe_finalize_stage_context(trx, ctx_offset, exec_base, ei);
    --trx->m_op_ptr;  // drop the context root
    return ctx_offset;
}

// Variant for zip stage: sequential read from N input buffers per cycle.
// Exec stack companions (below phase[0]):
//   [-5] output-buf
//   [-4] inputs-arr-offset
//   [-3] source-count
//   [-2] items-arr-offset (pre-allocated Object[N] for tuple construction)
//   [-1] read-idx
static vm_offset_t pipe_create_zip_coroutine(Trix *trx, Object output_buf, Object *input_bufs, int32_t source_count) {
    auto [ctx_offset, exec_base] = pipe_alloc_stage_context(trx);

    // Root the unlinked stage context (and, across the second allocation, the
    // first aux array) on the op stack: both global aux allocations can fire GC,
    // and neither the unlinked stage nor a not-yet-stored aux array is a GC root.
    auto ctx = trx->offset_to_ptr<CoroutineContext>(ctx_offset);
    ctx->m_exec_ptr = ctx->m_exec_base;
    trx->require_op_capacity(2);
    *++trx->m_op_ptr = Object::make_coroutine(ctx_offset);

    // Allocate (global, GC-walkable) array of input buffer Objects
    auto [inputs_obj, inputs_offset] = pipe_alloc_stage_aux(trx, source_count);
    auto inputs = trx->offset_to_ptr<Object>(inputs_offset);
    for (int32_t i = 0; i < source_count; ++i) {
        inputs[i] = input_bufs[i];
    }
    *++trx->m_op_ptr = inputs_obj;  // root inputs across the items allocation below

    // Allocate items array for tuple construction
    auto [items_obj, _] = pipe_alloc_stage_aux(trx, source_count);

    // Exec stack: [@coroutine-complete] [output-buf] [@pipe-cleanup] [@try-barrier]
    //             [output-buf] [inputs-arr] [count] [items-arr] [read-idx=0] [phase] [@pipe-stage]
    length_t ei = 0;
    exec_base[ei++] = Object::make_control_operator(SystemName::atCoroutineComplete);
    exec_base[ei++] = output_buf;
    exec_base[ei++] = Object::make_control_operator(SystemName::atPipeCleanup);
    exec_base[ei++] = Object::make_control_operator(SystemName::atTryBarrier);
    exec_base[ei++] = output_buf;
    exec_base[ei++] = inputs_obj;  // companion (literal Array)
    exec_base[ei++] = Object::make_integer(source_count);
    exec_base[ei++] = items_obj;                // companion (literal Array)
    exec_base[ei++] = Object::make_integer(0);  // read-idx
    exec_base[ei++] = Object::make_integer(PipeZipGet);
    exec_base[ei++] = Object::make_control_operator(SystemName::atPipeStage);

    pipe_finalize_stage_context(trx, ctx_offset, exec_base, ei);
    trx->m_op_ptr -= 2;  // drop the context + inputs roots
    return ctx_offset;
}

//===--- Stage descriptor ---===//

struct PipeStageInfo {
    int32_t type;     // PipeStageMap, etc.
    Object proc;      // user proc (null for batch/take/drop/distinct)
    int32_t batch_n;  // N parameter (batch size, take count, or drop count)
    Object init;      // initial accumulator (scan only)
};

//===--- Helper: walk descriptor chain, collect stages ---===//
//
// Walks the pipeline descriptor chain from outermost to root source.
// Returns the root source and fills stages[] in EXECUTION order (source->sink).

static Object pipe_walk_chain(Trix *trx, Object descriptor, PipeStageInfo *stages, int32_t &stage_count) {
    // Collect stages in reverse order (outermost first)
    PipeStageInfo rev[MaxPipeStages];
    int32_t rev_count = 0;

    auto current = descriptor;
    while (current.is_dict()) {
        if (rev_count >= MaxPipeStages) {
            trx->error(Error::LimitCheck, "pipeline: too many stages (max {})", MaxPipeStages);
        } else {
            auto dict = current.dict_value(trx);
            auto type_key = trx->wellknown_name(WellKnownName::IKeyType);
            auto source_key = trx->wellknown_name(WellKnownName::IKeySource);
            auto type_val = dict->get(trx, &type_key);
            if (type_val == nullptr) {
                // Dict has no /type key.  If it has /source, it's a malformed descriptor.
                if (dict->get(trx, &source_key) != nullptr) {
                    trx->error(Error::TypeCheck, "pipeline: descriptor dict has /source but no /type");
                } else {
                    break;  // not a pipeline descriptor -- treat as root source
                }
            }

            if (!type_val->is_name()) {
                trx->error(Error::TypeCheck, "pipeline: descriptor /type must be a name");
            } else {
                auto type_sv = type_val->name_sv(trx);
                PipeStageInfo info{};
                if (type_sv == "pipe-map") {
                    info.type = PipeStageMap;
                } else if (type_sv == "pipe-filter") {
                    info.type = PipeStageFilter;
                } else if (type_sv == "pipe-flat-map") {
                    info.type = PipeStageFlatMap;
                } else if (type_sv == "pipe-batch") {
                    info.type = PipeStageBatch;
                } else if (type_sv == "pipe-take") {
                    info.type = PipeStageTake;
                } else if (type_sv == "pipe-tap") {
                    info.type = PipeStageTap;
                } else if (type_sv == "pipe-drop") {
                    info.type = PipeStageDrop;
                } else if (type_sv == "pipe-scan") {
                    info.type = PipeStageScan;
                } else if (type_sv == "pipe-distinct") {
                    info.type = PipeStageDistinct;
                } else if (type_sv == "pipe-window") {
                    info.type = PipeStageWindow;
                } else if ((type_sv == "pipe-merge") || (type_sv == "pipe-zip")) {
                    // Multi-source descriptor: stop walking, return as root
                    break;
                } else {
                    trx->error(Error::TypeCheck, "pipeline: unknown stage type '{}'", type_sv);
                }

                if ((info.type == PipeStageBatch) || (info.type == PipeStageTake) || (info.type == PipeStageDrop) ||
                    (info.type == PipeStageWindow)) {
                    auto n_key = trx->wellknown_name(WellKnownName::IKeyN);
                    auto n_val = dict->get(trx, &n_key);
                    if ((n_val != nullptr) && !n_val->is_integer()) {
                        trx->error(Error::TypeCheck, "pipeline: descriptor /n must be an integer");
                    } else {
                        info.batch_n = (n_val != nullptr) ? n_val->integer_value() : 1;
                    }
                } else if (info.type == PipeStageScan) {
                    auto proc_key = trx->wellknown_name(WellKnownName::IKeyProc);
                    auto proc_val = dict->get(trx, &proc_key);
                    info.proc = (proc_val != nullptr) ? *proc_val : Object::make_null();
                    auto init_key = trx->wellknown_name(WellKnownName::IKeyInit);
                    auto init_val = dict->get(trx, &init_key);
                    info.init = (init_val != nullptr) ? *init_val : Object::make_null();
                } else {
                    auto proc_key = trx->wellknown_name(WellKnownName::IKeyProc);
                    auto proc_val = dict->get(trx, &proc_key);
                    info.proc = (proc_val != nullptr) ? *proc_val : Object::make_null();
                }

                rev[rev_count++] = info;

                auto source_val = dict->get(trx, &source_key);
                current = (source_val != nullptr) ? *source_val : Object::make_null();
            }
        }
    }

    // Reverse into execution order
    stage_count = rev_count;
    for (int32_t i = 0; i < rev_count; ++i) {
        stages[i] = rev[rev_count - 1 - i];
    }

    return current;  // root source
}

//===--- Helper: materialize pipeline ---===//
//
// Walks descriptor chain, allocates buffers, launches stage coroutines.
// Returns the last buffer (sink reads from it).
// Also returns coroutine offsets for cleanup.

// Maximum total coroutines in a materialized pipeline (including multi-source sub-pipelines).
static constexpr int32_t MaxPipeCoroutines = MaxPipeStages * 4;

struct PipeMaterialized {
    Object sink_buf;                            // last buffer -- caller reads from this
    vm_offset_t coroutines[MaxPipeCoroutines];  // all pipeline coroutines for cleanup
    int32_t coroutine_count;
};

// Handle multi-source (merge/zip) materialization in pipe_materialize.
// Returns true if root_source was a merge/zip descriptor (result populated with sub-pipelines).
static bool
pipe_materialize_multi_source(Trix *trx, Object root_source, Object first_buf, int32_t capacity, PipeMaterialized &result) {
    if (!root_source.is_dict()) {
        return false;
    } else {
        auto root_dict = root_source.dict_value(trx);
        auto type_key = trx->wellknown_name(WellKnownName::IKeyType);
        auto type_val = root_dict->get(trx, &type_key);
        if ((type_val == nullptr) || !type_val->is_name()) {
            return false;
        } else {
            auto type_sv = type_val->name_sv(trx);
            if ((type_sv != "pipe-merge") && (type_sv != "pipe-zip")) {
                return false;
            } else {
                auto is_merge = (type_sv == "pipe-merge");

                auto sources_key = trx->wellknown_name(WellKnownName::IKeySources);
                auto sources_val = root_dict->get(trx, &sources_key);
                if (sources_val == nullptr) {
                    trx->error(Error::TypeCheck, "pipeline: merge/zip descriptor missing /sources key");
                } else if (!sources_val->is_array()) {
                    trx->error(Error::TypeCheck, "pipeline: merge/zip /sources must be an array");
                } else {
                    auto [sources, sources_len] = sources_val->array_value(trx);
                    if (sources_len > MaxPipeStages) {
                        trx->error(
                                Error::LimitCheck, "pipeline: too many merge/zip sources ({}, max {})", sources_len, MaxPipeStages);
                    } else {
                        // Materialize each sub-source independently, collecting their sink buffers
                        Object sub_sink_bufs[MaxPipeStages];
                        for (length_t si = 0; si < sources_len; ++si) {
                            auto sub = pipe_materialize(trx, sources[si], capacity);
                            sub_sink_bufs[si] = sub.sink_buf;
                            for (int32_t ci = 0; ci < sub.coroutine_count; ++ci) {
                                if (result.coroutine_count >= MaxPipeCoroutines) {
                                    trx->error(Error::LimitCheck, "pipeline: too many coroutines (max {})", MaxPipeCoroutines);
                                } else {
                                    result.coroutines[result.coroutine_count++] = sub.coroutines[ci];
                                }
                            }
                        }

                        // Create merge/zip coroutine: reads from N sub-sink buffers, writes to first_buf
                        if (result.coroutine_count >= MaxPipeCoroutines) {
                            trx->error(Error::LimitCheck, "pipeline: too many coroutines (max {})", MaxPipeCoroutines);
                        } else if (is_merge) {
                            result.coroutines[result.coroutine_count++] =
                                    pipe_create_merge_coroutine(trx, first_buf, sub_sink_bufs, static_cast<int32_t>(sources_len));
                        } else {
                            result.coroutines[result.coroutine_count++] =
                                    pipe_create_zip_coroutine(trx, first_buf, sub_sink_bufs, static_cast<int32_t>(sources_len));
                        }
                        return true;
                    }
                }
            }
        }
    }
}

static PipeMaterialized pipe_materialize(Trix *trx, Object source_or_desc, int32_t capacity) {
    PipeStageInfo stages[MaxPipeStages];
    int32_t stage_count = 0;
    auto root_source = pipe_walk_chain(trx, source_or_desc, stages, stage_count);

    // Total buffers needed: stage_count + 1 (source->first-stage, ..., last-stage->sink)
    auto buf_count = stage_count + 1;

    // Root every global block this materialize holds only in C-locals across the
    // gvm_alloc storm below.  Two families are at risk, both swept by a vm_global_gc
    // fired from any pipe_alloc_buffer / stage context+stacks+aux alloc / multi-source
    // recursion step:
    //   1. The descriptor's contents -- root_source and each stage's proc/init.  When
    //      the pipeline was built inside ${...} these are GLOBAL composites (the source
    //      array, the map/filter/scan procs) reachable ONLY through `root_source` and
    //      the C-local `stages[]`; sweeping a stage proc corrupts the built stage proc
    //      and the stage coroutine then faults ("upstream error").  (stages[i].proc /
    //      .init are null Objects when unused -- PipeStageInfo is value-initialized --
    //      so rooting them unconditionally is safe.)
    //   2. The PipeBuffers -- GLOBAL (pipe_alloc_buffer -> gvm_alloc) and unreachable
    //      from any root until their referencing coroutine is finalized into the
    //      registry (pipe_finalize_stage_context).
    // Root all on the operand stack (not the gc-root stack): multi-source pipelines
    // recurse through pipe_materialize and the count scales with input structure --
    // the migration's "potentially-unbounded -> operand stack" case, and consistent
    // with pipe_alloc_stage_context / pipe_alloc_stage_aux.  Pop at the tail, by which
    // point root_source/procs are baked into the stage procs and every buffer is
    // reachable through its coroutine.
    Object buffers[MaxPipeStages + 1];
    trx->require_op_capacity(buf_count + 1 + (2 * stage_count));
    auto *materialize_roots = trx->m_op_ptr;  // restore to here at the tail
    *++trx->m_op_ptr = root_source;
    for (int32_t i = 0; i < stage_count; ++i) {
        *++trx->m_op_ptr = stages[i].proc;
        *++trx->m_op_ptr = stages[i].init;
    }
    for (int32_t i = 0; i < buf_count; ++i) {
        auto cap = static_cast<length_t>(capacity);
        auto [_, hdr_offset] = pipe_alloc_buffer(trx, cap);
        buffers[i] = Object::make_pipe_buffer(hdr_offset);
        *++trx->m_op_ptr = buffers[i];  // root across the subsequent gvm_allocs
    }

    PipeMaterialized result{};
    result.coroutine_count = 0;

    // Source feeder coroutine: reads root_source, writes to buffers[0]
    const bool is_multi_source = pipe_materialize_multi_source(trx, root_source, buffers[0], capacity, result);

    if (!is_multi_source) {
        // Check lazy-seq BEFORE array: a lazy-seq node IS a 2-element array.
        if (root_source.is_lazy_seq(trx)) {
            auto proc = pipe_build_lazy_source_proc(trx, buffers[0]);
            result.coroutines[result.coroutine_count++] = pipe_create_lazy_source_coroutine(trx, buffers[0], proc, root_source);
        } else if (root_source.is_sequence()) {
            auto proc = pipe_build_array_source_proc(trx, root_source, buffers[0]);
            result.coroutines[result.coroutine_count++] = pipe_create_stage_coroutine(trx, buffers[0], proc);
        } else {
            trx->error(Error::TypeCheck,
                       "pipeline: source must be array, packed, lazy-seq, or multi-source descriptor, got {}",
                       Object::type_sv(root_source.type()));
        }
    }

    // Mid-stage coroutines
    for (int32_t i = 0; i < stage_count; ++i) {
        auto &stage = stages[i];
        auto in_buf = buffers[i];
        auto out_buf = buffers[i + 1];

        if (stage.type == PipeStageBatch) {
            result.coroutines[result.coroutine_count++] = pipe_create_batch_coroutine(trx, in_buf, out_buf, stage.batch_n);
        } else if (stage.type == PipeStageTake) {
            result.coroutines[result.coroutine_count++] = pipe_create_take_coroutine(trx, in_buf, out_buf, stage.batch_n);
        } else if (stage.type == PipeStageDrop) {
            result.coroutines[result.coroutine_count++] = pipe_create_drop_coroutine(trx, in_buf, out_buf, stage.batch_n);
        } else if (stage.type == PipeStageDistinct) {
            result.coroutines[result.coroutine_count++] = pipe_create_distinct_coroutine(trx, in_buf, out_buf);
        } else if (stage.type == PipeStageWindow) {
            result.coroutines[result.coroutine_count++] = pipe_create_window_coroutine(trx, in_buf, out_buf, stage.batch_n);
        } else {
            Object proc;
            switch (stage.type) {
            case PipeStageMap:
                proc = pipe_build_map_proc(trx, in_buf, out_buf, stage.proc);
                break;

            case PipeStageFilter:
                proc = pipe_build_filter_proc(trx, in_buf, out_buf, stage.proc);
                break;

            case PipeStageFlatMap:
                proc = pipe_build_flat_map_proc(trx, in_buf, out_buf, stage.proc);
                break;

            case PipeStageTap:
                proc = pipe_build_tap_proc(trx, in_buf, out_buf, stage.proc);
                break;

            case PipeStageScan:
                proc = pipe_build_scan_proc(trx, in_buf, out_buf, stage.proc, stage.init);
                break;

            default:
                trx->error(Error::InternalError, "pipeline: unknown stage type {}", stage.type);
            }
            result.coroutines[result.coroutine_count++] = pipe_create_stage_coroutine(trx, out_buf, proc);
        }
    }

    result.sink_buf = buffers[buf_count - 1];
    trx->m_op_ptr = materialize_roots;  // pop all roots: procs are baked into stages, buffers reachable via coroutines
    return result;
}

//===--- Helper: kill all pipeline coroutines ---===//

static void pipe_kill_all(Trix *trx, const PipeMaterialized &pipeline) {
    for (int32_t i = 0; i < pipeline.coroutine_count; ++i) {
        auto ctx = trx->offset_to_ptr<CoroutineContext>(pipeline.coroutines[i]);
        if (ctx->m_status != CoroutineContext::Dead) {
            trx->coroutine_kill(ctx, trx->wellknown_name(WellKnownName::Killed));
        }
    }
}

//===--- Helper: build sink loop Trix proc ---===//
//
// For pipe-run:   { sink-buf pipe-get { user-proc exec } { exit } if-else } loop
// For pipe-collect/reduce: handled inline by the terminal operator.

static Object pipe_build_sink_proc(Trix *trx, Object sink_buf, Object user_proc) {
    // True case: { user-proc exec }
    Object true_items[2];
    true_items[0] = user_proc;
    true_items[1] = Object::make_operator(SystemName::Exec);
    auto true_proc = pipe_build_proc(trx, true_items, 2);

    // False case: { exit }
    Object false_items[1];
    false_items[0] = Object::make_operator(SystemName::Exit);
    auto false_proc = pipe_build_proc(trx, false_items, 1);

    auto body_proc = pipe_build_get_loop_body(trx, sink_buf, true_proc, false_proc);
    return pipe_build_loop_wrapper(trx, body_proc);
}

//===--- Helper: wrap sink proc with try + kill-all + rethrow ---===//
//
// Builds a Trix proc that:
//   1. Runs sink_proc inside `try` (catches any error)
//   2. Kills all pipeline coroutines (whether success or error)
//   3. Re-throws the error if one occurred
//
// Trix equivalent:
//   { { sink-proc } try
//     coro-0 coroutine-kill coro-1 coroutine-kill ...
//     dup /no-error ne { pop rethrow } if
//     pop }

static Object pipe_build_cleanup_wrapper(Trix *trx, Object sink_proc, const PipeMaterialized &pipeline) {
    // Rethrow proc: { pop rethrow }
    Object rethrow_items[2];
    rethrow_items[0] = Object::make_operator(SystemName::Pop);
    rethrow_items[1] = Object::make_operator(SystemName::ReThrow);
    auto rethrow_proc = pipe_build_proc(trx, rethrow_items, 2);

    // Build the full wrapper:
    //   sink-proc(literal) try [handle coroutine-kill] ... dup /no-error ne rethrow-proc if pop
    auto n = pipeline.coroutine_count;
    auto total = static_cast<length_t>(2 + (n * 2) + 6);  // sink try + N*(handle kill) + dup /no-error ne rethrow if pop
    auto items = trx->vm_alloc_n_ptr<Object>(total);
    auto offset = trx->ptr_to_offset(items);
    auto curr_save_level = trx->m_curr_save_level;
    length_t idx = 0;

    // sink-proc(executable) try -- PushOpDirect pushes it to op stack; try needs executable
    items[idx] = sink_proc;
    items[idx].set_save_level(curr_save_level);
    ++idx;
    items[idx] = Object::make_operator(SystemName::Try);
    items[idx].set_save_level(curr_save_level);
    ++idx;

    // coro-0 coroutine-kill coro-1 coroutine-kill ...
    for (int32_t i = 0; i < n; ++i) {
        items[idx] = Object::make_coroutine(pipeline.coroutines[i]);
        items[idx].set_literal();
        items[idx].set_save_level(curr_save_level);
        ++idx;
        items[idx] = Object::make_operator(SystemName::Kill);
        items[idx].set_save_level(curr_save_level);
        ++idx;
    }

    // dup /no-error ne rethrow-proc if pop
    items[idx] = Object::make_operator(SystemName::Dup);
    items[idx].set_save_level(curr_save_level);
    ++idx;
    items[idx] = trx->error_name(Error::NoError);
    items[idx].set_literal();
    items[idx].set_save_level(curr_save_level);
    ++idx;
    items[idx] = Object::make_operator(SystemName::Ne);
    items[idx].set_save_level(curr_save_level);
    ++idx;
    items[idx] = rethrow_proc;
    items[idx].set_save_level(curr_save_level);
    ++idx;
    items[idx] = Object::make_operator(SystemName::If);
    items[idx].set_save_level(curr_save_level);
    ++idx;
    items[idx] = Object::make_operator(SystemName::Pop);
    items[idx].set_save_level(curr_save_level);
    ++idx;

    auto result = Object::make_array(offset, total);
    result.set_executable();
    return result;
}

//===--- pipe-run: source capacity proc -- ---===//
//
// Execute pipeline.  Caller loops pipe-get on last buffer, calls proc on each item.
// throws: execstack-overflow, opstack-underflow, type-check, <any stage error>

static void pipe_run_op(Trix *trx) {
    trx->verify_operands(VerifyProc | VerifyCurry | VerifyContinuation, VerifyInteger, VerifyAny);

    auto proc_obj = *trx->m_op_ptr--;
    auto capacity = trx->m_op_ptr->integer_value();
    if (capacity <= 0) {
        trx->error(Error::RangeCheck, "pipe-run: capacity must be positive");
    } else {
        --trx->m_op_ptr;

        auto source_obj = *trx->m_op_ptr--;

        // Root the user proc across pipeline construction: it is popped into a C-local and
        // first used (baked into the sink proc) only after pipe_materialize's gvm_alloc
        // storm, so a ${...}-global proc would otherwise be swept.  Held until the wrapper
        // is on the exec stack (a root that the GC walks into to mark the embedded proc).
        // source_obj is consumed by pipe_materialize's alloc-free chain walk, which roots
        // root_source / stage procs itself.
        trx->gc_root_push_oneoff(proc_obj);

        // Materialize pipeline
        auto pipeline = pipe_materialize(trx, source_obj, capacity);

        // Build sink proc wrapped with try + kill-all + rethrow
        auto sink_proc = pipe_build_sink_proc(trx, pipeline.sink_buf, proc_obj);
        auto wrapper = pipe_build_cleanup_wrapper(trx, sink_proc, pipeline);

        trx->require_exec_capacity(1);
        *++trx->m_exec_ptr = wrapper;

        trx->gc_root_pop_oneoff();
    }
}

//===--- pipe-collect: source capacity -- array ---===//
//
// Execute pipeline.  Caller collects all items into an array.
// throws: execstack-overflow, opstack-underflow, type-check, <any stage error>

static void pipe_collect_op(Trix *trx) {
    trx->require_op_count(2);

    auto cap_ptr = trx->m_op_ptr;
    if (!cap_ptr->is_integer()) {
        trx->error(Error::TypeCheck, "pipe-collect: capacity must be integer");
    } else {
        auto capacity = cap_ptr->integer_value();
        if (capacity <= 0) {
            trx->error(Error::RangeCheck, "pipe-collect: capacity must be positive");
        } else {
            --trx->m_op_ptr;

            auto source_obj = *trx->m_op_ptr;
            --trx->m_op_ptr;

            auto pipeline = pipe_materialize(trx, source_obj, capacity);

            // Build collect proc: mark + get-loop + array-from-mark
            // Push mark, loop pipe-get (items accumulate above mark), then array-from-mark.

            // Exit proc: { exit }
            Object exit_items[1];
            exit_items[0] = Object::make_operator(SystemName::Exit);
            auto exit_proc = pipe_build_proc(trx, exit_items, 1);

            // Noop true proc: { } (value stays on op stack, inside the mark)
            auto noop_proc = pipe_build_proc(trx, nullptr, 0);

            // Loop body: { sink-buf pipe-get noop-proc exit-proc if-else }
            Object body_items[5];
            body_items[0] = pipeline.sink_buf;
            body_items[0].set_literal();
            body_items[1] = Object::make_operator(SystemName::PipeGet);
            body_items[2] = noop_proc;
            body_items[3] = exit_proc;
            body_items[4] = Object::make_operator(SystemName::IfElse);
            auto body_proc = pipe_build_proc(trx, body_items, 5);

            // Outer: { mark body-proc loop array-from-mark }
            Object collect_items[4];
            collect_items[0] = Object::make_operator(SystemName::Mark);
            collect_items[1] = body_proc;
            collect_items[2] = Object::make_operator(SystemName::Loop);
            collect_items[3] = Object::make_operator(SystemName::ArrayFromMark);
            auto collect_proc = pipe_build_proc(trx, collect_items, 4);

            auto wrapper = pipe_build_cleanup_wrapper(trx, collect_proc, pipeline);
            trx->require_exec_capacity(1);
            *++trx->m_exec_ptr = wrapper;
        }
    }
}

//===--- pipe-reduce: source capacity init proc -- value ---===//
//
// Execute pipeline.  Caller folds all items with proc(acc, item).
// throws: execstack-overflow, opstack-underflow, type-check, <any stage error>

static void pipe_reduce_op(Trix *trx) {
    trx->verify_operands(VerifyProc | VerifyCurry | VerifyContinuation, VerifyAny, VerifyInteger, VerifyAny);

    auto proc_obj = *trx->m_op_ptr--;
    auto init_obj = *trx->m_op_ptr--;
    auto capacity = trx->m_op_ptr->integer_value();
    if (capacity <= 0) {
        trx->error(Error::RangeCheck, "pipe-reduce: capacity must be positive");
    } else {
        --trx->m_op_ptr;

        auto source_obj = *trx->m_op_ptr--;

        // Root the user proc and the accumulator init across pipeline construction: both
        // are popped into C-locals and first used (baked into the reduce proc) only after
        // pipe_materialize's gvm_alloc storm, so ${...}-global values would otherwise be
        // swept.  Held until the wrapper is on the exec stack.  (See pipe_run_op.)
        trx->require_gc_root_capacity(2);
        *++trx->m_gc_roots_ptr = proc_obj;
        *++trx->m_gc_roots_ptr = init_obj;

        auto pipeline = pipe_materialize(trx, source_obj, capacity);

        // Build reduce proc: push init, then loop { pipe-get { proc exec } { exit } if-else }
        // The accumulator sits on the op stack.  Each iteration: acc is on stack,
        // pipe-get pushes value on top, then proc exec consumes acc+value and pushes new acc.

        // True case: { user-proc exec } -- proc takes (acc value) and returns new-acc
        Object true_items[2];
        true_items[0] = proc_obj;
        true_items[1] = Object::make_operator(SystemName::Exec);
        auto true_proc = pipe_build_proc(trx, true_items, 2);

        // False case: { exit }
        Object false_items[1];
        false_items[0] = Object::make_operator(SystemName::Exit);
        auto false_proc = pipe_build_proc(trx, false_items, 1);

        // Loop body: { sink-buf pipe-get true-proc false-proc if-else }
        Object body_items[5];
        body_items[0] = pipeline.sink_buf;
        body_items[0].set_literal();
        body_items[1] = Object::make_operator(SystemName::PipeGet);
        body_items[2] = true_proc;
        body_items[3] = false_proc;
        body_items[4] = Object::make_operator(SystemName::IfElse);
        auto body_proc = pipe_build_proc(trx, body_items, 5);

        // Outer: { init body-proc loop }
        // Push init on op stack as accumulator, then loop
        Object reduce_items[3];
        reduce_items[0] = init_obj;
        reduce_items[0].set_literal();
        reduce_items[1] = body_proc;
        reduce_items[2] = Object::make_operator(SystemName::Loop);
        auto reduce_proc = pipe_build_proc(trx, reduce_items, 3);

        auto wrapper = pipe_build_cleanup_wrapper(trx, reduce_proc, pipeline);
        trx->require_exec_capacity(1);
        *++trx->m_exec_ptr = wrapper;

        trx->reset_gc_root(2);
    }
}

//===--- GC walkers ---===//
//
// gc_walk_pipebuffer: walk the live items in a pipe buffer's
// circular Object array.  PipeBufferHeader is followed contiguously
// in the same block by the data array; m_data is the pre-computed
// offset to that array.  Bails out for parked buffers
// (m_head == PipeRecycleSentinel) so free-list bytes aren't
// misinterpreted.
//
// Other PipeBufferHeader fields (m_producer, m_consumer,
// m_blocked_writer, m_blocked_reader) point at coroutines reached
// via the coroutine-list root walk; defensive marking here would
// be redundant but harmless.  Skipped for clarity.
//
void gc_walk_pipebuffer(vm_offset_t payload_offset) {
    auto *hdr = offset_to_ptr<PipeBufferHeader>(payload_offset);
    if ((hdr->m_head != PipeRecycleSentinel) && (hdr->m_capacity != 0)) {
        auto *data = offset_to_ptr<Object>(hdr->m_data);
        for (length_t i = 0; i < hdr->m_count; ++i) {
            auto idx = static_cast<length_t>((hdr->m_head + i) % hdr->m_capacity);
            gc_mark_object(data[idx]);
        }
    }
}
