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

//===--- Output, Formatting, and Print Operators ---===//
//
// Stack and object display (=, ==, print, print-stack, stack), the operand-stack
// snapshot (opstack), format-string
// output (print-fmt, fprint-fmt, sprint-fmt and array variants), format-string
// input (sscan-fmt, fscan-fmt), eq-buffer accessors (=array, =string, =dict),
// nl, and the format-string grammar documentation.
//

// =array: :- arr
// Pushes the temporary =array, cleared to zero length.  The returned Object is an
// eqarray ref (SpecialFlag set; value slot holds m_generation, the creation-time
// counter), so any prior =array ref still on the stack becomes stale and errors
// cleanly on access.
// throws: opstack-overflow, limit-check
static void eqarray_op(Trix *trx) {
    trx->require_op_capacity(1);

    if (trx->m_eqarray_generation == std::numeric_limits<uint32_t>::max()) {
        trx->error(Error::LimitCheck, "=array generation counter exhausted (2^32 creations); cannot create more eqarray refs");
    } else {
        auto root_arr = trx->root_object(RootObject::EqArray);
        root_arr.array_clear(trx);
        ++trx->m_eqarray_generation;
        auto pushed_obj =
                Object::make_eqarray(trx->m_eqarray_generation, root_arr.arrays_length(), root_arr.attrib(), root_arr.access());
        *++trx->m_op_ptr = pushed_obj;
    }
}

// =string: :- str
// Pushes the temporary =string, cleared to zero length.  The returned Object is an
// eqstring ref (SpecialFlag set; value slot holds m_generation, the creation-time
// counter), so any prior =string ref still on the stack becomes stale and errors
// cleanly on access.
// throws: opstack-overflow, limit-check
static void eqstring_op(Trix *trx) {
    trx->require_op_capacity(1);

    if (trx->m_eqstring_generation == std::numeric_limits<uint32_t>::max()) {
        trx->error(Error::LimitCheck, "=string generation counter exhausted (2^32 creations); cannot create more eqstring refs");
    } else {
        auto root_str = trx->root_object(RootObject::EqString);
        root_str.string_clear(trx);
        ++trx->m_eqstring_generation;
        // The public-facing eqstring ref we push onto the stack carries the generation in
        // m_generation (union alias over the value slot), the full buffer capacity in
        // m_string_length so callers (sprint-fmt, printf, etc.) can size their writes, and
        // SpecialFlag so the stale-detection path fires if a later =string or )#= invalidates
        // this ref.
        auto pushed_obj =
                Object::make_eqstring(trx->m_eqstring_generation, root_str.string_length(), root_str.attrib(), root_str.access());
        *++trx->m_op_ptr = pushed_obj;
    }
}

// =dict: :- dict
// Pushes the temporary =dict, cleared to empty.  The returned Object is an eqdict ref
// (SpecialFlag set; value slot holds m_generation, the creation-time counter), so any
// prior =dict or <<>>#= ref still on the stack becomes stale and errors cleanly on
// access.
// throws: opstack-overflow, limit-check
static void eqdict_op(Trix *trx) {
    trx->require_op_capacity(1);

    if (trx->m_eqdict_generation == std::numeric_limits<uint32_t>::max()) {
        trx->error(Error::LimitCheck, "=dict generation counter exhausted (2^32 creations); cannot create more eqdict refs");
    } else {
        auto dict = trx->m_eqdict;
        dict->reset_dict(trx, trx->m_eqdict_maxlength, Object::ReadWriteAccess);
        ++trx->m_eqdict_generation;
        auto dict_obj = Object::make_eqdict(trx->m_eqdict_generation);
        *++trx->m_op_ptr = dict_obj;
    }
}

// nl: :- --
// Writes a newline to stdout.
// throws: unsupported
static void newline_op(Trix *trx) {
    auto out = trx->m_stdout;
    if (out != nullptr) {
        out->putc(trx, ASCII_LF);
    } else {
        trx->error(Error::Unsupported, "stdout support not enabled");
    }
}

// opstack: :- arr
// Snapshots the operand stack into a new array.
// throws: vm-full
static void op_stack_op(Trix *trx) {
    auto n = static_cast<length_t>((trx->m_op_ptr - trx->m_op_base) + 1);
    auto [dst_ptr, dst_offset] = trx->vm_alloc_n<Object>(static_cast<vm_size_t>(n));
    auto src_ptr = trx->m_op_base;
    auto curr_save_level = trx->m_curr_save_level;
    auto arr_obj = Object::make_array(dst_offset, n);
    while (n-- != 0) {
        *dst_ptr = src_ptr->make_clone(trx);
        dst_ptr->set_save_level(curr_save_level);
        ++src_ptr;
        ++dst_ptr;
    }
    if (trx->has_op_capacity(1)) {
        // Push the snapshot array on top of the existing stack
        ++trx->m_op_ptr;
    } else {
        // Stack is full: free all operands and use the base slot so the
        // snapshot is still available (at the cost of the original stack).
        for (auto p = trx->m_op_ptr; p >= trx->m_op_base; --p) {
            p->maybe_free_extvalue(trx);
        }
        trx->m_op_ptr = trx->m_op_base;
    }
    *trx->m_op_ptr = arr_obj;
}

