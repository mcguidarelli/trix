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

//===--- Record Operators ---===//
//
// Implements immutable named-field composite values (records / structs /
// named tuples).  Based on:
//
//   ML/Haskell records: immutable aggregates with named fields, structural
//   equality, and functional update (creating a new record with one field
//   changed).
//
//   Erlang maps (since OTP 17): immutable key-value structures with
//   pattern matching and functional update syntax (Map#{key := value}).
//
//   Clojure records (defrecord): named-field types with structural equality,
//   merge, and select operations.
//
// Records differ from dicts: records have a fixed schema (field names and
// order defined at creation), are always immutable, and support O(1) field
// access by index.  Dicts are mutable hash tables with dynamic keys.
//
// --- Core concepts for maintainers ---
//
// SCHEMAS AND INSTANCES
//   A record has two parts, both on the VM heap:
//
//   RecordSchema: defines the field names, shared across all instances of
//     the same record type.
//     Layout: [field_count (length_t)] [pad (uint16_t)] [Object[0]] ... [Object[N-1]]
//     Each Object holds a Name (the field name for that position).
//
//   RecordInstance: holds one record's field values, with a pointer back
//     to its schema.
//     Layout: [schema_offset (vm_offset_t)] [Object[0]] ... [Object[N-1]]
//
//   Multiple records with the same field names may have different schema
//   allocations (e.g., two calls to record-type with identical name arrays
//   produce separate schemas).  Structural/unification comparisons compare
//   field names, not schema offsets (see RECORD EQUALITY / UNIFICATION below;
//   the eq operator is identity-only and does NOT do this fallback).
//
// RECORD CREATION
//   Two paths:
//     record-type: takes a name array, returns an executable constructor
//       proc.  The proc, when called, pops N values from the stack and
//       builds a record.  The schema is allocated once; the constructor
//       can be called repeatedly to create instances with the same schema.
//
//     record: takes a name array + N values, creates a one-off record
//       without a reusable constructor.
//
// IMMUTABILITY
//   Records are always immutable.  "Updating" a field creates a new record
//   with the modified field and cloned copies of all other fields.  The
//   original record is unchanged.  This is functional update, matching
//   ML/Haskell/Erlang semantics.
//
// FIELD ACCESS
//   Fields are accessed by name, not index:
//     /field-name get  -- extract a field value
//     /field-name value record-update  -- functional update
//   Internally, field access searches the schema's name array for the
//   matching name (O(N) where N is the field count, but N is typically
//   small -- 3-10 fields).
//
// PIPELINE OPERATIONS
//   Records support data-processing operations inspired by SQL and
//   Clojure's sequence library:
//     record-group-by  -- group an array of records by a field value
//     record-zip       -- construct records from columnar arrays
//     record-select    -- project a subset of fields (like SQL SELECT)
//     record-merge     -- combine two records (right-side wins on overlap)
//     record-from-dict -- construct a record from a dict + name array
//     record-to-dict   -- convert a record to a dict
//
// RECORD EQUALITY
//   Under the default eq operator (compare_eq_impl -> Object::equal),
//   two records are equal only by IDENTITY: same instance offset and same
//   field count.  Two separately-allocated records with identical fields are
//   NOT eq-equal.  (= is a stdout print operator, not equality; use eq/ne for
//   equality, deep-eq for structural.)  Use deep-eq for structural comparison
//   -- same field count, same field names in the same order, and recursively
//   equal field values (deep_equal_records in ops_convert.inl).
//
// UNIFICATION
//   Records participate in logic unification (ops_logic.inl).  Two records
//   unify if they have the same schema (same field names) and all
//   corresponding fields unify pairwise.
//
// --- Operators ---
//
//   record-type     array -- proc          Define a record constructor
//   record          array val* -- record   Create a record directly
//   record-fields   record -- val*         Explode fields onto stack
//   record-schema   record -- array        Get field name array
//   record-update   record name value -- record'  Functional update
//   record-map-field  record name proc -- record'  Apply proc to one field
//   record-map      record proc -- record'  Apply proc to all fields
//   record-merge    record record -- record'  Merge two records
//   record-select   record array -- record'  Project subset of fields
//   record-to-dict  record -- dict          Convert to dict
//   record-from-dict  dict array -- record  Construct from dict
//   record-zip      array array -- array    Columnar to row-of-records
//   record-group-by array name -- dict      Group records by field
//   record-values   record -- array         All field values as array
//   record-known    record name -- bool     Test if field exists
//
//   Records are iterated with the generic for-all operator:
//     record proc for-all  -- iterate name/value pairs (drives @record-for-all)
//
// Control operators (internal, not user-visible):
//   @record-ctor              Constructor proc: pop values, build instance
//   @record-for-all           Iteration step
//   @record-map-field-complete  Collect mapped field, build new record
//   @record-map-step          Collect mapped field, continue to next
//

//===--- Record Data Structures ---===//

// Record schema: stored in VM heap, shared across all instances of a record-type.
//   [field_count (length_t==uint16_t)] [pad (uint16_t)] [Name[0]] ... [Name[N-1]]
// Record instance: stored in VM heap, one per record value.
//   [schema_offset (vm_offset_t)] [Object[0]] [Object[1]] ... [Object[N-1]]
// The schema's two uint16_t header fields total 4 bytes; the instance's single
// vm_offset_t (uint32_t) schema offset is 4 bytes -- both keep Object[] at
// 4-byte alignment.
struct RecordSchema {
    length_t m_field_count;
    uint16_t m_pad;     // maintain proper alignment for Object
    Object m_names[1];  // variable-length: m_names[0..m_field_count-1]

    [[nodiscard]] static vm_size_t alloc_size(length_t n) {
        return static_cast<vm_size_t>(offsetof(RecordSchema, m_names) + (n * sizeof(Object)));
    }
};
static_assert(alignof(RecordSchema) == alignof(Object));

struct RecordInstance {
    vm_offset_t m_schema;
    Object m_fields[1];  // variable-length: m_fields[0..field_count-1]

    [[nodiscard]] static vm_size_t alloc_size(length_t n) {
        return static_cast<vm_size_t>(offsetof(RecordInstance, m_fields) + (n * sizeof(Object)));
    }
};
static_assert(alignof(RecordInstance) == alignof(Object));

// Find the index of a named field in a record schema.
// Returns field_count (sentinel) if not found.
[[nodiscard]] static length_t record_find_field(Trix *trx, const RecordSchema *schema, length_t field_count, Object name) {
    auto index = field_count;
    for (length_t i = 0; i < field_count; ++i) {
        if (schema->m_names[i].equal(trx, name)) {
            index = i;
            break;
        }
    }
    return index;
}

