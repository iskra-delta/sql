/*
 * Implements a tiny execution-tree library for atomic SQL operations.
 * The code uses a fixed node arena, child/sibling tree links, and
 * fixed payload pools so later generator, optimizer, and executor
 * stages can share one compact SDCC-friendly representation.
 *
 * MIT License (see: LICENSE)
 * Copyright (C) 2026 tomaz stih
 */

#include "sqlexec.h"

#include <string.h>

#ifdef SQLEXEC_DEBUG
typedef struct dump_output {
    char *text;
    unsigned short size;
    unsigned short used;
    int failed;
} dump_output;
#endif

static void clear_node(sqlexec_node *node)
{
    memset(node, 0, sizeof(*node));
    node->opcode = sqlexec_invalid;
    node->first_child = sqlexec_nil;
    node->next_sibling = sqlexec_nil;
}

static int ref_is_active(const sqlexec_program *program, sqlexec_ref ref)
{
    return ref < sqlexec_max_nodes
        && program->nodes[ref].opcode != sqlexec_invalid;
}

static void copy_name(char *target, const char *source)
{
    unsigned short index;

    for (index = 0; index + 1 < sql_name_size; index++) {
        target[index] = source[index];
        if (source[index] == '\0') {
            return;
        }
    }
    target[sql_name_size - 1] = '\0';
}

static int name_present(const char *name)
{
    return name[0] != '\0';
}

static int value_present(const sql_value *value)
{
    if (value->type == sql_value_null) {
        return 1;
    }
    return value->type != sql_value_none && value->text[0] != '\0';
}

static int operand_present(const sql_predicate_operand *operand)
{
    if (operand->kind == sql_predicate_operand_column) {
        return operand->data.column.name[0] != '\0';
    }
    if (operand->kind != sql_predicate_operand_value) {
        return 0;
    }
    return value_present(&operand->data.value);
}

static int span_valid(unsigned char total, sqlexec_span span)
{
    if (span.count == 0) {
        return 0;
    }
    if (span.first > total) {
        return 0;
    }
    return (unsigned short)(span.first + span.count) <= total;
}

static const char *compare_name(sql_compare_operator operator)
{
    switch (operator) {
    case sql_compare_equal:
        return "=";
    case sql_compare_not_equal:
        return "!=";
    case sql_compare_less:
        return "<";
    case sql_compare_less_equal:
        return "<=";
    case sql_compare_greater:
        return ">";
    case sql_compare_greater_equal:
        return ">=";
    default:
        return "?";
    }
}

static const char *function_name(sql_select_function function)
{
    switch (function) {
    case sql_function_trim:
        return "TRIM";
    case sql_function_count:
        return "COUNT";
    case sql_function_min:
        return "MIN";
    case sql_function_max:
        return "MAX";
    case sql_function_sum:
        return "SUM";
    case sql_function_avg:
        return "AVG";
    default:
        return "";
    }
}

static int where_value_span_valid(const sql_where *where,
    unsigned char first, unsigned char count)
{
    if (count == 0) {
        return 0;
    }
    if (first >= where->value_count) {
        return 0;
    }
    return (unsigned short)(first + count) <= where->value_count;
}

