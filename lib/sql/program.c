/*
 * Builds sqlexec programs directly from parsed SQL text.
 * The code keeps the shared sqlexec tree as the parser output so later
 * optimizer and executor phases can run without depending on the
 * syntax-level sql_statement layout.
 *
 * MIT License (see: LICENSE)
 * Copyright (C) 2026 tomaz stih
 */

#include "sql.h"
#include "sqlexec.h"
#include "sqlctx.h"
#include "../common/common.h"
#include "../catalog/catalog.h"

#include <string.h>

#define stmt_column_count(stmt) ((stmt)->detail.variant.create_table.column_count)
#define stmt_columns(stmt) ((stmt)->detail.variant.create_table.columns)
#define stmt_index_unique(stmt) ((stmt)->detail.variant.create_index.unique)
#define stmt_key_count(stmt) ((stmt)->detail.variant.create_index.key_count)
#define stmt_key_names(stmt) ((stmt)->detail.variant.create_index.key_names)
#define stmt_assignment_count(stmt) ((stmt)->detail.variant.mutate.assignment_count)
#define stmt_assignments(stmt) ((stmt)->detail.variant.mutate.assignments)
#define stmt_from_is_subquery(stmt) ((stmt)->detail.select.from_is_subquery)
#define stmt_from_alias(stmt) ((stmt)->detail.select.from_alias)
#define stmt_join_count(stmt) ((stmt)->detail.select.join_count)
#define stmt_join_table_names(stmt) ((stmt)->detail.select.join_table_names)
#define stmt_join_aliases(stmt) ((stmt)->detail.select.join_aliases)
#define stmt_select_all(stmt) ((stmt)->detail.select.select_all)
#define stmt_select_distinct(stmt) ((stmt)->detail.select.select_distinct)
#define stmt_select_count_star(stmt) ((stmt)->detail.select.select_count_star)
#define stmt_select_has_aggregate(stmt) ((stmt)->detail.select.select_has_aggregate)
#define stmt_select_count(stmt) ((stmt)->detail.select.select_count)
#define stmt_select_items(stmt) ((stmt)->detail.select.select_items)
#define stmt_group_count(stmt) ((stmt)->detail.select.group_count)
#define stmt_group_items(stmt) ((stmt)->detail.select.group_items)

static unsigned char statement_join_count(const sql_statement *statement)
{
    return statement->type == sql_statement_select
        ? stmt_join_count(statement) : 0;
}

static unsigned char statement_from_is_subquery(
    const sql_statement *statement)
{
    return statement->type == sql_statement_select
        ? stmt_from_is_subquery(statement) : 0;
}

static int set_named_payload(sqlexec_program *program, sqlexec_ref ref,
    const char *name)
{
    sqlexec_node *node;

    node = sqlexec_get(program, ref);
    if (!node) {
        return -1;
    }
    copy_name(node->data.named.name, name);
    return 0;
}

static void set_program_table_name(sqlexec_program *program,
    const char *table_name)
{
    copy_name(program->table_name, table_name);
}

static int set_table_payload(sqlexec_program *program, sqlexec_ref ref,
    const sql_statement *statement)
{
    sqlexec_node *node;
    unsigned char first;

    if (sqlexec_add_columns(program, stmt_columns(statement),
        stmt_column_count(statement), &first) != 0) {
        return -1;
    }

    node = sqlexec_get(program, ref);
    if (!node) {
        return -1;
    }
    copy_name(node->data.table.name, statement->name);
    node->data.table.columns.first = first;
    node->data.table.columns.count = stmt_column_count(statement);
    return 0;
}

static int set_index_payload(sqlexec_program *program, sqlexec_ref ref,
    const sql_statement *statement, unsigned char first)
{
    sqlexec_node *node;

    node = sqlexec_get(program, ref);
    if (!node) {
        return -1;
    }
    copy_name(node->data.index.index_name, statement->name);
    copy_name(node->data.index.table_name, statement->table_name);
    node->data.index.unique = stmt_index_unique(statement);
    node->data.index.key_names.first = first;
    node->data.index.key_names.count = stmt_key_count(statement);
    return 0;
}

static int set_join_payload(sqlexec_program *program, sqlexec_ref ref,
    const sql_statement *statement)
{
    sqlexec_node *node;
    char table_names[sql_max_sources * 2][sql_name_size];
    unsigned char table_first;
    unsigned char table_count;
    unsigned char index;

    copy_name(table_names[0], statement->name);
    copy_name(table_names[1], stmt_from_alias(statement));
    table_count = 2;
    for (index = 0; index < stmt_join_count(statement); index++) {
        copy_name(table_names[table_count++],
            stmt_join_table_names(statement)[index]);
        copy_name(table_names[table_count++],
            stmt_join_aliases(statement)[index]);
    }

    if (sqlexec_add_names(program, table_names, table_count, &table_first)
        != 0) {
        return -1;
    }

    node = sqlexec_get(program, ref);
    if (!node) {
        return -1;
    }
    node->data.join.tables.first = table_first;
    node->data.join.tables.count = table_count;
    return 0;
}

