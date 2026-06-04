/*
 * Implements WHERE clause evaluation and row-source field resolution
 * for the sqlexec module.
 * The code stays with the executor because it operates directly on
 * sqlexec_program state and live row bindings.
 *
 * MIT License (see: LICENSE)
 * Copyright (C) 2026 tomaz stih
 */

#include "where.h"

#include <string.h>

static int like_matches_here(const char *text, const char *pattern)
{
    if (*pattern == '\0') {
        return *text == '\0';
    }
    if (*pattern == '%') {
        pattern++;
        if (*pattern == '\0') {
            return 1;
        }
        while (*text) {
            if (like_matches_here(text, pattern)) {
                return 1;
            }
            text++;
        }
        return like_matches_here(text, pattern);
    }
    if (*pattern == '_') {
        return *text != '\0' && like_matches_here(text + 1, pattern + 1);
    }
    return *text == *pattern && like_matches_here(text + 1, pattern + 1);
}

int qualifier_matches_source(const char *qualifier,
    const row_source *source)
{
    if (!source || qualifier[0] == '\0') {
        return qualifier[0] == '\0';
    }
    if (source->table_name
        && strcmp(qualifier, source->table_name) == 0) {
        return 1;
    }
    return source->alias && source->alias[0] != '\0'
        && strcmp(qualifier, source->alias) == 0;
}

int resolve_field_ref_n(const row_source *sources, unsigned char count,
    const char *qualifier, const char *name,
    const row_source **source_out, int *field_index_out)
{
    unsigned char i;
    int fi;
    const row_source *found_source = NULL;
    int found_index = -1;
    int ambiguous = 0;

    for (i = 0; i < count; i++) {
        if (!sources[i].fields) {
            continue;
        }
        fi = find_field_index(sources[i].fields, sources[i].field_count,
            name);
        if (fi < 0) {
            continue;
        }
        if (qualifier[0] != '\0') {
            if (!qualifier_matches_source(qualifier, &sources[i])) {
                continue;
            }
            *source_out = &sources[i];
            *field_index_out = fi;
            return 0;
        }
        if (found_source) {
            ambiguous = 1;
        }
        found_source = &sources[i];
        found_index  = fi;
    }

    if (ambiguous || !found_source) {
        return -1;
    }
    *source_out     = found_source;
    *field_index_out = found_index;
    return 0;
}

int resolve_field_ref(const row_source *left_source,
    const row_source *right_source, const char *qualifier,
    const char *name, const row_source **source_out,
    int *field_index_out)
{
    /* Implemented directly rather than via resolve_field_ref_n to
     * ensure *source_out points to the caller's originals, not to
     * local copies that would be invalid after return. */
    int left_index;
    int right_index;

    left_index = left_source
        ? find_field_index(left_source->fields,
            left_source->field_count, name) : -1;
    right_index = right_source
        ? find_field_index(right_source->fields,
            right_source->field_count, name) : -1;

    if (qualifier[0] != '\0') {
        if (left_source
            && qualifier_matches_source(qualifier, left_source)
            && left_index >= 0) {
            *source_out = left_source; *field_index_out = left_index;
            return 0;
        }
        if (right_source
            && qualifier_matches_source(qualifier, right_source)
            && right_index >= 0) {
            *source_out = right_source; *field_index_out = right_index;
            return 0;
        }
        return -1;
    }

    if (left_index >= 0 && right_index >= 0) return -1;
    if (left_index >= 0) {
        *source_out = left_source; *field_index_out = left_index; return 0;
    }
    if (right_index >= 0) {
        *source_out = right_source; *field_index_out = right_index; return 0;
    }
    return -1;
}

sql_truth_value truth_not_value(sql_truth_value value)
{
    if (value == sql_truth_true) {
        return sql_truth_false;
    }
    if (value == sql_truth_false) {
        return sql_truth_true;
    }
    return sql_truth_unknown;
}

