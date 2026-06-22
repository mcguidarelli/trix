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

//===--- Dictionary and Dictionary-Stack Operators ---===//
//
// Dictionary creation, definition, lookup, and dictionary-stack management.
// Includes dict, def, begin/end, known, load, where, undef, store, update,
// and dict-stack query operators (dictstack, countdictstack, cleardictstack).
//

// Exec-stack frame (6 slots).  Layout while @dict-for-all is on the stack
// (as seen by dict_forall_clear / dict_forall_update, ptr -> @dict-for-all):
//
//   ptr[0]  @dict-for-all
//   ptr[-1] saved_depth  (integer_t; opstack depth the body must restore to)
//   ptr[-2] offset       (vm_offset_t of next Entry, or nulloffset when done)
//   ptr[-3] bucket_idx   (integer_t; -1 on first call, bucket index thereafter)
//   ptr[-4] dict
//   ptr[-5] proc
//
// Layout inside at_dict_forall_op (interpreter has already popped @dict-for-all,
// so m_exec_ptr has been decremented; m_exec_ptr -> saved_depth):
//
//   enter (m_exec_ptr ->)  saved_depth  exit (m_exec_ptr ->)  proc
//                          offset                             @dict-for-all
//                          bucket_idx                         saved_depth
//                          dict                               offset
//                          proc                               bucket_idx
//                                                             dict
//                                                             proc
static void dict_forall_clear(Trix *trx, Dict *target) {
    // consider having a count of active dict-for-all running to short circuit?
    auto ptr = trx->m_exec_ptr;
    while (ptr >= trx->m_exec_base) {
        if (ptr->is_operator() && ptr->operator_is_dictforall()) {
            auto offset_ptr = (ptr - 2);  // ptr[-1] is saved_depth; offset is at ptr[-2]
            auto dict_ptr = (offset_ptr - 2);
            auto dict = dict_ptr->dict_value(trx);
            if (dict == target) {
                // entry_offset == nulloffset signals end of iteration over the dict
                offset_ptr->update_uinteger(static_cast<uinteger_t>(nulloffset));
            }
            ptr -= 6;  // step over the full 6-slot @dict-for-all frame
        } else {
            --ptr;
        }
    }
}

static void dict_forall_update(Trix *trx, vm_offset_t target) {
    auto ptr = trx->m_exec_ptr;
    while (ptr >= trx->m_exec_base) {
        if (ptr->is_operator() && ptr->operator_is_dictforall()) {
            auto offset_ptr = (ptr - 2);  // ptr[-1] is saved_depth; offset is at ptr[-2]
            auto entry_offset = vm_offset_t{offset_ptr->uinteger_value()};
            if (entry_offset == target) {
                auto bucket_ptr = (offset_ptr - 1);
                auto dict_ptr = (offset_ptr - 2);
                auto dict = dict_ptr->dict_value(trx);
                auto bucket_idx = bucket_ptr->integer_value();
                auto [next_offset, next_idx, key, value] = dict->next(trx, entry_offset, bucket_idx);
                offset_ptr->update_uinteger(static_cast<uinteger_t>(next_offset));
                bucket_ptr->update_integer(next_idx);
            }
            ptr -= 6;  // step over the full 6-slot @dict-for-all frame
        } else {
            --ptr;
        }
    }
}

// begin: dict :- --
// Pushes a dictionary onto the dict stack.
// throws: dictstack-overflow, opstack-underflow, type-check
static void begin_op(Trix *trx) {
    trx->verify_operands(VerifyDict);
    trx->require_dict_capacity(1);

    auto dict_obj = *trx->m_op_ptr--;
    *++trx->m_dict_ptr = dict_obj;
    auto dict = dict_obj.dict_value(trx);
    dict->set_name_bindings(trx);
}

// @end-locals: (exec stack: --)
// Pops the frame dictionary from the dict stack.
// throws: (none)
static void at_end_locals_op(Trix *trx) {
    // pop the frame dict from dict stack and recycle it
    auto popped = trx->pop_frame_dict_if_present();
    if (popped.valid()) {
        Dict::recycle(trx, popped.dict, popped.offset);
    }
}