static int copy_where_payload(sqlexec_program *program,
    const sql_statement *statement)
{
    unsigned char index;

    program->predicate_node_count = 0;
    program->predicate_value_count = 0;
    if (!statement->where.active) {
        memset(&program->where, 0, sizeof(program->where));
        program->where.root = (unsigned char)sql_where_nil;
        return 0;
    }

    if (statement->where.node_count > sql_where_max_nodes
        || statement->where.value_count > sql_where_max_values) {
        return -1;
    }

    program->where = statement->where;
    memcpy(program->where_nodes, statement->where_nodes,
        (unsigned short)(statement->where.node_count
            * sizeof(statement->where_nodes[0])));
    if (statement_join_count(statement) == 0) {
        for (index = 0; index < statement->where.node_count; index++) {
            program->where_nodes[index].qualifier[0] = '\0';
        }
    }
    memcpy(program->where_values, statement->where_values,
        (unsigned short)(statement->where.value_count
            * sizeof(statement->where_values[0])));
    program->predicate_node_count = statement->where.node_count;
    program->predicate_value_count = statement->where.value_count;
    return 0;
}

static int copy_predicate_subqueries(sqlexec_program *program,
    const sql_statement *statement)
{
    unsigned char index;

    if (statement->predicate_subquery_count > sql_max_predicate_subqueries) {
        return -1;
    }
    program->predicate_subquery_count = statement->predicate_subquery_count;
    for (index = 0; index < statement->predicate_subquery_count; index++) {
        copy_subquery(program->predicate_subqueries[index],
            statement->predicate_subqueries[index]);
    }
    return 0;
}

static int copy_having_payload(sqlexec_program *program,
    const sql_statement *statement)
{
    if (!statement->having.active) {
        memset(&program->having, 0, sizeof(program->having));
        program->having.root = (unsigned char)sql_where_nil;
        program->having_node_first = program->predicate_node_count;
        program->having_value_first = program->predicate_value_count;
        return 0;
    }

    if ((unsigned short)(program->predicate_node_count
            + statement->having.node_count) > sql_where_max_nodes
        || (unsigned short)(program->predicate_value_count
            + statement->having.value_count) > sql_where_max_values) {
        return -1;
    }

    program->having = statement->having;
    program->having_node_first = program->predicate_node_count;
    program->having_value_first = program->predicate_value_count;
    memcpy(program->where_nodes + program->having_node_first,
        statement_having_nodes(statement),
        (unsigned short)(statement->having.node_count
            * sizeof(statement_having_nodes(statement)[0])));
    memcpy(program->where_values + program->having_value_first,
        statement_having_values(statement),
        (unsigned short)(statement->having.value_count
            * sizeof(statement_having_values(statement)[0])));
    program->predicate_node_count = (unsigned char)(
        program->predicate_node_count + statement->having.node_count);
    program->predicate_value_count = (unsigned char)(
        program->predicate_value_count + statement->having.value_count);
    return 0;
}

static int append_scan_input(sqlexec_program *program, sqlexec_ref parent,
    const sql_statement *statement)
{
    sqlexec_ref ref;

    if (statement_join_count(statement) > 0) {
        ref = sqlexec_append_child(program, parent, sqlexec_join_scan);
        if (ref == sqlexec_nil || set_join_payload(program, ref, statement)
            != 0) {
            return -1;
        }
        return 0;
    }

    ref = sqlexec_append_child(program, parent, sqlexec_table_scan);
    return ref == sqlexec_nil ? -1 : 0;
}

static int lower_scan_root(sqlexec_program *program,
    const sql_statement *statement)
{
    sqlexec_ref root;

    if (statement_join_count(statement) > 0) {
        root = sqlexec_make_root(program, sqlexec_join_scan);
        return (root == sqlexec_nil
            || set_join_payload(program, root, statement) != 0) ? -1 : 0;
    }
    root = sqlexec_make_root(program, sqlexec_table_scan);
    return root == sqlexec_nil ? -1 : 0;
}

static int lower_named_root(sqlexec_program *program, sqlexec_opcode opcode,
    const char *name)
{
    sqlexec_ref root;

    root = sqlexec_make_root(program, opcode);
    if (root == sqlexec_nil) {
        return -1;
    }
    return set_named_payload(program, root, name);
}