// Clone a record instance, replacing one field with a new value.
// Returns the new instance offset.
// Routes through m_curr_alloc_global: when set, the new instance lands in
// global VM and is tagged ChunkKind::Record.
[[nodiscard]] static vm_offset_t
record_clone_replacing_field(Trix *trx, const RecordInstance *inst, length_t field_count, length_t target_idx, Object new_value) {
    auto [new_inst, new_offset] =
            trx->vm_alloc_dispatch<RecordInstance>(RecordInstance::alloc_size(field_count), Trix::ChunkKind::Record);
    if (trx->m_curr_alloc_global) {
        // Stamp user-visible field count for GC walker (eliminates
        // slack-walking on RecordInstance Object[] payload).  vm_alloc_dispatch
        // (single) does not auto-stamp because the alloc size and the
        // field-count don't coincide for header+payload structs.
        trx->gvm_set_obj_count(new_offset, field_count);
    }
    // The schema already exists and is rooted via the (caller-rooted) source
    // instance, so setting m_schema is safe.  Null-initialise the fields, then
    // place the replacement value (so its possibly-ExtValue cell is rooted via
    // new_inst before any allocation), then root the half-built instance on the
    // operand stack across the per-field make_clone calls: a heap-cell field
    // clone can fire a global-VM GC that would otherwise sweep the unrooted
    // new instance (and its already-written field cells).
    auto curr_save_level = trx->m_curr_save_level;
    new_inst->m_schema = inst->m_schema;
    for (length_t i = 0; i < field_count; ++i) {
        new_inst->m_fields[i] = Object::make_null();
    }
    new_value.set_save_level(curr_save_level);
    new_inst->m_fields[target_idx] = new_value;

    // Root the half-built instance on the gc-root stack across the per-field
    // make_clone calls (a heap-cell field clone can fire a global-VM GC that
    // would otherwise sweep the unrooted new instance and its written cells).
    // Both callers (record-update / @record-map-field-complete) hold no gc-roots
    // when they call this, so reset-to-empty == restore-to-entry here (and the
    // reset_gc_root assert would fire immediately if that ever changed).
    trx->gc_root_push_oneoff(Object::make_record(new_offset, field_count));
    for (length_t i = 0; i < field_count; ++i) {
        if (i != target_idx) {
            new_inst->m_fields[i] = inst->m_fields[i].make_clone(trx);
        }
    }

    trx->gc_root_pop_oneoff();  // drop the temp root; the caller publishes the record
    return new_offset;
}

// Helper: allocate a record schema in VM heap.
// Returns the schema vm_offset_t.
// Routes through m_curr_alloc_global: when set, the schema lands in global VM
// and is tagged ChunkKind::Record (the Record bucket holds both schemas and
// instances, per the gvm_heap.inl enum comment).
[[nodiscard]] static vm_offset_t allocate_record_schema(Trix *trx, Object arr_obj) {
    auto [names, field_count] = arr_obj.array_value(trx);
    auto [schema, schema_offset] =
            trx->vm_alloc_dispatch<RecordSchema>(RecordSchema::alloc_size(field_count), Trix::ChunkKind::Record);
    if (trx->m_curr_alloc_global) {
        // Stamp user-visible field count for GC walker.  Both
        // RecordSchema and RecordInstance use ChunkKind::Record;
        // the walker disambiguates via m_obj_count: instances stamp
        // it (record_clone_replacing_field, make_record_op, the
        // @record-ctor handler), schemas now stamp it too.  This
        // lets the walker walk Object[m_obj_count] uniformly and
        // reserves a separate test (looking at first_four) for
        // deciding whether to chase the schema offset.
        trx->gvm_set_obj_count(schema_offset, field_count);
    }
    schema->m_field_count = field_count;
    schema->m_pad = 0;

    auto curr_save_level = trx->m_curr_save_level;
    for (length_t i = 0; i < field_count; ++i) {
        auto name_obj = names[i].make_clone(trx);
        name_obj.set_literal();
        name_obj.set_save_level(curr_save_level);
        schema->m_names[i] = name_obj;
    }
    return schema_offset;
}

// Helper: validate a Name array for use as record schema.
// Checks: all elements are literal Names, no duplicates.
// WARNING this is (O(N^2), where N is the number of fields in the record; which should be small)
static void validate_record_names(Trix *trx, Object arr_obj) {
    auto [names, count] = arr_obj.array_value(trx);
    for (length_t i = 0; i < count; ++i) {
        if (!names[i].is_name()) {
            trx->error(Error::TypeCheck, "record-type: element {} is not a 'name'", i);
        }
    }
    for (length_t i = 0; i < count; ++i) {
        for (auto j = static_cast<length_t>(i + 1); j < count; ++j) {
            if (names[i].equal(trx, names[j])) {
                trx->error(Error::RangeCheck, "record-type: duplicate field name /{}", names[i].name_sv(trx));
            }
        }
    }
}

//===--- Record Operators ---===//

// record-type: name_array :- proc
// Defines a record constructor from an array of field names.
// Returns an executable procedure that, when called, pops N values from the
// operand stack and builds a Record instance.
// throws: opstack-underflow, type-check, range-check, vm-full, limit-check
static void record_type_op(Trix *trx) {
    trx->verify_operands(VerifyArray);

    auto arr_obj = *trx->m_op_ptr;
    validate_record_names(trx, arr_obj);

    // Build a 0-field "schema holder" record instance FIRST, root it, then allocate the
    // schema and link it into the holder.  The ctor proc references the schema only through
    // this holder (a markable Record), NOT a bare uinteger offset: a uinteger is an immediate
    // and is invisible to the GC, so a GLOBAL schema (under ${...}) referenced only that way
    // would be swept by any vm_global_gc firing between record-type and the type's first
    // construction.  The holder's m_schema mark -- like any live instance's -- keeps the
    // schema alive.  Mirrors make_record_op's instance-first / null-schema / root-then-link
    // ordering so the schema is never unrooted across an allocation.
    auto [holder, holder_offset] = trx->vm_alloc_dispatch<RecordInstance>(RecordInstance::alloc_size(0), Trix::ChunkKind::Record);
    if (trx->m_curr_alloc_global) {
        trx->gvm_set_obj_count(holder_offset, 0);
    }
    holder->m_schema = nulloffset;
    trx->gc_root_push_oneoff(Object::make_record(holder_offset, 0));

    auto schema_offset = allocate_record_schema(trx, arr_obj);
    holder->m_schema = schema_offset;

    // Build executable procedure: { <schema-holder record> @record-ctor }
    // When executed: pushes the holder (data; Record is PushOpDirect), then @record-ctor
    // reads its m_schema and pops N values.  Routes through m_curr_alloc_global so the ctor
    // proc + holder survive save/restore alongside the schema; tagged ChunkKind::Array.
    auto [proc_storage, proc_offset] = trx->vm_alloc_dispatch_n<Object>(2, Trix::ChunkKind::Array);
    proc_storage[0] = Object::make_record(holder_offset, 0);
    proc_storage[1] = Object::make_control_operator(SystemName::atRecordCtor);

    *trx->m_op_ptr = Object::make_array(proc_offset, 2, Object::ExecutableAttrib, Object::ReadOnlyAccess);
    trx->gc_root_pop_oneoff();
}

