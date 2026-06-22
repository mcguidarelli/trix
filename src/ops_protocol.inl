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

//===--- Protocol Operators ---===//
//
// Implements type-dispatched polymorphism (protocols), inspired by Clojure's
// protocols and Elixir's behaviours.  A protocol defines a set of named
// methods; types opt in by registering implementations via def-method.
//
// Architecture:
//
//   m_protocol_registry_offset -> Dict (protocol-name -> protocol-dict)
//
//   Each protocol-dict maps method-name -> dispatch-dict.
//   Each dispatch-dict maps type-name -> implementation proc.
//
//   When a method is called, the dispatch procedure:
//     1. Gets the argument's type name
//     2. Looks up in the method's dispatch dict
//     3. Falls back to /default if no type-specific impl
//     4. Raises Error::UndefinedCase (via type-case) if no handler and no /default
//
// Method names are globally unique across all protocols (error on collision).
// Each method name is bound in protocoldict (the dict-stack level between
// systemdict and userdict) to an auto-generated dispatch proc
// that performs the type lookup and execution.
//
//   def-protocol       method-names protocol-name --
//   def-method         proc method-name type-name --
//   def-default-method proc method-name --
//   extend-protocol    impl-dict type-name protocol-name --
//   protocol-satisfies? value protocol-name -- bool
//   protocol-methods   protocol-name -- name-array

// protocol_find_dispatch_dict: look up the dispatch dict for a method name.
// Returns nullptr if the method name is not registered in any protocol.
[[nodiscard]] Dict *protocol_find_dispatch_dict(const Object *method_name_ptr) {
    auto registry = offset_to_ptr<Dict>(m_protocol_registry_offset);

    // Walk all protocols looking for one that owns this method name
    auto method_hash = method_name_ptr->hash(this);
    auto entry_offset = nulloffset;
    integer_t bucket_idx = -1;
    while (true) {
        auto [next_offset, next_idx, key, value] = registry->next(this, entry_offset, bucket_idx);
        if (next_offset == nulloffset) {
            return nullptr;
        } else {
            // value is a protocol-dict (method-name -> dispatch-dict)
            auto protocol_dict = value.dict_value(this);
            auto method_entry = protocol_dict->find_dict_entry(this, *method_name_ptr, method_hash);
            if (method_entry == nullptr) {
                entry_offset = next_offset;
                bucket_idx = next_idx;
            } else {
                return method_entry->m_value.dict_value(this);
            }
        }
    }
}

// protocol_make_dispatch_proc: build a 3-element executable array that performs
// type-based dispatch: { dup <dispatch-dict> type-case }
// dup preserves the value; type-case consumes the copy + dict, dispatches to
// handler which receives the original value on the operand stack.
[[nodiscard]] Object protocol_make_dispatch_proc(vm_offset_t dispatch_dict_offset) {
    auto [elements, array_offset] = vm_alloc_n<Object>(3);

    elements[0] = Object::make_operator(SystemName::Dup);
    elements[1] = Object::make_dict(dispatch_dict_offset);
    elements[2] = Object::make_operator(SystemName::TypeCase);

    return Object::make_array(array_offset, 3, Object::ExecutableAttrib, Object::ReadOnlyAccess);
}