static int lower_create_table(sqlexec_program *program,
    const sql_statement *statement)
{
    sqlexec_ref root;

    root = sqlexec_make_root(program, sqlexec_create_table);
    if (root == sqlexec_nil) {
        return -1;
    }
    return set_table_payload(program, root, statement);
}

static int lower_create_index(sqlexec_program *program,
    const sql_statement *statement)
{
    char key_names[sql_max_columns][sql_name_size];
    sqlexec_ref root;
    unsigned char first;
    unsigned char index;

    root = sqlexec_make_root(program, sqlexec_create_index);
    if (root == sqlexec_nil) {
        return -1;
    }
    for (index = 0; index < stmt_key_count(statement); index++) {
        copy_name(key_names[index], stmt_key_names(statement)[index]);
    }
    if (sqlexec_add_names(program, key_names, stmt_key_count(statement), &first)
        != 0) {
        return -1;
    }

    return set_index_payload(program, root, statement, first);
}

static int is_simple_flattenable_select(const sql_statement *source)
{
    unsigned char index;

    if (source->type != sql_statement_select || stmt_from_is_subquery(source)) {
        return 0;
    }
    if (stmt_join_count(source) != 0
        || stmt_select_distinct(source)
        || stmt_select_count_star(source)
        || stmt_select_has_aggregate(source)
        || stmt_group_count(source) != 0
        || source->having.active) {
        return 0;
    }
    if (stmt_select_all(source)) {
        return stmt_select_count(source) == 0;
    }
    if (stmt_select_count(source) == 0) {
        return 0;
    }
    for (index = 0; index < stmt_select_count(source); index++) {
        if (stmt_select_items(source)[index].function != sql_function_none
            || stmt_select_items(source)[index].argument_is_star) {
            return 0;
        }
    }
    return 1;
}

static const char *source_select_item_output_name(
    const sql_select_item *item)
{
    return item->alias[0] != '\0' ? item->alias : item->column.name;
}

static int qualifier_matches_flatten_source(const sql_statement *source,
    const char *qualifier);

static int lookup_flattened_source_output(const sql_statement *source,
    const char *name, sql_column_ref *column_out)
{
    unsigned char index;
    int found;

    if (stmt_select_all(source)) {
        copy_name(column_out->qualifier, source->name);
        copy_name(column_out->name, name);
        return 1;
    }

    found = -1;
    for (index = 0; index < stmt_select_count(source); index++) {
        if (strcmp(source_select_item_output_name(
            &stmt_select_items(source)[index]), name) != 0) {
            continue;
        }
        if (found >= 0) {
            return -1;
        }
        found = (int)index;
    }
    if (found < 0) {
        return 0;
    }

    *column_out = stmt_select_items(source)[found].column;
    return 1;
}

static int rewrite_flattened_column_ref(sql_column_ref *column,
    const sql_statement *source, const char *source_qualifier,
    const char *target_qualifier, unsigned char allow_unqualified)
{
    sql_column_ref mapped;
    int lookup;

    if (column->qualifier[0] != '\0') {
        if (source_qualifier[0] == '\0'
            || strcmp(column->qualifier, source_qualifier) != 0) {
            return 0;
        }
    } else if (!allow_unqualified) {
        lookup = lookup_flattened_source_output(source, column->name, &mapped);
        return lookup == 0 ? 0 : -1;
    }

    lookup = lookup_flattened_source_output(source, column->name, &mapped);
    if (lookup <= 0) {
        return lookup < 0 ? -1 : 0;
    }
    copy_name(column->qualifier, target_qualifier);
    copy_name(column->name, mapped.name);
    return 1;
}

static int rewrite_flattened_named_ref(char *qualifier, char *name,
    const sql_statement *source, const char *source_qualifier,
    const char *target_qualifier,
    unsigned char allow_unqualified)
{
    sql_column_ref column;
    int result;

    copy_name(column.qualifier, qualifier);
    copy_name(column.name, name);
    result = rewrite_flattened_column_ref(&column, source, source_qualifier,
        target_qualifier, allow_unqualified);
    if (result > 0) {
        copy_name(qualifier, column.qualifier);
        copy_name(name, column.name);
    }
    return result;
}