// record: v0..vN-1 name_array :- record
// Constructs an anonymous record from values on the stack + a Name array.
// throws: opstack-underflow, type-check, range-check, vm-full, limit-check
static void make_record_op(Trix *trx) {
    trx->verify_operands(VerifyArray);

    auto arr_obj = *trx->m_op_ptr;
    auto arr_len = arr_obj.arrays_length();
    trx->require_op_count(arr_len + 1);

    validate_record_names(trx, arr_obj);

    // Allocate + null-initialise the instance and root it on the operand stack
    // BEFORE building the schema: allocate_record_schema can fire a global-VM
    // GC (gvm_alloc auto-collects when the region is near-full), and a record
    // built inside ${...} lives in the global region.  A null-initialised
    // instance with m_schema == nulloffset is walked by the GC as a null-field
    // schema-shaped block (safe); rooting it survives that GC, and the schema
    // becomes reachable via the live instance the instant m_schema is set.
    // (Just reordering schema-before-instance would leave the schema unrooted
    // across the instance's own allocation GC -- the dangling object would
    // merely move from instance to schema.)
    auto [inst, instance_offset] =
            trx->vm_alloc_dispatch<RecordInstance>(RecordInstance::alloc_size(arr_len), Trix::ChunkKind::Record);
    if (trx->m_curr_alloc_global) {
        trx->gvm_set_obj_count(instance_offset, arr_len);
    }
    inst->m_schema = nulloffset;
    for (length_t i = 0; i < arr_len; ++i) {
        inst->m_fields[i] = Object::make_null();
    }
    trx->gc_root_push_oneoff(Object::make_record(instance_offset, arr_len));

    auto schema_offset = allocate_record_schema(trx, arr_obj);
    inst->m_schema = schema_offset;

    // Store field values.  The name array is on top of the operand stack; the
    // field values sit below it (field[0] deepest, field[N-1] just below the name
    // array).  The half-built instance is rooted on the gc-root stack (not the
    // operand stack), so field[0] is at m_op_ptr - arr_len.
    auto curr_save_level = trx->m_curr_save_level;
    auto fields_base = (trx->m_op_ptr - arr_len);  // -> field[0]
    for (length_t i = 0; i < arr_len; ++i) {
        auto val = fields_base[i];
        val.set_save_level(curr_save_level);
        inst->m_fields[i] = val;
    }

    // Collapse the stack: the record (now fully built) replaces the deepest
    // operand; the name array above it is discarded.
    *fields_base = Object::make_record(instance_offset, arr_len);
    trx->m_op_ptr = fields_base;

    trx->gc_root_pop_oneoff();
}

// @record-ctor: internal control operator
// Pops the type's schema-holder record from the op stack, reads field_count from its
// schema, pops N values from the op stack, and builds a Record instance.
static void at_record_ctor_op(Trix *trx) {
    trx->verify_operands(VerifyRecord);
    auto schema_offset = trx->m_op_ptr->record_instance(trx)->m_schema;

    // Read field_count from schema
    auto schema = trx->offset_to_ptr<RecordSchema>(schema_offset);
    auto field_count = schema->m_field_count;

    trx->require_op_count(field_count + 1);

    auto [inst, instance_offset] =
            trx->vm_alloc_dispatch<RecordInstance>(RecordInstance::alloc_size(field_count), Trix::ChunkKind::Record);
    if (trx->m_curr_alloc_global) {
        trx->gvm_set_obj_count(instance_offset, field_count);
    }
    inst->m_schema = schema_offset;

    auto ptr = trx->m_op_ptr;
    auto curr_save_level = trx->m_curr_save_level;
    for (length_t i = field_count; i > 0; --i) {
        auto val = *--ptr;
        val.set_save_level(curr_save_level);
        inst->m_fields[i - 1] = val;
    }

    *ptr = Object::make_record(instance_offset, field_count);
    trx->m_op_ptr = ptr;
}

// @record-for-all: (exec stack: proc record index saved-depth)
// Verifies that the body returned the operand stack to the depth
// it had BEFORE the iteration's name/value pair was pushed; mismatch
// raises /range-check.  Pushes next name/value pair from record,
// re-pushes frame.
// Exec stack layout after interpreter pops @record-for-all:
//   m_exec_ptr -> saved-depth
//                 index
//                 record
//                 proc
static void at_record_forall_op(Trix *trx) {
    auto saved_depth_ptr = trx->m_exec_ptr;
    auto index_ptr = (saved_depth_ptr - 1);
    auto rec_ptr = (index_ptr - 1);

    auto saved_depth = saved_depth_ptr->integer_value();
    auto current_depth = static_cast<integer_t>(trx->m_op_ptr - trx->m_op_base);
    if (current_depth != saved_depth) {
        trx->error(Error::RangeCheck,
                   "for-all: body left {} item(s) on the operand stack (expected stack effect 'name value -- ')",
                   current_depth - saved_depth);
    }

    auto index = static_cast<length_t>(index_ptr->uinteger_value());
    if (index < rec_ptr->object_length()) {
        trx->require_op_capacity(2);
        trx->require_exec_capacity(2);

        auto inst = rec_ptr->record_instance(trx);
        auto schema = trx->offset_to_ptr<RecordSchema>(inst->m_schema);

        *++trx->m_op_ptr = schema->m_names[index].make_clone(trx);
        *++trx->m_op_ptr = inst->m_fields[index].make_clone(trx);

        index_ptr->update_uinteger(static_cast<uinteger_t>(index + 1));

        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atRecordForAll);
        *++trx->m_exec_ptr = index_ptr[-2];
    } else {
        trx->m_exec_ptr -= 4;
    }
}

// record-schema: record :- name_array
// Returns a new array containing copies of the field names.
// throws: opstack-underflow, type-check, vm-full
static void record_schema_op(Trix *trx) {
    trx->verify_operands(VerifyRecord);

    auto rec = trx->m_op_ptr;
    auto inst = rec->record_instance(trx);
    auto schema = trx->offset_to_ptr<RecordSchema>(inst->m_schema);
    auto field_count = schema->m_field_count;

    // Allocate a new array with copies of the field names
    auto [arr_storage, arr_offset] = trx->vm_alloc_dispatch_n<Object>(field_count, Trix::ChunkKind::Array);
    for (length_t i = 0; i < field_count; ++i) {
        arr_storage[i] = schema->m_names[i].make_clone(trx);
    }

    *trx->m_op_ptr = Object::make_array(arr_offset, field_count);
}

