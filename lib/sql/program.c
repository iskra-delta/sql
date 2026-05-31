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
#include "../shared/shared.h"
#include "../shared/catalog.h"

#include <string.h>

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

static int set_table_payload(sqlexec_program *program, sqlexec_ref ref,
    const sql_statement *statement)
{
    sqlexec_node *node;
    unsigned char first;

    if (sqlexec_add_columns(program, statement->columns,
        statement->column_count, &first) != 0) {
        return -1;
    }

    node = sqlexec_get(program, ref);
    if (!node) {
        return -1;
    }
    copy_name(node->data.table.name, statement->name);
    node->data.table.columns.first = first;
    node->data.table.columns.count = statement->column_count;
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
    node->data.index.unique = statement->create_index_unique;
    node->data.index.key_names.first = first;
    node->data.index.key_names.count = statement->key_count;
    return 0;
}

static int set_join_payload(sqlexec_program *program, sqlexec_ref ref,
    const sql_statement *statement)
{
    sqlexec_node *node;
    char table_names[4][sql_name_size];
    unsigned char table_first;

    copy_name(table_names[0], statement->name);
    copy_name(table_names[1], statement->from_alias);
    copy_name(table_names[2], statement->join_table_name);
    copy_name(table_names[3], statement->join_alias);

    if (sqlexec_add_names(program, table_names, 4, &table_first) != 0) {
        return -1;
    }

    node = sqlexec_get(program, ref);
    if (!node) {
        return -1;
    }
    node->data.join.tables.first = table_first;
    node->data.join.tables.count = 4;
    return 0;
}

static int copy_where_payload(sqlexec_program *program,
    const sql_statement *statement)
{
    unsigned char index;

    if (!statement->where.active) {
        program->where_node_count = 0;
        program->where_value_count = 0;
        return 0;
    }

    if (statement->where.node_count > sql_where_max_nodes
        || statement->where.value_count > sql_where_max_values) {
        return -1;
    }

    program->where_node_count = statement->where.node_count;
    memcpy(program->where_nodes, statement->where_nodes,
        (unsigned short)(statement->where.node_count
            * sizeof(statement->where_nodes[0])));
    if (!statement->join_active) {
        for (index = 0; index < statement->where.node_count; index++) {
            program->where_nodes[index].qualifier[0] = '\0';
        }
    }
    program->where_value_count = statement->where.value_count;
    memcpy(program->where_values, statement->where_values,
        (unsigned short)(statement->where.value_count
            * sizeof(statement->where_values[0])));
    return 0;
}