static int rewrite_flattened_where(sql_statement *stmt,
    const sql_statement *source, const char *source_qualifier,
    const char *target_qualifier,
    unsigned char allow_unqualified)
{
    unsigned char stack[sql_where_max_nodes];
    unsigned char depth;
    unsigned char ref;
    unsigned char value_index;
    int result;
    sql_where_node *node;

    if (!stmt->where.active) {
        return 0;
    }

    depth = 0;
    stack[depth++] = stmt->where.root;
    while (depth > 0) {
        ref = stack[--depth];
        node = &stmt->where_nodes[ref];
        switch (node->type) {
        case sql_where_compare:
        case sql_where_in:
        case sql_where_like:
        case sql_where_is_null:
        case sql_where_quantified:
            result = rewrite_flattened_named_ref(
                node->qualifier, node->column_name, source, source_qualifier,
                target_qualifier, allow_unqualified);
            if (result < 0) {
                return -1;
            }
            for (value_index = 0; value_index < node->value_count;
                value_index++) {
                sql_predicate_operand *operand;

                operand = &stmt->where_values[node->value_first + value_index];
                if (operand->kind != sql_predicate_operand_column) {
                    continue;
                }
                if (rewrite_flattened_column_ref(&operand->data.column,
                    source, source_qualifier, target_qualifier,
                    allow_unqualified) < 0) {
                    return -1;
                }
            }
            break;
        case sql_where_not:
            stack[depth++] = node->left;
            break;
        case sql_where_and:
        case sql_where_or:
            stack[depth++] = node->right;
            stack[depth++] = node->left;
            break;
        default:
            break;
        }
    }

    return 0;
}

static int rewrite_flattened_select_projection(sql_statement *stmt,
    const sql_statement *source, const char *source_qualifier,
    const char *target_qualifier,
    unsigned char allow_unqualified)
{
    unsigned char index;
    char original_name[sql_name_size];

    if (stmt_select_count_star(stmt)) {
        return 0;
    }
    if (stmt_select_all(stmt)) {
        if (stmt_select_all(source)) {
            return 0;
        }
        if (stmt_join_count(stmt) > 0
            || stmt_select_count(source) > sql_max_columns) {
            return -1;
        }
        stmt_select_all(stmt) = 0;
        stmt_select_count(stmt) = stmt_select_count(source);
        stmt_select_has_aggregate(stmt) = 0;
        for (index = 0; index < stmt_select_count(source); index++) {
            stmt_select_items(stmt)[index] = stmt_select_items(source)[index];
            copy_name(stmt_select_items(stmt)[index].column.qualifier,
                target_qualifier);
        }
        return 0;
    }

    for (index = 0; index < stmt_select_count(stmt); index++) {
        if (stmt_select_items(stmt)[index].argument_is_star) {
            continue;
        }
        copy_name(original_name, stmt_select_items(stmt)[index].column.name);
        if (rewrite_flattened_column_ref(&stmt_select_items(stmt)[index].column,
            source, source_qualifier, target_qualifier,
            allow_unqualified) < 0) {
            return -1;
        }
        if (stmt_select_items(stmt)[index].alias[0] == '\0'
            && strcmp(original_name,
                stmt_select_items(stmt)[index].column.name)
                != 0) {
            copy_name(stmt_select_items(stmt)[index].alias, original_name);
        }
    }
    return 0;
}

static int rewrite_flattened_group_items(sql_statement *stmt,
    const sql_statement *source, const char *source_qualifier,
    const char *target_qualifier,
    unsigned char allow_unqualified)
{
    unsigned char index;

    for (index = 0; index < stmt_group_count(stmt); index++) {
        if (rewrite_flattened_column_ref(&stmt_group_items(stmt)[index], source,
            source_qualifier, target_qualifier, allow_unqualified) < 0) {
            return -1;
        }
    }
    return 0;
}

static int qualifier_matches_flatten_source(const sql_statement *source,
    const char *qualifier)
{
    return qualifier[0] != '\0'
        && (strcmp(qualifier, source->name) == 0
            || (stmt_from_alias(source)[0] != '\0'
                && strcmp(qualifier, stmt_from_alias(source)) == 0));
}

static void rewrite_node_qualifier(sql_where_node *node,
    const sql_statement *source, const char *target_qualifier)
{
    if (qualifier_matches_flatten_source(source, node->qualifier)) {
        copy_name(node->qualifier, target_qualifier);
    }
}

static int rewrite_operand_qualifier(sql_predicate_operand *operand,
    const sql_statement *source, const char *target_qualifier)
{
    if (operand->kind != sql_predicate_operand_column) {
        return 0;
    }
    if (!qualifier_matches_flatten_source(source,
        operand->data.column.qualifier)) {
        return 0;
    }
    copy_name(operand->data.column.qualifier, target_qualifier);
    return 0;
}