// record-fields: record :- v0..vN-1
// Destructures a record: pushes all field values in field order.
// throws: opstack-underflow, type-check
static void record_fields_op(Trix *trx) {
    trx->verify_operands(VerifyRecord);

    auto rec = trx->m_op_ptr;
    auto field_count = rec->object_length();
    if (field_count == 0) {
        --trx->m_op_ptr;  // empty record: consume it, push nothing
    } else {
        trx->require_op_capacity(field_count - 1);

        auto inst = rec->record_instance(trx);
        // Keep the source record live in its slot (rooting inst) while cloning
        // fields 1..N-1 into the slots above it, advancing m_op_ptr per write so
        // each clone becomes a GC root before the next make_clone -- a heap-cell
        // field clone can fire a global-VM GC, and overwriting the record slot
        // first (as the old loop did) would un-root the instance it keeps reading.
        // Field 0 overwrites the record slot LAST, after which nothing allocates.
        for (length_t i = 1; i < field_count; ++i) {
            *++trx->m_op_ptr = inst->m_fields[i].make_clone(trx);
        }
        *rec = inst->m_fields[0].make_clone(trx);
    }
}

// record-update: record name value :- record'
// Returns a new Record with the named field replaced.
// throws: opstack-underflow, type-check, undefined, vm-full
static void record_update_op(Trix *trx) {
    trx->verify_operands(VerifyAny, VerifyName, VerifyRecord);

    auto value_ptr = trx->m_op_ptr;
    auto name_ptr = (value_ptr - 1);
    auto rec_ptr = (name_ptr - 1);

    auto inst = rec_ptr->record_instance(trx);
    auto schema = trx->offset_to_ptr<RecordSchema>(inst->m_schema);

    auto field_count = schema->m_field_count;  // == object_length(); matches record_map_field_op
    auto target_idx = record_find_field(trx, schema, field_count, *name_ptr);
    if (target_idx != field_count) {
        // Keep value + name on the stack (value rooted at m_op_ptr) across the
        // clone, THEN collapse: record_clone_replacing_field roots the new
        // instance via a temporary above m_op_ptr, and the replacement value's
        // own (possibly ExtValue) cell must stay rooted via the live value slot
        // until the helper stores it.  Decrementing first would un-root it.
        auto new_offset = record_clone_replacing_field(trx, inst, field_count, target_idx, *value_ptr);
        trx->m_op_ptr -= 2;
        *trx->m_op_ptr = Object::make_record(new_offset, field_count);
    } else {
        trx->error(Error::Undefined, "record-update: field /{} not found in record", name_ptr->name_sv(trx));
    }
}

//===--- Record pipeline operators ---===//

// record-map-field: record name proc -- record'
// Applies proc to the named field's value, returns new record with result.
// throws: opstack-underflow, type-check, undefined, vm-full
static void record_map_field_op(Trix *trx) {
    trx->verify_operands(VerifyProc, VerifyName, VerifyRecord);

    auto proc_ptr = trx->m_op_ptr;
    auto name_ptr = (proc_ptr - 1);
    auto rec_ptr = (name_ptr - 1);

    auto inst = rec_ptr->record_instance(trx);
    auto schema = trx->offset_to_ptr<RecordSchema>(inst->m_schema);
    auto field_count = schema->m_field_count;

    auto target_idx = record_find_field(trx, schema, field_count, *name_ptr);
    if (target_idx != field_count) {
        trx->require_exec_capacity(4);
        auto field_obj = inst->m_fields[target_idx].make_clone(trx);

        // exec stack: [field_index] [record] [@record-map-field-complete] [proc]
        *++trx->m_exec_ptr = Object::make_integer(static_cast<integer_t>(target_idx));
        *++trx->m_exec_ptr = *rec_ptr;
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atRecordMapFieldComplete);
        *++trx->m_exec_ptr = *proc_ptr;

        // push field value for proc to operate on
        trx->m_op_ptr -= 2;
        *trx->m_op_ptr = field_obj;
    } else {
        trx->error(Error::Undefined, "record-map-field: field /{} not found in 'record'", name_ptr->name_sv(trx));
    }
}

// @record-map-field-complete: internal control operator
// Collects proc result from op stack, builds new record with updated field.
static void at_record_map_field_complete_op(Trix *trx) {
    trx->require_op_count(1);

    auto rec_ptr = trx->m_exec_ptr;
    auto field_ptr = (rec_ptr - 1);

    auto inst = rec_ptr->record_instance(trx);
    auto field_count = rec_ptr->object_length();
    auto field_index = static_cast<length_t>(field_ptr->integer_value());
    auto new_offset = record_clone_replacing_field(trx, inst, field_count, field_index, *trx->m_op_ptr);

    trx->m_exec_ptr -= 2;
    *trx->m_op_ptr = Object::make_record(new_offset, field_count);
}

// record-map: record proc -- record'
// Applies proc to every field value, returns new record with same schema.
// throws: opstack-underflow, type-check, vm-full
static void record_map_op(Trix *trx) {
    trx->verify_operands(VerifyProc, VerifyRecord);

    auto proc_ptr = trx->m_op_ptr;
    auto rec_ptr = (proc_ptr - 1);
    auto field_count = rec_ptr->object_length();

    if (field_count != 0) {
        trx->require_exec_capacity(6);

        auto inst = rec_ptr->record_instance(trx);

        // Allocate the result instance and null-initialise its fields, then root
        // it on the exec stack as a real record Object -- NOT a bare uinteger.
        // The new instance rides the exec stack across arbitrary user-proc
        // executions between @record-map-step fires, and those procs can fire a
        // global-VM GC.  A bare uinteger offset is invisible to gc_mark_object,
        // so the half-built instance would be swept; a stamped-but-uninitialised
        // instance would be walked with garbage Object slots.  Null fields + a
        // record companion make every GC during the map safe: the field count is
        // fixed and every not-yet-filled slot reads as a valid null Object.
        auto [new_inst, new_offset] =
                trx->vm_alloc_dispatch<RecordInstance>(RecordInstance::alloc_size(field_count), Trix::ChunkKind::Record);
        if (trx->m_curr_alloc_global) {
            trx->gvm_set_obj_count(new_offset, field_count);
        }
        new_inst->m_schema = inst->m_schema;
        for (length_t i = 0; i < field_count; ++i) {
            new_inst->m_fields[i] = Object::make_null();
        }

        // exec stack (bottom to top): [proc] [record] [new-record] [field_index=0] [@record-map-step] [proc]
        // proc runs first on field[0] value, then @record-map-step collects result.
        auto proc = *proc_ptr;
        *++trx->m_exec_ptr = proc;
        *++trx->m_exec_ptr = *rec_ptr;
        *++trx->m_exec_ptr = Object::make_record(new_offset, field_count);
        *++trx->m_exec_ptr = Object::make_integer(0);
        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atRecordMapStep);
        *++trx->m_exec_ptr = proc;

        // Push the first field value for proc.  Clone AFTER the new instance is
        // rooted on the exec stack -- make_clone of an ExtValue field can fire a
        // global GC, and the source record stays rooted via the exec companion.
        *rec_ptr = inst->m_fields[0].make_clone(trx);
    }
    --trx->m_op_ptr;
}

