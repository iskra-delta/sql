/*
 * Implements subquery execution and built-in view generators.
 *
 * run_subquery materialises an inner SELECT (or a built-in view) into
 * a temporary DBF at <root>/sys/_tmp.dbf, then returns so the outer
 * query can open that file as an ordinary table.
 *
 * delete_temp removes the temporary file after the outer query closes
 * it.
 *
 * Built-in views (sys_ prefix) generate their rows procedurally from
 * catalog files and DBF headers rather than from stored SQL text.
 *
 * MIT License (see: LICENSE)
 * Copyright (C) 2026 tomaz stih
 */

#include "exec_impl.h"
#include "sqlctx.h"
#include "sql.h"
#include "sqlopt.h"
#include "../shared/catalog.h"

#include <string.h>

#if defined(__SDCC)
extern int unlink(const char *path);
#else
#include <unistd.h>
#include <dirent.h>
#endif

/* ------------------------------------------------------------------ */
/* Temp table path                                                      */
/* ------------------------------------------------------------------ */

/*
 * The temp table lives in the current database directory so that
 * open_table_file(root, current_db, "_tmp", ...) finds it naturally.
 * Falls back to <root>/sys/ when no database is selected.
 */
static int temp_table_path(const char *root, const char *db_name,
    char *path_out)
{
    char db_path[path_buffer_size];

    if (db_name && db_name[0]) {
        if (find_database_path(root, db_name, db_path) != 0) {
            return -1;
        }
    } else {
        if (join_path(db_path, root, "sys") != 0) {
            return -1;
        }
    }
    return join_path(path_out, db_path, "_tmp.dbf");
}

/* ------------------------------------------------------------------ */
/* Write-to-temp: schema determination                                  */
/* ------------------------------------------------------------------ */

/*
 * Determines the output schema of an inner query by opening the source
 * table and resolving the project columns. Writes the schema into
 * fields_out/count_out.
 */
static int determine_inner_schema(const char *root, const char *db,
    const char *table_name, unsigned char select_all,
    unsigned char select_count, const char (*names)[sql_name_size],
    dbf_field *fields_out, unsigned short *count_out)
{
    dbf_file file;
    dbf_field src_fields[sql_max_columns];
    unsigned short offsets[sql_max_columns];
    unsigned short i;
    int fi;

    if (open_table_file(root, db, table_name, &file, src_fields,
        offsets) != 0) {
        return -1;
    }

    if (select_all || select_count == 0) {
        for (i = 0; i < file.field_count; i++) {
            fields_out[i] = src_fields[i];
        }
        *count_out = file.field_count;
    } else {
        *count_out = 0;
        for (i = 0; i < select_count; i++) {
            fi = find_field_index(src_fields, file.field_count, names[i]);
            if (fi < 0) {
                dbf_close(&file);
                return -1;
            }
            fields_out[*count_out] = src_fields[fi];
            (*count_out)++;
        }
    }

    return dbf_close(&file);
}

/* ------------------------------------------------------------------ */
/* Append projected row to temp DBF                                     */
/* ------------------------------------------------------------------ */

/*
 * Builds one DBF record from the projected columns and appends it
 * to the temp table described by ctx.
 */
void append_projected_to_temp(exec_temp_ctx *ctx,
    const sqlexec_env *env,
    const sqlexec_project_def *project,
    const row_source *source)
{
    char record[table_record_size];
    const row_source *src;
    char value[sql_value_size];
    unsigned short i;
    unsigned short out_count;
    int fi;
    unsigned short offset;
    unsigned short record_len;

    out_count = project->select_all
        ? source->field_count : project->names.count;

    record_len = ctx->field_count > 0
        ? (unsigned short)(ctx->offsets[ctx->field_count - 1]
            + ctx->fields[ctx->field_count - 1].length)
        : 0;
    clear_record(record, record_len);

    offset = 0;
    for (i = 0; i < out_count; i++) {
        if (project->select_all) {
            src = source;
            fi = (int)i;
        } else {
            if (resolve_field_ref(source, NULL,
                env->program->names[project->qualifiers.first + i],
                env->program->names[project->names.first + i],
                &src, &fi) != 0) {
                return;
            }
        }
        trim_field_value(value, sizeof(value),
            src->record + src->offsets[fi],
            src->fields[fi].length);
        set_field(record + offset, ctx->fields[i].length, value);
        offset = (unsigned short)(offset + ctx->fields[i].length);
    }

    dbf_append(ctx->out, record);
}