// print: str :- --
// Writes a string to stdout.
// throws: io-write-error, opstack-underflow, type-check, unsupported
static void print_op(Trix *trx) {
    auto out = trx->m_stdout;
    if (out != nullptr) {
        trx->verify_operands(VerifyString);

        auto str_ptr = trx->m_op_ptr;
        auto str_data = str_ptr->string_vptr(trx);
        auto length = str_ptr->string_length();
        if (length > 1) {
            out->putn(trx, str_data, length);
        } else if (length == 1) {
            out->putc(trx, *str_data);
        }
        --trx->m_op_ptr;
    } else {
        trx->error(Error::Unsupported, "stdout support not enabled");
    }
}

// Pop and print the top operand to stdout.
// object_form == true prints in syntactic form (==), false prints default form (=).
static void eq_print_impl(Trix *trx, bool object_form) {
    auto out = trx->m_stdout;
    if (out != nullptr) {
        trx->require_op_count(1);

        auto top_ptr = trx->m_op_ptr;
        auto output_count = PrintFmt::process_object(trx, top_ptr, out, object_form);
        if (output_count != 0) {
            out->putc(trx, ASCII_LF);
            out->flush(trx);
        }
        top_ptr->maybe_free_extvalue(trx);
        trx->m_op_ptr = (top_ptr - 1);
    } else {
        trx->error(Error::Unsupported, "stdout support not enabled");
    }
}

// =: any :- --
// Pops and prints the top operand to stdout.
// throws: vm-full, io-write-error, opstack-underflow, unsupported
static void single_eq_op(Trix *trx) {
    eq_print_impl(trx, false);
}

// ==: any :- --
// Pops and prints the top operand in syntactic form.
// throws: vm-full, io-write-error, opstack-underflow, unsupported
static void double_eq_op(Trix *trx) {
    eq_print_impl(trx, true);
}

// Print the operand stack to stdout without consuming it.
// object_form == true prints in syntactic form (pstack), false prints default form (stack).
static void print_stack_impl(Trix *trx, bool object_form) {
    auto out = trx->m_stdout;
    if (out != nullptr) {
        bool any_output = false;
        for (auto curr_ptr = trx->m_op_ptr; curr_ptr >= trx->m_op_base; --curr_ptr) {
            auto output_count = PrintFmt::process_object(trx, curr_ptr, out, object_form);
            if (output_count != 0) {
                out->putc(trx, ASCII_LF);
                any_output = true;
            }
        }
        if (any_output) {
            out->flush(trx);
        }
    } else {
        trx->error(Error::Unsupported, "stdout support not enabled");
    }
}

// stack: :- --
// Prints the operand stack to stdout.
// throws: vm-full, io-write-error, unsupported
static void stack_op(Trix *trx) {
    print_stack_impl(trx, false);
}

// print-stack: :- --
// Prints the operand stack in syntactic form without consuming it.
// throws: vm-full, io-write-error, unsupported
static void printstack_op(Trix *trx) {
    print_stack_impl(trx, true);
}

// print-fmt: str mark any ... :- int bool
// Formatted print to stdout.
// throws: vm-full, invalid-format-string, io-write-error, limit-check, opstack-underflow, range-check, type-check,
// unmatched-mark, unsupported
static void printfmt_op(Trix *trx) {
    auto out = trx->m_stdout;
    if (out != nullptr) {
        auto [mark_ptr, args_count] = trx->find_opstack_mark();
        trx->verify_operands<MatchPolicy::AnyType>(mark_ptr, VerifyMark, VerifyString);

        auto args_ptr = (mark_ptr + 1);
        auto fmt_ptr = (mark_ptr - 1);
        auto formatstr = fmt_ptr->sv_value(trx);

        PrintFmt fmt(trx, formatstr.data(), sv_length(formatstr), args_ptr, args_count, out);
        auto [output_count, dropped_count] = fmt.process();
        auto result = (dropped_count == 0);
        *fmt_ptr = Object::make_integer(result ? output_count : dropped_count);
        *mark_ptr = Object::make_boolean(result);

        maybe_free_extvalue_opstack(trx, mark_ptr);
        trx->m_op_ptr = mark_ptr;
    } else {
        trx->error(Error::Unsupported, "stdout support not enabled");
    }
}

