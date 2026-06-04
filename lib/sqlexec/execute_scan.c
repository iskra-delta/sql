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

static int scan_emit_project_row(const sqlexec_env *env,
    const exec_project_binding *binding, const row_source *source)
{
    char output_values[sql_max_columns][sql_value_size];
    unsigned short index;
    unsigned short output_count;

    if (exec_build_project_output_values_bound(binding, source,
        output_values, &output_count) != 0) {
        scan_write_str(env, "error");
        scan_write_nl(env);
        return -1;
    }
    if (env->temp) {
        return exec_append_output_values_to_temp(env->temp, output_values,
            output_count);
    }
    if (env->collect) {
        return exec_collect_output_values(env->collect, output_values,
            binding->output_types, output_count);
    }
    for (index = 0; index < output_count; index++) {
        if (index > 0) {
            scan_write_str(env, " | ");
        }
        scan_write_str(env, output_values[index]);
    }
    scan_write_nl(env);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Shared record action dispatch                                        */
/* ------------------------------------------------------------------ */

static int apply_action(exec_index_scan_ctx *ctx, unsigned long record_number)
{
    int field_index;
    unsigned short i;

    if (!where_matches_bound_n(ctx->env->program, ctx->where,
        ctx->where_binding, ctx->source, ctx->subqueries)) {
        return 0;
    }

    switch (ctx->action) {
    case exec_scan_select:
        if (!ctx->count_only) {
            if (scan_emit_project_row(ctx->env, ctx->binding,
                ctx->source) != 0) {
                ctx->failed = 1;
                return 1;
            }
        }
        (*ctx->row_count)++;
        return 0;

    case exec_scan_update:
        for (i = 0; i < ctx->write_node->data.assignments.count; i++) {
            field_index = (int)ctx->assignment_fields[i];
            if (store_value_in_field(
                ctx->record + ctx->offsets[field_index],
                &ctx->fields[field_index],
                &program_assignments(ctx->env->program)[
                    ctx->write_node->data.assignments.first + i].value) != 0) {
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
    const ndx_scan_entry *entry)
{
    exec_index_scan_ctx *ctx;
    int cmp;

    ctx = (exec_index_scan_ctx *)entry->ctx;
    cmp = ndx_compare_key(ctx->ndx, entry->key, ctx->eq_key);
    if (cmp > 0) {
        return 1;
    }
    if (cmp < 0) {
        return 0;
    }
    return read_and_act(ctx, record_number);
}

int exec_range_scan_callback(unsigned long record_number,
    const ndx_scan_entry *entry)
{
    exec_index_scan_ctx *ctx;
    int cmp;

    ctx = (exec_index_scan_ctx *)entry->ctx;

    if (ctx->has_lower) {
        cmp = ndx_compare_key(ctx->ndx, entry->key, ctx->lower_key);
        if (ctx->lower_incl ? cmp < 0 : cmp <= 0) {
            return 0;
        }
    }

    if (ctx->has_upper) {
        cmp = ndx_compare_key(ctx->ndx, entry->key, ctx->upper_key);
        if (ctx->upper_incl ? cmp > 0 : cmp >= 0) {
            return 1;
        }
    }

    return read_and_act(ctx, record_number);
}

int exec_run_index_scan(const sqlexec_env *env, const sqlexec_node *scan_node,
    exec_index_scan_ctx *ctx)
{
    ndx_file ndx;
    int scan_result;

    if (!env || !scan_node || !ctx) {
        return -1;
    }
    if (scan_node->opcode != sqlexec_table_scan
        || scan_node->data.scan.access_kind == sqlexec_scan_full) {
        return -1;
    }

    if (open_ndx(env, scan_node->data.scan.index_name, &ndx) != 0) {
        return -1;
    }

    ctx->ndx = &ndx;
    if (scan_node->data.scan.access_kind == sqlexec_scan_index_eq) {
        if (build_eq_key(&ndx, &scan_node->data.scan.lower_value,
            ctx->eq_key) != 0) {
            ndx_close(&ndx);
            return -1;
        }
        scan_result = ndx_scan(&ndx, exec_eq_scan_callback, ctx);
    } else {
        if (scan_node->data.scan.lower_operator
            != sql_compare_invalid) {
            ctx->has_lower = 1;
            ctx->lower_incl =
                scan_node->data.scan.lower_operator
                == sql_compare_greater_equal;
            build_bound_key(&ndx,
                &scan_node->data.scan.lower_value,
                ctx->lower_key);
        }
        if (scan_node->data.scan.upper_operator
            != sql_compare_invalid) {
            ctx->has_upper = 1;
            ctx->upper_incl =
                scan_node->data.scan.upper_operator
                == sql_compare_less_equal;
            build_bound_key(&ndx,
                &scan_node->data.scan.upper_value,
                ctx->upper_key);
        }
        scan_result = ndx_scan(&ndx, exec_range_scan_callback, ctx);
    }

    ndx_close(&ndx);
    return ctx->failed || scan_result < 0 ? -1 : 0;
}