// def-protocol: method-names protocol-name --
// Creates a new protocol, registering method dispatch stubs in protocoldict.
// method-names is an array of literal Name objects.
// protocol-name is a literal Name.
// Errors if any method name is already claimed by another protocol.
// throws: dict-full, opstack-underflow, protocol, range-check, type-check, vm-full
static void def_protocol_op(Trix *trx) {
    trx->verify_operands(VerifyName, VerifyArray);

    auto protocol_name_obj = trx->m_op_ptr[0];
    auto methods_array_obj = trx->m_op_ptr[-1];

    auto [methods_ptr, methods_length] = methods_array_obj.array_value(trx);

    if (methods_length == 0) {
        trx->error(Error::RangeCheck, "def-protocol: method-names array must not be empty");
    } else {
        // Verify all elements are literal names and none are already claimed
        for (length_t i = 0; i < methods_length; ++i) {
            if (!methods_ptr[i].is_name()) {
                trx->error(Error::TypeCheck, "def-protocol: method-names must contain only names");
            } else if (trx->protocol_find_dispatch_dict(&methods_ptr[i]) != nullptr) {
                auto method_sv = methods_ptr[i].name_sv(trx);
                trx->error(Error::Protocol, "def-protocol: method /{} is already claimed by another protocol", method_sv);
            }
        }

        // Check that the protocol name is not already registered
        auto registry = trx->offset_to_ptr<Dict>(trx->m_protocol_registry_offset);
        if (registry->get(trx, protocol_name_obj) != nullptr) {
            auto name_sv = protocol_name_obj.name_sv(trx);
            trx->error(Error::Protocol, "def-protocol: protocol /{} already exists", name_sv);
        } else {
            // Create the protocol dict: maps method-name -> dispatch-dict
            auto [protocol_dict, protocol_dict_offset] = Dict::create_dict(trx, methods_length, Object::DictMode::ReadWriteFixed);

            for (length_t i = 0; i < methods_length; ++i) {
                // Create an empty dispatch dict for each method (type-name -> proc)
                auto [_, dispatch_dict_offset] =
                        Dict::create_dict(trx, DefaultInternalDictCapacity, Object::DictMode::ReadWriteDynamic);

                // Store dispatch dict in protocol dict under the method name
                auto method_name_obj = methods_ptr[i].make_clone(trx);
                static_cast<void>(protocol_dict->put(trx, method_name_obj, Object::make_dict(dispatch_dict_offset)));

                // Build the dispatch procedure and bind the method name in protocoldict
                auto dispatch_proc_obj = trx->protocol_make_dispatch_proc(dispatch_dict_offset);
                static_cast<void>(trx->m_protocoldict->put(trx, method_name_obj.make_clone(trx), dispatch_proc_obj));
            }

            // Register the protocol in the registry
            static_cast<void>(registry->put(trx, protocol_name_obj.make_clone(trx), Object::make_dict(protocol_dict_offset)));

            trx->m_op_ptr -= 2;
        }
    }
}

// protocol_lookup_method_dispatch: find the dispatch dict for a method name,
// verifying it belongs to an existing protocol.  Raises Error::Protocol if not found.
[[nodiscard]] Dict *protocol_lookup_method_dispatch(const Object *method_name_ptr) {
    auto dispatch_dict = protocol_find_dispatch_dict(method_name_ptr);
    if (dispatch_dict == nullptr) {
        auto method_sv = method_name_ptr->name_sv(this);
        error(Error::Protocol, "method /{} is not part of any protocol", method_sv);
    } else {
        return dispatch_dict;
    }
}

// def-method: proc method-name type-name --
// Registers an implementation of a protocol method for a specific type.
// throws: dict-full, opstack-underflow, protocol, type-check, vm-full
static void def_method_op(Trix *trx) {
    trx->verify_operands(VerifyName, VerifyName, VerifyProc);

    auto type_name_obj = *trx->m_op_ptr;
    auto method_name_obj = *(trx->m_op_ptr - 1);
    auto proc_obj = *(trx->m_op_ptr - 2);

    // Verify type-name is a valid type name
    auto [is_valid_type, _] = trx->is_type_name(&type_name_obj);
    if (is_valid_type) {
        auto dispatch_dict = trx->protocol_lookup_method_dispatch(&method_name_obj);
        static_cast<void>(dispatch_dict->put(trx, type_name_obj.make_clone(trx), proc_obj.make_clone(trx)));

        trx->m_op_ptr -= 3;
    } else {
        auto type_sv = type_name_obj.name_sv(trx);
        trx->error(Error::TypeCheck, "def-method: /{} is not a valid type name", type_sv);
    }
}