static int append_scan_input(sqlexec_program *program, sqlexec_ref parent,
    const sql_statement *statement)
{
    sqlexec_ref ref;
    sqlexec_node *node;

    if (statement->where.active) {
        if (copy_where_payload(program, statement) != 0) {
            return -1;
        }
        ref = sqlexec_append_child(program, parent, sqlexec_filter);
        if (ref == sqlexec_nil) {
            return -1;
        }
        node = sqlexec_get(program, ref);
        if (!node) {
            return -1;
        }
        node->data.where = statement->where;
        parent = ref;
    }

    if (statement->join_active) {
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
    sqlexec_ref build;
    sqlexec_ref register_op;
    unsigned char first;
    unsigned char index;

    root = sqlexec_make_root(program, sqlexec_sequence);
    if (root == sqlexec_nil) {
        return -1;
    }
    for (index = 0; index < statement->key_count; index++) {
        copy_name(key_names[index], statement->key_names[index]);
    }
    if (sqlexec_add_names(program, key_names, statement->key_count, &first)
        != 0) {
        return -1;
    }

    build = sqlexec_append_child(program, root, sqlexec_build_index);
    if (build == sqlexec_nil
        || set_index_payload(program, build, statement, first) != 0) {
        return -1;
    }

    register_op = sqlexec_append_child(program, root, sqlexec_register_index);
    if (register_op == sqlexec_nil
        || set_index_payload(program, register_op, statement, first) != 0) {
        return -1;
    }

    return 0;
}

static int lower_select(sqlexec_program *program,
    const sql_statement *statement)
{
    char names[sql_max_columns][sql_name_size];
    char qualifiers[sql_max_columns][sql_name_size];
    char aliases[sql_max_columns][sql_name_size];
    sqlexec_ref root;
    sqlexec_ref ref;
    sqlexec_node *node;
    unsigned char first;
    unsigned char qualifier_first;
    unsigned char alias_first;
    unsigned char index;

    root = sqlexec_make_root(program, sqlexec_sequence);
    if (root == sqlexec_nil) {
        return -1;
    }

    if (statement->from_is_subquery) {
        /* Materialise the inner query to a temp table first. */
        copy_subquery(program->subquery_text, statement->subquery_text);
        ref = sqlexec_append_child(program, root, sqlexec_run_subquery);
        if (ref == sqlexec_nil || set_named_payload(program, ref, "_tmp") != 0) {
            return -1;
        }
    }

    ref = sqlexec_append_child(program, root, sqlexec_open_table);
    if (ref == sqlexec_nil || set_named_payload(program, ref,
        statement->name) != 0) {
        return -1;
    }

    if (statement->select_count_star) {
        ref = sqlexec_append_child(program, root, sqlexec_emit_count);
        if (ref == sqlexec_nil) {
            return -1;
        }
        ref = sqlexec_append_child(program, ref, sqlexec_count_rows);
        if (ref == sqlexec_nil || append_scan_input(program, ref,
            statement) != 0) {
            return -1;
        }
    } else {
        ref = sqlexec_append_child(program, root, sqlexec_emit_rows);
        if (ref == sqlexec_nil) {
            return -1;
        }
        ref = sqlexec_append_child(program, ref, sqlexec_project);
        if (ref == sqlexec_nil) {
            return -1;
        }
        node = sqlexec_get(program, ref);
        if (!node) {
            return -1;
        }
        node->data.project.select_all = statement->select_all;
        if (!statement->select_all) {
            for (index = 0; index < statement->select_count; index++) {
                if (statement->join_active) {
                    copy_name(qualifiers[index],
                        statement->select_items[index].column.qualifier);
                } else {
                    qualifiers[index][0] = '\0';
                }
                copy_name(names[index],
                    statement->select_items[index].column.name);
                copy_name(aliases[index], statement->select_items[index].alias);
            }
            if (sqlexec_add_names(program, qualifiers, statement->select_count,
                &qualifier_first) != 0
                || sqlexec_add_names(program, names, statement->select_count,
                    &first) != 0
                || sqlexec_add_names(program, aliases,
                    statement->select_count, &alias_first) != 0) {
                return -1;
            }
            node->data.project.qualifiers.first = qualifier_first;
            node->data.project.qualifiers.count = statement->select_count;
            node->data.project.names.first = first;
            node->data.project.names.count = statement->select_count;
            node->data.project.aliases.first = alias_first;
            node->data.project.aliases.count = statement->select_count;
        }
        if (append_scan_input(program, ref, statement) != 0) {
            return -1;
        }
    }

    ref = sqlexec_append_child(program, root, sqlexec_close_table);
    if (ref == sqlexec_nil) {
        return -1;
    }

    if (statement->from_is_subquery) {
        ref = sqlexec_append_child(program, root, sqlexec_delete_temp);
        if (ref == sqlexec_nil || set_named_payload(program, ref, "_tmp") != 0) {
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
    copy_subquery(program->subquery_text, statement->subquery_text);
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
    sqlexec_ref ref;
    sqlexec_node *node;
    unsigned char first;

    root = sqlexec_make_root(program, sqlexec_sequence);
    if (root == sqlexec_nil) {
        return -1;
    }

    ref = sqlexec_append_child(program, root, sqlexec_open_table);
    if (ref == sqlexec_nil || set_named_payload(program, ref,
        statement->name) != 0) {
        return -1;
    }

    ref = sqlexec_append_child(program, root, sqlexec_count_affected);
    if (ref == sqlexec_nil) {
        return -1;
    }
    ref = sqlexec_append_child(program, ref, sqlexec_append_record);
    if (ref == sqlexec_nil) {
        return -1;
    }
    ref = sqlexec_append_child(program, ref, sqlexec_make_record);
    if (ref == sqlexec_nil) {
        return -1;
    }
    if (sqlexec_add_values(program, statement->values, statement->value_count,
        &first) != 0) {
        return -1;
    }
    node = sqlexec_get(program, ref);
    if (!node) {
        return -1;
    }
    node->data.values.first = first;
    node->data.values.count = statement->value_count;

    ref = sqlexec_append_child(program, root, sqlexec_rebuild_table_indexes);
    if (ref == sqlexec_nil || set_named_payload(program, ref,
        statement->name) != 0) {
        return -1;
    }

    ref = sqlexec_append_child(program, root, sqlexec_close_table);
    return ref == sqlexec_nil ? -1 : 0;
}

static int lower_update(sqlexec_program *program,
    const sql_statement *statement)
{
    sqlexec_ref root;
    sqlexec_ref ref;
    sqlexec_node *node;
    unsigned char first;

    root = sqlexec_make_root(program, sqlexec_sequence);
    if (root == sqlexec_nil) {
        return -1;
    }

    ref = sqlexec_append_child(program, root, sqlexec_open_table);
    if (ref == sqlexec_nil || set_named_payload(program, ref,
        statement->name) != 0) {
        return -1;
    }

    ref = sqlexec_append_child(program, root, sqlexec_count_affected);
    if (ref == sqlexec_nil) {
        return -1;
    }
    ref = sqlexec_append_child(program, ref, sqlexec_write_current);
    if (ref == sqlexec_nil) {
        return -1;
    }
    ref = sqlexec_append_child(program, ref, sqlexec_apply_assignments);
    if (ref == sqlexec_nil) {
        return -1;
    }
    if (sqlexec_add_assignments(program, statement->assignments,
        statement->assignment_count, &first) != 0) {
        return -1;
    }
    node = sqlexec_get(program, ref);
    if (!node) {
        return -1;
    }
    node->data.assignments.first = first;
    node->data.assignments.count = statement->assignment_count;
    if (append_scan_input(program, ref, statement) != 0) {
        return -1;
    }

    ref = sqlexec_append_child(program, root, sqlexec_rebuild_table_indexes);
    if (ref == sqlexec_nil || set_named_payload(program, ref,
        statement->name) != 0) {
        return -1;
    }

    ref = sqlexec_append_child(program, root, sqlexec_close_table);
    return ref == sqlexec_nil ? -1 : 0;
}

static int lower_delete(sqlexec_program *program,
    const sql_statement *statement)
{
    sqlexec_ref root;
    sqlexec_ref ref;

    root = sqlexec_make_root(program, sqlexec_sequence);
    if (root == sqlexec_nil) {
        return -1;
    }

    ref = sqlexec_append_child(program, root, sqlexec_open_table);
    if (ref == sqlexec_nil || set_named_payload(program, ref,
        statement->name) != 0) {
        return -1;
    }

    ref = sqlexec_append_child(program, root, sqlexec_count_affected);
    if (ref == sqlexec_nil) {
        return -1;
    }
    ref = sqlexec_append_child(program, ref, sqlexec_delete_current);
    if (ref == sqlexec_nil || append_scan_input(program, ref,
        statement) != 0) {
        return -1;
    }

    ref = sqlexec_append_child(program, root, sqlexec_rebuild_table_indexes);
    if (ref == sqlexec_nil || set_named_payload(program, ref,
        statement->name) != 0) {
        return -1;
    }

    ref = sqlexec_append_child(program, root, sqlexec_close_table);
    return ref == sqlexec_nil ? -1 : 0;
}

static int lower_drop_table(sqlexec_program *program,
    const sql_statement *statement)
{
    sqlexec_ref root;
    sqlexec_ref ref;

    root = sqlexec_make_root(program, sqlexec_sequence);
    if (root == sqlexec_nil) {
        return -1;
    }

    ref = sqlexec_append_child(program, root,
        sqlexec_unregister_table_indexes);
    if (ref == sqlexec_nil || set_named_payload(program, ref,
        statement->name) != 0) {
        return -1;
    }

    ref = sqlexec_append_child(program, root, sqlexec_drop_table);
    if (ref == sqlexec_nil || set_named_payload(program, ref,
        statement->name) != 0) {
        return -1;
    }

    return 0;
}

static int lower_drop_database(sqlexec_program *program,
    const sql_statement *statement)
{
    sqlexec_ref root;
    sqlexec_ref ref;

    root = sqlexec_make_root(program, sqlexec_sequence);
    if (root == sqlexec_nil) {
        return -1;
    }

    ref = sqlexec_append_child(program, root,
        sqlexec_unregister_database_indexes);
    if (ref == sqlexec_nil || set_named_payload(program, ref,
        statement->name) != 0) {
        return -1;
    }

    ref = sqlexec_append_child(program, root, sqlexec_drop_database);
    if (ref == sqlexec_nil || set_named_payload(program, ref,
        statement->name) != 0) {
        return -1;
    }

    return 0;
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
 * Rewrites an UPDATE or DELETE statement to use a view's base table.
 * The view's WHERE conditions are ANDed with the statement's own WHERE.
 * Returns zero for simple single-table views and -1 when the view is
 * too complex (has JOIN, aggregates) or there is no space to merge.
 */
static int flatten_view_into_mutation(sql_statement *stmt,
    const sql_statement *view)
{
    unsigned char vi;
    unsigned char orig_nc;
    unsigned char orig_vc;
    unsigned char adjusted_view_root;
    unsigned char new_root;
    sql_where_node *n;

    /* A view is updatable when it maps directly to one base table:
     * no join, no aggregate. The SELECT list is irrelevant for
     * UPDATE/DELETE — only the base table name and WHERE are used. */
    if (view->type != sql_statement_select
        || view->join_active
        || view->select_count_star) {
        return -1;
    }

    copy_name(stmt->name, view->name);

    if (!view->where.active) {
        return 0;
    }

    if (!stmt->where.active) {
        stmt->where = view->where;
        for (vi = 0; vi < view->where.node_count; vi++) {
            stmt->where_nodes[vi] = view->where_nodes[vi];
        }
        for (vi = 0; vi < view->where.value_count; vi++) {
            stmt->where_values[vi] = view->where_values[vi];
        }
        return 0;
    }

    orig_nc = stmt->where.node_count;
    orig_vc = stmt->where.value_count;
    if ((unsigned short)orig_nc + view->where.node_count + 1 > sql_where_max_nodes
        || (unsigned short)orig_vc + view->where.value_count > sql_where_max_values) {
        return -1;
    }

    for (vi = 0; vi < view->where.node_count; vi++) {
        n = &stmt->where_nodes[orig_nc + vi];
        *n = view->where_nodes[vi];
        if (n->left != (unsigned char)sql_where_nil)
            n->left = (unsigned char)(n->left + orig_nc);
        if (n->right != (unsigned char)sql_where_nil)
            n->right = (unsigned char)(n->right + orig_nc);
        n->value_first = (unsigned char)(n->value_first + orig_vc);
    }
    for (vi = 0; vi < view->where.value_count; vi++) {
        stmt->where_values[orig_vc + vi] = view->where_values[vi];
    }

    adjusted_view_root = (unsigned char)(orig_nc + view->where.root);
    new_root = (unsigned char)(orig_nc + view->where.node_count);
    n = &stmt->where_nodes[new_root];
    memset(n, 0, sizeof(*n));
    n->type = sql_where_and;
    n->left = stmt->where.root;
    n->right = adjusted_view_root;

    stmt->where.root = new_root;
    stmt->where.node_count = (unsigned char)(orig_nc + view->where.node_count + 1);
    stmt->where.value_count = (unsigned char)(orig_vc + view->where.value_count);
    return 0;
}

/*
 * Validates user view SQL by parsing it immediately and returns the
 * validated sql_statement through view_inner_out. Returns zero on
 * success and -1 when the view SQL has a syntax error.
 */
static int validate_view_sql(const char *view_stmt,
    sql_statement *view_inner_out)
{
    char buf[sql_subquery_size + 2];
    size_t vlen;

    vlen = strlen(view_stmt);
    if (vlen == 0 || vlen + 2 > sizeof(buf)) {
        return -1;
    }
    memcpy(buf, view_stmt, vlen);
    buf[vlen] = ';';
    buf[vlen + 1] = '\0';
    return sql_parse_statement(buf, view_inner_out);
}

int sql_parse(const char *text, sqlexec_program *program,
    const char *root, const char *current_db)
{
    sql_statement statement;
    sql_statement view_inner;
    char view_stmt[sql_subquery_size];

    if (!program || sql_parse_statement(text, &statement) != 0) {
        if (program) {
            sqlexec_reset(program);
        }
        return -1;
    }

    /*
     * View expansion. Three cases:
     *   SELECT sys_*  → built-in view by prefix, no catalog access.
     *   SELECT name   → user view: validate SQL now, materialise later.
     *   UPDATE/DELETE → user view: validate SQL now, flatten into base
     *                   table with merged WHERE conditions.
     */
    if (!statement.from_is_subquery && statement.name[0] != '_') {
        if (statement.type == sql_statement_select) {
            if (statement.name[0] == 's' && statement.name[1] == 'y'
                && statement.name[2] == 's' && statement.name[3] == '_') {
                copy_subquery(statement.subquery_text, statement.name);
                strcpy(statement.name, "_tmp");
                statement.from_is_subquery = 1;
            } else if (root && current_db && current_db[0]
                && find_view(root, current_db, statement.name,
                    NULL, view_stmt) == 0) {
                if (validate_view_sql(view_stmt, &view_inner) != 0) {
                    sqlexec_reset(program);
                    return -1;
                }
                copy_subquery(statement.subquery_text, view_stmt);
                strcpy(statement.name, "_tmp");
                statement.from_is_subquery = 1;
            }
        } else if ((statement.type == sql_statement_update
            || statement.type == sql_statement_delete)
            && root && current_db && current_db[0]
            && find_view(root, current_db, statement.name,
                NULL, view_stmt) == 0) {
            if (validate_view_sql(view_stmt, &view_inner) != 0
                || flatten_view_into_mutation(&statement,
                    &view_inner) != 0) {
                sqlexec_reset(program);
                return -1;
            }
        }
    }

    return build_program_from_statement(program, &statement);
}

int sql_run(sql_context *ctx)
{
    return sql_parse(ctx->text, &ctx->program, ctx->root, ctx->current_db);
}