// begin-locals: val1 ... valK name1 ... nameK K N proc :- --
// Creates a frame dict with capacity N from K name/value pairs and executes proc.
// K is the declared name count; N is the dict capacity (N >= K).  When emitted by the
// scanner for plain |a b| with no #N suffix, N == K; the |a b|#N suffix allows N > K
// so the body can /foo def working variables into the same dict.
// throws: vm-full, dict-full, dictstack-overflow, execstack-overflow, opstack-underflow, range-check
static void begin_locals_op(Trix *trx) {
    // pop the body proc, capacity N, and declared count K
    trx->verify_operands(VerifyProc, VerifyInteger, VerifyInteger);
    auto proc_obj = *trx->m_op_ptr--;

    auto n = trx->m_op_ptr->integer_value();
    --trx->m_op_ptr;

    auto k = trx->m_op_ptr->integer_value();
    --trx->m_op_ptr;

    // Upper bound keeps 2*k from overflowing signed integer_t arithmetic.  N is
    // clamped to length_t (uint16_t) because that's the dict-capacity type.
    // K=0 is valid: the ||#N form declares a scratch frame dict with N capacity
    // and no stack-popped bindings (the body uses /foo def to populate).
    constexpr auto max_k = std::numeric_limits<integer_t>::max() / 2;
    constexpr auto max_n = static_cast<integer_t>(std::numeric_limits<length_t>::max());
    if ((k < 0) || (k > max_k)) {
        trx->error(Error::RangeCheck, "begin-locals: name count {} out of range", k);
    } else if ((n < k) || (n > max_n) || ((k == 0) && (n == 0))) {
        trx->error(Error::RangeCheck, "begin-locals: capacity {} out of range (must be {} <= N <= {}, and N >= 1)", n, k, max_n);
    } else {
        // need K names + K values on the operand stack
        trx->require_op_count(2 * k);
        trx->require_dict_capacity(1);
        trx->require_exec_capacity(2);

        // Create (or recycle out of the save-level-free frame-dict pool)
        // the frame dict for this |locals|#N scope.  The returned dict
        // already has Dict::IsFrame set so save/restore treats it as
        // transparent.
        auto [dict, dict_offset] = Dict::create_or_recycle_frame_dict(trx, static_cast<length_t>(n));

        // pop K names, then K values, and def each pair
        // stack: val1 ... valK name1 ... nameK  (nameK on top)
        auto names_base = trx->m_op_ptr - (k - 1);
        auto values_base = names_base - k;
        for (integer_t i = 0; i < k; ++i) {
            auto name_obj = names_base[i];
            auto value_obj = values_base[i];
            dict->put(trx, name_obj, value_obj, Dict::BindingMode::Bind);
        }
        // put() takes ownership of name/value ExtValues -- do not free stack slots
        trx->m_op_ptr = values_base - 1;

        // push dict onto dict stack
        auto dict_obj = Object::make_dict(dict_offset);
        *++trx->m_dict_ptr = dict_obj;
        dict->set_name_bindings(trx);

        // push @end-locals + body proc onto exec stack
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atEndLocals);
        *++trx->m_exec_ptr = proc_obj;
    }
}

// cleardictstack: :- --
// Pops all but systemdict and userdict from the dict stack.
// throws: (none)
static void cleardictstack_op(Trix *trx) {
    // do not pop systemdict, protocoldict, and userdict
    auto dict_bottom = (trx->m_dict_base + PermanentDictCount - 1);
    for (auto ptr = trx->m_dict_ptr; ptr > dict_bottom; --ptr) {
        auto dict = ptr->dict_value(trx);
        dict->clear_name_bindings(trx);
    }
    trx->m_dict_ptr = dict_bottom;
}
// countdictstack: :- int
// Pushes the dict stack depth.
// throws: opstack-overflow
static void countdictstack_op(Trix *trx) {
    trx->require_op_capacity(1);

    auto count = static_cast<integer_t>((trx->m_dict_ptr - trx->m_dict_base) + 1);
    *++trx->m_op_ptr = Object::make_integer(count);
}

// Names beginning with '@' are reserved for internal trampoline / control
// operators: they manipulate the execution stack (push continuations, restore
// frames, unwind barriers) and are installed only by the dispatch table or
// emitted by the scanner -- never bound by user code.  Reject any user def of
// one so the "@ = internal, non-@ = user-facing" invariant (operator count,
// banner, docs) holds and a user cannot shadow or hand-bind a control op.
// Non-name keys cannot start with '@', so they pass through untouched.
// throws: invalid-name
static void reject_reserved_name(Trix *trx, const Object *key_ptr) {
    if (key_ptr->is_name()) {
        auto name_sv = key_ptr->name_sv(trx);
        if (!name_sv.empty() && (name_sv.front() == '@')) {
            trx->error(Error::InvalidName, "names beginning with '@' are reserved for internal use");
        }
    }
}

// True iff key already names a built-in operator in systemdict.  systemdict is
// the source of truth for built-ins (operators are ReadOnly Operator values
// there), so this answers "would binding this name shadow a built-in?"
// regardless of any user binding already standing higher on the dict stack --
// which is why re-`override`-ing an already-shadowed name still succeeds.
// Non-name keys (binary-token String keys) never name an operator.
[[nodiscard]] static bool names_builtin_operator(Trix *trx, const Object *key_ptr) {
    if (key_ptr->is_name()) {
        auto existing_ptr = trx->m_systemdict->get(trx, key_ptr);
        return ((existing_ptr != nullptr) && existing_ptr->is_operator());
    } else {
        return false;
    }
}

