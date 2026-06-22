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

//===--- Tagged Value Operators ---===//
//
// Implements discriminated unions (tagged unions / sum types / variant types)
// using a name tag paired with a payload value.  Based on:
//
//   ML/Haskell algebraic data types: constructors that wrap values with a
//   tag identifying the variant (e.g., Some(x) vs None, Ok(v) vs Err(e)).
//
//   Erlang tagged tuples: the convention of {:ok, value} and {:error, reason}
//   for result types, and {:tag, ...} for message dispatch in actors.
//
// Tagged values in Trix serve the same role: they distinguish between
// different "kinds" of a value at runtime, enabling pattern-based dispatch
// without inheritance or type hierarchies.
//
// --- Core concepts for maintainers ---
//
// TAGGED VALUES
//   A tagged value pairs a Name (the tag) with any Object (the payload).
//   Created with `tag` (e.g., `42 /ok tag` creates a tagged value with
//   tag /ok and payload 42).
//
//   Representation: 2 Objects stored contiguously on the VM heap:
//     [0] = Name object (the tag, always literal)
//     [1] = Object (the payload, any type including another tagged value)
//   The tagged Object itself holds a vm_offset_t pointing to this pair.
//
// USE CASES
//   Result types:     `value /ok tag` and `reason /error tag`
//   Option types:     `value /some tag` and `null /none tag`
//   Message types:    `payload /request tag` for actor dispatch
//   Enum variants:    `/red tag`, `/green tag`, `/blue tag` (null payload)
//   Logic variables:  `[null] /lvar tag` (used by ops_logic.inl)
//   Error results:    `{ ... } try-result` wraps the proc's result as `/ok`
//                     tagged and any error name as `/err` tagged
//
// PATTERN MATCHING
//   `untag` extracts the tag name and payload.
//   `tag-name` / `tag-value` access individual components.
//   `tag-match` dispatches on the tag using a dict of {name: handler} pairs:
//     << /ok { (success: ) print = }
//        /error { (error: ) print = }
//     >> tag-match
//   If the tag is not found and a /default handler exists, it is called.
//   Otherwise, an error is raised.
//
// TAGGED VALUE EQUALITY
//   Two tagged values are equal (via `eq` or `equal`) if their tag names
//   are the same Name and their payloads are equal.  This is shallow
//   equality; use `deep-eq` for recursive structural comparison.
//
// OWNERSHIP
//   `tag` transfers ownership of both the name and payload Objects from
//   the operand stack into the VM heap storage (same move semantics as
//   curry).  `untag` clones the tag and payload back onto the operand stack
//   (as do `tag-name` / `tag-value`); the tagged value's heap pair is left
//   in place, reclaimed only by save/restore.
//
// --- Operators ---
//
//   tag           value name -- tagged          Create a tagged value
//   untag         tagged -- value name          Extract tag and payload
//   tag-name      tagged -- name                Extract tag name only
//   tag-value     tagged -- value               Extract payload only
//   tag-value-or  tagged name default -- value  Payload if tag matches, else default
//   is-tagged     any -- bool                   Type test
//   tag?          tagged name -- bool           Test whether the tag name matches
//   tag-match     tagged dict --                Dispatch on tag name
//   tag-update    tagged proc -- tagged'        Rebuild with proc applied to the payload
//   tag-bind      tagged name proc -- tagged    Apply proc to payload when tag matches
//   try           proc -- name                  Execute; push /no-error or /error-name (ops_flow.inl)
//   try-catch     dict proc --                  Execute proc; dispatch /error-name to dict handler (ops_flow.inl)
//   try-result    proc -- tagged                Execute; wrap result /ok, error name /err (ops_higher.inl)
//
// Note: `try-result` (in ops_higher.inl) produces tagged results: the
// proc's result wrapped as /ok on success, or the error name wrapped as
// /err on failure.  `try`/`try-catch` (ops_flow.inl) do NOT produce tagged
// values -- `try` pushes /no-error or /error-name, `try-catch` dispatches
// to a handler dict.
//

