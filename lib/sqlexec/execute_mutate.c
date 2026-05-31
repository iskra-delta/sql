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
/* Index scan helpers                                                   */
/* ------------------------------------------------------------------ */

static int open_ndx(const sqlexec_env *env, const char *index_name,
    ndx_file *ndx)
{
    char index_path[path_buffer_size];

    if (build_index_path(env->root, env->current_db, index_name,
        index_path) != 0) {
        return -1;
    }
    return ndx_open(ndx, index_path);
}

static int build_eq_key(ndx_file *ndx, const sql_value *value,
    unsigned char *key_out)
{
    if (ndx->key_type == ndx_key_type_numeric) {
        return ndx_encode_number_key(key_out, value->text);
    }
    return ndx_encode_text_key(key_out, ndx->key_length, value->text);
}

static int build_bound_key(ndx_file *ndx, const sql_value *value,
    unsigned char *key_out)
{
    if (value->type == sql_value_none || value->text[0] == '\0') {
        return -1;
    }
    return build_eq_key(ndx, value, key_out);
}

/* ------------------------------------------------------------------ */
/* Tree navigation helpers                                              */
/* ------------------------------------------------------------------ */

static sqlexec_ref child_at(const sqlexec_program *program,
    sqlexec_ref parent, unsigned char index)
{
    sqlexec_ref child;

    child = program->nodes[parent].first_child;
    while (child != sqlexec_nil && index > 0) {
        child = program->nodes[child].next_sibling;
        index--;
    }
    return child;
}

static int resolve_scan_node(const sqlexec_program *program,
    sqlexec_ref input_ref, sql_where *where, sqlexec_ref *scan_ref_out)
{
    const sqlexec_node *node;

    if (!where || !scan_ref_out || input_ref == sqlexec_nil) {
        return -1;
    }
    memset(where, 0, sizeof(*where));
    node = sqlexec_get_const(program, input_ref);
    if (!node) {
        return -1;
    }
    if (node->opcode == sqlexec_filter) {
        *where = node->data.where;
        input_ref = child_at(program, input_ref, 0);
        if (input_ref == sqlexec_nil) {
            return -1;
        }
        node = sqlexec_get_const(program, input_ref);
        if (!node) {
            return -1;
        }
    }
    if (node->opcode != sqlexec_table_scan
        && node->opcode != sqlexec_join_scan
        && node->opcode != sqlexec_index_scan_eq
        && node->opcode != sqlexec_index_scan_range) {
        return -1;
    }
    *scan_ref_out = input_ref;
    return 0;
}

/* ------------------------------------------------------------------ */
/* INSERT                                                               */
/* ------------------------------------------------------------------ */