static int copy_flattened_source_where(sql_statement *stmt,
    const sql_statement *source, unsigned char node_base,
    unsigned char value_base, const unsigned char *subquery_map,
    const char *target_qualifier)
{
    unsigned char index;
    sql_where_node *node;

    for (index = 0; index < source->where.value_count; index++) {
        stmt->where_values[value_base + index] = source->where_values[index];
        if (rewrite_operand_qualifier(&stmt->where_values[value_base + index],
            source, target_qualifier) != 0) {
            return -1;
        }
    }
    for (index = 0; index < source->where.node_count; index++) {
        node = &stmt->where_nodes[node_base + index];
        *node = source->where_nodes[index];
        if (node->left != (unsigned char)sql_where_nil) {
            node->left = (unsigned char)(node->left + node_base);
        }
        if (node->right != (unsigned char)sql_where_nil) {
            node->right = (unsigned char)(node->right + node_base);
        }
        node->value_first = (unsigned char)(node->value_first + value_base);
        if (node->type == sql_where_quantified
            || node->type == sql_where_exists) {
            node->subquery_index = subquery_map[node->subquery_index];
        }
        rewrite_node_qualifier(node, source, target_qualifier);
    }
    return 0;
}

static int merge_source_predicate_subqueries(sql_statement *stmt,
    const sql_statement *source,
    unsigned char subquery_map[sql_max_predicate_subqueries])
{
    unsigned char source_index;
    unsigned char target_index;

    for (source_index = 0; source_index < source->predicate_subquery_count;
        source_index++) {
        target_index = 0;
        while (target_index < stmt->predicate_subquery_count) {
            if (strcmp(stmt->predicate_subqueries[target_index],
                source->predicate_subqueries[source_index]) == 0) {
                break;
            }
            target_index++;
        }
        if (target_index == stmt->predicate_subquery_count) {
            if (stmt->predicate_subquery_count
                >= sql_max_predicate_subqueries) {
                return -1;
            }
            copy_subquery(stmt->predicate_subqueries[target_index],
                source->predicate_subqueries[source_index]);
            stmt->predicate_subquery_count++;
        }
        subquery_map[source_index] = target_index;
    }
    return 0;
}

static int merge_flattened_source_where(sql_statement *stmt,
    const sql_statement *source, const char *target_qualifier)
{
    unsigned char original_node_count;
    unsigned char original_value_count;
    unsigned char adjusted_source_root;
    unsigned char new_root;
    unsigned char subquery_map[sql_max_predicate_subqueries];
    sql_where_node *node;

    if (merge_source_predicate_subqueries(stmt, source, subquery_map) != 0) {
        return -1;
    }
    if (!source->where.active) {
        return 0;
    }

    if (!stmt->where.active) {
        stmt->where = source->where;
        return copy_flattened_source_where(stmt, source, 0, 0, subquery_map,
            target_qualifier);
    }

    original_node_count = stmt->where.node_count;
    original_value_count = stmt->where.value_count;
    if ((unsigned short)original_node_count + source->where.node_count + 1
            > sql_where_max_nodes
        || (unsigned short)original_value_count + source->where.value_count
            > sql_where_max_values) {
        return -1;
    }
    if (copy_flattened_source_where(stmt, source, original_node_count,
        original_value_count, subquery_map, target_qualifier) != 0) {
        return -1;
    }

    adjusted_source_root = (unsigned char)(original_node_count
        + source->where.root);
    new_root = (unsigned char)(original_node_count + source->where.node_count);
    node = &stmt->where_nodes[new_root];
    memset(node, 0, sizeof(*node));
    node->type = sql_where_and;
    node->left = stmt->where.root;
    node->right = adjusted_source_root;

    stmt->where.root = new_root;
    stmt->where.node_count = (unsigned char)(original_node_count
        + source->where.node_count + 1);
    stmt->where.value_count = (unsigned char)(original_value_count
        + source->where.value_count);
    return 0;
}