// `def` / `def-persist` guard: refuse to silently shadow a built-in operator.
// A bare def of an operator name used to "work" (late binding resolved to the
// user value) yet silently broke the operator for the rest of the scope AND
// diverged under early binding -- the classic foot-gun.  Now it raises and
// points at the deliberate escape hatch.  `override` partners this: it REQUIRES
// an operator name (see require_operator_to_override), so the two operators
// partition every bindable name -- exactly one accepts any given name.
// throws: invalid-name
static void reject_operator_shadow(Trix *trx, const Object *key_ptr) {
    if (names_builtin_operator(trx, key_ptr)) {
        trx->error(Error::InvalidName,
                   "'{}' names a built-in operator; use `override` to shadow it deliberately",
                   key_ptr->name_sv(trx));
    }
}

// `override` guard: the name MUST name a built-in operator.  Using override on
// a non-operator name is a category error (use `def`) -- and a name that is no
// longer an operator (renamed / removed since the override was written) becomes
// a loud /undefined instead of silently degrading to a plain def.
// throws: undefined
static void require_operator_to_override(Trix *trx, const Object *key_ptr) {
    if (!names_builtin_operator(trx, key_ptr)) {
        trx->error(Error::Undefined, "override: no built-in operator of this name to shadow; use `def` instead");
    }
}

// def: key any :- --
// Defines key with value in the current dictionary, walking past any
// |...| frame dicts on the dict stack to find the first non-frame
// (typically userdict at module scope, or whatever was begin'd by the
// caller).  Use `local-def` to bind into the current frame.
// throws: vm-full, dict-full, opstack-underflow, read-only, type-check, invalid-name
static void def_op(Trix *trx) {
    trx->verify_operands(VerifyAny, VerifyKey);
    reject_reserved_name(trx, trx->m_op_ptr - 1);
    reject_operator_shadow(trx, trx->m_op_ptr - 1);

    auto dict_obj_ptr = Dict::dict_stack_first_nonframe(trx);
    auto dict = dict_obj_ptr->dict_value(trx);
    if (dict->has_write_access()) {
        auto val_ptr = trx->m_op_ptr;
        auto key_ptr = (val_ptr - 1);
        // NoBinding (clear), not Bind: `def` targets the first NON-frame dict, but a
        // |...| frame ABOVE may shadow this name -- Bind would repoint the m_binding
        // cache at this lower-priority entry so a later bare lookup wrongly skips the
        // shadowing frame.  Clearing forces the next lookup to re-walk and cache the
        // true topmost binding (engine bug #21).
        dict->put(trx, *key_ptr, *val_ptr, Dict::BindingMode::NoBinding);
        trx->m_op_ptr -= 2;
    } else {
        trx->error(Error::ReadOnly, "attempt to update a ReadOnly dict");
    }
}

// override: /name any --
// Identical to `def` (binds into the first non-frame dict, walking past |...|
// frames) EXCEPT it deliberately shadows a built-in operator of the same name.
// The name MUST name a systemdict operator -- otherwise /undefined (use `def`
// for non-operator names).  This makes operator-shadowing an explicit, self-
// documenting act rather than the silent foot-gun a bare `def` of an operator
// name used to be.  Note: shadowing changes only late-binding resolution; an
// already-`#e`-early-bound body or an already-cached name still reaches the
// operator -- override states intent, it does not retroactively rebind.
// throws: vm-full, dict-full, opstack-underflow, read-only, type-check, invalid-name, undefined
static void override_op(Trix *trx) {
    trx->verify_operands(VerifyAny, VerifyKey);
    reject_reserved_name(trx, trx->m_op_ptr - 1);
    require_operator_to_override(trx, trx->m_op_ptr - 1);

    auto dict_obj_ptr = Dict::dict_stack_first_nonframe(trx);
    auto dict = dict_obj_ptr->dict_value(trx);
    if (dict->has_write_access()) {
        auto val_ptr = trx->m_op_ptr;
        auto key_ptr = (val_ptr - 1);
        // NoBinding: like def_op, a frame above may shadow -- clear to keep the cache
        // coherent with dict-stack precedence (engine bug #21).
        dict->put(trx, *key_ptr, *val_ptr, Dict::BindingMode::NoBinding);
        trx->m_op_ptr -= 2;
    } else {
        trx->error(Error::ReadOnly, "attempt to update a ReadOnly dict");
    }
}