sql_truth_value truth_and_value(sql_truth_value left,
    sql_truth_value right)
{
    if (left == sql_truth_false || right == sql_truth_false) {
        return sql_truth_false;
    }
    if (left == sql_truth_unknown || right == sql_truth_unknown) {
        return sql_truth_unknown;
    }
    return sql_truth_true;
}

sql_truth_value truth_or_value(sql_truth_value left,
    sql_truth_value right)
{
    if (left == sql_truth_true || right == sql_truth_true) {
        return sql_truth_true;
    }
    if (left == sql_truth_unknown || right == sql_truth_unknown) {
        return sql_truth_unknown;
    }
    return sql_truth_false;
}

sql_truth_value compare_text_values_truth(const char *left, char left_type,
    const char *right, char right_type, sql_compare_operator op)
{
    int ok_left;
    int ok_right;
    long left_number;
    long right_number;

    if (left[0] == '\0' || right[0] == '\0') {
        return sql_truth_unknown;
    }
    if (left_type == 'N' && right_type == 'N') {
        left_number = parse_integer_text(left, &ok_left);
        right_number = parse_integer_text(right, &ok_right);
        if (!ok_left || !ok_right) {
            return sql_truth_false;
        }
        return compare_longs(left_number, right_number, op)
            ? sql_truth_true : sql_truth_false;
    }
    return compare_strings(left, right, op)
        ? sql_truth_true : sql_truth_false;
}

sql_truth_value text_matches_like_truth(const char *left,
    const sql_value *value)
{
    if (value->type != sql_value_string) {
        return sql_truth_unknown;
    }
    return text_matches_like_text_truth(left, value->text);
}

sql_truth_value text_matches_like_text_truth(const char *left,
    const char *pattern)
{
    if (left[0] == '\0') {
        return sql_truth_unknown;
    }
    if (!pattern || pattern[0] == '\0') {
        return sql_truth_unknown;
    }
    return like_matches_here(left, pattern)
        ? sql_truth_true : sql_truth_false;
}

sql_truth_value field_matches_value_truth(const char *left, char field_type,
    const sql_value *value, sql_compare_operator op)
{
    int ok_left;
    int ok_right;
    long left_number;
    long right_number;

    if (left[0] == '\0' || value->type == sql_value_null) {
        return sql_truth_unknown;
    }
    if (field_type == 'N' && value->type == sql_value_number) {
        left_number = parse_integer_text(left, &ok_left);
        right_number = parse_integer_text(value->text, &ok_right);
        if (!ok_left || !ok_right) {
            return sql_truth_false;
        }
        return compare_longs(left_number, right_number, op)
            ? sql_truth_true : sql_truth_false;
    }
    return compare_strings(left, value->text, op)
        ? sql_truth_true : sql_truth_false;
}

int field_matches_value(const char *left, char field_type,
    const sql_value *value, sql_compare_operator op)
{
    return field_matches_value_truth(left, field_type, value, op)
        == sql_truth_true;
}

int field_values_equal(const char *left, char left_type,
    const char *right, char right_type)
{
    return compare_text_values_truth(left, left_type, right, right_type,
        sql_compare_equal)
        == sql_truth_true;
}

static void init_where_binding(where_binding *binding)
{
    unsigned char index;

    for (index = 0; index < sql_where_max_nodes; index++) {
        binding->left[index].source_index = sql_where_nil;
        binding->left[index].field_index = sql_where_nil;
    }
    for (index = 0; index < sql_where_max_values; index++) {
        binding->values[index].source_index = sql_where_nil;
        binding->values[index].field_index = sql_where_nil;
    }
}

static void unpack_bound_slot(unsigned char slot,
    where_bound_field *field_out)
{
    if (slot == sql_where_nil) {
        field_out->source_index = sql_where_nil;
        field_out->field_index = sql_where_nil;
        return;
    }
    field_out->source_index = (unsigned char)((slot >> 4) & 0x03u);
    field_out->field_index = (unsigned char)(slot & 0x0fu);
}

