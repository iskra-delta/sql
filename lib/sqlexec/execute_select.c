/*
 * Executes SELECT statements, including COUNT(*), table scans, index
 * scans, joins, WHERE filtering, and column projection.
 *
 * MIT License (see: LICENSE)
 * Copyright (C) 2026 tomaz stih
 */

#include "exec_impl.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Output helpers (local to this module)                               */
/* ------------------------------------------------------------------ */

static void sel_write_char(const sqlexec_env *env, char c)
{
    if (env->io && env->io->write_char) {
        env->io->write_char(c);
    }
}

static void sel_write_nl(const sqlexec_env *env)
{
    sel_write_char(env, '\r');
    sel_write_char(env, '\n');
}

static void sel_write_str(const sqlexec_env *env, const char *s)
{
    while (*s) {
        sel_write_char(env, *s++);
    }
}

static void sel_write_uint(const sqlexec_env *env, unsigned short n)
{
    char buf[6];
    uint_to_str(n, buf);
    sel_write_str(env, buf);
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
/* Row output                                                           */
/* ------------------------------------------------------------------ */

static void write_projected_row(const sqlexec_env *env,
    const sqlexec_project_def *project, const row_source *left_source,
    const row_source *right_source)
{
    const row_source *source;
    char value[sql_value_size];
    unsigned short index;
    unsigned short output_count;
    int field_index;

    output_count = project->select_all
        ? (unsigned short)(left_source->field_count
            + (right_source ? right_source->field_count : 0))
        : project->names.count;
    for (index = 0; index < output_count; index++) {
        if (project->select_all) {
            if (index < left_source->field_count) {
                source = left_source;
                field_index = (int)index;
            } else if (right_source) {
                source = right_source;
                field_index = (int)(index - left_source->field_count);
            } else {
                sel_write_str(env, "error");
                sel_write_nl(env);
                return;
            }
        } else {
            if (resolve_field_ref(left_source, right_source,
                env->program->names[
                    project->qualifiers.first + index],
                env->program->names[
                    project->names.first + index],
                &source, &field_index) != 0) {
                sel_write_str(env, "error");
                sel_write_nl(env);
                return;
            }
        }
        trim_field_value(value, sizeof(value),
            source->record + source->offsets[field_index],
            source->fields[field_index].length);
        if (index > 0) {
            sel_write_str(env, " | ");
        }
        sel_write_str(env, value);
    }
    sel_write_nl(env);
}

/* ------------------------------------------------------------------ */
/* SELECT executor                                                      */
/* ------------------------------------------------------------------ */

int exec_select(sqlexec_env *env, const char *table_name,
    sqlexec_ref emit_ref)
{
    const sqlexec_node *emit_node;
    const sqlexec_node *project_node;
    const sqlexec_node *scan_node;
    dbf_file file;
    dbf_file right_file;
    dbf_field fields[sql_max_columns];
    dbf_field right_fields[sql_max_columns];
    unsigned short offsets[sql_max_columns];
    unsigned short right_offsets[sql_max_columns];
    char record[table_record_size];
    char right_record[table_record_size];
    char left_join_value[sql_value_size];
    char right_join_value[sql_value_size];
    sql_where where;
    row_source left_source;
    row_source right_source;
    unsigned long index;
    unsigned long right_index;
    unsigned short row_count;
    unsigned short output_count;
    unsigned short select_index;
    sqlexec_ref scan_ref;
    sqlexec_ref child_ref;
    const row_source *resolved_source;
    int state;
    int right_state;
    int field_index;
    int right_opened;
    int left_join_index;
    int right_join_index;
    ndx_file ndx;
    exec_index_scan_ctx scan_ctx;
    int scan_result;

    emit_node = sqlexec_get_const(env->program, emit_ref);
    if (!emit_node) {
        return -1;
    }
    right_opened = 0;
    if (open_table_file(env->root, env->current_db, table_name, &file,
        fields, offsets) != 0) {
        return -1;
    }

    if (emit_node->opcode == sqlexec_emit_rows) {
        child_ref = child_at(env->program, emit_ref, 0);
        project_node = sqlexec_get_const(env->program, child_ref);
        if (!project_node
            || project_node->opcode != sqlexec_project) {
            dbf_close(&file);
            return -1;
        }
        if (resolve_scan_node(env->program,
            child_at(env->program, child_ref, 0),
            &where, &scan_ref) != 0) {
            dbf_close(&file);
            return -1;
        }
    } else if (emit_node->opcode == sqlexec_emit_count) {
        child_ref = child_at(env->program, emit_ref, 0);
        if (child_ref == sqlexec_nil
            || env->program->nodes[child_ref].opcode
                != sqlexec_count_rows
            || resolve_scan_node(env->program,
                child_at(env->program, child_ref, 0),
                &where, &scan_ref) != 0) {
            dbf_close(&file);
            return -1;
        }
        project_node = NULL;
    } else {
        dbf_close(&file);
        return -1;
    }

    scan_node = sqlexec_get_const(env->program, scan_ref);
    if (!scan_node || scan_ref == sqlexec_nil) {
        dbf_close(&file);
        return -1;
    }

    left_source.table_name = table_name;
    left_source.alias = "";
    left_source.fields = fields;
    left_source.offsets = offsets;
    left_source.record = record;
    left_source.field_count = file.field_count;
    right_source.table_name = NULL;
    right_source.alias = NULL;
    right_source.fields = NULL;
    right_source.offsets = NULL;
    right_source.record = right_record;
    right_source.field_count = 0;
    left_join_index = -1;
    right_join_index = -1;

    if (scan_node->opcode == sqlexec_join_scan) {
        left_source.table_name =
            scan_node->data.join.left_table_name;
        left_source.alias = scan_node->data.join.left_alias;
        if (open_table_file(env->root, env->current_db,
            scan_node->data.join.right_table_name, &right_file,
            right_fields, right_offsets) != 0) {
            dbf_close(&file);
            return -1;
        }
        right_opened = 1;
        right_source.table_name =
            scan_node->data.join.right_table_name;
        right_source.alias = scan_node->data.join.right_alias;
        right_source.fields = right_fields;
        right_source.offsets = right_offsets;
        right_source.field_count = right_file.field_count;
        left_join_index = find_field_index(fields, file.field_count,
            scan_node->data.join.left_key_name);
        right_join_index = find_field_index(right_fields,
            right_file.field_count,
            scan_node->data.join.right_key_name);
        if (left_join_index < 0 || right_join_index < 0) {
            dbf_close(&right_file);
            dbf_close(&file);
            return -1;
        }
    }

    if (emit_node->opcode == sqlexec_emit_rows) {
        output_count = project_node->data.project.select_all
            ? (unsigned short)(left_source.field_count
                + (right_opened ? right_source.field_count : 0))
            : project_node->data.project.names.count;
        for (select_index = 0; select_index < output_count;
            select_index++) {
            if (project_node->data.project.select_all) {
                continue;
            }
            if (resolve_field_ref(&left_source,
                right_opened ? &right_source : NULL,
                env->program->names[
                    project_node->data.project.qualifiers.first
                        + select_index],
                env->program->names[
                    project_node->data.project.names.first
                        + select_index],
                &resolved_source, &field_index) != 0) {
                if (right_opened) {
                    dbf_close(&right_file);
                }
                dbf_close(&file);
                return -1;
            }
        }
    }

    if (!where_references_known_fields(env->program, &where,
        &left_source, right_opened ? &right_source : NULL)) {
        if (right_opened) {
            dbf_close(&right_file);
        }
        dbf_close(&file);
        return -1;
    }

    row_count = 0;
    if (!right_opened
        && scan_node->opcode != sqlexec_table_scan) {
        /* Index scan: use NDX to drive record retrieval. */
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
        scan_ctx.source = &left_source;
        scan_ctx.env = env;
        scan_ctx.where = &where;
        scan_ctx.action = exec_scan_select;
        scan_ctx.project = project_node
            ? &project_node->data.project : NULL;
        scan_ctx.count_only =
            (emit_node->opcode == sqlexec_emit_count);
        scan_ctx.row_count = &row_count;

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
        /* Table scan (also covers join scans). */
        for (index = 0; index < file.record_count; index++) {
            state = dbf_read(&file, index, record);
            if (state < 0) {
                if (right_opened) {
                    dbf_close(&right_file);
                }
                dbf_close(&file);
                return -1;
            }
            if (state == 1) {
                continue;
            }
            if (!right_opened) {
                if (!where_matches(env->program, &where,
                    &left_source, NULL)) {
                    continue;
                }
                if (emit_node->opcode != sqlexec_emit_count) {
                    if (env->write_to_temp) {
                        append_projected_to_temp(env,
                            &project_node->data.project,
                            &left_source);
                    } else {
                        write_projected_row(env,
                            &project_node->data.project,
                            &left_source, NULL);
                    }
                }
                row_count++;
                continue;
            }

            for (right_index = 0;
                right_index < right_file.record_count;
                right_index++) {
                right_state = dbf_read(&right_file, right_index,
                    right_record);
                if (right_state < 0) {
                    dbf_close(&right_file);
                    dbf_close(&file);
                    return -1;
                }
                if (right_state == 1) {
                    continue;
                }
                trim_field_value(left_join_value,
                    sizeof(left_join_value),
                    record + offsets[left_join_index],
                    fields[left_join_index].length);
                trim_field_value(right_join_value,
                    sizeof(right_join_value),
                    right_record + right_offsets[right_join_index],
                    right_fields[right_join_index].length);
                if (!field_values_equal(left_join_value,
                    fields[left_join_index].type,
                    right_join_value,
                    right_fields[right_join_index].type)) {
                    continue;
                }
                if (!where_matches(env->program, &where,
                    &left_source, &right_source)) {
                    continue;
                }
                if (emit_node->opcode != sqlexec_emit_count) {
                    write_projected_row(env,
                        &project_node->data.project,
                        &left_source, &right_source);
                }
                row_count++;
            }
        }
    }
    if (right_opened && dbf_close(&right_file) != 0) {
        dbf_close(&file);
        return -1;
    }
    if (dbf_close(&file) != 0) {
        return -1;
    }

    sel_write_uint(env, row_count);
    if (emit_node->opcode != sqlexec_emit_count) {
        sel_write_str(env, " row");
        if (row_count != 1) {
            sel_write_char(env, 's');
        }
    }
    sel_write_nl(env);
    return 0;
}