// def-persist: key any :- --
// Same shape as def, but the binding is NOT journaled -- it persists
// across the enclosing save/restore.  At sl=0 silently degrades to def.
// At sl > 0 enforces the -persist family ref-check: above-barrier key
// or value raises /above-barrier.  Missing keys are CREATED -- new
// DictEntry slots are allocated in global VM so the new
// binding survives restore.  The target dict is the first non-frame
// dict from the top of the dict stack (matching def's resolution).
// throws: above-barrier, vm-full, dict-full, opstack-underflow, read-only, type-check, invalid-name
static void def_persist_op(Trix *trx) {
    trx->verify_operands(VerifyAny, VerifyKey);
    reject_reserved_name(trx, trx->m_op_ptr - 1);
    reject_operator_shadow(trx, trx->m_op_ptr - 1);

    auto dict_obj_ptr = Dict::dict_stack_first_nonframe(trx);
    auto dict = dict_obj_ptr->dict_value(trx);
    if (!dict->has_write_access()) {
        trx->error(Error::ReadOnly, "attempt to update a ReadOnly dict");
    } else {
        auto val_ptr = trx->m_op_ptr;
        auto key_ptr = (val_ptr - 1);

        if (Save::is_active(trx)) {
            if (Save::is_above_barrier(trx, *val_ptr)) {
                trx->error(Error::AboveBarrier, "def-persist: value lives above the save barrier and would dangle on restore");
            } else if (Save::is_above_barrier(trx, *key_ptr)) {
                trx->error(Error::AboveBarrier, "def-persist: key lives above the save barrier");
            } else {
                dict->put_persist_or_create(trx, *key_ptr, *val_ptr);
            }
        } else {
            // sl=0: silent degrade to def's binding semantics -- NoBinding so a
            // shadowing frame is not bypassed (see def_op, engine bug #21).
            dict->put(trx, *key_ptr, *val_ptr, Dict::BindingMode::NoBinding);
        }
        trx->m_op_ptr -= 2;
    }
}

// local-def: key any :- --
// Defines key with value in the current locals frame on top of the dict
// stack.  The top of the dict stack must be a |...| frame dict; if not,
// raises Unsupported.  Counterpart to `def`, which (post-flip) skips
// frames and writes to the enclosing module scope.  Use `local-def`
// only when the binding genuinely belongs to the current frame
// (scratch state in a |...| proc, loop iteration variables, etc.).
// throws: vm-full, dict-full, opstack-underflow, read-only, type-check, unsupported, invalid-name
static void local_def_op(Trix *trx) {
    trx->verify_operands(VerifyAny, VerifyKey);
    reject_reserved_name(trx, trx->m_op_ptr - 1);

    auto dict = trx->m_dict_ptr->dict_value(trx);
    if (!dict->is_frame()) {
        trx->error(Error::Unsupported, "local-def: requires a |...| frame on the dict stack");
    } else if (!dict->has_write_access()) {
        trx->error(Error::ReadOnly, "attempt to update a ReadOnly dict");
    } else {
        auto val_ptr = trx->m_op_ptr;
        auto key_ptr = (val_ptr - 1);
        dict->put(trx, *key_ptr, *val_ptr, Dict::BindingMode::Bind);
        trx->m_op_ptr -= 2;
    }
}

// bind-locals: v1 ... vN names-array :- --
// Atomic batch binding into the current frame dict (top of dict stack).
// Each value vi is moved into the frame under names-array[i-1]; the
// leftmost array name receives the bottommost stack value.  The entire
// batch is pre-validated (top-of-dict-stack is a frame, all elements are
// Names, no duplicates, capacity holds N net-new, opstack has N values
// below the names-array) before any frame mutation, so an error leaves
// zero new bindings landed.  The names-array may be a literal Array or
// an eqarray reference (`[...]#=`); a stale eqarray raises /unsupported.
// Save journaling: N per-binding entries (same path as local-def), so
// save/restore rolls the whole batch back atomically.
// throws: type-check, range-check, limit-check, unsupported, read-only, invalid-name
static void bind_locals_op(Trix *trx) {
    trx->verify_operands(VerifyRWArray);

    auto names_obj = *trx->m_op_ptr;
    auto [names_base, names_length] = names_obj.array_value(trx);

    if (names_length == 0) {
        trx->error(Error::Unsupported, "bind-locals: names-array must be non-empty");
    } else {
        auto dict = trx->m_dict_ptr->dict_value(trx);
        if (!dict->is_frame()) {
            trx->error(Error::Unsupported, "bind-locals: requires a |...| frame on the dict stack");
        } else if (!dict->has_write_access()) {
            trx->error(Error::ReadOnly, "attempt to update a ReadOnly dict");
        } else {
            trx->require_op_count(static_cast<integer_t>(names_length) + 1);

            for (length_t i = 0; i < names_length; ++i) {
                if (!names_base[i].is_name()) {
                    trx->error(Error::TypeCheck, "bind-locals: names-array element {} is not a Name", i);
                } else {
                    reject_reserved_name(trx, &names_base[i]);
                    for (length_t j = 0; j < i; ++j) {
                        if (names_base[i].name_offset() == names_base[j].name_offset()) {
                            trx->error(Error::Unsupported,
                                       "bind-locals: duplicate name '{}' at index {}",
                                       names_base[i].name_sv(trx),
                                       i);
                        }
                    }
                }
            }

            // Frame dicts are fixed-capacity; count net-new keys (those not
            // already in the frame) so re-binding existing names in a loop body
            // does not falsely overflow.
            auto net_new = length_t{0};
            for (length_t i = 0; i < names_length; ++i) {
                if (dict->get(trx, names_base[i]) == nullptr) {
                    ++net_new;
                }
            }
            if ((dict->length() + net_new) > dict->maxlength()) {
                trx->error(Error::LimitCheck,
                           "bind-locals: net-new names {} would exceed frame capacity {} (current length {})",
                           net_new,
                           dict->maxlength(),
                           dict->length());
            }

            // Names carry no ExtValue, so passing names_base[i] by value is safe.
            // Values are moved from the stack -- put() takes ownership of any
            // ExtValue/WideValue the value holds.
            auto values_base = (trx->m_op_ptr - names_length);
            for (length_t i = 0; i < names_length; ++i) {
                dict->put(trx, names_base[i], values_base[i], Dict::BindingMode::Bind);
            }

            trx->m_op_ptr = values_base - 1;
        }
    }
}