int where_can_use_program_binding(const sqlexec_program *program,
    const sql_where *where)
{
    return program && where && where->active
        && program->where_bound_slot_count == where->node_count;
}

static int program_has_source_masks(const sqlexec_program *program,
    const sql_where *where)
{
    return program && where && where->active
        && program->where_source_mask_count == where->node_count;
}

static void decode_program_bound_slots(const sqlexec_program *program,
    const sql_where *where, where_binding *binding)
{
    unsigned char index;

    for (index = 0; index < where->node_count; index++) {
        unpack_bound_slot(program->where_left_slots[index],
            &binding->left[index]);
    }
    for (index = 0; index < where->value_count; index++) {
        unpack_bound_slot(program->where_value_slots[index],
            &binding->values[index]);
    }
}

static void get_left_bound_field(const sqlexec_program *program,
    const sql_where *where, const where_binding *binding,
    unsigned char ref, where_bound_field *field_out)
{
    if (binding) {
        *field_out = binding->left[ref];
        return;
    }
    if (where_can_use_program_binding(program, where)) {
        unpack_bound_slot(program->where_left_slots[ref], field_out);
        return;
    }
    field_out->source_index = sql_where_nil;
    field_out->field_index = sql_where_nil;
}

static void get_value_bound_field(const sqlexec_program *program,
    const sql_where *where, const where_binding *binding,
    unsigned char value_ref, where_bound_field *field_out)
{
    if (binding) {
        *field_out = binding->values[value_ref];
        return;
    }
    if (where_can_use_program_binding(program, where)) {
        unpack_bound_slot(program->where_value_slots[value_ref], field_out);
        return;
    }
    field_out->source_index = sql_where_nil;
    field_out->field_index = sql_where_nil;
}

static int source_index_for_source(const row_source *sources,
    unsigned char source_count, const row_source *source)
{
    unsigned char index;

    for (index = 0; index < source_count; index++) {
        if (&sources[index] == source) {
            return index;
        }
    }
    return -1;
}

static int bind_field_ref_n(const row_source *sources,
    unsigned char source_count, const char *qualifier,
    const char *name, where_bound_field *field_out)
{
    const row_source *source;
    int source_index;
    int field_index;

    if (resolve_field_ref_n(sources, source_count, qualifier, name,
        &source, &field_index) != 0) {
        return -1;
    }
    source_index = source_index_for_source(sources, source_count, source);
    if (source_index < 0 || field_index < 0) {
        return -1;
    }
    field_out->source_index = (unsigned char)source_index;
    field_out->field_index = (unsigned char)field_index;
    return 0;
}

static int bind_operand_field(const row_source *sources,
    unsigned char source_count, const sql_predicate_operand *operand,
    where_bound_field *field_out)
{
    if (operand->kind != sql_predicate_operand_column) {
        return 1;
    }
    return bind_field_ref_n(sources, source_count,
        operand->data.column.qualifier, operand->data.column.name, field_out);
}

static int bound_field_value(const row_source *sources,
    const where_bound_field *field, char value_out[sql_value_size],
    char *type_out);

static int where_bind_ref_n(const sqlexec_program *program,
    const sql_where *where, unsigned char ref,
    const row_source *sources, unsigned char source_count,
    where_binding *binding)
{
    const sql_where_node *node;
    unsigned char index;

    if (!where || ref == (unsigned char)sql_where_nil
        || ref >= where->node_count) {
        return -1;
    }

    node = &program->where_nodes[ref];
    switch (node->type) {
    case sql_where_false:
        break;
    case sql_where_compare:
    case sql_where_in:
    case sql_where_like:
    case sql_where_is_null:
    case sql_where_quantified:
        if (bind_field_ref_n(sources, source_count, node->qualifier,
            node->column_name, &binding->left[ref]) != 0) {
            return -1;
        }
        for (index = 0; index < node->value_count; index++) {
            if (bind_operand_field(sources, source_count,
                &program->where_values[node->value_first + index],
                &binding->values[node->value_first + index]) < 0) {
                return -1;
            }
        }
        break;
    case sql_where_exists:
        break;
    case sql_where_not:
        if (node->left == (unsigned char)sql_where_nil) {
            return -1;
        }
        if (where_bind_ref_n(program, where, node->left, sources,
            source_count, binding) != 0) {
            return -1;
        }
        break;
    case sql_where_and:
    case sql_where_or:
        if (node->left == (unsigned char)sql_where_nil
            || node->right == (unsigned char)sql_where_nil
            || where_bind_ref_n(program, where, node->left, sources,
                source_count, binding) != 0
            || where_bind_ref_n(program, where, node->right, sources,
                source_count, binding) != 0) {
            return -1;
        }
        break;
    default:
        return -1;
    }
    return 0;
}