// fprint-fmt: stream str mark any ... :- int bool
// Formatted print to stream.
// throws: vm-full, invalid-format-string, invalid-stream-access, io-write-error, limit-check, opstack-underflow,
// range-check, type-check, unmatched-mark
static void fprintfmt_op(Trix *trx) {
    auto [mark_ptr, args_count] = trx->find_opstack_mark();
    trx->verify_operands<MatchPolicy::AnyType>(mark_ptr, VerifyMark, VerifyString, VerifyRWStream);

    auto args_ptr = (mark_ptr + 1);
    auto fmt_ptr = (mark_ptr - 1);
    auto stream_ptr = (fmt_ptr - 1);
    auto [stream, sid] = stream_ptr->stream_value(trx);
    if (stream->is_writable(sid)) {
        auto formatstr = fmt_ptr->sv_value(trx);

        PrintFmt fmt(trx, formatstr.data(), sv_length(formatstr), args_ptr, args_count, stream);
        auto [output_count, dropped_count] = fmt.process();
        auto result = (dropped_count == 0);
        *stream_ptr = Object::make_integer(result ? output_count : dropped_count);
        *fmt_ptr = Object::make_boolean(result);

        maybe_free_extvalue_opstack(trx, mark_ptr);
        trx->m_op_ptr = fmt_ptr;
    } else {
        trx->error(Error::InvalidStreamAccess, "fprint-fmt: stream is not writable");
    }
}

// sprint-fmt: dst-str fmt-str mark any ... :- str bool
// Formatted print to string.
// throws: vm-full, invalid-format-string, limit-check, opstack-underflow, range-check, type-check, unmatched-mark
static void sprintfmt_op(Trix *trx) {
    auto [mark_ptr, args_count] = trx->find_opstack_mark();
    trx->verify_operands<MatchPolicy::AnyType>(mark_ptr, VerifyMark, VerifyString, VerifyRWString);

    auto args_ptr = (mark_ptr + 1);
    auto fmt_ptr = (mark_ptr - 1);
    auto out_ptr = (fmt_ptr - 1);
    auto formatstr = fmt_ptr->sv_value(trx);

    PrintFmt fmt(
            trx, formatstr.data(), sv_length(formatstr), args_ptr, args_count, out_ptr->string_vptr(trx), out_ptr->string_length());
    auto [output_count, dropped_count] = fmt.process();
    auto result = (dropped_count == 0);
    if (result) {
        out_ptr->set_string_length(trx, static_cast<length_t>(output_count));
    } else {
        // On overflow report dropped_count (how much did not fit), matching asprint-fmt
        // and the four stream/stdout print-fmt ops; the caller uses it to size the buffer.
        *out_ptr = Object::make_integer(dropped_count);
    }
    *fmt_ptr = Object::make_boolean(result);

    maybe_free_extvalue_opstack(trx, mark_ptr);
    trx->m_op_ptr = fmt_ptr;
}

// sscan-fmt: input-str fmt-str mark any ... :- any ... int
// Scans formatted values from string.
// throws: invalid-format-string, limit-check, opstack-underflow, range-check, scan-duplicate-arg-id, scan-input-fail,
// scan-match-fail, scan-type-fail, scan-type-mismatch, type-check, unmatched-mark
static void sscanfmt_op(Trix *trx) {
    auto [mark_ptr, args_count] = trx->find_opstack_mark();
    trx->verify_operands<MatchPolicy::AnyType>(mark_ptr, VerifyMark, VerifyString, VerifyString);

    auto args_ptr = (mark_ptr + 1);
    auto fmt_ptr = (mark_ptr - 1);
    auto input_ptr = (fmt_ptr - 1);
    auto formatstr = fmt_ptr->sv_value(trx);

    ScanFmt fmt(trx,
                formatstr.data(),
                sv_length(formatstr),
                args_ptr,
                args_count,
                input_ptr->string_vptr(trx),
                input_ptr->string_length());
    auto count = fmt.process();

    std::copy_n(args_ptr, args_count, input_ptr);
    auto top = (input_ptr + args_count);
    *top = Object::make_integer(count);
    trx->m_op_ptr = top;
}