// @record-map-step: internal control operator
// Collects proc result, stores in new instance, advances to next field.
// Exec stack (after interpreter pops @record-map-step):
//   m_exec_ptr -> field_index
//                 new_instance_record   (a real record Object, GC-rooted)
//                 record
//                 proc
static void at_record_map_step_op(Trix *trx) {
    trx->require_op_count(1);

    auto index_ptr = trx->m_exec_ptr;
    auto new_record_ptr = (index_ptr - 1);
    auto record_ptr = (new_record_ptr - 1);
    auto proc_ptr = (record_ptr - 1);

    auto field_index = static_cast<length_t>(index_ptr->integer_value());
    auto field_count = record_ptr->object_length();
    auto inst = record_ptr->record_instance(trx);
    auto new_inst = new_record_ptr->record_instance(trx);

    // Collect proc result from op stack
    auto result = *trx->m_op_ptr;
    result.set_save_level(trx->m_curr_save_level);
    new_inst->m_fields[field_index] = result;

    auto next_index = static_cast<length_t>(field_index + 1);
    if (next_index < field_count) {
        trx->require_exec_capacity(2);

        // More fields to process
        index_ptr->update_integer(static_cast<integer_t>(next_index));

        *++trx->m_exec_ptr = Object::make_control_operator(SystemName::atRecordMapStep);
        *++trx->m_exec_ptr = *proc_ptr;

        // Replace op stack top with next field value
        *trx->m_op_ptr = inst->m_fields[next_index].make_clone(trx);
    } else {
        // All fields processed -- push the completed record.  Read the rooted
        // record companion before popping the four exec companions (pops are
        // non-erasing, but read-then-pop keeps the dependency explicit).
        auto completed = *new_record_ptr;
        trx->m_exec_ptr -= 4;
        *trx->m_op_ptr = completed;
    }
}

// record-merge: record1 record2 -- record'
// Merges two records. Fields from record2 override record1.
// Result schema: record1 fields first, then new record2 fields.
// throws: opstack-underflow, type-check, vm-full, limit-check
static void record_merge_op(Trix *trx) {
    trx->verify_operands(VerifyRecord, VerifyRecord);

    auto rec2_ptr = trx->m_op_ptr;
    auto rec1_ptr = (rec2_ptr - 1);

    auto inst1 = rec1_ptr->record_instance(trx);
    auto schema1 = trx->offset_to_ptr<RecordSchema>(inst1->m_schema);
    auto count1 = schema1->m_field_count;

    auto inst2 = rec2_ptr->record_instance(trx);
    auto schema2 = trx->offset_to_ptr<RecordSchema>(inst2->m_schema);
    auto count2 = schema2->m_field_count;

    // Count merged fields: all of record1 + record2 fields not in record1
    vm_size_t merged_count32 = count1;
    for (length_t j = 0; j < count2; ++j) {
        if (record_find_field(trx, schema1, count1, schema2->m_names[j]) == count1) {
            ++merged_count32;
        }
    }
    if (merged_count32 > MaxLength) {
        trx->error(Error::LimitCheck, "record-merge: merged field count {} exceeds maximum {}", merged_count32, MaxLength);
    } else {
        auto merged_count = static_cast<length_t>(merged_count32);

        // Allocate the INSTANCE first, null-init it, and root it BEFORE the schema
        // alloc (mirrors make_record_op): the per-field make_clone loops below
        // allocate ExtValue cells and, inside ${...}, fire a global GC that walks
        // merged_inst's m_fields AND -- via its m_schema -- merged_schema's m_names
        // by obj_count.  Both Object arrays must read as valid Null Objects until
        // filled, and the instance must stay rooted (it keeps the schema reachable
        // through m_schema; the schema cannot itself sit on the gc-root stack as an
        // Object).  With m_schema == nulloffset a GC during the schema alloc walks
        // the instance as a harmless null-field schema-shaped block.  rec1/rec2
        // stay rooted at their operand slots, so inst1/inst2/schema1/schema2 stay
        // valid across the non-moving GC.
        auto [merged_inst, m_inst_offset] =
                trx->vm_alloc_dispatch<RecordInstance>(RecordInstance::alloc_size(merged_count), Trix::ChunkKind::Record);
        if (trx->m_curr_alloc_global) {
            trx->gvm_set_obj_count(m_inst_offset, merged_count);
        }
        merged_inst->m_schema = nulloffset;
        for (length_t i = 0; i < merged_count; ++i) {
            merged_inst->m_fields[i] = Object::make_null();
        }
        trx->gc_root_push_oneoff(Object::make_record(m_inst_offset, merged_count));

        auto [merged_schema, m_schema_offset] =
                trx->vm_alloc_dispatch<RecordSchema>(RecordSchema::alloc_size(merged_count), Trix::ChunkKind::Record);
        if (trx->m_curr_alloc_global) {
            trx->gvm_set_obj_count(m_schema_offset, merged_count);
        }
        for (length_t i = 0; i < merged_count; ++i) {
            merged_schema->m_names[i] = Object::make_null();
        }
        merged_schema->m_field_count = merged_count;
        merged_schema->m_pad = 0;
        merged_inst->m_schema = m_schema_offset;

        auto curr_save_level = trx->m_curr_save_level;
        length_t idx = 0;

        // First: all record1 fields (overridden by record2 if present)
        for (length_t i = 0; i < count1; ++i) {
            auto name = schema1->m_names[i].make_clone(trx);
            name.set_save_level(curr_save_level);
            merged_schema->m_names[idx] = name;

            // Check if record2 has this field
            auto override_idx = record_find_field(trx, schema2, count2, schema1->m_names[i]);
            auto obj = (override_idx < count2) ? inst2->m_fields[override_idx] : inst1->m_fields[i];
            obj = obj.make_clone(trx);
            obj.set_save_level(curr_save_level);
            merged_inst->m_fields[idx] = obj;
            ++idx;
        }

        // Then: record2 fields not in record1
        for (length_t i = 0; i < count2; ++i) {
            if (record_find_field(trx, schema1, count1, schema2->m_names[i]) == count1) {
                auto name = schema2->m_names[i].make_clone(trx);
                name.set_save_level(curr_save_level);
                merged_schema->m_names[idx] = name;
                merged_inst->m_fields[idx] = inst2->m_fields[i].make_clone(trx);
                merged_inst->m_fields[idx].set_save_level(curr_save_level);
                ++idx;
            }
        }

        --trx->m_op_ptr;
        *trx->m_op_ptr = Object::make_record(m_inst_offset, merged_count);

        trx->gc_root_pop_oneoff();
    }
}