int where_bind_n(const sqlexec_program *program, const sql_where *where,
    const row_source *sources, unsigned char source_count,
    where_binding *binding)
{
    init_where_binding(binding);
    if (!where->active) {
        return 0;
    }
    if (where->root == (unsigned char)sql_where_nil
        || where->root >= where->node_count) {
        return -1;
    }
    if (where_can_use_program_binding(program, where)) {
        decode_program_bound_slots(program, where, binding);
        return 0;
    }
    return where_bind_ref_n(program, where, where->root, sources,
        source_count, binding);
}

int where_references_known_fields_n(const sqlexec_program *program,
    const sql_where *where,
    const row_source *sources, unsigned char source_count)
{
    where_binding binding;

    return where_bind_n(program, where, sources, source_count,
        &binding) == 0;
}

int where_references_known_fields(const sqlexec_program *program,
    const sql_where *where, const row_source *left_source,
    const row_source *right_source)
{
    row_source sources[2];
    unsigned char count;

    count = 0;
    if (left_source) {
        sources[count++] = *left_source;
    }
    if (right_source) {
        sources[count++] = *right_source;
    }
    return where_references_known_fields_n(program, where, sources, count);
}

int where_is_constant_false(const sql_where *where,
    const sql_where_node *nodes)
{
    if (!where->active
        || where->root == (unsigned char)sql_where_nil
        || where->root >= where->node_count) {
        return 0;
    }
    return nodes[where->root].type == sql_where_false;
}

static sql_truth_value where_in_values_truth(const sqlexec_program *program,
    const sql_where *where, const char *left, char field_type,
    const sql_predicate_operand *operands, unsigned char first,
    unsigned char count, const where_binding *binding,
    const row_source *sources)
{
    unsigned char index;
    sql_truth_value truth;
    int saw_unknown;
    char right[sql_value_size];
    char right_type;
    const sql_predicate_operand *operand;
    where_bound_field field;

    saw_unknown = 0;
    for (index = 0; index < count; index++) {
        operand = &operands[first + index];
        if (operand->kind == sql_predicate_operand_column) {
            get_value_bound_field(program, where, binding,
                (unsigned char)(first + index), &field);
            if (bound_field_value(sources, &field, right, &right_type)
                != 0) {
                return sql_truth_false;
            }
            truth = compare_text_values_truth(left, field_type, right,
                right_type, sql_compare_equal);
        } else {
            truth = field_matches_value_truth(left, field_type,
                &operand->data.value, sql_compare_equal);
        }
        if (truth == sql_truth_true) {
            return sql_truth_true;
        }
        if (truth == sql_truth_unknown) {
            saw_unknown = 1;
        }
    }
    return saw_unknown ? sql_truth_unknown : sql_truth_false;
}