// bind-into-dict: v1 ... vN keys-array dict :- --
// Atomic batch binding into the dict on top of the operand stack.  Keys
// may be any non-Null type (matching def/put semantics).  Each value vi
// is moved under keys-array[i-1]; leftmost key gets the bottommost stack
// value.  Pre-validates type, dup (Object::equal), capacity (net-new
// count for fixed dicts), and stack arity before mutating.  An empty
// keys-array raises /unsupported.  Save journaling: N per-binding
// entries.  Keys are deep-cloned with make_clone so the source array's
// ExtValue/WideValue cells remain valid even when put() updates an
// existing entry.
// throws: type-check, range-check, limit-check, unsupported, read-only
static void bind_into_dict_op(Trix *trx) {
    trx->verify_operands(VerifyRWDict, VerifyRWArray);

    auto dict_obj = *trx->m_op_ptr;
    auto keys_obj = *(trx->m_op_ptr - 1);

    auto dict = dict_obj.dict_value(trx);
    auto [keys_base, keys_length] = keys_obj.array_value(trx);

    if (keys_length == 0) {
        trx->error(Error::Unsupported, "bind-into-dict: keys-array must be non-empty");
    } else {
        trx->require_op_count(static_cast<integer_t>(keys_length) + 2);

        for (length_t i = 0; i < keys_length; ++i) {
            if (keys_base[i].is_null()) {
                trx->error(Error::TypeCheck, "bind-into-dict: keys-array element {} is Null", i);
            } else {
                for (length_t j = 0; j < i; ++j) {
                    if (keys_base[i].equal(trx, keys_base[j])) {
                        trx->error(Error::Unsupported, "bind-into-dict: duplicate key at index {}", i);
                    }
                }
            }
        }

        // Capacity: only fixed-capacity dicts need an explicit pre-check;
        // dynamic dicts grow inside put().  Net-new count avoids rejecting
        // updates that overwrite existing entries without growing the dict.
        if (!dict->is_dynamic()) {
            auto net_new = length_t{0};
            for (length_t i = 0; i < keys_length; ++i) {
                if (dict->get(trx, keys_base[i]) == nullptr) {
                    ++net_new;
                }
            }
            if ((dict->length() + net_new) > dict->maxlength()) {
                trx->error(Error::LimitCheck,
                           "bind-into-dict: net-new entries {} would exceed dict capacity {} (current length {})",
                           net_new,
                           dict->maxlength(),
                           dict->length());
            }
        }

        auto values_base = (trx->m_op_ptr - 1 - keys_length);
        // NoBinding: the target dict is an OPERAND, not on the dict stack -- Bind would
        // cache these names to off-stack entries and shadow real dict-stack bindings on
        // the next lookup.  Clear instead (engine bug #21).
        for (length_t i = 0; i < keys_length; ++i) {
            dict->put(trx, keys_base[i].make_clone(trx), values_base[i], Dict::BindingMode::NoBinding);
        }

        trx->m_op_ptr = values_base - 1;
    }
}

// current-dict: :- dict
// Pushes the current writable dictionary -- the first non-frame dict
// from the top of the dict stack.  Skips |...| frames since `def` and
// `store` skip them too; current-dict matches their target.
// throws: opstack-overflow
static void currentdict_op(Trix *trx) {
    trx->require_op_capacity(1);

    auto dict_obj_ptr = Dict::dict_stack_first_nonframe(trx);
    *++trx->m_op_ptr = *dict_obj_ptr;
}