// def-default-method: proc method-name --
// Registers a default (fallback) implementation for a protocol method.
// Used when no type-specific implementation is found.
// throws: dict-full, opstack-underflow, protocol, type-check, vm-full
static void def_default_method_op(Trix *trx) {
    trx->verify_operands(VerifyName, VerifyProc);

    auto method_name_obj = trx->m_op_ptr[0];
    auto proc_obj = trx->m_op_ptr[-1];

    auto dispatch_dict = trx->protocol_lookup_method_dispatch(&method_name_obj);
    auto default_name_obj = trx->wellknown_name(WellKnownName::Default);
    static_cast<void>(dispatch_dict->put(trx, default_name_obj, proc_obj.make_clone(trx)));

    trx->m_op_ptr -= 2;
}

// extend-protocol: impl-dict type-name protocol-name --
// Batch-registers all method implementations for a type on a protocol.
// impl-dict maps method-name -> proc for each method to implement.
// throws: dict-full, opstack-underflow, protocol, type-check, vm-full
static void extend_protocol_op(Trix *trx) {
    trx->verify_operands(VerifyName, VerifyName, VerifyDict);

    auto protocol_name_obj = *trx->m_op_ptr;
    auto type_name_obj = *(trx->m_op_ptr - 1);
    auto impl_dict_obj = *(trx->m_op_ptr - 2);

    // Verify type-name
    auto [is_valid_type, _] = trx->is_type_name(&type_name_obj);
    if (!is_valid_type) {
        auto type_sv = type_name_obj.name_sv(trx);
        trx->error(Error::TypeCheck, "extend-protocol: /{} is not a valid type name", type_sv);
    } else {
        // Look up the protocol in the registry
        auto registry = trx->offset_to_ptr<Dict>(trx->m_protocol_registry_offset);
        auto proto_ptr = registry->get(trx, protocol_name_obj);
        if (proto_ptr == nullptr) {
            auto name_sv = protocol_name_obj.name_sv(trx);
            trx->error(Error::Protocol, "extend-protocol: protocol /{} does not exist", name_sv);
        } else {
            auto protocol_dict = proto_ptr->dict_value(trx);
            auto impl_dict = impl_dict_obj.dict_value(trx);

            // Iterate impl_dict entries and register each in the appropriate dispatch dict
            auto entry_offset = nulloffset;
            integer_t bucket_idx = -1;
            while (true) {
                auto [next_offset, next_idx, key, value] = impl_dict->next(trx, entry_offset, bucket_idx);
                if (next_offset != nulloffset) {
                    // Find the dispatch dict for this method
                    auto method_hash = key.hash(trx);
                    auto method_entry = protocol_dict->find_dict_entry(trx, key, method_hash);
                    if (method_entry == nullptr) {
                        auto method_sv = key.name_sv(trx);
                        auto protocol_sv = protocol_name_obj.name_sv(trx);
                        trx->error(Error::Protocol, "extend-protocol: /{} is not a method of protocol /{}", method_sv, protocol_sv);
                    } else if (!value.is_executable() || !value.is_sequence()) {
                        // Validate that the implementation value is an executable procedure
                        auto method_sv = key.name_sv(trx);
                        trx->error(Error::TypeCheck, "extend-protocol: value for /{} must be an executable procedure", method_sv);
                    } else {
                        auto dispatch_dict = method_entry->m_value.dict_value(trx);
                        static_cast<void>(dispatch_dict->put(trx, type_name_obj.make_clone(trx), value.make_clone(trx)));

                        entry_offset = next_offset;
                        bucket_idx = next_idx;
                    }
                } else {
                    break;
                }
            }
            trx->m_op_ptr -= 3;
        }
    }
}

