/*
 * Implements a tiny catalog-driven optimizer for SQL execution trees.
 * The code normalizes simple predicate forms in-place and then applies
 * conservative access-path rewrites, annotating table scans with
 * single-field index equality or range probes when safe.
 *
 * MIT License (see: LICENSE)
 * Copyright (C) 2026 tomaz stih
 */

#include "sqlopt.h"
#include "sqlctx.h"
#include "../common/common.h"
#include "../catalog/catalog.h"

#include <string.h>

typedef struct range_candidate {
    unsigned char used;
    char index_name[sql_name_size];
    sql_compare_operator lower_operator;
    sql_compare_operator upper_operator;
    unsigned char lower_ref;
    unsigned char upper_ref;
    sql_value lower_value;
    sql_value upper_value;
} range_candidate;

typedef struct predicate_source {
    const char *table_name;
    const char *alias;
} predicate_source;

typedef struct predicate_source_meta {
    const char *table_name;
    const char *alias;
    dbf_field fields[sql_max_columns];
    unsigned short field_count;
} predicate_source_meta;

/*
 * Returns 1 when the text contains exactly one field name (no commas),
 * copying it into field_name. Returns -1 when a comma is found or the
 * name is empty or too long.
 */
static int parse_single_field_name(const char *text, char *field_name)
{
    unsigned short index;

    if (!text || text[0] == '\0') {
        return -1;
    }
    index = 0;
    while (text[index] != '\0') {
        if (text[index] == ',') {
            return -1;
        }
        if ((unsigned short)(index + 1) >= 12u) {
            return -1;
        }
        field_name[index] = text[index];
        index++;
    }
    if (index == 0) {
        return -1;
    }
    field_name[index] = '\0';
    return 0;
}

static int operator_uses_index(sql_compare_operator operator)
{
    return operator == sql_compare_equal
        || operator == sql_compare_less
        || operator == sql_compare_less_equal
        || operator == sql_compare_greater
        || operator == sql_compare_greater_equal;
}

static int value_matches_field_type(const dbf_field *field,
    const sql_value *value)
{
    switch (field->type) {
    case 'C':
        return value->text[0] != '\0';
    case 'N':
        return value->type == sql_value_number;
    case 'D':
        return strlen(value->text) == 8u;
    default:
        return 0;
    }
}

static int find_matching_index(const char *root, const char *db_name,
    const char *table_name, const char *field_name, char *index_name);
static int compact_where_tree(sqlexec_program *program);
static void clear_where_source_masks(sqlexec_program *program);
static void clear_where_bound_slots(sqlexec_program *program);

static int like_has_wildcards(const char *text)
{
    while (*text != '\0') {
        if (*text == '%' || *text == '_') {
            return 1;
        }
        text++;
    }
    return 0;
}

static void make_false_node(sql_where_node *node)
{
    memset(node, 0, sizeof(*node));
    node->type = sql_where_false;
    node->left = (unsigned char)sql_where_nil;
    node->right = (unsigned char)sql_where_nil;
}

static int invert_compare_operator(sql_compare_operator in,
    sql_compare_operator *out)
{
    switch (in) {
    case sql_compare_equal:
        *out = sql_compare_not_equal;
        return 0;
    case sql_compare_not_equal:
        *out = sql_compare_equal;
        return 0;
    case sql_compare_less:
        *out = sql_compare_greater_equal;
        return 0;
    case sql_compare_less_equal:
        *out = sql_compare_greater;
        return 0;
    case sql_compare_greater:
        *out = sql_compare_less_equal;
        return 0;
    case sql_compare_greater_equal:
        *out = sql_compare_less;
        return 0;
    default:
        return -1;
    }
}

static int sql_value_equal(const sql_value *left, const sql_value *right)
{
    return left->type == right->type
        && strcmp(left->text, right->text) == 0;
}

static int operand_is_literal(const sql_predicate_operand *operand)
{
    return operand->kind == sql_predicate_operand_value;
}

static int predicate_operand_equal(const sql_predicate_operand *left,
    const sql_predicate_operand *right)
{
    if (left->kind != right->kind) {
        return 0;
    }
    if (left->kind == sql_predicate_operand_column) {
        return strcmp(left->data.column.qualifier,
                right->data.column.qualifier) == 0
            && strcmp(left->data.column.name, right->data.column.name) == 0;
    }
    return sql_value_equal(&left->data.value, &right->data.value);
}

static int sql_value_order(const sql_value *left, const sql_value *right,
    int *order_out)
{
    int ok_left;
    int ok_right;
    long left_number;
    long right_number;
    int order;

    if (left->type == sql_value_number && right->type == sql_value_number) {
        left_number = parse_integer_text(left->text, &ok_left);
        right_number = parse_integer_text(right->text, &ok_right);
        if (!ok_left || !ok_right) {
            return -1;
        }
        order = left_number < right_number ? -1
            : (left_number > right_number ? 1 : 0);
    } else {
        order = strcmp(left->text, right->text);
        if (order < 0) {
            order = -1;
        } else if (order > 0) {
            order = 1;
        }
    }
    *order_out = order;
    return 0;
}

static int node_names_match(const sql_where_node *left,
    const sql_where_node *right)
{
    return strcmp(left->column_name, right->column_name) == 0
        && strcmp(left->qualifier, right->qualifier) == 0;
}

static int operand_matches_node_column(const sql_where_node *node,
    const sql_predicate_operand *operand)
{
    return operand->kind == sql_predicate_operand_column
        && strcmp(node->column_name, operand->data.column.name) == 0
        && strcmp(node->qualifier, operand->data.column.qualifier) == 0;
}

static void rewrite_node_to_is_not_null(sql_where_node *node)
{
    node->type = sql_where_is_null;
    node->operator = sql_compare_not_equal;
    node->value_first = 0;
    node->value_count = 0;
}

static int node_requires_non_null(const sql_where_node *node)
{
    return node->type == sql_where_compare
        || node->type == sql_where_in
        || node->type == sql_where_like
        || node->type == sql_where_quantified;
}

static int operator_is_lower(sql_compare_operator op)
{
    return op == sql_compare_greater || op == sql_compare_greater_equal;
}

static int operator_is_upper(sql_compare_operator op)
{
    return op == sql_compare_less || op == sql_compare_less_equal;
}

static int equality_violates_bound(const sql_value *equal_value,
    sql_compare_operator bound_operator, const sql_value *bound_value)
{
    int order;

    if (sql_value_order(equal_value, bound_value, &order) != 0) {
        return 0;
    }
    if (bound_operator == sql_compare_greater) {
        return order <= 0;
    }
    if (bound_operator == sql_compare_greater_equal) {
        return order < 0;
    }
    if (bound_operator == sql_compare_less) {
        return order >= 0;
    }
    if (bound_operator == sql_compare_less_equal) {
        return order > 0;
    }
    return 0;
}