static int validate_where(const sql_where *where,
    const sql_where_node *nodes, unsigned char node_total,
    const sql_predicate_operand *values, unsigned char value_total,
    unsigned char predicate_subquery_count)
{
    unsigned char stack[sql_where_max_nodes];
    unsigned char seen[sql_where_max_nodes];
    unsigned char depth;
    unsigned char visited_count;
    unsigned char ref;
    unsigned char index;
    const sql_where_node *node;

    if (!where->active) {
        return where->root == (unsigned char)sql_where_nil
            && where->node_count == 0 && where->value_count == 0;
    }
    if (where->root == (unsigned char)sql_where_nil || where->node_count == 0
        || where->node_count > sql_where_max_nodes
        || where->value_count > sql_where_max_values
        || where->node_count > node_total
        || where->value_count > value_total) {
        return 0;
    }

    memset(seen, 0, sizeof(seen));
    depth = 0;
    visited_count = 0;
    stack[depth++] = where->root;
    while (depth > 0) {
        ref = stack[--depth];
        if (ref >= where->node_count || seen[ref]) {
            return 0;
        }
        seen[ref] = 1;
        visited_count++;
        node = &nodes[ref];

        switch (node->type) {
        case sql_where_false:
            break;
        case sql_where_compare:
            if (!name_present(node->column_name)
                || node->operator <= sql_compare_invalid
                || node->operator > sql_compare_greater_equal
                || !where_value_span_valid(where, node->value_first, 1)
                || !operand_present(&values[node->value_first])) {
                return 0;
            }
            break;
        case sql_where_like:
            if (!name_present(node->column_name)
                || !where_value_span_valid(where, node->value_first, 1)
                || !operand_present(&values[node->value_first])) {
                return 0;
            }
            break;
        case sql_where_in:
            if (!name_present(node->column_name)
                || !where_value_span_valid(where, node->value_first,
                    node->value_count)) {
                return 0;
            }
            for (index = 0; index < node->value_count; index++) {
                if (!operand_present(&values[node->value_first + index])) {
                    return 0;
                }
            }
            break;
        case sql_where_is_null:
            if (!name_present(node->column_name)
                || (node->operator != sql_compare_equal
                    && node->operator != sql_compare_not_equal)) {
                return 0;
            }
            break;
        case sql_where_quantified:
            if (!name_present(node->column_name)
                || node->operator <= sql_compare_invalid
                || node->operator > sql_compare_greater_equal
                || node->quantifier < sql_quantifier_any
                || node->quantifier > sql_quantifier_all
                || node->subquery_index >= predicate_subquery_count) {
                return 0;
            }
            break;
        case sql_where_exists:
            if (node->subquery_index >= predicate_subquery_count) {
                return 0;
            }
            break;
        case sql_where_not:
            if (node->left == (unsigned char)sql_where_nil
                || depth + 1u > sql_where_max_nodes) {
                return 0;
            }
            stack[depth++] = node->left;
            break;
        case sql_where_and:
        case sql_where_or:
            if (node->left == (unsigned char)sql_where_nil
                || node->right == (unsigned char)sql_where_nil) {
                return 0;
            }
            if (depth + 2u > sql_where_max_nodes) {
                return 0;
            }
            stack[depth++] = node->right;
            stack[depth++] = node->left;
            break;
        default:
            return 0;
        }
    }

    return visited_count == where->node_count;
}

static sqlexec_ref alloc_node(sqlexec_program *program, sqlexec_opcode opcode)
{
    sqlexec_ref ref;

    if (program->free_head == sqlexec_nil) {
        return sqlexec_nil;
    }

    ref = program->free_head;
    program->free_head = program->nodes[ref].next_sibling;
    clear_node(&program->nodes[ref]);
    program->nodes[ref].opcode = opcode;
    program->node_count++;
    return ref;
}

static void free_node(sqlexec_program *program, sqlexec_ref ref)
{
    clear_node(&program->nodes[ref]);
    program->nodes[ref].next_sibling = program->free_head;
    program->free_head = ref;
    program->node_count--;
}

static void release_subtree(sqlexec_program *program, sqlexec_ref root)
{
    sqlexec_ref stack[sqlexec_max_nodes];
    unsigned short depth;
    sqlexec_ref ref;
    sqlexec_ref child;
    sqlexec_ref next;

    depth = 0;
    stack[depth++] = root;
    while (depth > 0) {
        ref = stack[--depth];
        child = program->nodes[ref].first_child;
        while (child != sqlexec_nil) {
            next = program->nodes[child].next_sibling;
            program->nodes[child].next_sibling = sqlexec_nil;
            stack[depth++] = child;
            child = next;
        }
        free_node(program, ref);
    }
}

