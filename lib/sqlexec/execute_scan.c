/*
 * Implements the ndx_scan callbacks shared between the SELECT and
 * MUTATE executors. Both callbacks read a DBF record identified by
 * the index, apply the WHERE guard, and dispatch to the appropriate
 * action (project+emit, apply+write, or delete).
 *
 * MIT License (see: LICENSE)
 * Copyright (C) 2026 tomaz stih
 */

#include "exec_impl.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Output helpers                                                       */
/* ------------------------------------------------------------------ */

static void scan_write_char(const sqlexec_env *env, char c)
{
    if (env->io && env->io->write_char) {
        env->io->write_char(c);
    }
}

static void scan_write_nl(const sqlexec_env *env)
{
    scan_write_char(env, '\r');
    scan_write_char(env, '\n');
}

static void scan_write_str(const sqlexec_env *env, const char *s)
{
    while (*s) {
        scan_write_char(env, *s++);
    }
}

/* ------------------------------------------------------------------ */
/* Row projection (SELECT only)                                         */
/* ------------------------------------------------------------------ */

static void scan_project_row(const sqlexec_env *env,
    const sqlexec_project_def *project, const row_source *source)
{
    const row_source *src;
    char value[sql_value_size];
    unsigned short index;
    unsigned short output_count;
    int field_index;

    output_count = project->select_all
        ? source->field_count : project->names.count;

    for (index = 0; index < output_count; index++) {
        if (project->select_all) {
            src = source;
            field_index = (int)index;
        } else {
            if (resolve_field_ref(source, NULL,
                env->program->names[
                    project->qualifiers.first + index],
                env->program->names[
                    project->names.first + index],
                &src, &field_index) != 0) {
                scan_write_str(env, "error");
                scan_write_nl(env);
                return;
            }
        }
        trim_field_value(value, sizeof(value),
            src->record + src->offsets[field_index],
            src->fields[field_index].length);
        if (index > 0) {
            scan_write_str(env, " | ");
        }
        scan_write_str(env, value);
    }
    scan_write_nl(env);
}

/* ------------------------------------------------------------------ */
/* Shared record action dispatch                                        */
/* ------------------------------------------------------------------ */

static int apply_action(exec_index_scan_ctx *ctx, unsigned long record_number)
{
    int field_index;
    unsigned short i;

    if (!where_matches(ctx->env->program, ctx->where, ctx->source, NULL)) {
        return 0;
    }

    switch (ctx->action) {
    case exec_scan_select:
        if (!ctx->count_only) {
            scan_project_row(ctx->env, ctx->project, ctx->source);
        }
        (*ctx->row_count)++;
        return 0;

    case exec_scan_update:
        for (i = 0; i < ctx->apply_node->data.assignments.count; i++) {
            field_index = find_field_index(
                ctx->fields, ctx->file->field_count,
                ctx->env->program->assignments[
                    ctx->apply_node->data.assignments.first + i].column_name);
            if (store_value_in_field(
                ctx->record + ctx->offsets[field_index],
                &ctx->fields[field_index],
                &ctx->env->program->assignments[
                    ctx->apply_node->data.assignments.first + i].value) != 0) {
                ctx->failed = 1;
                return 1;
            }
        }
        if (dbf_write(ctx->file, record_number, ctx->record) != 0) {
            ctx->failed = 1;
            return 1;
        }
        (*ctx->changed_count)++;
        return 0;

    case exec_scan_delete:
        if (dbf_delete(ctx->file, record_number) != 0) {
            ctx->failed = 1;
            return 1;
        }
        (*ctx->deleted_count)++;
        return 0;

    default:
        return 0;
    }
}

static int read_and_act(exec_index_scan_ctx *ctx,
    unsigned long record_number)
{
    int state;
    unsigned long dbf_index;

    if (record_number == 0UL) {
        return 0;
    }
    dbf_index = ndx_to_dbf_index(record_number);
    state = dbf_read(ctx->file, dbf_index, ctx->record);
    if (state < 0) {
        ctx->failed = 1;
        return 1;
    }
    if (state == 1) {
        return 0;
    }
    return apply_action(ctx, dbf_index);
}

/* ------------------------------------------------------------------ */
/* ndx_scan callbacks                                                   */
/* ------------------------------------------------------------------ */

int exec_eq_scan_callback(unsigned long record_number,
    const unsigned char *key, unsigned short key_length, void *vctx)
{
    exec_index_scan_ctx *ctx;
    int cmp;

    (void)key_length;
    ctx = (exec_index_scan_ctx *)vctx;
    cmp = ndx_compare_key(ctx->ndx, key, ctx->eq_key);
    if (cmp > 0) {
        return 1;
    }
    if (cmp < 0) {
        return 0;
    }
    return read_and_act(ctx, record_number);
}

int exec_range_scan_callback(unsigned long record_number,
    const unsigned char *key, unsigned short key_length, void *vctx)
{
    exec_index_scan_ctx *ctx;
    int cmp;

    (void)key_length;
    ctx = (exec_index_scan_ctx *)vctx;

    if (ctx->has_lower) {
        cmp = ndx_compare_key(ctx->ndx, key, ctx->lower_key);
        if (ctx->lower_incl ? cmp < 0 : cmp <= 0) {
            return 0;
        }
    }

    if (ctx->has_upper) {
        cmp = ndx_compare_key(ctx->ndx, key, ctx->upper_key);
        if (ctx->upper_incl ? cmp > 0 : cmp >= 0) {
            return 1;
        }
    }

    return read_and_act(ctx, record_number);
}
