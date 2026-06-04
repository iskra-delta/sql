/*
 * Executes INSERT, UPDATE, and DELETE statements.
 * Shares the scan-filter pattern with execute_select.c but writes
 * back to the table instead of projecting output.
 *
 * MIT License (see: LICENSE)
 * Copyright (C) 2026 tomaz stih
 */

#include "exec_impl.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Output helpers (local to this module)                               */
/* ------------------------------------------------------------------ */

static void mut_write_char(const sqlexec_env *env, char c)
{
    if (env->io && env->io->write_char) {
        env->io->write_char(c);
    }
}

static void mut_write_nl(const sqlexec_env *env)
{
    mut_write_char(env, '\r');
    mut_write_char(env, '\n');
}

static void mut_write_str(const sqlexec_env *env, const char *s)
{
    while (*s) {
        mut_write_char(env, *s++);
    }
}

static void mut_write_uint(const sqlexec_env *env, unsigned short n)
{
    char buf[6];
    uint_to_str(n, buf);
    mut_write_str(env, buf);
}

/* ------------------------------------------------------------------ */
/* INSERT                                                               */
/* ------------------------------------------------------------------ */

int exec_insert(sqlexec_env *env, const char *table_name,
    sqlexec_span assignments)
{
    dbf_file file;
    dbf_field fields[sql_max_columns];
    unsigned short offsets[sql_max_columns];
    char record[table_record_size];
    int field_index;
    int explicit_names;
    unsigned short index;

    if (open_table_file(env->root, env->current_db, table_name, &file,
        fields, offsets) != 0) {
        return -1;
    }
    clear_record(record, file.record_length);
    explicit_names = 0;
    for (index = 0; index < assignments.count; index++) {
        if (program_assignments(env->program)[assignments.first + index]
            .column_name[0] != '\0') {
            explicit_names = 1;
            break;
        }
    }
    if (!explicit_names && assignments.count != file.field_count) {
        dbf_close(&file);
        return -1;
    }
    for (index = 0; index < assignments.count; index++) {
        if (!explicit_names) {
            field_index = (int)index;
        } else {
            field_index = find_field_index(fields, file.field_count,
                program_assignments(env->program)[assignments.first + index]
                    .column_name);
            if (field_index < 0) {
                dbf_close(&file);
                return -1;
            }
        }
        if (store_value_in_field(record + offsets[field_index],
            &fields[field_index],
            &program_assignments(env->program)[assignments.first + index]
                .value) != 0) {
            dbf_close(&file);
            return -1;
        }
    }
    if (dbf_append(&file, record) != 0) {
        dbf_close(&file);
        return -1;
    }
    if (dbf_close(&file) != 0) {
        return -1;
    }
    if (rebuild_table_indexes(env->root, env->current_db,
        table_name) != 0) {
        return -1;
    }
    mut_write_str(env, "inserted 1");
    mut_write_nl(env);
    return 0;
}

/* ------------------------------------------------------------------ */
/* UPDATE                                                               */
/* ------------------------------------------------------------------ */