static int validate_node(const sqlexec_program *program, sqlexec_ref ref,
    unsigned char child_count)
{
    const sqlexec_node *node;
    unsigned char index;

    node = &program->nodes[ref];
    switch (node->opcode) {
    case sqlexec_sequence:
        return child_count > 0 ? 0 : -1;
    case sqlexec_create_database:
    case sqlexec_use_database:
    case sqlexec_drop_database:
    case sqlexec_drop_table:
        return child_count == 0 && name_present(node->data.named.name)
            ? 0 : -1;
    case sqlexec_table_scan:
        if (child_count != 0) {
            return -1;
        }
        if (node->data.scan.access_kind > sqlexec_scan_index_range) {
            return -1;
        }
        if (node->data.scan.access_kind == sqlexec_scan_full) {
            return 0;
        }
        if (!name_present(node->data.scan.index_name)) {
            return -1;
        }
        if (node->data.scan.access_kind == sqlexec_scan_index_eq) {
            return value_present(&node->data.scan.lower_value) ? 0 : -1;
        }
        if (node->data.scan.lower_operator > sql_compare_greater_equal
            || node->data.scan.upper_operator > sql_compare_greater_equal) {
            return -1;
        }
        if (node->data.scan.lower_operator != sql_compare_invalid
            && !value_present(&node->data.scan.lower_value)) {
            return -1;
        }
        if (node->data.scan.upper_operator != sql_compare_invalid
            && !value_present(&node->data.scan.upper_value)) {
            return -1;
        }
        return 0;
    case sqlexec_create_view:
    case sqlexec_drop_view:
        return child_count == 0 && name_present(node->data.named.name)
            ? 0 : -1;
    case sqlexec_join_scan:
        return child_count == 0
            && span_valid(program->name_count, node->data.join.tables)
            && node->data.join.tables.count >= 4
            && node->data.join.tables.count <= (sql_max_sources * 2)
            && (node->data.join.tables.count & 1u) == 0 ? 0 : -1;
    case sqlexec_create_table:
        if (child_count != 0 || !name_present(node->data.table.name)) {
            return -1;
        }
        return span_valid(program_column_count(program),
            node->data.table.columns)
            ? 0 : -1;
    case sqlexec_create_index:
        if (child_count != 0
            || !name_present(node->data.index.index_name)
            || !name_present(node->data.index.table_name)) {
            return -1;
        }
        return span_valid(program->name_count, node->data.index.key_names)
            ? 0 : -1;
    case sqlexec_project:
        if (child_count != 1) {
            return -1;
        }
        if (node->data.project.select_all) {
            return node->data.project.names.count == 0
                && node->data.project.aliases.count == 0
                && node->data.project.qualifiers.count == 0
                && node->data.project.group_names.count == 0
                && node->data.project.group_qualifiers.count == 0
                && !node->data.project.has_aggregate
                && !program->having.active ? 0 : -1;
        }
        if (!span_valid(program->name_count, node->data.project.qualifiers)
            || !span_valid(program->name_count, node->data.project.names)
            || !span_valid(program->name_count, node->data.project.aliases)
            || (node->data.project.group_names.count > 0
                && !span_valid(program->name_count,
                    node->data.project.group_names))
            || (node->data.project.group_qualifiers.count > 0
                && !span_valid(program->name_count,
                    node->data.project.group_qualifiers))
            || node->data.project.qualifiers.count
                != node->data.project.names.count
            || node->data.project.aliases.count
                != node->data.project.names.count
            || node->data.project.group_qualifiers.count
                != node->data.project.group_names.count) {
            return -1;
        }
        for (index = 0; index < node->data.project.names.count; index++) {
            if (node->data.project.functions[index] > sql_function_avg) {
                return -1;
            }
            if (node->data.project.function_arg_is_star[index]
                && node->data.project.functions[index]
                    != sql_function_count) {
                return -1;
            }
        }
        if (!validate_where(&program->having,
            program_having_nodes(program), program->having.node_count,
            program_having_values(program), program->having.value_count,
            program->predicate_subquery_count)) {
            return -1;
        }
        return 0;
    case sqlexec_append_record:
        return child_count == 0
            && span_valid(program_assignment_count(program),
                node->data.assignments) ? 0 : -1;
    case sqlexec_write_current:
        return child_count == 1
            && span_valid(program_assignment_count(program),
                node->data.assignments) ? 0 : -1;
    case sqlexec_delete_current:
        return child_count == 1 ? 0 : -1;
    default:
        return -1;
    }
}