static sql_truth_value where_quantified_truth(const char *left,
    char field_type, sql_compare_operator op, sql_quantifier quantifier,
    const sql_predicate_subquery_result *result)
{
    unsigned short index;
    sql_truth_value truth;
    int saw_unknown;

    if (quantifier == sql_quantifier_any && result->row_count == 0) {
        return sql_truth_false;
    }
    if (quantifier == sql_quantifier_all && result->row_count == 0) {
        return sql_truth_true;
    }

    saw_unknown = 0;
    for (index = 0; index < result->row_count; index++) {
        truth = field_matches_value_truth(left, field_type,
            &result->values[index], op);
        if (quantifier == sql_quantifier_any) {
            if (truth == sql_truth_true) {
                return sql_truth_true;
            }
        } else if (truth == sql_truth_false) {
            return sql_truth_false;
        }
        if (truth == sql_truth_unknown) {
            saw_unknown = 1;
        }
    }

    if (saw_unknown) {
        return sql_truth_unknown;
    }
    return quantifier == sql_quantifier_any
        ? sql_truth_false : sql_truth_true;
}

static const row_source *bound_field_source(const row_source *sources,
    const where_bound_field *field)
{
    if (field->source_index == sql_where_nil) {
        return NULL;
    }
    return &sources[field->source_index];
}

static int bound_field_value(const row_source *sources,
    const where_bound_field *field, char value_out[sql_value_size],
    char *type_out)
{
    const row_source *source;

    source = bound_field_source(sources, field);
    if (!source || field->field_index >= source->field_count) {
        return -1;
    }
    trim_field_value(value_out, sql_value_size,
        source->record + source->offsets[field->field_index],
        source->fields[field->field_index].length);
    *type_out = source->fields[field->field_index].type;
    return 0;
}

static int bound_field_is_null(const row_source *sources,
    const where_bound_field *field)
{
    const row_source *source;

    source = bound_field_source(sources, field);
    if (!source || field->field_index >= source->field_count) {
        return 0;
    }
    return field_is_null(source->record + source->offsets[field->field_index],
        source->fields[field->field_index].length);
}

static sql_truth_value where_node_truth_bound_n(
    const sqlexec_program *program, const sql_where *where,
    unsigned char ref, const where_binding *binding,
    const row_source *sources,
    const sql_predicate_subquery_cache *subqueries)
{
    const sql_where_node *node;
    const sql_predicate_operand *operand;
    const sql_predicate_subquery_result *subquery_result;
    char left[sql_value_size];
    char right[sql_value_size];
    char left_type;
    char right_type;
    int is_null;
    where_bound_field field;

    if (!where || ref == (unsigned char)sql_where_nil
        || ref >= where->node_count) {
        return sql_truth_false;
    }

    node = &program->where_nodes[ref];
    switch (node->type) {
    case sql_where_false:
        return sql_truth_false;
    case sql_where_compare:
        get_left_bound_field(program, where, binding, ref, &field);
        if (bound_field_value(sources, &field, left, &left_type) != 0) {
            return sql_truth_false;
        }
        operand = &program->where_values[node->value_first];
        if (operand->kind == sql_predicate_operand_column) {
            get_value_bound_field(program, where, binding,
                node->value_first, &field);
            if (bound_field_value(sources, &field, right, &right_type)
                != 0) {
                return sql_truth_false;
            }
            return compare_text_values_truth(left, left_type, right,
                right_type, node->operator);
        }
        return field_matches_value_truth(left, left_type,
            &operand->data.value, node->operator);
    case sql_where_in:
        get_left_bound_field(program, where, binding, ref, &field);
        if (bound_field_value(sources, &field, left, &left_type) != 0) {
            return sql_truth_false;
        }
        return where_in_values_truth(program, where, left, left_type,
            program->where_values, node->value_first, node->value_count,
            binding, sources);
    case sql_where_like:
        get_left_bound_field(program, where, binding, ref, &field);
        if (bound_field_value(sources, &field, left, &left_type) != 0) {
            return sql_truth_false;
        }
        operand = &program->where_values[node->value_first];
        (void)left_type;
        if (operand->kind == sql_predicate_operand_column) {
            get_value_bound_field(program, where, binding,
                node->value_first, &field);
            if (bound_field_value(sources, &field, right, &right_type)
                != 0) {
                return sql_truth_false;
            }
            if (right[0] == '\0' || left[0] == '\0') {
                return sql_truth_unknown;
            }
            return like_matches_here(left, right)
                ? sql_truth_true : sql_truth_false;
        }
        return text_matches_like_truth(left, &operand->data.value);
    case sql_where_is_null:
        get_left_bound_field(program, where, binding, ref, &field);
        is_null = bound_field_is_null(sources, &field);
        if (node->operator == sql_compare_not_equal) {
            return is_null ? sql_truth_false : sql_truth_true;
        }
        return is_null ? sql_truth_true : sql_truth_false;
    case sql_where_quantified:
        get_left_bound_field(program, where, binding, ref, &field);
        if (!subqueries || node->subquery_index >= subqueries->count
            || bound_field_value(sources, &field, left, &left_type) != 0) {
            return sql_truth_false;
        }
        subquery_result = &subqueries->results[node->subquery_index];
        return where_quantified_truth(left, left_type, node->operator,
            (sql_quantifier)node->quantifier, subquery_result);
    case sql_where_exists:
        if (!subqueries || node->subquery_index >= subqueries->count) {
            return sql_truth_false;
        }
        return subqueries->results[node->subquery_index].row_count > 0
            ? sql_truth_true : sql_truth_false;
    case sql_where_not:
        return truth_not_value(where_node_truth_bound_n(program, where,
            node->left, binding, sources, subqueries));
    case sql_where_and:
        return truth_and_value(
            where_node_truth_bound_n(program, where, node->left,
                binding, sources, subqueries),
            where_node_truth_bound_n(program, where, node->right,
                binding, sources, subqueries));
    case sql_where_or:
        return truth_or_value(
            where_node_truth_bound_n(program, where, node->left,
                binding, sources, subqueries),
            where_node_truth_bound_n(program, where, node->right,
                binding, sources, subqueries));
    default:
        return sql_truth_false;
    }
}