int exec_insert(sqlexec_env *env, const char *table_name,
    sqlexec_span values)
{
    dbf_file file;
    dbf_field fields[sql_max_columns];
    unsigned short offsets[sql_max_columns];
    char record[table_record_size];
    unsigned short index;

    if (open_table_file(env->root, env->current_db, table_name, &file,
        fields, offsets) != 0) {
        return -1;
    }
    if (values.count != file.field_count) {
        dbf_close(&file);
        return -1;
    }
    clear_record(record, file.record_length);
    for (index = 0; index < file.field_count; index++) {
        if (store_value_in_field(record + offsets[index], &fields[index],
            &env->program->values[values.first + index]) != 0) {
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
    sqlexec_ref count_ref)
{
    const sqlexec_node *apply_node;
    const sqlexec_node *scan_node;
    dbf_file file;
    dbf_field fields[sql_max_columns];
    unsigned short offsets[sql_max_columns];
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
    ndx_file ndx;
    exec_index_scan_ctx scan_ctx;
    int scan_result;

    ref = child_at(env->program, count_ref, 0);
    if (ref == sqlexec_nil
        || env->program->nodes[ref].opcode
            != sqlexec_write_current) {
        return -1;
    }
    ref = child_at(env->program, ref, 0);
    apply_node = sqlexec_get_const(env->program, ref);
    if (!apply_node
        || apply_node->opcode != sqlexec_apply_assignments
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
        assignment_index < apply_node->data.assignments.count;
        assignment_index++) {
        field_index = find_field_index(fields, file.field_count,
            env->program->assignments[
                apply_node->data.assignments.first
                    + assignment_index].column_name);
        if (field_index < 0) {
            dbf_close(&file);
            return -1;
        }
    }
    if (!where_references_known_fields(env->program, &where,
        &source, NULL)) {
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
    if (scan_node->opcode != sqlexec_table_scan) {
        if (open_ndx(env, scan_node->opcode == sqlexec_index_scan_eq
            ? scan_node->data.index_probe.index_name
            : scan_node->data.index_range.index_name, &ndx) != 0) {
            dbf_close(&file);
            return -1;
        }
        memset(&scan_ctx, 0, sizeof(scan_ctx));
        scan_ctx.ndx = &ndx;
        scan_ctx.file = &file;
        scan_ctx.fields = fields;
        scan_ctx.offsets = offsets;
        scan_ctx.record = record;
        scan_ctx.source = &source;
        scan_ctx.env = env;
        scan_ctx.where = &where;
        scan_ctx.action = exec_scan_update;
        scan_ctx.apply_node = apply_node;
        scan_ctx.changed_count = &changed_count;

        if (scan_node->opcode == sqlexec_index_scan_eq) {
            if (build_eq_key(&ndx,
                &scan_node->data.index_probe.value,
                scan_ctx.eq_key) != 0) {
                ndx_close(&ndx);
                dbf_close(&file);
                return -1;
            }
            scan_result = ndx_scan(&ndx, exec_eq_scan_callback,
                &scan_ctx);
        } else {
            if (scan_node->data.index_range.lower_operator
                != sql_compare_invalid) {
                scan_ctx.has_lower = 1;
                scan_ctx.lower_incl =
                    scan_node->data.index_range.lower_operator
                    == sql_compare_greater_equal;
                build_bound_key(&ndx,
                    &scan_node->data.index_range.lower_value,
                    scan_ctx.lower_key);
            }
            if (scan_node->data.index_range.upper_operator
                != sql_compare_invalid) {
                scan_ctx.has_upper = 1;
                scan_ctx.upper_incl =
                    scan_node->data.index_range.upper_operator
                    == sql_compare_less_equal;
                build_bound_key(&ndx,
                    &scan_node->data.index_range.upper_value,
                    scan_ctx.upper_key);
            }
            scan_result = ndx_scan(&ndx, exec_range_scan_callback,
                &scan_ctx);
        }

        ndx_close(&ndx);
        if (scan_ctx.failed || scan_result < 0) {
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
            if (!where_matches(env->program, &where, &source, NULL)) {
                continue;
            }
            for (assignment_index = 0;
                assignment_index < apply_node->data.assignments.count;
                assignment_index++) {
                field_index = find_field_index(fields, file.field_count,
                    env->program->assignments[
                        apply_node->data.assignments.first
                            + assignment_index].column_name);
                if (store_value_in_field(record + offsets[field_index],
                    &fields[field_index],
                    &env->program->assignments[
                        apply_node->data.assignments.first
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
    sqlexec_ref count_ref)
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
    ndx_file ndx;
    exec_index_scan_ctx scan_ctx;
    int scan_result;

    ref = child_at(env->program, count_ref, 0);
    if (ref == sqlexec_nil
        || env->program->nodes[ref].opcode
            != sqlexec_delete_current
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
    if (!where_references_known_fields(env->program, &where,
        &source, NULL)) {
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
    if (scan_node->opcode != sqlexec_table_scan) {
        if (open_ndx(env, scan_node->opcode == sqlexec_index_scan_eq
            ? scan_node->data.index_probe.index_name
            : scan_node->data.index_range.index_name, &ndx) != 0) {
            dbf_close(&file);
            return -1;
        }
        memset(&scan_ctx, 0, sizeof(scan_ctx));
        scan_ctx.ndx = &ndx;
        scan_ctx.file = &file;
        scan_ctx.fields = fields;
        scan_ctx.offsets = offsets;
        scan_ctx.record = record;
        scan_ctx.source = &source;
        scan_ctx.env = env;
        scan_ctx.where = &where;
        scan_ctx.action = exec_scan_delete;
        scan_ctx.deleted_count = &deleted_count;

        if (scan_node->opcode == sqlexec_index_scan_eq) {
            if (build_eq_key(&ndx,
                &scan_node->data.index_probe.value,
                scan_ctx.eq_key) != 0) {
                ndx_close(&ndx);
                dbf_close(&file);
                return -1;
            }
            scan_result = ndx_scan(&ndx, exec_eq_scan_callback,
                &scan_ctx);
        } else {
            if (scan_node->data.index_range.lower_operator
                != sql_compare_invalid) {
                scan_ctx.has_lower = 1;
                scan_ctx.lower_incl =
                    scan_node->data.index_range.lower_operator
                    == sql_compare_greater_equal;
                build_bound_key(&ndx,
                    &scan_node->data.index_range.lower_value,
                    scan_ctx.lower_key);
            }
            if (scan_node->data.index_range.upper_operator
                != sql_compare_invalid) {
                scan_ctx.has_upper = 1;
                scan_ctx.upper_incl =
                    scan_node->data.index_range.upper_operator
                    == sql_compare_less_equal;
                build_bound_key(&ndx,
                    &scan_node->data.index_range.upper_value,
                    scan_ctx.upper_key);
            }
            scan_result = ndx_scan(&ndx, exec_range_scan_callback,
                &scan_ctx);
        }

        ndx_close(&ndx);
        if (scan_ctx.failed || scan_result < 0) {
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
            if (!where_matches(env->program, &where, &source, NULL)) {
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