#ifdef SQLEXEC_DEBUG
static void dump_char(dump_output *out, char c)
{
    if (out->failed) {
        return;
    }
    if (out->used + 1 >= out->size) {
        out->failed = 1;
        return;
    }
    out->text[out->used++] = c;
    out->text[out->used] = '\0';
}

static void dump_text(dump_output *out, const char *text)
{
    while (*text) {
        dump_char(out, *text++);
    }
}

static void dump_uint(dump_output *out, unsigned short value)
{
    char digits[6];
    unsigned short count;
    unsigned short index;
    char tmp;

    if (value == 0) {
        dump_char(out, '0');
        return;
    }

    count = 0;
    while (value > 0) {
        digits[count++] = (char)('0' + (value % 10));
        value = (unsigned short)(value / 10);
    }
    for (index = 0; index < count / 2; index++) {
        tmp = digits[index];
        digits[index] = digits[count - 1 - index];
        digits[count - 1 - index] = tmp;
    }
    digits[count] = '\0';
    dump_text(out, digits);
}

static void dump_value(dump_output *out, const sql_value *value)
{
    if (value->type == sql_value_null) {
        dump_text(out, "NULL");
        return;
    }
    if (value->type == sql_value_string) {
        dump_char(out, '\'');
        dump_text(out, value->text);
        dump_char(out, '\'');
        return;
    }
    dump_text(out, value->text);
}

static void dump_column_ref(dump_output *out, const char *qualifier,
    const char *name);

static void dump_operand(dump_output *out,
    const sql_predicate_operand *operand)
{
    if (operand->kind == sql_predicate_operand_column) {
        dump_column_ref(out, operand->data.column.qualifier,
            operand->data.column.name);
        return;
    }
    dump_value(out, &operand->data.value);
}

static void dump_column_ref(dump_output *out, const char *qualifier,
    const char *name)
{
    if (qualifier[0] != '\0') {
        dump_text(out, qualifier);
        dump_char(out, '.');
    }
    dump_text(out, name);
}

static void dump_names(dump_output *out, const sqlexec_program *program,
    sqlexec_span names)
{
    unsigned char index;

    for (index = 0; index < names.count; index++) {
        if (index > 0) {
            dump_text(out, ", ");
        }
        dump_text(out, program->names[names.first + index]);
    }
}

static void dump_project_names(dump_output *out,
    const sqlexec_program *program, const sqlexec_project_def *project)
{
    unsigned char index;
    sql_select_function function;

    for (index = 0; index < project->names.count; index++) {
        function = project->functions[index];
        if (index > 0) {
            dump_text(out, ", ");
        }
        if (function != sql_function_none) {
            dump_text(out, function_name(function));
            dump_char(out, '(');
        }
        if (project->function_arg_is_star[index]) {
            dump_char(out, '*');
        } else {
            dump_column_ref(out,
                project->qualifiers.count == project->names.count
                    ? program->names[project->qualifiers.first + index] : "",
                program->names[project->names.first + index]);
        }
        if (function != sql_function_none) {
            dump_char(out, ')');
        }
        if (project->aliases.count == project->names.count
            && program->names[project->aliases.first + index][0] != '\0') {
            dump_text(out, " as ");
            dump_text(out, program->names[project->aliases.first + index]);
        }
    }
}