static int bounds_contradict(sql_compare_operator lower_operator,
    const sql_value *lower_value, sql_compare_operator upper_operator,
    const sql_value *upper_value)
{
    int order;

    if (sql_value_order(lower_value, upper_value, &order) != 0) {
        return 0;
    }
    if (order > 0) {
        return 1;
    }
    if (order < 0) {
        return 0;
    }
    return lower_operator == sql_compare_greater
        || upper_operator == sql_compare_less;
}

static int where_refs_equal(const sqlexec_program *program,
    unsigned char left_ref, unsigned char right_ref)
{
    const sql_where_node *left;
    const sql_where_node *right;
    unsigned char index;

    if (left_ref == right_ref) {
        return 1;
    }
    left = &program->where_nodes[left_ref];
    right = &program->where_nodes[right_ref];
    if (left->type != right->type) {
        return 0;
    }
    switch (left->type) {
    case sql_where_false:
        return 1;
    case sql_where_compare:
    case sql_where_like:
        if (!node_names_match(left, right)
            || left->operator != right->operator
            || left->value_count != right->value_count
            || left->value_count != 1u) {
            return 0;
        }
        return predicate_operand_equal(
            &program->where_values[left->value_first],
            &program->where_values[right->value_first]);
    case sql_where_in:
        if (!node_names_match(left, right)
            || left->value_count != right->value_count) {
            return 0;
        }
        for (index = 0; index < left->value_count; index++) {
            if (!predicate_operand_equal(
                &program->where_values[left->value_first + index],
                &program->where_values[right->value_first + index])) {
                return 0;
            }
        }
        return 1;
    case sql_where_is_null:
        return node_names_match(left, right)
            && left->operator == right->operator;
    case sql_where_quantified:
        return node_names_match(left, right)
            && left->operator == right->operator
            && left->quantifier == right->quantifier
            && left->subquery_index == right->subquery_index;
    case sql_where_exists:
        return left->subquery_index == right->subquery_index;
    case sql_where_not:
        return where_refs_equal(program, left->left, right->left);
    case sql_where_and:
    case sql_where_or:
        return where_refs_equal(program, left->left, right->left)
            && where_refs_equal(program, left->right, right->right);
    default:
        return 0;
    }
}

static int where_refs_contradict(const sqlexec_program *program,
    unsigned char left_ref, unsigned char right_ref)
{
    const sql_where_node *left;
    const sql_where_node *right;
    const sql_predicate_operand *left_value;
    const sql_predicate_operand *right_value;

    left = &program->where_nodes[left_ref];
    right = &program->where_nodes[right_ref];
    if (left->type == sql_where_false || right->type == sql_where_false) {
        return 1;
    }
    if (!node_names_match(left, right)) {
        return 0;
    }

    if (left->type == sql_where_is_null && right->type == sql_where_is_null) {
        return left->operator != right->operator;
    }
    if (left->type == sql_where_is_null
        && left->operator == sql_compare_equal
        && node_requires_non_null(right)) {
        return 1;
    }
    if (right->type == sql_where_is_null
        && right->operator == sql_compare_equal
        && node_requires_non_null(left)) {
        return 1;
    }
    if (left->type != sql_where_compare || right->type != sql_where_compare
        || left->value_count != 1u || right->value_count != 1u) {
        return 0;
    }

    left_value = &program->where_values[left->value_first];
    right_value = &program->where_values[right->value_first];
    if (!operand_is_literal(left_value) || !operand_is_literal(right_value)) {
        return 0;
    }
    if (left->operator == sql_compare_equal
        && right->operator == sql_compare_equal) {
        return !sql_value_equal(&left_value->data.value,
            &right_value->data.value);
    }
    if (left->operator == sql_compare_equal
        && right->operator == sql_compare_not_equal) {
        return sql_value_equal(&left_value->data.value,
            &right_value->data.value);
    }
    if (left->operator == sql_compare_not_equal
        && right->operator == sql_compare_equal) {
        return sql_value_equal(&left_value->data.value,
            &right_value->data.value);
    }
    if (left->operator == sql_compare_equal) {
        return equality_violates_bound(&left_value->data.value,
            right->operator, &right_value->data.value);
    }
    if (right->operator == sql_compare_equal) {
        return equality_violates_bound(&right_value->data.value,
            left->operator, &left_value->data.value);
    }
    if (operator_is_lower(left->operator) && operator_is_upper(right->operator)) {
        return bounds_contradict(left->operator, &left_value->data.value,
            right->operator, &right_value->data.value);
    }
    if (operator_is_upper(left->operator) && operator_is_lower(right->operator)) {
        return bounds_contradict(right->operator, &right_value->data.value,
            left->operator, &left_value->data.value);
    }
    return 0;
}

static void rewrite_compare_ref_to_equal(sqlexec_program *program,
    unsigned char ref)
{
    program->where_nodes[ref].type = sql_where_compare;
    program->where_nodes[ref].operator = sql_compare_equal;
}

static int where_compare_refs_redundant(sqlexec_program *program,
    unsigned char left_ref, unsigned char right_ref,
    unsigned char *remove_left_out, unsigned char *remove_right_out)
{
    sql_where_node *left;
    sql_where_node *right;
    const sql_predicate_operand *left_value;
    const sql_predicate_operand *right_value;
    int order;

    *remove_left_out = 0;
    *remove_right_out = 0;

    left = &program->where_nodes[left_ref];
    right = &program->where_nodes[right_ref];
    if (!node_names_match(left, right)
        || left->type != sql_where_compare
        || right->type != sql_where_compare
        || left->value_count != 1u || right->value_count != 1u) {
        return 0;
    }

    left_value = &program->where_values[left->value_first];
    right_value = &program->where_values[right->value_first];
    if (!operand_is_literal(left_value) || !operand_is_literal(right_value)) {
        return 0;
    }

    if (left->operator == sql_compare_equal) {
        if (right->operator == sql_compare_not_equal
            && !sql_value_equal(&left_value->data.value,
                &right_value->data.value)) {
            *remove_right_out = 1;
            return 1;
        }
        if (right->operator != sql_compare_not_equal
            && !equality_violates_bound(&left_value->data.value,
                right->operator, &right_value->data.value)) {
            *remove_right_out = 1;
            return 1;
        }
        return 0;
    }
    if (right->operator == sql_compare_equal) {
        if (left->operator == sql_compare_not_equal
            && !sql_value_equal(&left_value->data.value,
                &right_value->data.value)) {
            *remove_left_out = 1;
            return 1;
        }
        if (left->operator != sql_compare_not_equal
            && !equality_violates_bound(&right_value->data.value,
                left->operator, &left_value->data.value)) {
            *remove_left_out = 1;
            return 1;
        }
        return 0;
    }

    if (left->operator == sql_compare_not_equal
        || right->operator == sql_compare_not_equal) {
        return 0;
    }

    if (operator_is_lower(left->operator)
        && operator_is_lower(right->operator)) {
        if (sql_value_order(&left_value->data.value, &right_value->data.value,
            &order) != 0) {
            return 0;
        }
        if (order > 0 || (order == 0
                && left->operator == sql_compare_greater
                && right->operator == sql_compare_greater_equal)) {
            *remove_right_out = 1;
            return 1;
        }
        if (order < 0 || (order == 0
                && right->operator == sql_compare_greater
                && left->operator == sql_compare_greater_equal)) {
            *remove_left_out = 1;
            return 1;
        }
        return 0;
    }

    if (operator_is_upper(left->operator)
        && operator_is_upper(right->operator)) {
        if (sql_value_order(&left_value->data.value, &right_value->data.value,
            &order) != 0) {
            return 0;
        }
        if (order < 0 || (order == 0
                && left->operator == sql_compare_less
                && right->operator == sql_compare_less_equal)) {
            *remove_right_out = 1;
            return 1;
        }
        if (order > 0 || (order == 0
                && right->operator == sql_compare_less
                && left->operator == sql_compare_less_equal)) {
            *remove_left_out = 1;
            return 1;
        }
        return 0;
    }

    if (operator_is_lower(left->operator) && operator_is_upper(right->operator)
        && left->operator == sql_compare_greater_equal
        && right->operator == sql_compare_less_equal
        && sql_value_equal(&left_value->data.value, &right_value->data.value)) {
        rewrite_compare_ref_to_equal(program, left_ref);
        *remove_right_out = 1;
        return 1;
    }
    if (operator_is_upper(left->operator) && operator_is_lower(right->operator)
        && left->operator == sql_compare_less_equal
        && right->operator == sql_compare_greater_equal
        && sql_value_equal(&left_value->data.value, &right_value->data.value)) {
        rewrite_compare_ref_to_equal(program, right_ref);
        *remove_left_out = 1;
        return 1;
    }

    return 0;
}

