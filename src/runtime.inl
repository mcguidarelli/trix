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

//===--- Runtime Helpers ---===//
[[nodiscard]] Object save_stack(Object stack_obj, const Object *stack_base, const Object *stack_ptr) {
    auto curr_save_level = m_curr_save_level;
    auto src_ptr = stack_base;
    auto dst_ptr = stack_obj.array_objects(this);
    auto offset = ptr_to_offset(dst_ptr);
    auto n = (stack_ptr - stack_base + 1);
    auto result_obj = Object::make_array(offset, static_cast<length_t>(n), Object::LiteralAttrib, Object::ReadOnlyAccess);
    while (n-- != 0) {
        // copy from stack to array
        *dst_ptr = *src_ptr;
        // SourceLoc repurposes m_object_save_level as stream_id; do not overwrite it
        if (!dst_ptr->is_sourceloc()) {
            dst_ptr->set_save_level(curr_save_level);
        }

        ++dst_ptr;
        ++src_ptr;
    }
    return result_obj;
}

// Uses a self-pipe to probe address accessibility without risk of SIGSEGV.
// Protocol:
//   write(WRITE_END, x, n): EFAULT -> address not in any readable mapping (Invalid)
//                           success -> address is readable
//   read(READ_END, x, n):   EFAULT -> address is readable but not writable (IsReadOnly)
//                           success -> address is both readable and writable (IsReadWrite)
// The pipe is opened once at startup and closed when the interpreter exits.
//
// EMPTY-PIPE INVARIANT.  Every exit path leaves the pipe empty, so each probe
// starts from a known-clean state and a write never blocks on a full pipe.
// This is why the success path needs no explicit drain:
//   * IsNullPtr: nothing was written.
//   * Invalid (write == -1): write returns -1 only when zero bytes were copied
//     (the source faults at its first byte).  A partially-readable source
//     returns a short count > 0 and falls through to the read step instead, so
//     this branch never leaves bytes in the pipe.
//   * IsReadWrite (read succeeds): the pipe started empty and n <= 64 KiB
//     (asserted) <= the pipe capacity, so the write into an empty pipe is never
//     short -- exactly n bytes are buffered -- and a blocking pipe read of n
//     with n bytes available returns all n, leaving the pipe empty again.
//   * IsReadOnly (read == -1/EFAULT): the failed read consumed none of the n
//     buffered bytes, so they are drained explicitly below.  The full write
//     above guarantees the drain of n neither under-drains nor blocks.
[[nodiscard]] AddressState address_state(const void *address, size_t n) const {
    // the default pipe size is 64k, we do not want to block on a pipe full condition
    assert((n > 0) && (n <= (64ULL * 1024)));

    constexpr int READ_END = 0;
    constexpr int WRITE_END = 1;

    auto probe_addr = const_cast<void *>(address);
    if (probe_addr == nullptr) {
        return AddressState::IsNullPtr;
    } else if (::write(m_pipe_fds[WRITE_END], probe_addr, n) == -1) {
        if (errno != EFAULT) {
            diag_println("Trix readability probe of address {} failed: errno {}/{}", probe_addr, errno, errno_string());
        }
        // outside of readable address space
        return AddressState::Invalid;
    } else if (::read(m_pipe_fds[READ_END], probe_addr, n) == -1) {
        auto saved_errno = errno;
        if (saved_errno != EFAULT) {
            diag_println("Trix writability probe of address {} failed: errno {}/{}", probe_addr, saved_errno, errno_string());
        }
        // Drain the n bytes from the pipe unconditionally: read() failed without
        // consuming them regardless of errno (EFAULT, EINTR, etc.).
        do {
            char buffer[16];  // drain chunk; size is a throughput trade-off, not a correctness limit
            auto count = (n < sizeof(buffer)) ? n : sizeof(buffer);
            auto actual_count = ::read(m_pipe_fds[READ_END], buffer, count);
            if (actual_count <= 0) {
                if (actual_count == -1) {
                    diag_println("Trix pipe drain read failed: errno {}/{}", errno, errno_string());
                }
                break;
            } else {
                n -= static_cast<size_t>(actual_count);
            }
        } while (n != 0);
        // outside of writable address space
        return AddressState::IsReadOnly;
    } else {
        return AddressState::IsReadWrite;
    }
}
// Push a for-all exec frame: [proc, container, saved-depth, @array-for-all or @packed-for-all].
// saved-depth captures m_op_ptr at frame-push time so each at_*_forall
// fire can verify the body's stack effect matches "value -- ".
// Caller must ensure require_exec_capacity(4) has been called and that the
// operand stack is at the depth the iteration body should restore to after
// each pass.
static void push_forall_frame(Trix *trx, Object proc_obj, Object container_obj) {
    *++trx->m_exec_ptr = proc_obj;
    *++trx->m_exec_ptr = container_obj;
    auto saved_depth = static_cast<integer_t>(trx->m_op_ptr - trx->m_op_base);
    *++trx->m_exec_ptr = Object::make_integer(saved_depth);
    if (container_obj.is_array()) {
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atArrayForAll);
    } else {
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atPackedForAll);
    }
}
private:
// scan_interpolation_escape_impl: handles \{name} escape inside (string) literals.
// Defined as a Trix static method (not Stream member) because it calls
// PrintFmt::process_object(), which is defined after Stream.
[[nodiscard]]
static std::pair<int, Object> scan_interpolation_escape_impl(Stream *self, Trix *trx, vm_t *string_ptr, const vm_t *string_limit) {
    auto output_count = 0;
    auto result_obj = Object::make_null();

    // 1. Read name into token buffer until '}'
    auto name_base = trx->m_token_base;
    auto name_ptr = name_base;
    length_t name_len = 0;
    while (true) {
        auto ch = self->getc(trx);
        if (ch == EOFc) {
            output_count = Stream::InterpSyntax;
            result_obj = Object::make_error_string(trx, "EndOfStream in \\{name} interpolation");
            break;
        } else if (ch == '}') {
            if (name_len == 0) {
                output_count = Stream::InterpSyntax;
                result_obj = Object::make_error_string(trx, "empty \\{} interpolation");
            }
            break;
        } else if (name_len >= MaxNameLength) {
            output_count = Stream::InterpSyntax;
            result_obj = Object::make_error_string(trx, "\\{name} exceeds maximum name length");
            break;
        } else {
            *name_ptr++ = static_cast<vm_t>(ch);
            ++name_len;
        }
    }
    if (output_count == 0) {
        // 2. Look up the name (no VM allocation)
        auto name_sv = make_sv(name_base, name_len);
        auto value_ptr = Name::string_search(trx, name_sv);
        if (value_ptr == nullptr) {
            output_count = Stream::InterpUndefined;
            result_obj = Object::make_error_string(trx, "\\{{{}}}: name not found", name_sv);
        } else {
            // 3. Format value into the string build buffer via PrintFmt
            auto capacity = static_cast<length_t>(string_limit - string_ptr);
            int dropped_count = 0;
            std::tie(output_count, dropped_count) = PrintFmt::process_object(trx, value_ptr, string_ptr, capacity);
            if (dropped_count != 0) {
                output_count = Stream::InterpOverflow;
            }
        }
    }
    return std::pair{output_count, result_obj};
}