static void dump_where_expression(dump_output *out,
    const sqlexec_program *program, const sql_where_node *nodes,
    const sql_predicate_operand *values,
    unsigned char ref)
{
    const sql_where_node *node;
    unsigned char index;

    node = &nodes[ref];
    switch (node->type) {
    case sql_where_false:
        dump_text(out, "FALSE");
        break;
    case sql_where_compare:
        dump_column_ref(out, node->qualifier, node->column_name);
        dump_char(out, ' ');
        dump_text(out, compare_name(node->operator));
        dump_char(out, ' ');
        dump_operand(out, &values[node->value_first]);
        break;
    case sql_where_in:
        dump_column_ref(out, node->qualifier, node->column_name);
        dump_text(out, " IN (");
        for (index = 0; index < node->value_count; index++) {
            if (index > 0) {
                dump_text(out, ", ");
            }
            dump_operand(out, &values[node->value_first + index]);
        }
        dump_char(out, ')');
        break;
    case sql_where_like:
        dump_column_ref(out, node->qualifier, node->column_name);
        dump_text(out, " LIKE ");
        dump_operand(out, &values[node->value_first]);
        break;
    case sql_where_is_null:
        dump_column_ref(out, node->qualifier, node->column_name);
        dump_text(out, node->operator == sql_compare_not_equal
            ? " IS NOT NULL" : " IS NULL");
        break;
    case sql_where_quantified:
        dump_column_ref(out, node->qualifier, node->column_name);
        dump_char(out, ' ');
        dump_text(out, compare_name(node->operator));
        dump_char(out, ' ');
        dump_text(out, node->quantifier == sql_quantifier_all
            ? "ALL (" : "ANY (");
        dump_text(out, program->predicate_subqueries[node->subquery_index]);
        dump_char(out, ')');
        break;
    case sql_where_exists:
        dump_text(out, "EXISTS (");
        dump_text(out, program->predicate_subqueries[node->subquery_index]);
        dump_char(out, ')');
        break;
    case sql_where_not:
        dump_text(out, "NOT ");
        dump_where_expression(out, program, nodes, values, node->left);
        break;
    case sql_where_and:
    case sql_where_or:
        dump_char(out, '(');
        dump_where_expression(out, program, nodes, values, node->left);
        dump_text(out, node->type == sql_where_and ? " AND " : " OR ");
        dump_where_expression(out, program, nodes, values, node->right);
        dump_char(out, ')');
        break;
    default:
        dump_text(out, "?");
        break;
    }

}

static void dump_node_summary(dump_output *out, const sqlexec_program *program,
    const sqlexec_node *node)
{
    switch (node->opcode) {
    case sqlexec_create_database:
    case sqlexec_use_database:
    case sqlexec_drop_database:
    case sqlexec_drop_table:
        dump_char(out, ' ');
        dump_text(out, node->data.named.name);
        break;
    case sqlexec_create_table:
        dump_char(out, ' ');
        dump_text(out, node->data.table.name);
        dump_text(out, " (");
        dump_uint(out, node->data.table.columns.count);
        dump_text(out, " cols)");
        break;
    case sqlexec_create_index:
        dump_char(out, ' ');
        dump_text(out, node->data.index.index_name);
        dump_text(out, " on ");
        dump_text(out, node->data.index.table_name);
        dump_text(out, " (");
        dump_names(out, program, node->data.index.key_names);
        dump_char(out, ')');
        if (node->data.index.unique) {
            dump_text(out, " unique");
        }
        break;
    case sqlexec_join_scan: {
        unsigned char index;
        unsigned char source_count;

        source_count = join_source_count(node->data.join);
        dump_char(out, ' ');
        for (index = 0; index < source_count; index++) {
            if (index > 0) {
                dump_text(out, " join ");
            }
            dump_text(out, join_table_at(program, node->data.join, index));
            if (join_alias_at(program, node->data.join, index)[0] != '\0') {
                dump_text(out, " as ");
                dump_text(out, join_alias_at(program, node->data.join, index));
            }
        }
        break;
    }
    case sqlexec_table_scan:
        if (node->data.scan.access_kind == sqlexec_scan_index_eq) {
            dump_char(out, ' ');
            dump_text(out, node->data.scan.index_name);
            dump_text(out, " = ");
            dump_value(out, &node->data.scan.lower_value);
        } else if (node->data.scan.access_kind
            == sqlexec_scan_index_range) {
            dump_char(out, ' ');
            dump_text(out, node->data.scan.index_name);
            if (node->data.scan.lower_operator != sql_compare_invalid) {
                dump_char(out, ' ');
                dump_text(out, compare_name(
                    node->data.scan.lower_operator));
                dump_char(out, ' ');
                dump_value(out, &node->data.scan.lower_value);
            }
            if (node->data.scan.upper_operator != sql_compare_invalid) {
                dump_text(out, " .. ");
                dump_text(out, compare_name(
                    node->data.scan.upper_operator));
                dump_char(out, ' ');
                dump_value(out, &node->data.scan.upper_value);
            }
        }
        break;
    case sqlexec_project:
        dump_char(out, ' ');
        if (node->data.project.distinct) {
            dump_text(out, "DISTINCT ");
        }
        if (node->data.project.select_all) {
            dump_char(out, '*');
        } else {
            dump_project_names(out, program, &node->data.project);
            if (program->where.active) {
                dump_text(out, " where ");
                dump_where_expression(out, program, program->where_nodes,
                    program->where_values, program->where.root);
            }
            if (node->data.project.group_names.count > 0) {
                unsigned char index;

                dump_text(out, " group_by ");
                for (index = 0; index < node->data.project.group_names.count;
                    index++) {
                    if (index > 0) {
                        dump_text(out, ", ");
                    }
                    dump_column_ref(out,
                        program->names[node->data.project.group_qualifiers.first
                            + index],
                        program->names[node->data.project.group_names.first
                            + index]);
                }
            }
        }
        if (node->data.project.select_all && program->where.active) {
            dump_text(out, " where ");
            dump_where_expression(out, program, program->where_nodes,
                program->where_values, program->where.root);
        }
        if (program->having.active) {
            dump_text(out, " having ");
            dump_where_expression(out, program, program_having_nodes(program),
                program_having_values(program), program->having.root);
        }
        break;
    case sqlexec_append_record:
        dump_text(out, " values=");
        dump_uint(out, node->data.assignments.count);
        break;
    case sqlexec_write_current:
        dump_text(out, " assignments=");
        dump_uint(out, node->data.assignments.count);
        break;
    default:
        break;
    }
}
#endif /* SQLEXEC_DEBUG */

