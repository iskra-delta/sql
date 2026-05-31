/*
 * Implements a tiny catalog-driven optimizer for SQL execution trees.
 * The code applies conservative rewrites that keep filter nodes in
 * place and only swap table-scan leaves for index-scan leaves when one
 * registered single-field index safely matches the current predicate.
 *
 * MIT License (see: LICENSE)
 * Copyright (C) 2026 tomaz stih
 */

#include "sqlopt.h"
#include "sqlctx.h"
#include "../shared/shared.h"
#include "../shared/catalog.h"

#include <string.h>

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

static int extract_simple_where_compare(const sqlexec_program *program,
    const sql_where *where, char *field_name,
    sql_compare_operator *operator_out, sql_value *value_out)
{
    const sql_where_node *node;

    if (!where->active || where->node_count != 1 || where->value_count != 1
        || where->root >= where->node_count) {
        return 0;
    }
    node = &program->where_nodes[where->root];
    if (node->type != sql_where_compare || node->value_count != 1
        || node->value_first >= where->value_count) {
        return 0;
    }
    copy_name(field_name, node->column_name);
    *operator_out = node->operator;
    *value_out = program->where_values[node->value_first];
    return 1;
}

static const char *find_open_table_name(const sqlexec_program *program)
{
    sqlexec_ref child;

    if (program->root == sqlexec_nil) {
        return NULL;
    }
    if (program->nodes[program->root].opcode == sqlexec_open_table) {
        return program->nodes[program->root].data.named.name;
    }
    if (program->nodes[program->root].opcode != sqlexec_sequence) {
        return NULL;
    }
    child = program->nodes[program->root].first_child;
    while (child != sqlexec_nil) {
        if (program->nodes[child].opcode == sqlexec_open_table) {
            return program->nodes[child].data.named.name;
        }
        child = program->nodes[child].next_sibling;
    }
    return NULL;
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

static int rewrite_scan_node(sqlexec_program *program, sqlexec_ref scan_ref,
    const char *index_name, sql_compare_operator operator,
    const sql_value *value)
{
    sqlexec_node *scan_node;

    if (operator == sql_compare_equal) {
        if (sqlexec_replace(program, scan_ref, sqlexec_index_scan_eq) != 0) {
            return -1;
        }
        scan_node = sqlexec_get(program, scan_ref);
        if (!scan_node) {
            return -1;
        }
        copy_name(scan_node->data.index_probe.index_name, index_name);
        scan_node->data.index_probe.value = *value;
        return 0;
    }

    if (sqlexec_replace(program, scan_ref, sqlexec_index_scan_range) != 0) {
        return -1;
    }
    scan_node = sqlexec_get(program, scan_ref);
    if (!scan_node) {
        return -1;
    }
    memset(&scan_node->data.index_range, 0,
        sizeof(scan_node->data.index_range));
    copy_name(scan_node->data.index_range.index_name, index_name);
    scan_node->data.index_range.lower_operator = sql_compare_invalid;
    scan_node->data.index_range.upper_operator = sql_compare_invalid;
    if (operator == sql_compare_less
        || operator == sql_compare_less_equal) {
        scan_node->data.index_range.upper_operator = operator;
        scan_node->data.index_range.upper_value = *value;
    } else if (operator == sql_compare_greater
        || operator == sql_compare_greater_equal) {
        scan_node->data.index_range.lower_operator = operator;
        scan_node->data.index_range.lower_value = *value;
    } else {
        return -1;
    }
    return 0;
}

static int optimize_filter_node(sqlexec_program *program,
    sqlexec_ref filter_ref, const char *root, const char *db_name,
    const char *table_name)
{
    const sqlexec_node *filter_node;
    const sqlexec_node *scan_node;
    dbf_field fields[sql_max_columns];
    unsigned short field_count;
    sqlexec_ref scan_ref;
    sql_compare_operator operator;
    sql_value value;
    int field_index;
    char field_name[sql_name_size];
    char index_name[sql_name_size];

    filter_node = sqlexec_get_const(program, filter_ref);
    if (!filter_node || filter_node->opcode != sqlexec_filter
        || !extract_simple_where_compare(program,
            &filter_node->data.where, field_name, &operator, &value)
        || !operator_uses_index(operator)) {
        return 0;
    }
    scan_ref = filter_node->first_child;
    scan_node = sqlexec_get_const(program, scan_ref);
    if (!scan_node || scan_node->opcode != sqlexec_table_scan) {
        return 0;
    }
    if (open_table_fields(root, db_name, table_name, fields,
        &field_count) != 0) {
        return 0;
    }
    field_index = find_field_index(fields, field_count, field_name);
    if (field_index < 0
        || !value_matches_field_type(&fields[field_index], &value)) {
        return 0;
    }
    if (find_matching_index(root, db_name, table_name, field_name,
        index_name) != 0) {
        return 0;
    }
    return rewrite_scan_node(program, scan_ref, index_name, operator,
        &value);
}

int sqlopt_optimize(sqlexec_program *program, const char *root,
    const char *db_name)
{
    sqlexec_ref stack[sqlexec_max_nodes];
    unsigned short depth;
    sqlexec_ref ref;
    sqlexec_ref child;
    const char *table_name;

    if (!program) {
        return -1;
    }
    if (!root || !root[0] || !db_name || !db_name[0]
        || program->root == sqlexec_nil) {
        return 0;
    }
    table_name = find_open_table_name(program);
    if (!table_name || !table_name[0]) {
        return 0;
    }
    depth = 0;
    stack[depth++] = program->root;
    while (depth > 0) {
        ref = stack[--depth];
        if (program->nodes[ref].opcode == sqlexec_filter
            && optimize_filter_node(program, ref, root, db_name,
                table_name) != 0) {
            return -1;
        }
        child = program->nodes[ref].first_child;
        while (child != sqlexec_nil) {
            if (depth >= sqlexec_max_nodes) {
                return -1;
            }
            stack[depth++] = child;
            child = program->nodes[child].next_sibling;
        }
    }
    return 0;
}

int sqlopt_run(sql_context *ctx)
{
    return sqlopt_optimize(&ctx->program, ctx->root, ctx->current_db);
}