// int dict
// Routes through m_curr_alloc_global: when the global flag is set
// (e.g. inside ${...} or after `true set-global`), the Dict struct
// allocates in global VM and survives save/restore; otherwise local.
static void create_dict(Trix *trx, Object::DictMode mode) {
    trx->verify_operands(VerifyIntegers | VerifyNotNegative);

    auto int_ptr = trx->m_op_ptr;
    auto [valid, maxlength] = int_ptr->integer_value(trx, 0, MaxDictLength);
    if (valid) {
        int_ptr->maybe_free_extvalue(trx);

        auto [_, offset] = trx->m_curr_alloc_global ? Dict::create_global_dict(trx, static_cast<length_t>(maxlength), mode)
                                                    : Dict::create_dict(trx, static_cast<length_t>(maxlength), mode);
        *int_ptr = Object::make_dict(offset);
    } else {
        trx->error(Error::LimitCheck, "dict length {} exceeds maximum {}", maxlength, MaxDictLength);
    }
}

// dict: int :- dict
// Creates a fixed-size dictionary with the given capacity.  Routes
// to global VM when m_curr_alloc_global is set (e.g. inside ${...}
// or after `true set-global`).
// throws: vm-full
static void dict_op(Trix *trx) {
    create_dict(trx, Object::DictMode::ReadWriteFixed);
}

// dynamic-dict: int :- dict
// Creates a dynamic dictionary with the given initial capacity.
// Routes to global VM when m_curr_alloc_global is set.
// throws: vm-full
static void dynamic_dict_op(Trix *trx) {
    create_dict(trx, Object::DictMode::ReadWriteDynamic);
}

// dictstack: arr :- arr
// Copies the dict stack into arr.
// throws: opstack-underflow, range-check, type-check
static void dictstack_op(Trix *trx) {
    trx->verify_operands(VerifyRWArray);

    auto first = trx->m_op_ptr;
    auto count = static_cast<stack_depth_t>((trx->m_dict_ptr - trx->m_dict_base) + 1);
    auto [ptr, length] = first->array_value(trx);
    if (count <= length) {
        auto curr_save_level = trx->m_curr_save_level;
        auto src = trx->m_dict_base;
        auto dst = ptr;
        for (auto i = count; i != 0; --i, ++src, ++dst) {
            if (dst->save_level() == curr_save_level) {
                dst->maybe_free_extvalue(trx);
            } else {
                Save::save_object(trx, dst);
            }
            *dst = *src;
            dst->set_save_level(curr_save_level);
        }
        first->set_array_length(count);
    } else {
        trx->error(Error::RangeCheck, "dictstack: array capacity {} < dict stack depth {}", length, count);
    }
}

// end: :- --
// Pops the top dictionary from the dict stack.  If the top is a |...|
// frame dict, raises Unsupported -- frames are owned by their proc
// invocation and popped automatically on proc return; user code must
// not pop them.
// throws: dictstack-underflow, unsupported
static void end_op(Trix *trx) {
    if (trx->m_dict_ptr > (trx->m_dict_base + PermanentDictCount - 1)) {
        auto dict = trx->m_dict_ptr->dict_value(trx);
        if (dict->is_frame()) {
            trx->error(Error::Unsupported, "end: cannot pop a locals frame; this dict was pushed by a |...| proc");
        } else {
            --trx->m_dict_ptr;
            dict->clear_name_bindings(trx);
        }
    } else {
        trx->error(Error::DictStackUnderflow, "cannot pop permanent dicts from Dictionary Stack");
    }
}

// Common implementation for eq and ne.
static void compare_eq_impl(Trix *trx, bool negate) {
    trx->require_op_count(2);

    auto y_ptr = trx->m_op_ptr--;
    auto x_ptr = trx->m_op_ptr;
    auto result = x_ptr->equal(trx, *y_ptr) != negate;
    x_ptr->maybe_free_extvalue(trx);
    y_ptr->maybe_free_extvalue(trx);
    *x_ptr = Object::make_boolean(result);
}

// known?: dict key :- bool
// Tests whether key exists in dict.
// throws: opstack-underflow, type-check
static void known_op(Trix *trx) {
    trx->verify_operands(VerifyKey, VerifyDict);

    auto key_ptr = trx->m_op_ptr;
    auto dict_ptr = (key_ptr - 1);
    auto dict = dict_ptr->dict_value(trx);
    auto is_known = (dict->get(trx, key_ptr) != nullptr);
    key_ptr->maybe_free_extvalue(trx);
    *dict_ptr = Object::make_boolean(is_known);
    trx->m_op_ptr = dict_ptr;
}

// knownget: dict key :- any true | false
// Gets value for key if present, else pushes false.
// throws: vm-full, opstack-underflow, type-check
static void knownget_op(Trix *trx) {
    trx->verify_operands(VerifyKey, VerifyDict);

    auto key_ptr = trx->m_op_ptr;
    auto dict_ptr = (key_ptr - 1);
    auto dict = dict_ptr->dict_value(trx);
    auto value = dict->get(trx, key_ptr);
    auto result = (value != nullptr);
    key_ptr->maybe_free_extvalue(trx);
    if (result) {
        *dict_ptr = value->make_clone(trx);
    } else {
        trx->m_op_ptr = dict_ptr;
    }
    *trx->m_op_ptr = Object::make_boolean(result);
}