// tag: value name :- tagged
// Creates a tagged value pairing a name tag with a payload value.
// Ownership: the two operands are transferred from the operand stack into the
// tagged's VM storage (same pattern as curry).
// throws: opstack-underflow, type-check, vm-full
static void tag_op(Trix *trx) {
    trx->verify_operands(VerifyName, VerifyAny);

    auto name_obj = *trx->m_op_ptr--;
    auto value_obj = *trx->m_op_ptr;

    // Transfer value ownership from stack to tagged
    *trx->m_op_ptr = Object::make_tagged(trx, name_obj, value_obj);
}

// untag: tagged :- value name
// Decomposes a tagged value into its payload and tag name.
// throws: opstack-underflow, type-check
static void untag_op(Trix *trx) {
    trx->verify_operands(VerifyTagged);
    trx->require_op_capacity(1);

    auto tagged_ptr = trx->m_op_ptr;
    auto offset = tagged_ptr->tagged_offset();
    auto pair = trx->offset_to_ptr<Object>(offset);

    *tagged_ptr = pair[Object::TaggedValueIndex].make_clone(trx);
    *++tagged_ptr = pair[Object::TaggedNameIndex].make_clone(trx);
    trx->m_op_ptr = tagged_ptr;
}

// tag-name: tagged :- name
// Extracts the tag name from a tagged value.
// throws: opstack-underflow, type-check
static void tag_name_op(Trix *trx) {
    trx->verify_operands(VerifyTagged);

    auto offset = trx->m_op_ptr->tagged_offset();
    auto pair = trx->offset_to_ptr<Object>(offset);
    *trx->m_op_ptr = pair[Object::TaggedNameIndex].make_clone(trx);
}

// tag-value: tagged :- value
// Extracts the payload from a tagged value.
// throws: opstack-underflow, type-check
static void tag_value_op(Trix *trx) {
    trx->verify_operands(VerifyTagged);

    auto offset = trx->m_op_ptr->tagged_offset();
    auto pair = trx->offset_to_ptr<Object>(offset);
    *trx->m_op_ptr = pair[Object::TaggedValueIndex].make_clone(trx);
}

// tag?: tagged name :- bool
// Tests whether the tagged value's tag matches the given name.
// throws: opstack-underflow, type-check
static void tag_pred_op(Trix *trx) {
    trx->verify_operands(VerifyName, VerifyTagged);

    auto name_ptr = trx->m_op_ptr;
    auto tagged_ptr = (name_ptr - 1);
    auto offset = tagged_ptr->tagged_offset();
    auto pair = trx->offset_to_ptr<Object>(offset);

    --trx->m_op_ptr;
    *trx->m_op_ptr = Object::make_boolean(pair[Object::TaggedNameIndex].equal(trx, *name_ptr));
}

// tag-match: tagged dict :- ...
// Dispatches on the tag name of a tagged value.
// Looks up the tag name in the dict:
//   Found: pushes tag-value, executes the associated proc
//   Not found: tries /default key -- pushes full tagged value and executes
//   Neither: throws undefined error
// throws: opstack-underflow, type-check, undefined
static void tag_match_op(Trix *trx) {
    trx->verify_operands(VerifyDict, VerifyTagged);

    auto dict_obj = trx->m_op_ptr[0];
    auto tagged_ptr = (trx->m_op_ptr - 1);
    auto offset = tagged_ptr->tagged_offset();
    auto pair = trx->offset_to_ptr<Object>(offset);
    auto tag_name_obj = pair[Object::TaggedNameIndex];

    // Look up tag name in dispatch dict
    auto dict = dict_obj.dict_value(trx);
    auto entry = dict->find_dict_entry(trx, tag_name_obj, tag_name_obj.hash(trx));

    if (entry != nullptr) {
        // Matched: push tag-value, execute proc
        --trx->m_op_ptr;
        *trx->m_op_ptr = pair[Object::TaggedValueIndex].make_clone(trx);
        trx->execute_value(entry->m_value.make_clone(trx));
    } else {
        // Try /default
        auto default_name_obj = trx->wellknown_name(WellKnownName::Default);
        entry = dict->find_dict_entry(trx, default_name_obj, default_name_obj.hash(trx));

        if (entry != nullptr) {
            // use full tagged value for default handler
            --trx->m_op_ptr;
            trx->execute_value(entry->m_value.make_clone(trx));
        } else {
            trx->error(Error::Undefined, "tag-match: no handler for tag /{} and no /default", tag_name_obj.name_sv(trx));
        }
    }
}