int exec_update(sqlexec_env *env, const char *table_name,
    sqlexec_ref action_ref)
{
    const sqlexec_node *write_node;
    const sqlexec_node *scan_node;
    dbf_file file;
    dbf_field fields[sql_max_columns];
    unsigned short offsets[sql_max_columns];
    unsigned short assignment_fields[sql_max_columns];
    char record[table_record_size];
    sql_where where;
    unsigned long index;
    unsigned short changed_count;
    unsigned short assignment_index;
    sqlexec_ref scan_ref;
    sqlexec_ref ref;
    row_source source;
    int state;
    int field_index;
    where_binding where_binding_storage;
    const where_binding *where_binding;
    exec_index_scan_ctx scan_ctx;
    sql_predicate_subquery_cache predicate_subqueries;

    memset(&predicate_subqueries, 0, sizeof(predicate_subqueries));
    where_binding = NULL;

    ref = action_ref;
    if (ref == sqlexec_nil
        || env->program->nodes[ref].opcode != sqlexec_write_current) {
        return -1;
    }
    write_node = sqlexec_get_const(env->program, ref);
    if (!write_node
        || resolve_scan_node(env->program,
            child_at(env->program, ref, 0), &where,
            &scan_ref) != 0) {
        return -1;
    }
    if (open_table_file(env->root, env->current_db, table_name, &file,
        fields, offsets) != 0) {
        return -1;
    }
    source.table_name = table_name;
    source.alias = "";
    source.fields = fields;
    source.offsets = offsets;
    source.record = record;
    source.field_count = file.field_count;

    for (assignment_index = 0;
        assignment_index < write_node->data.assignments.count;
        assignment_index++) {
        field_index = find_field_index(fields, file.field_count,
            program_assignments(env->program)[
                write_node->data.assignments.first
                    + assignment_index].column_name);
        if (field_index < 0) {
            dbf_close(&file);
            return -1;
        }
        assignment_fields[assignment_index] = (unsigned short)field_index;
    }
    if (!where_can_use_program_binding(env->program, &where)) {
        if (where_bind_n(env->program, &where, &source, 1,
            &where_binding_storage) != 0) {
            dbf_close(&file);
            return -1;
        }
        where_binding = &where_binding_storage;
    }
    if (where_is_constant_false(&where, env->program->where_nodes)) {
        if (dbf_close(&file) != 0) {
            return -1;
        }
        mut_write_uint(env, 0);
        mut_write_str(env, " updated");
        mut_write_nl(env);
        return 0;
    }
    if (env->program->predicate_subquery_count > 0
        && load_predicate_subqueries(env, &predicate_subqueries) != 0) {
        dbf_close(&file);
        return -1;
    }
    if (scan_ref == sqlexec_nil) {
        dbf_close(&file);
        return -1;
    }
    scan_node = sqlexec_get_const(env->program, scan_ref);
    if (!scan_node) {
        dbf_close(&file);
        return -1;
    }

    changed_count = 0;
    if (scan_uses_index(scan_node)) {
        memset(&scan_ctx, 0, sizeof(scan_ctx));
        scan_ctx.file = &file;
        scan_ctx.fields = fields;
        scan_ctx.offsets = offsets;
        scan_ctx.record = record;
        scan_ctx.source = &source;
        scan_ctx.env = env;
        scan_ctx.where = &where;
        scan_ctx.where_binding = where_binding;
        scan_ctx.action = exec_scan_update;
        scan_ctx.write_node = write_node;
        scan_ctx.assignment_fields = assignment_fields;
        scan_ctx.changed_count = &changed_count;
        scan_ctx.subqueries = &predicate_subqueries;
        if (exec_run_index_scan(env, scan_node, &scan_ctx) != 0) {
            dbf_close(&file);
            return -1;
        }
    } else {
        for (index = 0; index < file.record_count; index++) {
            state = dbf_read(&file, index, record);
            if (state < 0) {
                dbf_close(&file);
                return -1;
            }
            if (state == 1) {
                continue;
            }
            if (!where_matches_bound_n(env->program, &where, where_binding,
                &source, &predicate_subqueries)) {
                continue;
            }
            for (assignment_index = 0;
                assignment_index < write_node->data.assignments.count;
                assignment_index++) {
                field_index = (int)assignment_fields[assignment_index];
                if (store_value_in_field(record + offsets[field_index],
                    &fields[field_index],
                    &program_assignments(env->program)[
                        write_node->data.assignments.first
                            + assignment_index].value) != 0) {
                    dbf_close(&file);
                    return -1;
                }
            }
            if (dbf_write(&file, index, record) != 0) {
                dbf_close(&file);
                return -1;
            }
            changed_count++;
        }
    }
    if (dbf_close(&file) != 0) {
        return -1;
    }
    if (changed_count > 0
        && rebuild_table_indexes(env->root, env->current_db,
            table_name) != 0) {
        return -1;
    }
    mut_write_uint(env, changed_count);
    mut_write_str(env, " updated");
    mut_write_nl(env);
    return 0;
}