// record-select: record name_array -- record'
// Returns new record with only the named fields.
// throws: opstack-underflow, type-check, undefined, range-check, vm-full
static void record_select_op(Trix *trx) {
    trx->verify_operands(VerifyArray, VerifyRecord);

    auto arr_ptr = trx->m_op_ptr;
    auto rec_ptr = (arr_ptr - 1);

    auto arr_obj = *arr_ptr;
    validate_record_names(trx, arr_obj);

    auto inst = rec_ptr->record_instance(trx);
    auto schema = trx->offset_to_ptr<RecordSchema>(inst->m_schema);
    auto field_count = schema->m_field_count;
    auto [sel_names, sel_count] = arr_obj.array_value(trx);

    // Allocate the instance FIRST, null-init + root it, THEN build the schema
    // (mirrors make_record_op): the per-field make_clone loop allocates ExtValue
    // cells and, inside ${...}, fires a global GC that walks new_inst's m_fields by
    // obj_count -- they must read as valid Nulls until filled and the instance must
    // be rooted.  With m_schema == nulloffset a GC during allocate_record_schema
    // walks the instance as a harmless null-field schema-shaped block.  rec/arr
    // stay rooted at their operand slots, so inst/schema/sel_names stay valid
    // across the non-moving GC.
    auto [new_inst, new_offset] =
            trx->vm_alloc_dispatch<RecordInstance>(RecordInstance::alloc_size(sel_count), Trix::ChunkKind::Record);
    if (trx->m_curr_alloc_global) {
        trx->gvm_set_obj_count(new_offset, sel_count);
    }
    new_inst->m_schema = nulloffset;
    for (length_t s = 0; s < sel_count; ++s) {
        new_inst->m_fields[s] = Object::make_null();
    }
    trx->gc_root_push_oneoff(Object::make_record(new_offset, sel_count));

    // The new schema copies arr_obj's names in order, so new_inst->m_fields[s]
    // stays positionally aligned with sel_names[s].  allocate_record_schema fully
    // builds the schema (Name clones never allocate) before it returns, so it is
    // safe the instant m_schema is set.
    new_inst->m_schema = allocate_record_schema(trx, arr_obj);

    // Single pass: locate each selected field in the source and copy it
    // (the existence check and the copy share one record_find_field lookup).
    auto curr_save_level = trx->m_curr_save_level;
    for (length_t s = 0; s < sel_count; ++s) {
        auto idx = record_find_field(trx, schema, field_count, sel_names[s]);
        if (idx == field_count) {
            trx->error(Error::Undefined, "record-select: field /{} not found in 'record'", sel_names[s].name_sv(trx));
        } else {
            auto obj = inst->m_fields[idx].make_clone(trx);
            obj.set_save_level(curr_save_level);
            new_inst->m_fields[s] = obj;
        }
    }

    --trx->m_op_ptr;
    *trx->m_op_ptr = Object::make_record(new_offset, sel_count);

    trx->gc_root_pop_oneoff();
}

// record-to-dict: record -- dict
// Converts a record to a read-write Dict.
// throws: opstack-underflow, type-check, vm-full
static void record_to_dict_op(Trix *trx) {
    trx->verify_operands(VerifyRecord);

    auto rec_ptr = trx->m_op_ptr;
    auto inst = rec_ptr->record_instance(trx);
    auto schema = trx->offset_to_ptr<RecordSchema>(inst->m_schema);

    auto field_count = rec_ptr->object_length();
    auto [dict, dict_offset] =
            trx->m_curr_alloc_global ? Dict::create_global_dict(trx, field_count) : Dict::create_or_recycle(trx, field_count);

    // Root the result dict across the per-field key/value make_clone + put loop: a
    // value clone allocates an ExtValue cell and (under ${...}) can fire a global
    // GC that would otherwise sweep the unrooted global dict along with the entries
    // already put into it.  The dict is pre-sized to field_count, so put never
    // grows/reallocs (no alloc between a clone and its put).  The source record
    // stays rooted at rec_ptr, so inst/schema read through it across the GC.
    trx->gc_root_push_oneoff(Object::make_dict(dict_offset));
    for (length_t i = 0; i < field_count; ++i) {
        auto key = schema->m_names[i].make_clone(trx);
        auto value = inst->m_fields[i].make_clone(trx);
        dict->put(trx, key, value);
    }

    *trx->m_op_ptr = Object::make_dict(dict_offset);

    trx->gc_root_pop_oneoff();
}

// record-from-dict: dict name_array -- record
// Constructs a record by extracting named fields from a Dict.
// throws: opstack-underflow, type-check, undefined, vm-full
static void record_from_dict_op(Trix *trx) {
    trx->verify_operands(VerifyArray, VerifyDict);

    auto arr_ptr = trx->m_op_ptr;
    auto dict_ptr = (arr_ptr - 1);

    auto arr_obj = *arr_ptr;
    validate_record_names(trx, arr_obj);

    auto [names, name_count] = arr_obj.array_value(trx);

    // Allocate + null-initialise the instance and root it on the operand stack
    // BEFORE building the schema and cloning field values.  allocate_record_schema
    // and each per-field make_clone alloc via vm_alloc_dispatch and can fire a
    // global-VM GC when the record lives in ${...} -- the same schema-first /
    // instance-last window make_record_op closes.  A null-initialised instance
    // (m_schema == nulloffset) is safe to GC-walk, the schema becomes reachable
    // the instant m_schema is set, and rooting the instance survives every
    // clone's GC.  (The array + dict stay rooted via arr_ptr / dict_ptr, so the
    // names/dict raw pointers remain valid across the non-moving GC.)
    auto [inst, inst_offset] =
            trx->vm_alloc_dispatch<RecordInstance>(RecordInstance::alloc_size(name_count), Trix::ChunkKind::Record);
    if (trx->m_curr_alloc_global) {
        trx->gvm_set_obj_count(inst_offset, name_count);
    }
    inst->m_schema = nulloffset;
    for (length_t i = 0; i < name_count; ++i) {
        inst->m_fields[i] = Object::make_null();
    }
    trx->gc_root_push_oneoff(Object::make_record(inst_offset, name_count));

    auto schema_offset = allocate_record_schema(trx, arr_obj);
    inst->m_schema = schema_offset;

    auto dict = dict_ptr->dict_value(trx);
    auto curr_save_level = trx->m_curr_save_level;
    for (length_t i = 0; i < name_count; ++i) {
        auto value = dict->get(trx, &names[i]);
        if (value == nullptr) {
            // error() resets the gc-root stack on the throw path, so the temp
            // root needs no manual drop here.
            trx->error(Error::Undefined, "record-from-dict: key /{} not found in 'dict'", names[i].name_sv(trx));
        } else {
            auto field = value->make_clone(trx);
            field.set_save_level(curr_save_level);
            inst->m_fields[i] = field;
        }
    }

    // Collapse the stack: the fully-built record replaces the deepest operand
    // (dict); the name array above it is discarded.
    *dict_ptr = Object::make_record(inst_offset, name_count);
    trx->m_op_ptr = dict_ptr;

    trx->gc_root_pop_oneoff();
}