static void compact_unique_refs(unsigned char *refs, unsigned char *count_io,
    const unsigned char *remove_flags)
{
    unsigned char read_index;
    unsigned char write_index;

    write_index = 0;
    for (read_index = 0; read_index < *count_io; read_index++) {
        if (!remove_flags[read_index]) {
            refs[write_index++] = refs[read_index];
        }
    }
    *count_io = write_index;
}

static int simplify_redundant_compare_refs(sqlexec_program *program,
    unsigned char *unique_refs, unsigned char *unique_count_io)
{
    unsigned char remove_flags[sql_where_max_nodes];
    unsigned char left_index;
    unsigned char right_index;
    unsigned char remove_left;
    unsigned char remove_right;
    int changed;

    do {
        changed = 0;
        memset(remove_flags, 0, sizeof(remove_flags));
        for (left_index = 0; left_index < *unique_count_io; left_index++) {
            if (remove_flags[left_index]) {
                continue;
            }
            for (right_index = (unsigned char)(left_index + 1u);
                right_index < *unique_count_io; right_index++) {
                if (remove_flags[right_index]) {
                    continue;
                }
                if (!where_compare_refs_redundant(program,
                    unique_refs[left_index], unique_refs[right_index],
                    &remove_left, &remove_right)) {
                    continue;
                }
                remove_flags[left_index] |= remove_left;
                remove_flags[right_index] |= remove_right;
                changed = changed || remove_left || remove_right;
            }
        }
        if (changed) {
            compact_unique_refs(unique_refs, unique_count_io, remove_flags);
        }
    } while (changed && *unique_count_io > 1u);

    return 0;
}

static int collect_and_refs(const sqlexec_program *program, unsigned char ref,
    unsigned char *leaf_refs, unsigned char *leaf_count_out,
    unsigned char *and_refs, unsigned char *and_count_out)
{
    const sql_where_node *node;

    node = &program->where_nodes[ref];
    if (node->type == sql_where_and) {
        if (*and_count_out >= sql_where_max_nodes) {
            return -1;
        }
        and_refs[(*and_count_out)++] = ref;
        if (collect_and_refs(program, node->left, leaf_refs, leaf_count_out,
            and_refs, and_count_out) != 0
            || collect_and_refs(program, node->right, leaf_refs,
                leaf_count_out, and_refs, and_count_out) != 0) {
            return -1;
        }
        return 0;
    }
    if (*leaf_count_out >= sql_where_max_nodes) {
        return -1;
    }
    leaf_refs[(*leaf_count_out)++] = ref;
    return 0;
}

static void rebuild_and_chain(sqlexec_program *program, unsigned char ref,
    const unsigned char *leaf_refs, unsigned char leaf_count,
    const unsigned char *and_refs)
{
    sql_where_node *node;
    unsigned char current_ref;
    unsigned char and_index;
    unsigned char remaining;

    if (leaf_count == 1u) {
        program->where_nodes[ref] = program->where_nodes[leaf_refs[0]];
        return;
    }

    current_ref = ref;
    and_index = 1u;
    remaining = leaf_count;
    while (remaining > 2u) {
        node = &program->where_nodes[current_ref];
        memset(node, 0, sizeof(*node));
        node->type = sql_where_and;
        node->left = and_refs[and_index++];
        node->right = leaf_refs[remaining - 1u];
        current_ref = node->left;
        remaining--;
    }

    node = &program->where_nodes[current_ref];
    memset(node, 0, sizeof(*node));
    node->type = sql_where_and;
    node->left = leaf_refs[0];
    node->right = leaf_refs[1];
}

static int simplify_and_ref(sqlexec_program *program, unsigned char ref)
{
    unsigned char leaf_refs[sql_where_max_nodes];
    unsigned char and_refs[sql_where_max_nodes];
    unsigned char unique_refs[sql_where_max_nodes];
    unsigned char leaf_count;
    unsigned char and_count;
    unsigned char unique_count;
    unsigned char left_index;
    unsigned char right_index;
    int duplicate;

    leaf_count = 0;
    and_count = 0;
    if (collect_and_refs(program, ref, leaf_refs, &leaf_count, and_refs,
        &and_count) != 0) {
        return -1;
    }

    unique_count = 0;
    for (left_index = 0; left_index < leaf_count; left_index++) {
        if (program->where_nodes[leaf_refs[left_index]].type
            == sql_where_false) {
            make_false_node(&program->where_nodes[ref]);
            return 0;
        }
        duplicate = 0;
        for (right_index = 0; right_index < unique_count; right_index++) {
            if (where_refs_equal(program, leaf_refs[left_index],
                unique_refs[right_index])) {
                duplicate = 1;
                break;
            }
        }
        if (!duplicate) {
            unique_refs[unique_count++] = leaf_refs[left_index];
        }
    }

    for (left_index = 0; left_index < unique_count; left_index++) {
        for (right_index = (unsigned char)(left_index + 1u);
            right_index < unique_count; right_index++) {
            if (where_refs_contradict(program, unique_refs[left_index],
                unique_refs[right_index])) {
                make_false_node(&program->where_nodes[ref]);
                return 0;
            }
        }
    }

    if (simplify_redundant_compare_refs(program, unique_refs,
        &unique_count) != 0) {
        return -1;
    }

    if (unique_count == 0u) {
        make_false_node(&program->where_nodes[ref]);
        return 0;
    }
    rebuild_and_chain(program, ref, unique_refs, unique_count, and_refs);
    return 0;
}