void sqlexec_reset(sqlexec_program *program)
{
    sqlexec_ref ref;

    memset(program, 0, sizeof(*program));
    program->root = sqlexec_nil;
    program->free_head = 0;
    program->where.root = (unsigned char)sql_where_nil;
    program->having.root = (unsigned char)sql_where_nil;
    for (ref = 0; ref < sqlexec_max_nodes; ref++) {
        clear_node(&program->nodes[ref]);
        program->nodes[ref].next_sibling = ref + 1 < sqlexec_max_nodes
            ? (sqlexec_ref)(ref + 1) : sqlexec_nil;
    }
}

const char *sqlexec_opcode_name(sqlexec_opcode opcode)
{
    switch (opcode) {
    case sqlexec_sequence: return "sequence";
    case sqlexec_create_database: return "create_database";
    case sqlexec_use_database: return "use_database";
    case sqlexec_drop_database: return "drop_database";
    case sqlexec_create_table: return "create_table";
    case sqlexec_drop_table: return "drop_table";
    case sqlexec_create_index: return "create_index";
    case sqlexec_table_scan: return "table_scan";
    case sqlexec_join_scan: return "join_scan";
    case sqlexec_project: return "project";
    case sqlexec_append_record: return "append_record";
    case sqlexec_write_current: return "write_current";
    case sqlexec_delete_current: return "delete_current";
    case sqlexec_create_view: return "create_view";
    case sqlexec_drop_view: return "drop_view";
    default: return "invalid";
    }
}

sqlexec_ref sqlexec_make_root(sqlexec_program *program, sqlexec_opcode opcode)
{
    sqlexec_ref ref;

    if (program->root != sqlexec_nil) {
        return sqlexec_nil;
    }
    ref = alloc_node(program, opcode);
    if (ref == sqlexec_nil) {
        return sqlexec_nil;
    }
    program->root = ref;
    return ref;
}

sqlexec_ref sqlexec_append_child(sqlexec_program *program, sqlexec_ref parent,
    sqlexec_opcode opcode)
{
    sqlexec_ref ref;
    sqlexec_ref sibling;

    if (!ref_is_active(program, parent)) {
        return sqlexec_nil;
    }

    ref = alloc_node(program, opcode);
    if (ref == sqlexec_nil) {
        return sqlexec_nil;
    }

    if (program->nodes[parent].first_child == sqlexec_nil) {
        program->nodes[parent].first_child = ref;
        return ref;
    }

    sibling = program->nodes[parent].first_child;
    while (program->nodes[sibling].next_sibling != sqlexec_nil) {
        sibling = program->nodes[sibling].next_sibling;
    }
    program->nodes[sibling].next_sibling = ref;
    return ref;
}