// record-zip: value_array name_array -- record_array
// Constructs array of records from columnar arrays.
// throws: opstack-underflow, type-check, range-check, vm-full
static void record_zip_op(Trix *trx) {
    trx->verify_operands(VerifyArray, VerifyArray);

    auto name_arr_ptr = trx->m_op_ptr;
    auto col_arr_ptr = (name_arr_ptr - 1);

    auto name_count = name_arr_ptr->arrays_length();
    auto [columns, col_count] = col_arr_ptr->array_value(trx);

    validate_record_names(trx, *name_arr_ptr);

    if (name_count != col_count) {
        trx->error(Error::RangeCheck, "record-zip: name count {} != column count {}", name_count, col_count);
    } else {
        // Validate all columns are arrays with same length
        length_t row_count = 0;
        for (length_t c = 0; c < col_count; ++c) {
            if (columns[c].is_array()) {
                auto col_len = columns[c].arrays_length();
                if (c == 0) {
                    row_count = col_len;
                } else if (col_len != row_count) {
                    trx->error(Error::RangeCheck, "record-zip: column {} length {} != column 0 length {}", c, col_len, row_count);
                }
            } else {
                trx->error(Error::TypeCheck, "record-zip: column {} is not an 'array'", c);
            }
        }

        // Allocate the result array FIRST, null-init + root it, and build each
        // row's instance INTO the rooted array so every completed + in-progress
        // instance stays marked across the per-field make_clone GCs.  Each instance
        // carries m_schema = nulloffset while its fields are cloned, so a GC walks
        // it as a harmless null-field schema-shaped block.  Allocate the shared
        // schema LAST -- once all instances are rooted via the result array -- then
        // link it into every instance in a final no-allocation pass, so the fresh
        // schema cannot be swept before it is referenced (make_record_op's
        // instance-first / schema-last pattern, generalised to N instances sharing
        // one schema).  name_arr / col_arr stay rooted at their operand slots, so
        // columns / names stay valid across the non-moving GC.
        auto [result_storage, result_offset] = trx->vm_alloc_dispatch_n<Object>(row_count, Trix::ChunkKind::Array);
        for (length_t r = 0; r < row_count; ++r) {
            result_storage[r] = Object::make_null();
        }
        trx->gc_root_push_oneoff(Object::make_array(result_offset, row_count));

        auto curr_save_level = trx->m_curr_save_level;
        for (length_t r = 0; r < row_count; ++r) {
            auto [inst, inst_offset] =
                    trx->vm_alloc_dispatch<RecordInstance>(RecordInstance::alloc_size(name_count), Trix::ChunkKind::Record);
            if (trx->m_curr_alloc_global) {
                trx->gvm_set_obj_count(inst_offset, name_count);
            }
            inst->m_schema = nulloffset;
            for (length_t c = 0; c < name_count; ++c) {
                inst->m_fields[c] = Object::make_null();
            }
            result_storage[r] = Object::make_record(inst_offset, name_count);

            for (length_t c = 0; c < col_count; ++c) {
                auto col_data = columns[c].array_objects(trx);
                auto field = col_data[r].make_clone(trx);
                field.set_save_level(curr_save_level);
                inst->m_fields[c] = field;
            }
        }

        // Link the shared schema into every built instance (no allocation here, so
        // the freshly-built schema cannot be swept before it is referenced).
        if (row_count > 0) {
            auto schema_offset = allocate_record_schema(trx, *name_arr_ptr);
            for (length_t r = 0; r < row_count; ++r) {
                result_storage[r].record_instance(trx)->m_schema = schema_offset;
            }
        }

        --trx->m_op_ptr;
        *trx->m_op_ptr = Object::make_array(result_offset, row_count);

        trx->gc_root_pop_oneoff();
    }
}