static int normalize_where_ref(sqlexec_program *program, unsigned char ref)
{
    sql_where_node *node;
    sql_where_node *child;
    sql_compare_operator inverted;
    const sql_predicate_operand *operand;

    node = &program->where_nodes[ref];
    if (node->type == sql_where_not) {
        if (node->left == (unsigned char)sql_where_nil) {
            return -1;
        }
        if (normalize_where_ref(program, node->left) != 0) {
            return -1;
        }
        child = &program->where_nodes[node->left];
        if (child->type == sql_where_not
            && child->left != (unsigned char)sql_where_nil) {
            *node = program->where_nodes[child->left];
            return normalize_where_ref(program, ref);
        }
        if (child->type == sql_where_compare
            && invert_compare_operator(child->operator, &inverted) == 0) {
            *node = *child;
            node->operator = inverted;
            return 0;
        }
        if (child->type == sql_where_is_null) {
            *node = *child;
            node->operator = child->operator == sql_compare_equal
                ? sql_compare_not_equal : sql_compare_equal;
        }
        return 0;
    }
    if (node->type == sql_where_and || node->type == sql_where_or) {
        if (node->left == (unsigned char)sql_where_nil
            || node->right == (unsigned char)sql_where_nil
            || normalize_where_ref(program, node->left) != 0
            || normalize_where_ref(program, node->right) != 0) {
            return -1;
        }
        if (node->type == sql_where_and) {
            return simplify_and_ref(program, ref);
        }
        if (program->where_nodes[node->left].type == sql_where_false) {
            *node = program->where_nodes[node->right];
            return 0;
        }
        if (program->where_nodes[node->right].type == sql_where_false) {
            *node = program->where_nodes[node->left];
            return 0;
        }
        if (where_refs_equal(program, node->left, node->right)) {
            *node = program->where_nodes[node->left];
        }
        return 0;
    }
    if (node->type == sql_where_in && node->value_count == 1u) {
        node->type = sql_where_compare;
        node->operator = sql_compare_equal;
        return 0;
    }
    if (node->type == sql_where_compare
        && node->value_count == 1u
        && node->value_first < program->where.value_count) {
        operand = &program->where_values[node->value_first];
        if (operand_matches_node_column(node, operand)) {
            if (node->operator == sql_compare_equal
                || node->operator == sql_compare_less_equal
                || node->operator == sql_compare_greater_equal) {
                rewrite_node_to_is_not_null(node);
            } else {
                make_false_node(node);
            }
            return 0;
        }
    }
    if (node->type == sql_where_like
        && node->value_count == 1u
        && node->value_first < program->where.value_count) {
        operand = &program->where_values[node->value_first];
        if (operand_matches_node_column(node, operand)) {
            rewrite_node_to_is_not_null(node);
            return 0;
        }
        if (operand->kind == sql_predicate_operand_value
            && operand->data.value.type == sql_value_string
            && !like_has_wildcards(operand->data.value.text)) {
            node->type = sql_where_compare;
            node->operator = sql_compare_equal;
        }
    }
    return 0;
}

static int normalize_where_tree(sqlexec_program *program)
{
    if (!program->where.active) {
        return 0;
    }
    if (program->where.root >= program->where.node_count
        || program->where.root == (unsigned char)sql_where_nil) {
        return -1;
    }
    if (normalize_where_ref(program, program->where.root) != 0) {
        return -1;
    }
    return compact_where_tree(program);
}

static int collect_conjunct_refs(const sqlexec_program *program,
    unsigned char ref, unsigned char *refs, unsigned char *count_out)
{
    const sql_where_node *node;
    unsigned char count;

    node = &program->where_nodes[ref];
    if (node->type == sql_where_and) {
        count = *count_out;
        if (collect_conjunct_refs(program, node->left, refs, &count) != 0
            || collect_conjunct_refs(program, node->right, refs, &count)
                != 0) {
            return -1;
        }
        *count_out = count;
        return 0;
    }
    if (*count_out >= sql_where_max_nodes) {
        return -1;
    }
    refs[(*count_out)++] = ref;
    return 0;
}

static int mark_where_usage(const sqlexec_program *program,
    unsigned char ref, unsigned char *node_used,
    unsigned char *value_used)
{
    const sql_where_node *node;
    unsigned char index;

    if (ref == (unsigned char)sql_where_nil || ref >= program->where.node_count) {
        return -1;
    }
    if (node_used[ref]) {
        return 0;
    }
    node_used[ref] = 1u;
    node = &program->where_nodes[ref];
    switch (node->type) {
    case sql_where_compare:
    case sql_where_like:
    case sql_where_in:
        if ((unsigned short)node->value_first + node->value_count
            > program->where.value_count) {
            return -1;
        }
        for (index = 0; index < node->value_count; index++) {
            value_used[node->value_first + index] = 1u;
        }
        return 0;
    case sql_where_is_null:
    case sql_where_quantified:
    case sql_where_exists:
    case sql_where_false:
        return 0;
    case sql_where_not:
        return mark_where_usage(program, node->left, node_used, value_used);
    case sql_where_and:
    case sql_where_or:
        if (mark_where_usage(program, node->left, node_used,
            value_used) != 0) {
            return -1;
        }
        return mark_where_usage(program, node->right, node_used, value_used);
    default:
        return -1;
    }
}