// fscan-fmt: stream fmt-str mark any ... :- any ... int
// Scans formatted values from a readable stream.  Blocks on interactive streams.
// throws: invalid-format-string, invalid-stream-access, limit-check, opstack-underflow, range-check,
// scan-duplicate-arg-id, scan-input-fail, scan-match-fail, scan-type-fail, scan-type-mismatch, type-check,
// unmatched-mark
static void fscanfmt_op(Trix *trx) {
    auto [mark_ptr, args_count] = trx->find_opstack_mark();
    trx->verify_operands<MatchPolicy::AnyType>(mark_ptr, VerifyMark, VerifyString, VerifyStream);

    auto args_ptr = (mark_ptr + 1);
    auto fmt_ptr = (mark_ptr - 1);
    auto stream_ptr = (fmt_ptr - 1);
    auto [stream, sid] = stream_ptr->stream_value(trx);
    if (stream->is_readable(sid)) {
        auto formatstr = fmt_ptr->sv_value(trx);

        ScanFmt fmt(trx, formatstr.data(), sv_length(formatstr), args_ptr, args_count, stream);
        auto count = fmt.process();

        std::copy_n(args_ptr, args_count, stream_ptr);
        auto top = (stream_ptr + args_count);
        *top = Object::make_integer(count);
        trx->m_op_ptr = top;
    } else {
        trx->error(Error::InvalidStreamAccess, "fscan-fmt: stream is not readable");
    }
}

// aprint-fmt: str arr :- int bool
// Formatted print to stdout using array args.
// throws: vm-full, invalid-format-string, io-write-error, limit-check, opstack-underflow, range-check, type-check,
// unsupported
static void aprintfmt_op(Trix *trx) {
    auto out = trx->m_stdout;
    if (out != nullptr) {
        trx->verify_operands(VerifyArray, VerifyString);

        auto arr_ptr = trx->m_op_ptr;
        auto fmt_ptr = (arr_ptr - 1);
        auto [args_ptr, args_count] = arr_ptr->array_value(trx);
        auto formatstr = fmt_ptr->sv_value(trx);

        PrintFmt fmt(trx, formatstr.data(), sv_length(formatstr), args_ptr, args_count, out);
        auto [output_count, dropped_count] = fmt.process();
        auto result = (dropped_count == 0);
        *fmt_ptr = Object::make_integer(result ? output_count : dropped_count);
        *arr_ptr = Object::make_boolean(result);
        trx->m_op_ptr = arr_ptr;
    } else {
        trx->error(Error::Unsupported, "stdout support not enabled");
    }
}

// afprint-fmt: stream str arr :- int bool
// Formatted print to stream using array args.
// throws: vm-full, invalid-format-string, invalid-stream-access, io-write-error, limit-check, opstack-underflow,
// range-check, type-check
static void afprintfmt_op(Trix *trx) {
    trx->verify_operands(VerifyArray, VerifyString, VerifyRWStream);

    auto arr_ptr = trx->m_op_ptr;
    auto fmt_ptr = (arr_ptr - 1);
    auto stream_ptr = (fmt_ptr - 1);

    auto [stream, sid] = stream_ptr->stream_value(trx);
    if (stream->is_writable(sid)) {
        auto [args_ptr, args_count] = arr_ptr->array_value(trx);
        auto formatstr = fmt_ptr->sv_value(trx);

        PrintFmt fmt(trx, formatstr.data(), sv_length(formatstr), args_ptr, args_count, stream);
        auto [output_count, dropped_count] = fmt.process();
        auto result = (dropped_count == 0);
        *stream_ptr = Object::make_integer(result ? output_count : dropped_count);
        *fmt_ptr = Object::make_boolean(result);
        trx->m_op_ptr = fmt_ptr;
    } else {
        trx->error(Error::InvalidStreamAccess, "afprint-fmt: stream is not writable");
    }
}

// asprint-fmt: dst-str fmt-str arr :- str bool
// Formatted print to string using array args.
// throws: vm-full, invalid-format-string, limit-check, opstack-underflow, range-check, type-check
static void asprintfmt_op(Trix *trx) {
    trx->verify_operands(VerifyArray, VerifyString, VerifyRWString);

    auto arr_ptr = trx->m_op_ptr;
    auto [args_ptr, args_count] = arr_ptr->array_value(trx);
    auto fmt_ptr = (arr_ptr - 1);
    auto formatstr = fmt_ptr->sv_value(trx);
    auto out_ptr = (fmt_ptr - 1);

    PrintFmt fmt(
            trx, formatstr.data(), sv_length(formatstr), args_ptr, args_count, out_ptr->string_vptr(trx), out_ptr->string_length());
    auto [output_count, dropped_count] = fmt.process();
    auto result = (dropped_count == 0);
    if (result) {
        out_ptr->set_string_length(trx, static_cast<length_t>(output_count));
    } else {
        *out_ptr = Object::make_integer(dropped_count);
    }
    *fmt_ptr = Object::make_boolean(result);
    trx->m_op_ptr = fmt_ptr;
}