// record-group-by: record_array name -- dict
// Groups records by field value. Two-pass: count groups, then populate.
// throws: opstack-underflow, type-check, undefined, vm-full
static void record_group_by_op(Trix *trx) {
    trx->verify_operands(VerifyName, VerifyArray);

    auto name_ptr = trx->m_op_ptr;
    auto arr_ptr = (name_ptr - 1);

    auto [records, rec_count] = arr_ptr->array_value(trx);
    if (rec_count == 0) {
        // Empty array -> empty dict.  Honor m_curr_alloc_global so the result
        // lands in the same region as the populated path (and as the per-group
        // arrays), mirroring record-to-dict; default (Fixed) mode also matches
        // the populated path's create_or_recycle dict (both are full at return).
        auto [_, dict_offset] = trx->m_curr_alloc_global ? Dict::create_global_dict(trx, 0) : Dict::create_dict(trx, 0);
        *arr_ptr = Object::make_dict(dict_offset);
    } else {
        // Pass 1: count group sizes using a temporary dict (key -> count)
        // Pre-size to rec_count -- at most that many distinct groups. Over-allocates
        // for few-group cases but avoids repeated doubling and degraded hashing.
        auto name_obj = *name_ptr;
        auto [count_dict, count_dict_offset] = Dict::create_dict(trx, rec_count);

        // Root the three working dicts (count / result / idx) across the op.  Inside
        // ${...} every key make_clone allocates the key in GLOBAL VM, but these dicts
        // are reachable from no GC root, so a later make_clone / make_empty_array GC
        // would sweep their global keys (and result_dict's per-group arrays) out
        // from under them -- the pass-2 value lookups would then miss.  Rooting each
        // dict marks its contents (gc_mark_object descends into a local dict's
        // entries and marks the global keys).  Slot 2 (scratch) holds each freshly-
        // cloned key across the make_empty_array that follows it.  The source array
        // stays rooted at arr_ptr, so `records` reads through it across the GC.
        trx->require_gc_root_capacity(4);  // count_dict, scratch key, result_dict, idx_dict

        *++trx->m_gc_roots_ptr = Object::make_dict(count_dict_offset);
        *++trx->m_gc_roots_ptr = Object::make_null();  // rolling slot for cloned keys
        auto *key_root = trx->m_gc_roots_ptr;

        for (length_t i = 0; i < rec_count; ++i) {
            if (records[i].is_record()) {
                auto inst = records[i].record_instance(trx);
                auto schema = trx->offset_to_ptr<RecordSchema>(inst->m_schema);
                auto field_count = schema->m_field_count;

                auto fidx = record_find_field(trx, schema, field_count, name_obj);
                if (fidx != field_count) {
                    auto key_ptr = &inst->m_fields[fidx];
                    if (key_ptr->is_null()) {
                        trx->error(Error::TypeCheck,
                                   "record-group-by: field /{} in 'record' {} must not be null",
                                   name_obj.name_sv(trx),
                                   i);
                    } else if (key_ptr->is_floating_point()) {
                        // NaN/Inf cannot be used as dict keys -- NaN != NaN makes
                        // the later get() fail even though put() silently succeeded.
                        if (key_ptr->is_floating_point_nan(trx)) {
                            trx->error(Error::NumericalNaN,
                                       "record-group-by: field /{} in 'record' {} is NaN and cannot be used as a dict key",
                                       name_obj.name_sv(trx),
                                       i);
                        } else if (key_ptr->is_floating_point_inf(trx)) {
                            trx->error(Error::NumericalINF,
                                       "record-group-by: field /{} in 'record' {} is Inf and cannot be used as a dict key",
                                       name_obj.name_sv(trx),
                                       i);
                        }
                    }
                    count_dict = trx->offset_to_ptr<Dict>(count_dict_offset);
                    auto existing = count_dict->get(trx, key_ptr);
                    if (existing != nullptr) {
                        existing->update_integer(existing->integer_value() + 1);
                    } else {
                        count_dict->put(trx, key_ptr->make_clone(trx), Object::make_integer(1));
                    }
                } else {
                    trx->error(Error::Undefined, "record-group-by: field /{} not found in 'record' {}", name_obj.name_sv(trx), i);
                }
            } else {
                trx->error(Error::TypeCheck, "record-group-by: element {} is not a 'record'", i);
            }
        }

        // Allocate result dict + arrays.  Honor m_curr_alloc_global like the
        // sibling record-to-dict: inside ${...} the per-group arrays land in
        // global VM (make_empty_array routes on the flag), so the result dict
        // must too -- otherwise a local dict holds global arrays and is
        // reclaimed by a save/restore below its alloc level.  (The count/idx
        // working dicts below stay local: they are recycled before return.)
        auto group_count = count_dict->length();
        auto [result_dict, result_dict_offset] =
                trx->m_curr_alloc_global ? Dict::create_global_dict(trx, group_count) : Dict::create_or_recycle(trx, group_count);

        *++trx->m_gc_roots_ptr = Object::make_dict(result_dict_offset);  // slot 3

        // Create empty arrays for each group
        auto entry_offset = nulloffset;
        integer_t bucket_idx = -1;
        while (true) {
            auto [next_offset, next_idx, key, count_obj] = count_dict->next(trx, entry_offset, bucket_idx);
            if (next_offset != nulloffset) {
                *key_root = key.make_clone(trx);  // rooted across the make_empty_array below
                auto arr = Object::make_empty_array(trx, static_cast<length_t>(count_obj.integer_value()));
                result_dict->put(trx, *key_root, arr);

                entry_offset = next_offset;
                bucket_idx = next_idx;
            } else {
                break;
            }
        }

        // Pass 2: populate arrays (use a write-index dict to track positions)
        auto [idx_dict, idx_dict_offset] = Dict::create_or_recycle(trx, group_count);

        *++trx->m_gc_roots_ptr = Object::make_dict(idx_dict_offset);  // slot 4

        // Initialize all indices to 0
        entry_offset = nulloffset;
        bucket_idx = -1;
        while (true) {
            auto [next_offset, next_idx, key, value] = result_dict->next(trx, entry_offset, bucket_idx);
            if (next_offset != nulloffset) {
                idx_dict->put(trx, key.make_clone(trx), Object::make_integer(0));
                entry_offset = next_offset;
                bucket_idx = next_idx;
            } else {
                break;
            }
        }

        // Place each record in its group
        for (length_t i = 0; i < rec_count; ++i) {
            records = arr_ptr->array_objects(trx);
            auto inst = records[i].record_instance(trx);
            auto schema = trx->offset_to_ptr<RecordSchema>(inst->m_schema);
            auto field_count = schema->m_field_count;

            auto fidx = record_find_field(trx, schema, field_count, name_obj);
            assert(fidx != field_count);

            auto key = inst->m_fields[fidx];

            auto arr_obj = result_dict->get(trx, &key);
            auto idx_obj = idx_dict->get(trx, &key);
            if ((arr_obj == nullptr) || (idx_obj == nullptr)) {
                trx->error(Error::InternalError, "record-group-by: key missing from result_dict/idx_dict in pass 2 (record {})", i);
            } else {
                auto write_idx = static_cast<length_t>(idx_obj->integer_value());
                auto arr_data = arr_obj->array_objects(trx);
                arr_data[write_idx] = records[i].make_clone(trx);
                idx_obj->update_integer(static_cast<integer_t>(write_idx + 1));
            }
        }

        // Recycle temporary working dicts (no-op if capacity > MaxDictPoolSize)
        Dict::recycle(trx, count_dict, count_dict_offset);
        Dict::recycle(trx, idx_dict, idx_dict_offset);

        *arr_ptr = Object::make_dict(result_dict_offset);

        trx->reset_gc_root(4);
    }
    --trx->m_op_ptr;
}

// record-values: record -- array
// Returns a new array containing copies of all field values in field order.
// throws: opstack-underflow, type-check, vm-full
static void record_values_op(Trix *trx) {
    trx->verify_operands(VerifyRecord);

    auto rec_ptr = trx->m_op_ptr;

    auto field_count = rec_ptr->object_length();
    auto [arr_storage, arr_offset] = trx->vm_alloc_dispatch_n<Object>(field_count, Trix::ChunkKind::Array);

    // Null-init + root the result array across the per-field make_clone loop: a
    // clone of an ExtValue field value allocates and (under ${...}) can fire a
    // global GC that walks this array's obj_count slots -- the array must be
    // reachable (else swept) and every not-yet-filled slot must read as a valid
    // Null Object (else the walker reads garbage).  The source record stays
    // rooted at rec_ptr, so inst (a pointer into its payload) stays valid.
    for (length_t i = 0; i < field_count; ++i) {
        arr_storage[i] = Object::make_null();
    }
    trx->gc_root_push_oneoff(Object::make_array(arr_offset, field_count));
    auto inst = rec_ptr->record_instance(trx);
    for (length_t i = 0; i < field_count; ++i) {
        arr_storage[i] = inst->m_fields[i].make_clone(trx);
    }

    *rec_ptr = Object::make_array(arr_offset, field_count);

    trx->gc_root_pop_oneoff();
}

// record-known: record name -- bool
// Tests whether the record's schema contains the named field.
// throws: opstack-underflow, type-check
static void record_known_op(Trix *trx) {
    trx->verify_operands(VerifyName, VerifyRecord);

    auto name_ptr = trx->m_op_ptr;
    auto rec_ptr = (name_ptr - 1);

    auto inst = rec_ptr->record_instance(trx);
    auto schema = trx->offset_to_ptr<RecordSchema>(inst->m_schema);
    auto field_count = schema->m_field_count;
    auto found = (record_find_field(trx, schema, field_count, *name_ptr) != field_count);

    *rec_ptr = Object::make_boolean(found);
    trx->m_op_ptr = rec_ptr;
}