static int compact_where_tree(sqlexec_program *program)
{
    unsigned char node_used[sql_where_max_nodes];
    unsigned char value_used[sql_where_max_values];
    unsigned char node_map[sql_where_max_nodes];
    unsigned char value_map[sql_where_max_values];
    sql_where_node compact_nodes[sql_where_max_nodes];
    sql_predicate_operand compact_values[sql_where_max_values];
    sql_where_node having_nodes[sql_where_max_nodes];
    sql_predicate_operand having_values[sql_where_max_values];
    unsigned char having_node_count;
    unsigned char having_value_count;
    unsigned char old_index;
    unsigned char new_index;

    having_node_count = 0;
    having_value_count = 0;
    if (program->having.active) {
        if ((unsigned short)(program->having_node_first
                + program->having.node_count) > program->predicate_node_count
            || (unsigned short)(program->having_value_first
                + program->having.value_count)
                > program->predicate_value_count) {
            return -1;
        }
        having_node_count = program->having.node_count;
        having_value_count = program->having.value_count;
        memcpy(having_nodes, program_having_nodes(program),
            (unsigned short)(having_node_count * sizeof(having_nodes[0])));
        memcpy(having_values, program_having_values(program),
            (unsigned short)(having_value_count * sizeof(having_values[0])));
    }

    if (!program->where.active) {
        program->where.root = (unsigned char)sql_where_nil;
        program->where.node_count = 0;
        program->where.value_count = 0;
        program->having_node_first = 0;
        program->having_value_first = 0;
        program->predicate_node_count = having_node_count;
        program->predicate_value_count = having_value_count;
        if (program->having.active) {
            memcpy(program->where_nodes, having_nodes,
                (unsigned short)(having_node_count * sizeof(having_nodes[0])));
            memcpy(program->where_values, having_values,
                (unsigned short)(having_value_count
                    * sizeof(having_values[0])));
        }
        return 0;
    }
    if (program->where.root == (unsigned char)sql_where_nil
        || program->where.root >= program->where.node_count) {
        return -1;
    }

    memset(node_used, 0, sizeof(node_used));
    memset(value_used, 0, sizeof(value_used));
    if (mark_where_usage(program, program->where.root, node_used,
        value_used) != 0) {
        return -1;
    }

    memset(node_map, sql_where_nil, sizeof(node_map));
    memset(value_map, sql_where_nil, sizeof(value_map));
    new_index = 0;
    for (old_index = 0; old_index < program->where.node_count; old_index++) {
        if (node_used[old_index]) {
            node_map[old_index] = new_index++;
        }
    }
    program->where.node_count = new_index;

    new_index = 0;
    for (old_index = 0; old_index < program->where.value_count; old_index++) {
        if (value_used[old_index]) {
            value_map[old_index] = new_index++;
        }
    }
    program->where.value_count = new_index;

    memset(compact_nodes, 0, sizeof(compact_nodes));
    memset(compact_values, 0, sizeof(compact_values));
    for (old_index = 0; old_index < sql_where_max_values; old_index++) {
        if (value_used[old_index]) {
            compact_values[value_map[old_index]]
                = program->where_values[old_index];
        }
    }
    for (old_index = 0; old_index < sql_where_max_nodes; old_index++) {
        sql_where_node *node;

        if (!node_used[old_index]) {
            continue;
        }
        compact_nodes[node_map[old_index]] = program->where_nodes[old_index];
        node = &compact_nodes[node_map[old_index]];
        if (node->left != (unsigned char)sql_where_nil) {
            node->left = node_map[node->left];
        }
        if (node->right != (unsigned char)sql_where_nil) {
            node->right = node_map[node->right];
        }
        if (node->value_count > 0u) {
            node->value_first = value_map[node->value_first];
        }
    }

    memcpy(program->where_nodes, compact_nodes, sizeof(compact_nodes));
    memcpy(program->where_values, compact_values, sizeof(compact_values));
    program->where.root = node_map[program->where.root];
    program->having_node_first = program->where.node_count;
    program->having_value_first = program->where.value_count;
    if (program->having.active) {
        memcpy(program->where_nodes + program->having_node_first,
            having_nodes,
            (unsigned short)(having_node_count * sizeof(having_nodes[0])));
        memcpy(program->where_values + program->having_value_first,
            having_values,
            (unsigned short)(having_value_count
                * sizeof(having_values[0])));
    }
    program->predicate_node_count = (unsigned char)(
        program->where.node_count + having_node_count);
    program->predicate_value_count = (unsigned char)(
        program->where.value_count + having_value_count);
    return 0;
}

static int ref_list_contains(const unsigned char *refs, unsigned char count,
    unsigned char ref)
{
    unsigned char index;

    for (index = 0; index < count; index++) {
        if (refs[index] == ref) {
            return 1;
        }
    }
    return 0;
}

static int prune_top_level_conjuncts(sqlexec_program *program,
    const unsigned char *remove_refs, unsigned char remove_count)
{
    unsigned char leaf_refs[sql_where_max_nodes];
    unsigned char and_refs[sql_where_max_nodes];
    unsigned char keep_refs[sql_where_max_nodes];
    unsigned char leaf_count;
    unsigned char and_count;
    unsigned char keep_count;
    unsigned char index;

    if (!program->where.active || remove_count == 0u) {
        return 0;
    }
    if (program->where.root == (unsigned char)sql_where_nil
        || program->where.root >= program->where.node_count) {
        return -1;
    }

    leaf_count = 0;
    and_count = 0;
    if (collect_and_refs(program, program->where.root, leaf_refs,
        &leaf_count, and_refs, &and_count) != 0) {
        return -1;
    }

    keep_count = 0;
    for (index = 0; index < leaf_count; index++) {
        if (!ref_list_contains(remove_refs, remove_count, leaf_refs[index])) {
            keep_refs[keep_count++] = leaf_refs[index];
        }
    }

    if (keep_count == leaf_count) {
        return 0;
    }
    if (keep_count == 0u) {
        program->where.active = 0;
        return compact_where_tree(program);
    }

    rebuild_and_chain(program, program->where.root, keep_refs, keep_count,
        and_refs);
    return compact_where_tree(program);
}

static int text_value_order(char field_type, const sql_value *left,
    const sql_value *right, int *order_out)
{
    int ok_left;
    int ok_right;
    long left_number;
    long right_number;
    int order;

    if (field_type == 'N') {
        left_number = parse_integer_text(left->text, &ok_left);
        right_number = parse_integer_text(right->text, &ok_right);
        if (!ok_left || !ok_right) {
            return -1;
        }
        order = left_number < right_number ? -1
            : (left_number > right_number ? 1 : 0);
    } else {
        order = strcmp(left->text, right->text);
        if (order < 0) {
            order = -1;
        } else if (order > 0) {
            order = 1;
        }
    }
    *order_out = order;
    return 0;
}

static int lower_bound_is_stronger(char field_type,
    sql_compare_operator new_operator, const sql_value *new_value,
    sql_compare_operator old_operator, const sql_value *old_value)
{
    int order;

    if (text_value_order(field_type, new_value, old_value, &order) != 0) {
        return 0;
    }
    if (order > 0) {
        return 1;
    }
    if (order < 0) {
        return 0;
    }
    return new_operator == sql_compare_greater
        && old_operator == sql_compare_greater_equal;
}

static int upper_bound_is_stronger(char field_type,
    sql_compare_operator new_operator, const sql_value *new_value,
    sql_compare_operator old_operator, const sql_value *old_value)
{
    int order;

    if (text_value_order(field_type, new_value, old_value, &order) != 0) {
        return 0;
    }
    if (order < 0) {
        return 1;
    }
    if (order > 0) {
        return 0;
    }
    return new_operator == sql_compare_less
        && old_operator == sql_compare_less_equal;
}