sqlexec_ref sqlexec_insert_sibling_after(sqlexec_program *program, sqlexec_ref after,
    sqlexec_opcode opcode)
{
    sqlexec_ref ref;

    if (!ref_is_active(program, after) || after == program->root) {
        return sqlexec_nil;
    }

    ref = alloc_node(program, opcode);
    if (ref == sqlexec_nil) {
        return sqlexec_nil;
    }

    program->nodes[ref].next_sibling = program->nodes[after].next_sibling;
    program->nodes[after].next_sibling = ref;
    return ref;
}

int sqlexec_remove(sqlexec_program *program, sqlexec_ref target)
{
    sqlexec_ref stack[sqlexec_max_nodes];
    unsigned short depth;
    sqlexec_ref parent;
    sqlexec_ref child;
    sqlexec_ref prev;

    if (!ref_is_active(program, target)) {
        return -1;
    }

    if (program->root == target) {
        program->root = sqlexec_nil;
        release_subtree(program, target);
        return 0;
    }

    depth = 0;
    if (program->root != sqlexec_nil) {
        stack[depth++] = program->root;
    }
    while (depth > 0) {
        parent = stack[--depth];
        prev = sqlexec_nil;
        child = program->nodes[parent].first_child;
        while (child != sqlexec_nil) {
            if (child == target) {
                if (prev == sqlexec_nil) {
                    program->nodes[parent].first_child =
                        program->nodes[child].next_sibling;
                } else {
                    program->nodes[prev].next_sibling =
                        program->nodes[child].next_sibling;
                }
                program->nodes[child].next_sibling = sqlexec_nil;
                release_subtree(program, child);
                return 0;
            }
            stack[depth++] = child;
            prev = child;
            child = program->nodes[child].next_sibling;
        }
    }

    return -1;
}

int sqlexec_replace(sqlexec_program *program, sqlexec_ref target,
    sqlexec_opcode opcode)
{
    sqlexec_ref first_child;
    sqlexec_ref next_sibling;

    if (!ref_is_active(program, target)) {
        return -1;
    }

    first_child = program->nodes[target].first_child;
    next_sibling = program->nodes[target].next_sibling;
    clear_node(&program->nodes[target]);
    program->nodes[target].opcode = opcode;
    program->nodes[target].first_child = first_child;
    program->nodes[target].next_sibling = next_sibling;
    return 0;
}

sqlexec_node *sqlexec_get(sqlexec_program *program, sqlexec_ref ref)
{
    return ref_is_active(program, ref) ? &program->nodes[ref] : NULL;
}

const sqlexec_node *sqlexec_get_const(const sqlexec_program *program, sqlexec_ref ref)
{
    return ref_is_active(program, ref) ? &program->nodes[ref] : NULL;
}

int sqlexec_add_names(sqlexec_program *program,
    char names[][sql_name_size], unsigned char count,
    unsigned char *first_out)
{
    unsigned char index;

    if (count == 0
        || (unsigned short)(program->name_count + count) > sqlexec_max_names) {
        return -1;
    }

    *first_out = program->name_count;
    for (index = 0; index < count; index++) {
        copy_name(program->names[program->name_count + index], names[index]);
    }
    program->name_count = (unsigned char)(program->name_count + count);
    return 0;
}

int sqlexec_add_columns(sqlexec_program *program, const sql_column *columns,
    unsigned char count, unsigned char *first_out)
{
    if (count == 0
        || (unsigned short)(program_column_count(program) + count)
            > sql_max_columns) {
        return -1;
    }

    *first_out = program_column_count(program);
    memcpy(program_columns(program) + program_column_count(program), columns,
        (unsigned short)(count * sizeof(sql_column)));
    program_column_count(program) = (unsigned char)(
        program_column_count(program) + count);
    return 0;
}