// getdefault: dict key default :- value
// Returns value for key if found in dict, otherwise returns default.
// throws: opstack-underflow, type-check
static void getdefault_op(Trix *trx) {
    trx->verify_operands(VerifyAny, VerifyKey, VerifyDict);

    auto default_ptr = trx->m_op_ptr;
    auto key_ptr = (default_ptr - 1);
    auto dict_ptr = (default_ptr - 2);

    auto dict = dict_ptr->dict_value(trx);
    auto value = dict->get(trx, key_ptr);
    key_ptr->maybe_free_extvalue(trx);
    if (value != nullptr) {
        *dict_ptr = value->make_clone(trx);
        default_ptr->maybe_free_extvalue(trx);
    } else {
        *dict_ptr = *default_ptr;
    }
    trx->m_op_ptr = dict_ptr;
}

// load: key :- any
// Looks up key in the dict stack and pushes its value.
// throws: vm-full, opstack-underflow, type-check, undefined
static void load_op(Trix *trx) {
    trx->verify_operands(VerifyKey);

    auto key_ptr = trx->m_op_ptr;
    const Object *value{nullptr};
    if (key_ptr->is_name()) {
        value = Name::name_search(trx, key_ptr);
    } else {
        std::tie(std::ignore, value) = Dict::key_lookup(trx, key_ptr);
    }
    if (value != nullptr) {
        key_ptr->maybe_free_extvalue(trx);
        *key_ptr = value->make_clone(trx);
    } else if (key_ptr->is_name()) {
        trx->error(Error::Undefined, "load: {} not found in dict stack", key_ptr->name_sv(trx));
    } else {
        trx->error(Error::Undefined, "load: key not present in dict");
    }
}

// maxlength: dict :- int
// Returns the maximum capacity of the dictionary.
// throws: opstack-underflow, type-check
static void maxlength_op(Trix *trx) {
    trx->verify_operands(VerifyDict);

    auto top_ptr = trx->m_op_ptr;
    auto dict = top_ptr->dict_value(trx);
    *top_ptr = Object::make_integer(dict->maxlength());
}

// store: key value :- --
// Stores value under key in the first writable dict that contains
// key.  If key is not present anywhere on the dict stack, falls back
// to the current dict -- the first non-frame from the top, matching
// `def`'s target.  Existing-binding hits inside a |...| frame are
// honored (they reflect explicit local-def intent and `store` is the
// idiomatic way to update a local in place).
// throws: opstack-underflow, type-check
static void store_op(Trix *trx) {
    trx->verify_operands(VerifyAny, VerifyKey);

    auto val_ptr = trx->m_op_ptr;
    auto key_ptr = (val_ptr - 1);
    auto [dict_ptr, _] = Dict::key_lookup(trx, key_ptr);
    // Fallback to first non-frame on dict stack when key not found anywhere.
    auto dict = (dict_ptr != nullptr) ? dict_ptr->dict_value(trx) : Dict::dict_stack_first_nonframe(trx)->dict_value(trx);
    if (dict->has_write_access()) {
        dict->put(trx, *key_ptr, *val_ptr, Dict::BindingMode::Bind);
        trx->m_op_ptr -= 2;
    } else {
        trx->error(Error::ReadOnly, "attempt to update a ReadOnly dict");
    }
}

// update: dict key proc :- --
// Looks up key in dict, executes proc with current value, stores result.
// throws: opstack-underflow, read-only, type-check, undefined
static void update_op(Trix *trx) {
    trx->verify_operands(VerifyProc, VerifyKey, VerifyRWDict);

    auto proc_ptr = trx->m_op_ptr;
    auto key_ptr = (proc_ptr - 1);
    auto dict_ptr = (proc_ptr - 2);

    auto dict = dict_ptr->dict_value(trx);
    auto value = dict->get(trx, key_ptr);
    if (value == nullptr) {
        trx->error(Error::Undefined, "update: key not found in dict");
    } else {
        auto current_value = value->make_clone(trx);

        trx->require_exec_capacity(4);
        *++trx->m_exec_ptr = *dict_ptr;
        *++trx->m_exec_ptr = *key_ptr;
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atUpdate);
        *++trx->m_exec_ptr = *proc_ptr;

        trx->m_op_ptr -= 3;
        trx->require_op_capacity(1);
        *++trx->m_op_ptr = current_value;
    }
}