static int extract_indexable_compare(const sqlexec_program *program,
    unsigned char ref, const dbf_field *fields, unsigned short field_count,
    const char *root, const char *db_name, const char *table_name,
    int exact_only, char *field_name_out, int *field_index_out,
    char *index_name_out, sql_compare_operator *operator_out,
    sql_value *value_out)
{
    const sql_where_node *node;
    const sql_predicate_operand *operand;
    int field_index;

    node = &program->where_nodes[ref];
    if (node->type != sql_where_compare || node->value_count != 1
        || !operator_uses_index(node->operator)
        || node->value_first >= program->where.value_count) {
        return 0;
    }
    if (exact_only && node->operator != sql_compare_equal) {
        return 0;
    }
    operand = &program->where_values[node->value_first];
    if (!operand_is_literal(operand)) {
        return 0;
    }
    field_index = find_field_index(fields, field_count, node->column_name);
    if (field_index < 0 || !value_matches_field_type(&fields[field_index],
        &operand->data.value)) {
        return 0;
    }
    if (find_matching_index(root, db_name, table_name, node->column_name,
        index_name_out) != 0) {
        return 0;
    }
    copy_name(field_name_out, node->column_name);
    *field_index_out = field_index;
    *operator_out = node->operator;
    *value_out = operand->data.value;
    return 1;
}

static const char *find_plan_table_name(const sqlexec_program *program)
{
    if (program->root == sqlexec_nil) {
        return NULL;
    }
    if (program->table_name[0] == '\0') {
        return NULL;
    }
    return program->table_name;
}

static sqlexec_ref find_plan_scan_ref(const sqlexec_program *program)
{
    const sqlexec_node *node;

    if (!program || program->root == sqlexec_nil) {
        return sqlexec_nil;
    }
    node = sqlexec_get_const(program, program->root);
    if (!node) {
        return sqlexec_nil;
    }
    if (node->opcode == sqlexec_project
        || node->opcode == sqlexec_write_current
        || node->opcode == sqlexec_delete_current) {
        return node->first_child;
    }
    return program->root;
}

static unsigned char all_source_mask(unsigned char source_count)
{
    if (source_count == 0) {
        return 0;
    }
    return (unsigned char)((1u << source_count) - 1u);
}

static int collect_plan_sources(const sqlexec_program *program,
    predicate_source *sources, unsigned char *source_count_out)
{
    const sqlexec_node *scan_node;
    sqlexec_ref scan_ref;
    unsigned char index;
    unsigned char source_count;

    *source_count_out = 0;
    scan_ref = find_plan_scan_ref(program);
    scan_node = sqlexec_get_const(program, scan_ref);
    if (!scan_node) {
        return -1;
    }

    if (scan_node->opcode == sqlexec_table_scan) {
        if (program->table_name[0] == '\0') {
            return -1;
        }
        sources[0].table_name = program->table_name;
        sources[0].alias = "";
        *source_count_out = 1;
        return 0;
    }
    if (scan_node->opcode != sqlexec_join_scan) {
        return -1;
    }

    source_count = join_source_count(scan_node->data.join);
    if (source_count == 0 || source_count > sql_max_sources) {
        return -1;
    }
    for (index = 0; index < source_count; index++) {
        sources[index].table_name =
            join_table_at(program, scan_node->data.join, index);
        sources[index].alias =
            join_alias_at(program, scan_node->data.join, index);
    }
    *source_count_out = source_count;
    return 0;
}

static int collect_plan_source_meta(const sqlexec_program *program,
    const char *root, const char *db_name, predicate_source_meta *sources,
    unsigned char *source_count_out)
{
    const sqlexec_node *scan_node;
    sqlexec_ref scan_ref;
    unsigned char index;
    unsigned char source_count;

    *source_count_out = 0;
    if (!root || !root[0] || !db_name || !db_name[0]) {
        return -1;
    }
    scan_ref = find_plan_scan_ref(program);
    scan_node = sqlexec_get_const(program, scan_ref);
    if (!scan_node) {
        return -1;
    }

    if (scan_node->opcode == sqlexec_table_scan) {
        if (program->table_name[0] == '\0'
            || open_table_fields(root, db_name, program->table_name,
                sources[0].fields, &sources[0].field_count) != 0) {
            return -1;
        }
        sources[0].table_name = program->table_name;
        sources[0].alias = "";
        *source_count_out = 1;
        return 0;
    }
    if (scan_node->opcode != sqlexec_join_scan) {
        return -1;
    }

    source_count = join_source_count(scan_node->data.join);
    if (source_count == 0 || source_count > sql_max_sources) {
        return -1;
    }
    for (index = 0; index < source_count; index++) {
        sources[index].table_name =
            join_table_at(program, scan_node->data.join, index);
        sources[index].alias =
            join_alias_at(program, scan_node->data.join, index);
        if (open_table_fields(root, db_name, sources[index].table_name,
            sources[index].fields, &sources[index].field_count) != 0) {
            return -1;
        }
    }
    *source_count_out = source_count;
    return 0;
}

static unsigned char qualifier_source_mask(const predicate_source *sources,
    unsigned char source_count, const char *qualifier)
{
    unsigned char index;

    if (source_count == 0) {
        return 0;
    }
    if (qualifier[0] == '\0') {
        return source_count == 1 ? 1u : all_source_mask(source_count);
    }
    for (index = 0; index < source_count; index++) {
        if (strcmp(qualifier, sources[index].table_name) == 0
            || (sources[index].alias[0] != '\0'
                && strcmp(qualifier, sources[index].alias) == 0)) {
            return (unsigned char)(1u << index);
        }
    }
    return all_source_mask(source_count);
}

static unsigned char operand_source_mask(const predicate_source *sources,
    unsigned char source_count, const sql_predicate_operand *operand)
{
    if (operand->kind != sql_predicate_operand_column) {
        return 0;
    }
    return qualifier_source_mask(sources, source_count,
        operand->data.column.qualifier);
}

static unsigned char pack_bound_slot(unsigned char source_index,
    unsigned char field_index)
{
    if (source_index >= sql_max_sources || field_index >= sql_max_columns) {
        return sql_where_nil;
    }
    return (unsigned char)((source_index << 4) | field_index);
}

static int resolve_meta_field_ref(
    const predicate_source_meta *sources, unsigned char source_count,
    const char *qualifier, const char *name, unsigned char *source_index_out,
    unsigned char *field_index_out)
{
    unsigned char index;
    int field_index;
    int found_source;
    int found_field;

    found_source = -1;
    found_field = -1;
    for (index = 0; index < source_count; index++) {
        if (qualifier[0] != '\0'
            && strcmp(qualifier, sources[index].table_name) != 0
            && (sources[index].alias[0] == '\0'
                || strcmp(qualifier, sources[index].alias) != 0)) {
            continue;
        }
        field_index = find_field_index(sources[index].fields,
            sources[index].field_count, name);
        if (field_index < 0) {
            continue;
        }
        if (qualifier[0] == '\0' && found_source >= 0) {
            return -1;
        }
        found_source = index;
        found_field = field_index;
        if (qualifier[0] != '\0') {
            break;
        }
    }
    if (found_source < 0 || found_field < 0) {
        return -1;
    }
    *source_index_out = (unsigned char)found_source;
    *field_index_out = (unsigned char)found_field;
    return 0;
}