static unsigned char where_ready_depth(unsigned char mask)
{
    unsigned char depth;

    if (mask == 0) {
        return 0;
    }
    for (depth = sql_max_sources; depth > 0; depth--) {
        if ((unsigned char)(mask & (unsigned char)(1u << (depth - 1u)))
            != 0) {
            return (unsigned char)(depth - 1u);
        }
    }
    return 0;
}

static unsigned char bound_field_mask(const where_bound_field *field)
{
    if (field->source_index == sql_where_nil) {
        return 0;
    }
    return (unsigned char)(1u << field->source_index);
}

static unsigned char where_ref_source_mask(const sqlexec_program *program,
    const sql_where *where, const where_binding *binding, unsigned char ref)
{
    const sql_where_node *node;
    unsigned char index;
    unsigned char mask;

    if (!where || ref == (unsigned char)sql_where_nil
        || ref >= where->node_count) {
        return 0;
    }
    if (program_has_source_masks(program, where)) {
        return program->where_source_masks[ref];
    }
    if (!binding) {
        return 0;
    }

    node = &program->where_nodes[ref];
    switch (node->type) {
    case sql_where_false:
    case sql_where_exists:
        return 0;
    case sql_where_compare:
    case sql_where_in:
    case sql_where_like:
    case sql_where_is_null:
    case sql_where_quantified:
        mask = bound_field_mask(&binding->left[ref]);
        for (index = 0; index < node->value_count; index++) {
            mask = (unsigned char)(mask | bound_field_mask(
                &binding->values[node->value_first + index]));
        }
        return mask;
    case sql_where_not:
        return where_ref_source_mask(program, where, binding, node->left);
    case sql_where_and:
    case sql_where_or:
        return (unsigned char)(
            where_ref_source_mask(program, where, binding, node->left)
            | where_ref_source_mask(program, where, binding, node->right));
    default:
        return 0;
    }
}