static int lower_select(sqlexec_program *program,
    const sql_statement *statement)
{
    char names[sql_max_columns][sql_name_size];
    char qualifiers[sql_max_columns][sql_name_size];
    char aliases[sql_max_columns][sql_name_size];
    char group_names[sql_max_columns][sql_name_size];
    char group_qualifiers[sql_max_columns][sql_name_size];
    sqlexec_ref root;
    sqlexec_node *node;
    unsigned char first;
    unsigned char qualifier_first;
    unsigned char alias_first;
    unsigned char group_first;
    unsigned char group_qualifier_first;
    unsigned char index;

    set_program_table_name(program, statement->name);
    if (copy_predicate_subqueries(program, statement) != 0) {
        return -1;
    }
    if (copy_where_payload(program, statement) != 0) {
        return -1;
    }

    if (stmt_from_is_subquery(statement)) {
        /* Materialise the inner query to a temp table first. */
        copy_subquery(program_subquery_text(program), statement->subquery_text);
    }

    if (stmt_select_count_star(statement)) {
        if (lower_scan_root(program, statement) != 0) {
            return -1;
        }
    } else {
        root = sqlexec_make_root(program, sqlexec_project);
        if (root == sqlexec_nil) {
            return -1;
        }
        node = sqlexec_get(program, root);
        if (!node) {
            return -1;
        }
        node->data.project.select_all = stmt_select_all(statement);
        node->data.project.distinct = stmt_select_distinct(statement);
        node->data.project.has_aggregate =
            stmt_select_has_aggregate(statement);
        if (!stmt_select_all(statement)) {
            for (index = 0; index < stmt_select_count(statement); index++) {
                node->data.project.functions[index] =
                    stmt_select_items(statement)[index].function;
                node->data.project.function_arg_is_star[index] =
                    stmt_select_items(statement)[index].argument_is_star;
                if (stmt_join_count(statement) > 0) {
                    copy_name(qualifiers[index],
                        stmt_select_items(statement)[index].column.qualifier);
                } else {
                    qualifiers[index][0] = '\0';
                }
                copy_name(names[index],
                    stmt_select_items(statement)[index].column.name);
                copy_name(aliases[index],
                    stmt_select_items(statement)[index].alias);
            }
            if (sqlexec_add_names(program, qualifiers,
                    stmt_select_count(statement),
                &qualifier_first) != 0
                || sqlexec_add_names(program, names,
                    stmt_select_count(statement),
                    &first) != 0
                || sqlexec_add_names(program, aliases,
                    stmt_select_count(statement), &alias_first) != 0) {
                return -1;
            }
            node->data.project.qualifiers.first = qualifier_first;
            node->data.project.qualifiers.count = stmt_select_count(statement);
            node->data.project.names.first = first;
            node->data.project.names.count = stmt_select_count(statement);
            node->data.project.aliases.first = alias_first;
            node->data.project.aliases.count = stmt_select_count(statement);

            if (stmt_group_count(statement) > 0) {
                for (index = 0; index < stmt_group_count(statement); index++) {
                    if (stmt_join_count(statement) > 0) {
                        copy_name(group_qualifiers[index],
                            stmt_group_items(statement)[index].qualifier);
                    } else {
                        group_qualifiers[index][0] = '\0';
                    }
                    copy_name(group_names[index],
                        stmt_group_items(statement)[index].name);
                }
                if (sqlexec_add_names(program, group_qualifiers,
                    stmt_group_count(statement), &group_qualifier_first) != 0
                    || sqlexec_add_names(program, group_names,
                        stmt_group_count(statement), &group_first) != 0) {
                    return -1;
                }
                node->data.project.group_qualifiers.first =
                    group_qualifier_first;
                node->data.project.group_qualifiers.count =
                    stmt_group_count(statement);
                node->data.project.group_names.first = group_first;
                node->data.project.group_names.count =
                    stmt_group_count(statement);
            }
        }
        if (copy_having_payload(program, statement) != 0) {
            return -1;
        }
        if (append_scan_input(program, root, statement) != 0) {
            return -1;
        }
    }

    return 0;
}

static int lower_create_view(sqlexec_program *program,
    const sql_statement *statement)
{
    sqlexec_ref root;

    root = sqlexec_make_root(program, sqlexec_create_view);
    if (root == sqlexec_nil) {
        return -1;
    }
    copy_subquery(program_subquery_text(program), statement->subquery_text);
    return set_named_payload(program, root, statement->name);
}

static int lower_drop_view(sqlexec_program *program,
    const sql_statement *statement)
{
    sqlexec_ref root;

    root = sqlexec_make_root(program, sqlexec_drop_view);
    return (root == sqlexec_nil
        || set_named_payload(program, root, statement->name) != 0) ? -1 : 0;
}

static int lower_insert(sqlexec_program *program,
    const sql_statement *statement)
{
    sqlexec_ref root;
    sqlexec_node *node;
    unsigned char first;

    set_program_table_name(program, statement->name);
    if (copy_predicate_subqueries(program, statement) != 0) {
        return -1;
    }

    root = sqlexec_make_root(program, sqlexec_append_record);
    if (root == sqlexec_nil) {
        return -1;
    }
    if (sqlexec_add_assignments(program, stmt_assignments(statement),
        stmt_assignment_count(statement), &first) != 0) {
        return -1;
    }
    node = sqlexec_get(program, root);
    if (!node) {
        return -1;
    }
    node->data.assignments.first = first;
    node->data.assignments.count = stmt_assignment_count(statement);

    return 0;
}