static unsigned char annotate_where_source_mask_ref(sqlexec_program *program,
    const sql_where *where, unsigned char ref,
    const predicate_source *sources, unsigned char source_count)
{
    const sql_where_node *node;
    unsigned char index;
    unsigned char mask;

    if (!where || ref == (unsigned char)sql_where_nil
        || ref >= where->node_count) {
        return 0;
    }

    node = &program->where_nodes[ref];
    switch (node->type) {
    case sql_where_false:
    case sql_where_exists:
        mask = 0;
        break;
    case sql_where_compare:
    case sql_where_in:
    case sql_where_like:
    case sql_where_is_null:
    case sql_where_quantified:
        mask = qualifier_source_mask(sources, source_count, node->qualifier);
        for (index = 0; index < node->value_count; index++) {
            mask = (unsigned char)(mask | operand_source_mask(sources,
                source_count,
                &program->where_values[node->value_first + index]));
        }
        break;
    case sql_where_not:
        mask = annotate_where_source_mask_ref(program, where, node->left,
            sources, source_count);
        break;
    case sql_where_and:
    case sql_where_or:
        mask = (unsigned char)(
            annotate_where_source_mask_ref(program, where, node->left,
                sources, source_count)
            | annotate_where_source_mask_ref(program, where, node->right,
                sources, source_count));
        break;
    default:
        mask = 0;
        break;
    }

    program->where_source_masks[ref] = mask;
    return mask;
}

static void clear_where_source_masks(sqlexec_program *program)
{
    program->where_source_mask_count = 0;
    memset(program->where_source_masks, 0, sizeof(program->where_source_masks));
}

static void clear_where_bound_slots(sqlexec_program *program)
{
    program->where_bound_slot_count = 0;
    memset(program->where_left_slots, sql_where_nil,
        sizeof(program->where_left_slots));
    memset(program->where_value_slots, sql_where_nil,
        sizeof(program->where_value_slots));
}

static void annotate_where_source_masks(sqlexec_program *program)
{
    predicate_source sources[sql_max_sources];
    unsigned char source_count;

    clear_where_source_masks(program);
    if (!program->where.active
        || program->where.root == (unsigned char)sql_where_nil
        || program->where.root >= program->where.node_count
        || collect_plan_sources(program, sources, &source_count) != 0) {
        return;
    }
    (void)annotate_where_source_mask_ref(program, &program->where,
        program->where.root, sources, source_count);
    program->where_source_mask_count = program->where.node_count;
}

static int bind_where_slots_ref(sqlexec_program *program,
    const sql_where *where, unsigned char ref,
    const predicate_source_meta *sources, unsigned char source_count)
{
    const sql_where_node *node;
    const sql_predicate_operand *operand;
    unsigned char index;
    unsigned char source_index;
    unsigned char field_index;

    if (!where || ref == (unsigned char)sql_where_nil
        || ref >= where->node_count) {
        return -1;
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
        if (resolve_meta_field_ref(sources, source_count, node->qualifier,
            node->column_name, &source_index, &field_index) != 0) {
            return -1;
        }
        program->where_left_slots[ref] = pack_bound_slot(source_index,
            field_index);
        for (index = 0; index < node->value_count; index++) {
            operand = &program->where_values[node->value_first + index];
            if (operand->kind != sql_predicate_operand_column) {
                continue;
            }
            if (resolve_meta_field_ref(sources, source_count,
                operand->data.column.qualifier, operand->data.column.name,
                &source_index, &field_index) != 0) {
                return -1;
            }
            program->where_value_slots[node->value_first + index] =
                pack_bound_slot(source_index, field_index);
        }
        return 0;
    case sql_where_not:
        return bind_where_slots_ref(program, where, node->left, sources,
            source_count);
    case sql_where_and:
    case sql_where_or:
        return bind_where_slots_ref(program, where, node->left, sources,
            source_count) != 0
            || bind_where_slots_ref(program, where, node->right, sources,
                source_count) != 0 ? -1 : 0;
    default:
        return -1;
    }
}

static void annotate_where_bound_slots(sqlexec_program *program,
    const char *root, const char *db_name)
{
    predicate_source_meta sources[sql_max_sources];
    unsigned char source_count;

    clear_where_bound_slots(program);
    if (!program->where.active
        || program->where.root == (unsigned char)sql_where_nil
        || program->where.root >= program->where.node_count
        || collect_plan_source_meta(program, root, db_name, sources,
            &source_count) != 0
        || bind_where_slots_ref(program, &program->where, program->where.root,
            sources, source_count) != 0) {
        clear_where_bound_slots(program);
        return;
    }
    program->where_bound_slot_count = program->where.node_count;
}

/*
 * Searches the index catalog for a single-field index on field_name
 * belonging to (db_name, table_name). Writes the index name into
 * index_name on success. Returns 0 when found, 1 when not found, and
 * the catalog cannot be opened, -1 on a read error.
 */
static int find_matching_index(const char *root, const char *db_name,
    const char *table_name, const char *field_name, char *index_name)
{
    dbf_file file;
    char catalog_path[path_buffer_size];
    char record[index_catalog_record_length];
    char record_db[index_catalog_db_length + 1];
    char record_name[index_catalog_name_length + 1];
    char record_table[index_catalog_table_length + 1];
    char record_fields[index_catalog_fields_length + 1];
    char indexed_field[12];
    int state;
    unsigned long index;

    if (ensure_index_catalog(root, catalog_path) != 0) {
        return 1;
    }
    if (dbf_open(&file, catalog_path) != 0) {
        return 1;
    }
    for (index = 0; index < file.record_count; index++) {
        state = dbf_read(&file, index, record);
        if (state < 0) {
            dbf_close(&file);
            return 1;
        }
        if (state == 1) {
            continue;
        }
        get_field(record_db, sizeof(record_db), record,
            index_catalog_db_length);
        get_field(record_name, sizeof(record_name),
            record + index_catalog_db_length, index_catalog_name_length);
        get_field(record_table, sizeof(record_table),
            record + index_catalog_db_length + index_catalog_name_length,
            index_catalog_table_length);
        get_field(record_fields, sizeof(record_fields),
            record + index_catalog_db_length + index_catalog_name_length
                + index_catalog_table_length,
            index_catalog_fields_length);
        if (strcmp(record_db, db_name) != 0
            || strcmp(record_table, table_name) != 0) {
            continue;
        }
        if (parse_single_field_name(record_fields, indexed_field) != 0) {
            continue;
        }
        if (strcmp(indexed_field, field_name) != 0) {
            continue;
        }
        copy_name(index_name, record_name);
        dbf_close(&file);
        return 0;
    }
    dbf_close(&file);
    return 1;
}

static void reset_scan_access(sqlexec_node *scan_node)
{
    memset(&scan_node->data.scan, 0, sizeof(scan_node->data.scan));
    scan_node->data.scan.access_kind = sqlexec_scan_full;
    scan_node->data.scan.lower_operator = sql_compare_invalid;
    scan_node->data.scan.upper_operator = sql_compare_invalid;
}