int sqlexec_add_assignments(sqlexec_program *program,
    const sql_assignment *assignments, unsigned char count,
    unsigned char *first_out)
{
    if (count == 0 || (unsigned short)(
        program_assignment_count(program) + count) > sql_max_columns) {
        return -1;
    }

    *first_out = program_assignment_count(program);
    memcpy(program_assignments(program) + program_assignment_count(program),
        assignments,
        (unsigned short)(count * sizeof(sql_assignment)));
    program_assignment_count(program) = (unsigned char)(
        program_assignment_count(program) + count);
    return 0;
}

#ifdef SQLEXEC_DEBUG
int sqlexec_validate(const sqlexec_program *program)
{
    sqlexec_ref stack[sqlexec_max_nodes];
    unsigned char seen[sqlexec_max_nodes];
    unsigned short depth;
    unsigned short visited_count;
    unsigned short child_guard;
    unsigned char child_count;
    sqlexec_ref ref;
    sqlexec_ref child;

    memset(seen, 0, sizeof(seen));
    if (program->node_count == 0) {
        return program->root == sqlexec_nil ? 0 : -1;
    }
    if (!ref_is_active(program, program->root)
        || program->nodes[program->root].next_sibling != sqlexec_nil) {
        return -1;
    }
    if (program->predicate_subquery_count > sql_max_predicate_subqueries) {
        return -1;
    }
    if ((unsigned short)(program->having_node_first
            + program->having.node_count) > program->predicate_node_count
        || (unsigned short)(program->having_value_first
            + program->having.value_count) > program->predicate_value_count) {
        return -1;
    }
    if (!validate_where(&program->where, program->where_nodes,
        program->where.node_count, program->where_values,
        program->where.value_count, program->predicate_subquery_count)
        || !validate_where(&program->having, program_having_nodes(program),
            program->having.node_count, program_having_values(program),
            program->having.value_count,
            program->predicate_subquery_count)) {
        return -1;
    }

    depth = 0;
    visited_count = 0;
    stack[depth++] = program->root;
    while (depth > 0) {
        ref = stack[--depth];
        if (!ref_is_active(program, ref) || seen[ref]) {
            return -1;
        }

        seen[ref] = 1;
        visited_count++;
        child_count = 0;
        child_guard = 0;
        child = program->nodes[ref].first_child;
        while (child != sqlexec_nil) {
            if (!ref_is_active(program, child)) {
                return -1;
            }
            child_count++;
            if (++child_guard > sqlexec_max_nodes) {
                return -1;
            }
            stack[depth++] = child;
            child = program->nodes[child].next_sibling;
        }
        if (validate_node(program, ref, child_count) != 0) {
            return -1;
        }
    }

    return visited_count == program->node_count ? 0 : -1;
}

int sqlexec_dump(const sqlexec_program *program, char *text,
    unsigned short size)
{
    dump_output out;
    sqlexec_ref stack[sqlexec_max_nodes];
    unsigned short depths[sqlexec_max_nodes];
    unsigned short stack_size;
    unsigned short depth;
    unsigned short index;
    unsigned char child_count;
    sqlexec_ref ref;
    sqlexec_ref child;
    sqlexec_ref children[sqlexec_max_nodes];

    if (sqlexec_validate(program) != 0 || size == 0) {
        return -1;
    }

    out.text = text;
    out.size = size;
    out.used = 0;
    out.failed = 0;
    text[0] = '\0';

    if (program->root == sqlexec_nil) {
        return 0;
    }

    stack_size = 0;
    stack[stack_size] = program->root;
    depths[stack_size++] = 0;
    while (stack_size > 0) {
        ref = stack[--stack_size];
        depth = depths[stack_size];
        for (index = 0; index < depth; index++) {
            dump_text(&out, "  ");
        }
        dump_text(&out, sqlexec_opcode_name(program->nodes[ref].opcode));
        dump_node_summary(&out, program, &program->nodes[ref]);
        dump_char(&out, '\n');

        child_count = 0;
        child = program->nodes[ref].first_child;
        while (child != sqlexec_nil) {
            children[child_count++] = child;
            child = program->nodes[child].next_sibling;
        }
        while (child_count > 0) {
            child_count--;
            stack[stack_size] = children[child_count];
            depths[stack_size++] = (unsigned short)(depth + 1);
        }
    }

    return out.failed ? -1 : 0;
}
#endif /* SQLEXEC_DEBUG */