static int lower_update(sqlexec_program *program,
    const sql_statement *statement)
{
    sqlexec_ref root;
    sqlexec_node *node;
    unsigned char first;

    set_program_table_name(program, statement->name);
    if (copy_predicate_subqueries(program, statement) != 0) {
        return -1;
    }
    if (copy_where_payload(program, statement) != 0) {
        return -1;
    }

    root = sqlexec_make_root(program, sqlexec_write_current);
    if (root == sqlexec_nil) {
        return -1;
    }
    if (sqlexec_add_assignments(program, stmt_assignments(statement),
        stmt_assignment_count(statement), &first) != 0) {
        return -1;
    }
    node = sqlexec_get(program, root);
    if (!node) {
        return -1;
    }
    node->data.assignments.first = first;
    node->data.assignments.count = stmt_assignment_count(statement);
    if (append_scan_input(program, root, statement) != 0) {
        return -1;
    }

    return 0;
}

static int lower_delete(sqlexec_program *program,
    const sql_statement *statement)
{
    sqlexec_ref root;

    set_program_table_name(program, statement->name);
    if (copy_predicate_subqueries(program, statement) != 0) {
        return -1;
    }
    if (copy_where_payload(program, statement) != 0) {
        return -1;
    }

    root = sqlexec_make_root(program, sqlexec_delete_current);
    if (root == sqlexec_nil || append_scan_input(program, root,
        statement) != 0) {
        return -1;
    }

    return 0;
}

static int lower_drop_table(sqlexec_program *program,
    const sql_statement *statement)
{
    return lower_named_root(program, sqlexec_drop_table, statement->name);
}

static int lower_drop_database(sqlexec_program *program,
    const sql_statement *statement)
{
    return lower_named_root(program, sqlexec_drop_database, statement->name);
}

static int build_program_from_statement(sqlexec_program *program,
    const sql_statement *statement)
{
    int result;

    sqlexec_reset(program);
    result = -1;
    switch (statement->type) {
    case sql_statement_create_database:
        result = lower_named_root(program, sqlexec_create_database,
            statement->name);
        break;
    case sql_statement_use:
        result = lower_named_root(program, sqlexec_use_database,
            statement->name);
        break;
    case sql_statement_drop_database:
        result = lower_drop_database(program, statement);
        break;
    case sql_statement_drop_table:
        result = lower_drop_table(program, statement);
        break;
    case sql_statement_create_table:
        result = lower_create_table(program, statement);
        break;
    case sql_statement_create_index:
        result = lower_create_index(program, statement);
        break;
    case sql_statement_create_view:
        result = lower_create_view(program, statement);
        break;
    case sql_statement_drop_view:
        result = lower_drop_view(program, statement);
        break;
    case sql_statement_select:
        result = lower_select(program, statement);
        break;
    case sql_statement_insert:
        result = lower_insert(program, statement);
        break;
    case sql_statement_update:
        result = lower_update(program, statement);
        break;
    case sql_statement_delete:
        result = lower_delete(program, statement);
        break;
    default:
        result = -1;
        break;
    }

    if (result != 0 || sqlexec_validate(program) != 0) {
        sqlexec_reset(program);
        return -1;
    }

    return 0;
}

/*
 * Rewrites an UPDATE or DELETE statement to use a simple base-table
 * view. The view WHERE is merged into the mutation WHERE after
 * rebinding its qualifiers to the base table name.
 */
static int flatten_view_into_mutation(sql_statement *stmt,
    const sql_statement *view)
{
    if (!is_simple_flattenable_select(view)) {
        return -1;
    }

    copy_name(stmt->name, view->name);
    return merge_flattened_source_where(stmt, view, view->name);
}

/*
 * Rewrites a SELECT over a simple source view or inline subquery to
 * read directly from the base table and merge the inner WHERE tree.
 */
static int flatten_simple_select_source(sql_statement *stmt,
    const sql_statement *source, const char *source_qualifier,
    const char *target_qualifier)
{
    if (!is_simple_flattenable_select(source)) {
        return -1;
    }
    if (!stmt_select_all(source)) {
        if (rewrite_flattened_select_projection(stmt, source,
            source_qualifier,
            target_qualifier, stmt_join_count(stmt) == 0) != 0
            || rewrite_flattened_group_items(stmt, source,
                source_qualifier,
                target_qualifier, stmt_join_count(stmt) == 0) != 0
            || rewrite_flattened_where(stmt, source, source_qualifier,
                target_qualifier,
                stmt_join_count(stmt) == 0) != 0) {
            return -1;
        }
    }
    copy_name(stmt->name, source->name);
    stmt_from_is_subquery(stmt) = 0;
    stmt->subquery_text[0] = '\0';
    return merge_flattened_source_where(stmt, source, target_qualifier);
}