static int annotate_eq_scan(sqlexec_program *program, sqlexec_ref scan_ref,
    const char *index_name, const sql_value *value)
{
    sqlexec_node *scan_node;

    scan_node = sqlexec_get(program, scan_ref);
    if (!scan_node || scan_node->opcode != sqlexec_table_scan) {
        return -1;
    }
    reset_scan_access(scan_node);
    scan_node->data.scan.access_kind = sqlexec_scan_index_eq;
    copy_name(scan_node->data.scan.index_name, index_name);
    scan_node->data.scan.lower_value = *value;
    return 0;
}

static int annotate_range_scan(sqlexec_program *program, sqlexec_ref scan_ref,
    const char *index_name, const range_candidate *candidate)
{
    sqlexec_node *scan_node;

    scan_node = sqlexec_get(program, scan_ref);
    if (!scan_node || scan_node->opcode != sqlexec_table_scan) {
        return -1;
    }
    reset_scan_access(scan_node);
    scan_node->data.scan.access_kind = sqlexec_scan_index_range;
    copy_name(scan_node->data.scan.index_name, index_name);
    scan_node->data.scan.lower_operator = candidate->lower_operator;
    scan_node->data.scan.upper_operator = candidate->upper_operator;
    scan_node->data.scan.lower_value = candidate->lower_value;
    scan_node->data.scan.upper_value = candidate->upper_value;
    return 0;
}

static int choose_access_path(sqlexec_program *program, sqlexec_ref scan_ref,
    const dbf_field *fields, unsigned short field_count, const char *root,
    const char *db_name, const char *table_name)
{
    unsigned char refs[sql_where_max_nodes];
    unsigned char remove_refs[2];
    unsigned char remove_count;
    unsigned char ref_count;
    range_candidate ranges[sql_max_columns];
    unsigned char ref_index;
    char field_name[sql_name_size];
    char index_name[sql_name_size];
    sql_compare_operator operator;
    sql_value value;
    int field_index;
    range_candidate *range;

    if (!program->where.active) {
        return 0;
    }

    {
        sqlexec_node *scan_node;

        scan_node = sqlexec_get(program, scan_ref);
        if (!scan_node || scan_node->opcode != sqlexec_table_scan) {
            return -1;
        }
        reset_scan_access(scan_node);
    }

    ref_count = 0;
    remove_count = 0;
    if (collect_conjunct_refs(program, program->where.root, refs,
        &ref_count) != 0) {
        return -1;
    }

    memset(ranges, 0, sizeof(ranges));
    for (ref_index = 0; ref_index < ref_count; ref_index++) {
        if (!extract_indexable_compare(program, refs[ref_index], fields,
            field_count, root, db_name, table_name, 0, field_name,
            &field_index, index_name, &operator, &value)) {
            continue;
        }
        if (operator == sql_compare_equal) {
            if (annotate_eq_scan(program, scan_ref, index_name, &value) != 0) {
                return -1;
            }
            remove_refs[0] = refs[ref_index];
            return prune_top_level_conjuncts(program, remove_refs, 1u);
        }
        range = &ranges[field_index];
        if (!range->used) {
            memset(range, 0, sizeof(*range));
            range->used = 1;
            copy_name(range->index_name, index_name);
            range->lower_operator = sql_compare_invalid;
            range->upper_operator = sql_compare_invalid;
            range->lower_ref = (unsigned char)sql_where_nil;
            range->upper_ref = (unsigned char)sql_where_nil;
        }
        if (operator == sql_compare_greater
            || operator == sql_compare_greater_equal) {
            if (range->lower_operator == sql_compare_invalid
                || lower_bound_is_stronger(fields[field_index].type,
                    operator, &value, range->lower_operator,
                    &range->lower_value)) {
                range->lower_operator = operator;
                range->lower_value = value;
                range->lower_ref = refs[ref_index];
            }
        } else if (operator == sql_compare_less
            || operator == sql_compare_less_equal) {
            if (range->upper_operator == sql_compare_invalid
                || upper_bound_is_stronger(fields[field_index].type,
                    operator, &value, range->upper_operator,
                    &range->upper_value)) {
                range->upper_operator = operator;
                range->upper_value = value;
                range->upper_ref = refs[ref_index];
            }
        }
    }

    for (field_index = 0; field_index < sql_max_columns; field_index++) {
        if (!ranges[field_index].used) {
            continue;
        }
        if (ranges[field_index].lower_operator != sql_compare_invalid
            || ranges[field_index].upper_operator != sql_compare_invalid) {
            if (annotate_range_scan(program, scan_ref,
                ranges[field_index].index_name, &ranges[field_index]) != 0) {
                return -1;
            }
            if (ranges[field_index].lower_ref != (unsigned char)sql_where_nil) {
                remove_refs[remove_count++] = ranges[field_index].lower_ref;
            }
            if (ranges[field_index].upper_ref != (unsigned char)sql_where_nil
                && !ref_list_contains(remove_refs, remove_count,
                    ranges[field_index].upper_ref)) {
                remove_refs[remove_count++] = ranges[field_index].upper_ref;
            }
            return prune_top_level_conjuncts(program, remove_refs,
                remove_count);
        }
    }
    return 0;
}

static int optimize_table_scan(sqlexec_program *program, const char *root,
    const char *db_name, const char *table_name)
{
    sqlexec_node *scan_node;
    dbf_field fields[sql_max_columns];
    unsigned short field_count;
    sqlexec_ref scan_ref;

    scan_ref = find_plan_scan_ref(program);
    scan_node = sqlexec_get(program, scan_ref);
    if (!scan_node || scan_node->opcode != sqlexec_table_scan) {
        return 0;
    }
    reset_scan_access(scan_node);
    if (!program->where.active || program->where.root >= program->where.node_count
        || open_table_fields(root, db_name, table_name, fields,
            &field_count) != 0) {
        return 0;
    }
    return choose_access_path(program, scan_ref, fields, field_count, root,
        db_name, table_name);
}

int sqlopt_optimize(sqlexec_program *program, const char *root,
    const char *db_name)
{
    const char *table_name;
    int result;

    if (!program) {
        return -1;
    }
    clear_where_source_masks(program);
    clear_where_bound_slots(program);
    if (program->root == sqlexec_nil) {
        return 0;
    }
    if (normalize_where_tree(program) != 0) {
        return -1;
    }
    result = 0;
    table_name = find_plan_table_name(program);
    if (root && root[0] && db_name && db_name[0]
        && table_name && table_name[0]) {
        result = optimize_table_scan(program, root, db_name, table_name);
    }
    if (result != 0) {
        return result;
    }
    annotate_where_source_masks(program);
    annotate_where_bound_slots(program, root, db_name);
    return 0;
}

int sqlopt_run(sql_context *ctx)
{
    return sqlopt_optimize(&ctx->program, ctx->root, ctx->current_db);
}