// tag-update: tagged proc :- tagged'
// Applies proc to the payload of tagged, preserving the tag name.
// Returns a new tagged value with the same tag and the proc's result as payload.
// throws: opstack-underflow, type-check, vm-full
static void tag_update_op(Trix *trx) {
    trx->verify_operands(VerifyProc, VerifyTagged);
    trx->require_exec_capacity(3);

    auto tagged_ptr = trx->m_op_ptr - 1;
    auto offset = tagged_ptr->tagged_offset();
    auto pair = trx->offset_to_ptr<Object>(offset);

    // exec stack: [tag-name] [@tag-update-complete] [proc]
    // proc executes first (top), then @tag-update-complete re-tags the result
    *++trx->m_exec_ptr = pair[Object::TaggedNameIndex].make_clone(trx);
    *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atTagUpdateComplete);
    *++trx->m_exec_ptr = *trx->m_op_ptr;

    // push payload for proc to operate on
    --trx->m_op_ptr;
    *trx->m_op_ptr = pair[Object::TaggedValueIndex].make_clone(trx);
}

// @tag-update-complete: internal control operator
// Pops saved tag name from exec stack, takes proc result from op stack,
// wraps them into a new tagged value.
static void at_tag_update_complete_op(Trix *trx) {
    trx->require_op_count(1);

    // pop saved tag name from exec stack (slot below us, already decremented by interpreter)
    auto tag_name_obj = *trx->m_exec_ptr--;
    auto result_obj = *trx->m_op_ptr;
    // Transfer result ownership from stack to tagged
    *trx->m_op_ptr = Object::make_tagged(trx, tag_name_obj, result_obj);
}

// tag-value-or: tagged name default :- value
// If the tag name of tagged matches name, returns the payload.
// Otherwise returns default.
// throws: opstack-underflow, type-check
static void tag_value_or_op(Trix *trx) {
    trx->verify_operands(VerifyAny, VerifyName, VerifyTagged);

    auto default_ptr = trx->m_op_ptr;
    auto name_ptr = (default_ptr - 1);
    auto tagged_ptr = (name_ptr - 1);

    auto offset = tagged_ptr->tagged_offset();
    auto pair = trx->offset_to_ptr<Object>(offset);

    Object result_obj;
    if (pair[Object::TaggedNameIndex].equal(trx, *name_ptr)) {
        default_ptr->maybe_free_extvalue(trx);
        result_obj = pair[Object::TaggedValueIndex].make_clone(trx);
    } else {
        result_obj = *default_ptr;
    }
    trx->m_op_ptr -= 2;
    *trx->m_op_ptr = result_obj;
}

// tag-bind: tagged name proc :- tagged
// Monadic bind for tagged values.  If the tag name of tagged matches name,
// unwraps the payload and executes proc (which should return a tagged value).
// If the tag does not match, returns the original tagged unchanged (pass-through).
// throws: opstack-underflow, type-check
static void tag_bind_op(Trix *trx) {
    trx->verify_operands(VerifyProc, VerifyName, VerifyTagged);

    auto proc_ptr = trx->m_op_ptr;
    auto name_ptr = (proc_ptr - 1);
    auto tagged_ptr = (name_ptr - 1);

    auto offset = tagged_ptr->tagged_offset();
    auto pair = trx->offset_to_ptr<Object>(offset);

    if (pair[Object::TaggedNameIndex].equal(trx, *name_ptr)) {
        // Unwrap payload, execute proc
        auto proc_obj = *proc_ptr;
        auto val_obj = pair[Object::TaggedValueIndex].make_clone(trx);

        trx->m_op_ptr -= 2;
        *trx->m_op_ptr = val_obj;
        trx->execute_value(proc_obj);
    } else {
        // Pass through: pop proc and name, leave tagged unchanged
        trx->m_op_ptr -= 2;
    }
}