/* ------------------------------------------------------------------ */
/* DELETE                                                               */
/* ------------------------------------------------------------------ */

int exec_delete(sqlexec_env *env, const char *table_name,
    sqlexec_ref action_ref)
{
    const sqlexec_node *scan_node;
    dbf_file file;
    dbf_field fields[sql_max_columns];
    unsigned short offsets[sql_max_columns];
    char record[table_record_size];
    sql_where where;
    unsigned long index;
    unsigned short deleted_count;
    sqlexec_ref scan_ref;
    sqlexec_ref ref;
    row_source source;
    int state;
    where_binding where_binding_storage;
    const where_binding *where_binding;
    exec_index_scan_ctx scan_ctx;
    sql_predicate_subquery_cache predicate_subqueries;

    memset(&predicate_subqueries, 0, sizeof(predicate_subqueries));
    where_binding = NULL;

    ref = action_ref;
    if (ref == sqlexec_nil
        || env->program->nodes[ref].opcode != sqlexec_delete_current
        || resolve_scan_node(env->program,
            child_at(env->program, ref, 0), &where,
            &scan_ref) != 0) {
        return -1;
    }
    if (open_table_file(env->root, env->current_db, table_name, &file,
        fields, offsets) != 0) {
        return -1;
    }
    source.table_name = table_name;
    source.alias = "";
    source.fields = fields;
    source.offsets = offsets;
    source.record = record;
    source.field_count = file.field_count;
    if (!where_can_use_program_binding(env->program, &where)) {
        if (where_bind_n(env->program, &where, &source, 1,
            &where_binding_storage) != 0) {
            dbf_close(&file);
            return -1;
        }
        where_binding = &where_binding_storage;
    }
    if (where_is_constant_false(&where, env->program->where_nodes)) {
        if (dbf_close(&file) != 0) {
            return -1;
        }
        mut_write_uint(env, 0);
        mut_write_str(env, " deleted");
        mut_write_nl(env);
        return 0;
    }
    if (env->program->predicate_subquery_count > 0
        && load_predicate_subqueries(env, &predicate_subqueries) != 0) {
        dbf_close(&file);
        return -1;
    }
    if (scan_ref == sqlexec_nil) {
        dbf_close(&file);
        return -1;
    }
    scan_node = sqlexec_get_const(env->program, scan_ref);
    if (!scan_node) {
        dbf_close(&file);
        return -1;
    }

    deleted_count = 0;
    if (scan_uses_index(scan_node)) {
        memset(&scan_ctx, 0, sizeof(scan_ctx));
        scan_ctx.file = &file;
        scan_ctx.fields = fields;
        scan_ctx.offsets = offsets;
        scan_ctx.record = record;
        scan_ctx.source = &source;
        scan_ctx.env = env;
        scan_ctx.where = &where;
        scan_ctx.where_binding = where_binding;
        scan_ctx.action = exec_scan_delete;
        scan_ctx.deleted_count = &deleted_count;
        scan_ctx.subqueries = &predicate_subqueries;
        if (exec_run_index_scan(env, scan_node, &scan_ctx) != 0) {
            dbf_close(&file);
            return -1;
        }
    } else {
        for (index = 0; index < file.record_count; index++) {
            state = dbf_read(&file, index, record);
            if (state < 0) {
                dbf_close(&file);
                return -1;
            }
            if (state == 1) {
                continue;
            }
            if (!where_matches_bound_n(env->program, &where, where_binding,
                &source, &predicate_subqueries)) {
                continue;
            }
            if (dbf_delete(&file, index) != 0) {
                dbf_close(&file);
                return -1;
            }
            deleted_count++;
        }
    }
    if (dbf_close(&file) != 0) {
        return -1;
    }
    if (deleted_count > 0
        && rebuild_table_indexes(env->root, env->current_db,
            table_name) != 0) {
        return -1;
    }
    mut_write_uint(env, deleted_count);
    mut_write_str(env, " deleted");
    mut_write_nl(env);
    return 0;
}