/* ------------------------------------------------------------------ */
/* Execute inner SQL to temp DBF                                        */
/* ------------------------------------------------------------------ */

/*
 * Parses and executes an inner SELECT, redirecting output to a temp DBF
 * instead of the terminal. The temp DBF is created at temp_path with
 * a schema matching the inner SELECT's project columns.
 */
static int exec_sql_to_temp(const char *root, const char *db,
    const char *sql_text, const char *temp_path)
{
    sql_context inner;
    char sql_with_semi[sql_subquery_size + 2];
    size_t slen;
    unsigned short len;
    dbf_field fields[sql_max_columns];
    unsigned short field_count;

    /* Prepare the inner SQL text with a trailing semicolon. */
    slen = strlen(sql_text);
    len = (unsigned short)slen;
    if (len == 0 || slen + 2 > sizeof(sql_with_semi)) {
        return -1;
    }
    memcpy(sql_with_semi, sql_text, len);
    sql_with_semi[len] = ';';
    sql_with_semi[len + 1] = '\0';

    /* Parse the inner query without view expansion (one level only). */
    inner.root = root;
    inner.current_db[0] = '\0';
    if (db) {
        copy_name(inner.current_db, db);
    }
    inner.text = sql_with_semi;
    inner.io.write_char = NULL;
    inner.result = 0;

    if (sql_parse(inner.text, &inner.program, NULL, NULL) != 0) {
        return -1;
    }
    if (sqlopt_run(&inner) != 0) {
        return -1;
    }

    /* Determine the temp table schema via the stable API. */
    {
        const char *src_table;
        unsigned char sel_all;
        sqlexec_span names;
        const char (*col_names)[sql_name_size] = NULL;

        if (sqlexec_get_output_info(&inner.program, &src_table,
            &sel_all, &names) != 0) {
            return -1;
        }
        if (!sel_all && names.count > 0) {
            col_names = (const char (*)[sql_name_size])
                &inner.program.names[names.first];
        }
        if (determine_inner_schema(root, db, src_table, sel_all,
            names.count, col_names, fields, &field_count) != 0) {
            return -1;
        }
    }

    /* Create the temp DBF. */
    {
        sqlexec_env env2;
        exec_temp_ctx tctx;
        dbf_file temp_dbf;
        unsigned short temp_offsets[sql_max_columns];
        int ret;

        if (dbf_create(&temp_dbf, temp_path, fields, field_count) != 0) {
            return -1;
        }
        build_field_offsets(fields, field_count, temp_offsets);

        tctx.out         = &temp_dbf;
        tctx.fields      = fields;
        tctx.offsets     = temp_offsets;
        tctx.field_count = (unsigned short)field_count;

        env2.root       = root;
        env2.program    = &inner.program;
        env2.current_db = inner.current_db;
        env2.io         = &inner.io;
        env2.temp       = &tctx;

        ret = sqlexec_execute_env(&env2);
        dbf_close(&temp_dbf);
        return ret;
    }
}

/* ------------------------------------------------------------------ */
/* Public entry points                                                  */
/* ------------------------------------------------------------------ */

int exec_run_subquery(sqlexec_env *env)
{
    char temp_path[path_buffer_size];
    const char *subquery = env->program->subquery_text;

    if (temp_table_path(env->root, env->current_db, temp_path) != 0) {
        return -1;
    }
    /* Remove any leftover temp file from a previous failed query. */
    unlink(temp_path);

    /* Built-in views start with "sys_" prefix. */
    if (subquery[0] == 's' && subquery[1] == 'y'
        && subquery[2] == 's' && subquery[3] == '_') {
        return dispatch_builtin_view(env, subquery, temp_path);
    }

    /* User SQL text — execute inner query to temp DBF. */
    return exec_sql_to_temp(env->root, env->current_db,
        subquery, temp_path);
}

int exec_delete_temp(sqlexec_env *env)
{
    char temp_path[path_buffer_size];

    if (temp_table_path(env->root, env->current_db, temp_path) != 0) {
        return 0;
    }
    unlink(temp_path);
    return 0;
}