static int where_append_conjuncts_bound(const sqlexec_program *program,
    const sql_where *where, const where_binding *binding, unsigned char ref,
    where_term *terms,
    unsigned char *term_count_out)
{
    const sql_where_node *node;
    unsigned char source_mask;

    node = &program->where_nodes[ref];
    if (node->type == sql_where_and) {
        if (where_append_conjuncts_bound(program, where, binding, node->left,
            terms, term_count_out) != 0
            || where_append_conjuncts_bound(program, where, binding,
                node->right, terms, term_count_out) != 0) {
            return -1;
        }
        return 0;
    }
    if (*term_count_out >= sql_where_max_nodes) {
        return -1;
    }
    source_mask = where_ref_source_mask(program, where, binding, ref);
    terms[*term_count_out].ref = ref;
    terms[*term_count_out].source_mask = source_mask;
    terms[*term_count_out].ready_depth = where_ready_depth(source_mask);
    (*term_count_out)++;
    return 0;
}

int where_matches_bound_n(const sqlexec_program *program,
    const sql_where *where, const where_binding *binding,
    const row_source *sources,
    const sql_predicate_subquery_cache *subqueries)
{
    if (!where->active) {
        return 1;
    }
    if (!binding && !where_can_use_program_binding(program, where)) {
        return 0;
    }
    return where_node_truth_bound_n(program, where, where->root,
        binding, sources, subqueries) == sql_truth_true;
}

int where_matches_ref_bound_n(const sqlexec_program *program,
    const sql_where *where, unsigned char ref,
    const where_binding *binding, const row_source *sources,
    const sql_predicate_subquery_cache *subqueries)
{
    if (!where->active) {
        return 1;
    }
    if (!binding && !where_can_use_program_binding(program, where)) {
        return 0;
    }
    return where_node_truth_bound_n(program, where, ref, binding, sources,
        subqueries) == sql_truth_true;
}

int where_matches_n(const sqlexec_program *program, const sql_where *where,
    const row_source *sources, unsigned char source_count,
    const sql_predicate_subquery_cache *subqueries)
{
    where_binding binding;

    if (where_bind_n(program, where, sources, source_count,
        &binding) != 0) {
        return 0;
    }
    return where_matches_bound_n(program, where, &binding, sources,
        subqueries);
}

int where_matches_ref_n(const sqlexec_program *program, const sql_where *where,
    unsigned char ref, const row_source *sources, unsigned char source_count,
    const sql_predicate_subquery_cache *subqueries)
{
    where_binding binding;

    if (where_bind_n(program, where, sources, source_count,
        &binding) != 0) {
        return 0;
    }
    return where_matches_ref_bound_n(program, where, ref, &binding, sources,
        subqueries);
}

int where_split_conjuncts_bound(const sqlexec_program *program,
    const sql_where *where, const where_binding *binding,
    where_term *terms,
    unsigned char *term_count_out)
{
    *term_count_out = 0;
    if (!where->active) {
        return 0;
    }
    if (!binding && !program_has_source_masks(program, where)) {
        return -1;
    }
    return where_append_conjuncts_bound(program, where, binding,
        where->root, terms, term_count_out);
}

int where_split_conjuncts_n(const sqlexec_program *program,
    const sql_where *where, const row_source *sources,
    unsigned char source_count, where_term *terms,
    unsigned char *term_count_out)
{
    where_binding binding;

    if (where_bind_n(program, where, sources, source_count,
        &binding) != 0) {
        return -1;
    }
    return where_split_conjuncts_bound(program, where, &binding, terms,
        term_count_out);
}

int where_matches(const sqlexec_program *program, const sql_where *where,
    const row_source *left_source, const row_source *right_source,
    const sql_predicate_subquery_cache *subqueries)
{
    row_source sources[2];
    unsigned char count;

    if (!where->active) {
        return 1;
    }
    count = 0;
    if (left_source) {
        sources[count++] = *left_source;
    }
    if (right_source) {
        sources[count++] = *right_source;
    }
    return where_matches_n(program, where, sources, count, subqueries);
}