/*
 * Validates user view SQL by parsing it immediately and returns the
 * validated sql_statement through view_inner_out. Returns zero on
 * success and -1 when the view SQL has a syntax error.
 */
static int validate_view_sql(const char *view_stmt,
    sql_statement *view_inner_out)
{
    return sql_parse_select_body(view_stmt, view_inner_out);
}

static int expand_and_build_program(sql_statement *statement,
    sqlexec_program *program, const char *root, const char *current_db)
{
    sql_statement view_inner;
    char original_from_name[sql_name_size];
    char view_stmt[sql_subquery_size];

    if (!program) {
        return -1;
    }

    /*
     * Source expansion and flattening.
     *   SELECT sys_*                → built-in view, materialise later.
     *   SELECT simple_view          → flatten now when safe, else materialise.
     *   SELECT FROM (simple query)  → flatten now when safe, else materialise.
     *   UPDATE/DELETE simple_view   → flatten now into base-table mutation.
     */
    if (statement->type == sql_statement_select
        && statement_from_is_subquery(statement)) {
        if (validate_view_sql(statement->subquery_text, &view_inner) != 0) {
            sqlexec_reset(program);
            return -1;
        }
        if ((stmt_join_count(statement) == 0
                || stmt_from_alias(statement)[0] != '\0')
            && flatten_simple_select_source(statement, &view_inner,
                stmt_from_alias(statement),
                stmt_join_count(statement) > 0 ? stmt_from_alias(statement)
                    : view_inner.name) == 0) {
            /* flattened inline subquery */
        }
    } else if (!statement_from_is_subquery(statement)
        && statement->name[0] != '_') {
        if (statement->type == sql_statement_select) {
            if (statement->name[0] == 's' && statement->name[1] == 'y'
                && statement->name[2] == 's' && statement->name[3] == '_') {
                copy_subquery(statement->subquery_text, statement->name);
                strcpy(statement->name, "_tmp");
                stmt_from_is_subquery(statement) = 1;
            } else if (root && current_db && current_db[0]
                && find_view(root, current_db, statement->name,
                    NULL, view_stmt) == 0) {
                copy_name(original_from_name, statement->name);
                if (validate_view_sql(view_stmt, &view_inner) != 0) {
                    sqlexec_reset(program);
                    return -1;
                }
                if (stmt_join_count(statement) > 0
                    && stmt_from_alias(statement)[0] == '\0') {
                    copy_name(stmt_from_alias(statement), original_from_name);
                }
                if (flatten_simple_select_source(statement, &view_inner,
                    stmt_from_alias(statement)[0] != '\0'
                        ? stmt_from_alias(statement) : original_from_name,
                    stmt_join_count(statement) > 0
                        ? stmt_from_alias(statement)
                        : view_inner.name) != 0) {
                    copy_subquery(statement->subquery_text, view_stmt);
                    strcpy(statement->name, "_tmp");
                    stmt_from_is_subquery(statement) = 1;
                }
            }
        } else if ((statement->type == sql_statement_update
            || statement->type == sql_statement_delete)
            && root && current_db && current_db[0]
            && find_view(root, current_db, statement->name,
                NULL, view_stmt) == 0) {
            if (validate_view_sql(view_stmt, &view_inner) != 0
                || flatten_view_into_mutation(statement, &view_inner) != 0) {
                sqlexec_reset(program);
                return -1;
            }
        }
    }

    return build_program_from_statement(program, statement);
}

static int parse_and_build_program(const char *text,
    sqlexec_program *program, const char *root,
    const char *current_db, unsigned char select_only)
{
    sql_statement statement;

    if (!program
        || ((select_only
                ? sql_parse_select_body(text, &statement)
                : sql_parse_statement(text, &statement)) != 0)) {
        if (program) {
            sqlexec_reset(program);
        }
        return -1;
    }

    return expand_and_build_program(&statement, program, root, current_db);
}

int sql_parse(const char *text, sqlexec_program *program,
    const char *root, const char *current_db)
{
    return parse_and_build_program(text, program, root, current_db, 0);
}

int sql_parse_select_program(const char *text, sqlexec_program *program,
    const char *root, const char *current_db)
{
    return parse_and_build_program(text, program, root, current_db, 1);
}

int sql_run(sql_context *ctx)
{
    return sql_parse(ctx->text, &ctx->program, ctx->root, ctx->current_db);
}