// protocol-satisfies?: value protocol-name -- bool
// Tests whether the value's type has implementations for all methods of the protocol.
// throws: opstack-underflow, protocol, type-check
static void protocol_satisfies_op(Trix *trx) {
    trx->verify_operands(VerifyName, VerifyAny);

    auto protocol_name_obj = *trx->m_op_ptr;
    auto value_obj = *(trx->m_op_ptr - 1);

    // Look up protocol
    auto registry = trx->offset_to_ptr<Dict>(trx->m_protocol_registry_offset);
    auto proto_ptr = registry->get(trx, protocol_name_obj);
    if (proto_ptr == nullptr) {
        auto name_sv = protocol_name_obj.name_sv(trx);
        trx->error(Error::Protocol, "protocol-satisfies?: protocol /{} does not exist", name_sv);
    } else {
        auto protocol_dict = proto_ptr->dict_value(trx);
        auto type_name_obj = trx->type_name(value_obj.type());
        auto type_hash = type_name_obj.hash(trx);
        auto satisfied = true;

        // Check each method's dispatch dict for a type-specific or default impl
        auto entry_offset = nulloffset;
        integer_t bucket_idx = -1;
        while (true) {
            auto [next_offset, next_idx, key, value] = protocol_dict->next(trx, entry_offset, bucket_idx);
            if (next_offset != nulloffset) {
                auto dispatch_dict = value.dict_value(trx);
                if (dispatch_dict->find_dict_entry(trx, type_name_obj, type_hash) == nullptr) {
                    // No type-specific impl -- check for /default
                    auto default_name_obj = trx->wellknown_name(WellKnownName::Default);
                    auto default_hash = default_name_obj.hash(trx);
                    if (dispatch_dict->find_dict_entry(trx, default_name_obj, default_hash) == nullptr) {
                        satisfied = false;
                        break;
                    }
                }
                entry_offset = next_offset;
                bucket_idx = next_idx;
            } else {
                break;
            }
        }

        value_obj.maybe_free_extvalue(trx);
        --trx->m_op_ptr;
        *trx->m_op_ptr = Object::make_boolean(satisfied);
    }
}

// protocol-methods: protocol-name -- name-array
// Returns an array of method names defined by the protocol.
// throws: opstack-underflow, protocol, type-check, vm-full
static void protocol_methods_op(Trix *trx) {
    trx->verify_operands(VerifyName);

    auto protocol_name_obj = *trx->m_op_ptr;

    // Look up protocol
    auto registry = trx->offset_to_ptr<Dict>(trx->m_protocol_registry_offset);
    auto proto_ptr = registry->get(trx, protocol_name_obj);
    if (proto_ptr == nullptr) {
        auto name_sv = protocol_name_obj.name_sv(trx);
        trx->error(Error::Protocol, "protocol-methods: protocol /{} does not exist", name_sv);
    } else {
        auto protocol_dict = proto_ptr->dict_value(trx);
        auto count = protocol_dict->length();

        // Allocate result array and fill with method names.  Region-aware so the
        // array survives save/restore under ${...}; no GC-rooting is needed across
        // the fill because def-protocol enforces method keys are Names, and a Name
        // uses no ExtValue/WideValue -- make_clone is a shallow copy that never
        // allocates, so no vm_global_gc fires before the result reaches the stack.
        auto [elem_ptr, array_offset] = trx->vm_alloc_dispatch_n<Object>(count, ChunkKind::Array);
        length_t idx = 0;
        auto entry_offset = nulloffset;
        integer_t bucket_idx = -1;
        while (true) {
            auto [next_offset, next_idx, key, value] = protocol_dict->next(trx, entry_offset, bucket_idx);
            if (next_offset != nulloffset) {
                elem_ptr[idx++] = key.make_clone(trx);
                entry_offset = next_offset;
                bucket_idx = next_idx;
            } else {
                break;
            }
        }
        *trx->m_op_ptr = Object::make_array(array_offset, count, Object::LiteralAttrib, Object::ReadOnlyAccess);
    }
}