// update-persist: dict key proc :- --
// Same shape as update, but the post-proc store is NOT journaled.  At
// sl=0 silently degrades to update.  At sl > 0 enforces the -persist
// ref-check: the value produced by proc is checked at write time and
// rejected with /above-barrier if it would dangle on restore.  Missing
// key raises /undefined as in update (no -persist nuance there).
// throws: above-barrier, opstack-underflow, read-only, type-check, undefined
static void update_persist_op(Trix *trx) {
    trx->verify_operands(VerifyProc, VerifyKey, VerifyRWDict);

    auto proc_ptr = trx->m_op_ptr;
    auto key_ptr = (proc_ptr - 1);
    auto dict_ptr = (proc_ptr - 2);

    if (Save::is_active(trx) && Save::is_above_barrier(trx, *key_ptr)) {
        trx->error(Error::AboveBarrier, "update-persist: key lives above the save barrier");
    } else {
        auto dict = dict_ptr->dict_value(trx);
        auto value = dict->get(trx, key_ptr);
        if (value == nullptr) {
            trx->error(Error::Undefined, "update-persist: key not found in dict");
        } else {
            auto current_value = value->make_clone(trx);

            trx->require_exec_capacity(4);
            *++trx->m_exec_ptr = *dict_ptr;
            *++trx->m_exec_ptr = *key_ptr;
            *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atUpdatePersist);
            *++trx->m_exec_ptr = *proc_ptr;

            trx->m_op_ptr -= 3;
            trx->require_op_capacity(1);
            *++trx->m_op_ptr = current_value;
        }
    }
}

// @update-persist: continuation for update-persist -- stores proc result
// via Dict::put_persist (no journal) instead of put.  Errors with
// /above-barrier if the proc's result lives above the barrier.
// throws: above-barrier, internal-error
static void at_update_persist_op(Trix *trx) {
    auto key_ptr = trx->m_exec_ptr;
    auto dict_ptr = (key_ptr - 1);

    trx->require_op_count(1);
    auto val_ptr = trx->m_op_ptr;
    auto dict = dict_ptr->dict_value(trx);

    if (Save::is_active(trx)) {
        if (Save::is_above_barrier(trx, *val_ptr)) {
            trx->error(Error::AboveBarrier, "update-persist: proc result lives above the save barrier and would dangle on restore");
        } else if (dict->put_persist(trx, *key_ptr, *val_ptr) == nullptr) {
            // The key was found at update-persist entry; reaching nullptr
            // here means the proc undef'd it concurrently.  Treat as the
            // missing-key footgun and reject.
            trx->error(Error::AboveBarrier, "update-persist: key removed during proc execution");
        }
    } else {
        dict->put(trx, *key_ptr, *val_ptr);
    }
    --trx->m_op_ptr;
    trx->m_exec_ptr -= 2;
}

// @update: continuation for update -- stores proc result back into dict.
// exec stack: [dict] [key] (already popped: [@update])
// operand stack top: new-value from proc
static void at_update_op(Trix *trx) {
    auto key_ptr = trx->m_exec_ptr;
    auto dict_ptr = (key_ptr - 1);

    trx->require_op_count(1);
    auto dict = dict_ptr->dict_value(trx);
    dict->put(trx, *key_ptr, *trx->m_op_ptr);
    --trx->m_op_ptr;
    trx->m_exec_ptr -= 2;
}

// undef: dict key :- --
// Removes key from dict.
// throws: opstack-underflow, type-check
static void undef_op(Trix *trx) {
    trx->verify_operands(VerifyKey, VerifyRWDict);

    auto key_ptr = trx->m_op_ptr;
    auto dict_ptr = (key_ptr - 1);
    auto dict = dict_ptr->dict_value(trx);
    dict->undef(trx, key_ptr);
    key_ptr->maybe_free_extvalue(trx);
    trx->m_op_ptr -= 2;
}
// undef-persist: dict key :- --
// Same shape as undef, but the removal is NOT journaled -- it persists
// across the enclosing save/restore.  At sl=0 silently degrades to
// undef.  No allocation, so no above-barrier hazard on the dict
// mutations themselves; missing key is silently a no-op (matching
// undef).
// throws: opstack-underflow, type-check
static void undef_persist_op(Trix *trx) {
    trx->verify_operands(VerifyKey, VerifyRWDict);

    auto key_ptr = trx->m_op_ptr;
    auto dict_ptr = (key_ptr - 1);
    auto dict = dict_ptr->dict_value(trx);
    if (Save::is_active(trx)) {
        dict->undef_persist(trx, key_ptr);
    } else {
        dict->undef(trx, key_ptr);
    }
    key_ptr->maybe_free_extvalue(trx);
    trx->m_op_ptr -= 2;
}

// where: key :- dict true | false
// Searches the dict stack for key.
// throws: opstack-overflow, opstack-underflow, type-check
static void where_op(Trix *trx) {
    trx->verify_operands(VerifyKey);

    auto key_obj = *trx->m_op_ptr;
    auto [dict_ptr, _] = Dict::key_lookup(trx, &key_obj);
    auto result = (dict_ptr != nullptr);
    if (result) {
        trx->require_op_capacity(1);

        *trx->m_op_ptr++ = *dict_ptr;
    }
    key_obj.maybe_free_extvalue(trx);

    *trx->m_op_ptr = Object::make_boolean(result);
}
