/*
 * Implements FROM-subquery execution and built-in view generators.
 *
 * When a query root carries subquery_text, this module materialises
 * the inner SELECT (or a built-in view) into a temporary DBF at
 * <root>/sys/_tmp.dbf before the outer query runs, then removes the
 * temp file afterward.
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
#include "../catalog/catalog.h"

#include <stdlib.h>
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
static int temp_table_path_named(const char *root, const char *db_name,
    const char *table_name, char *path_out)
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
    return join_path(path_out, db_path, table_name);
}

static int temp_table_path(const char *root, const char *db_name,
    char *path_out)
{
    return temp_table_path_named(root, db_name, "_tmp.dbf", path_out);
}

/* ------------------------------------------------------------------ */
/* Write-to-temp: schema determination                                  */
/* ------------------------------------------------------------------ */

static int close_sources(dbf_file *files, unsigned char count)
{
    unsigned char index;

    for (index = count; index > 0; index--) {
        if (dbf_close(&files[index - 1u]) != 0) {
            return -1;
        }
    }
    return 0;
}

/*
 * Determines the output schema of an inner query by opening every source
 * table and resolving the projected columns against the same row-source
 * rules used by the SELECT executor. Writes the schema into
 * fields_out/count_out.
 */
static void free_subq_source_fields(dbf_field **sf)
{
    unsigned char i;
    for (i = 0; i < sql_max_sources; i++) {
        free(sf[i]);
        sf[i] = NULL;
    }
}

static int determine_inner_schema(const sqlexec_program *program,
    const char *root, const char *db, dbf_field *fields_out,
    unsigned short *count_out)
{
    const sqlexec_node *project_node;
    const sqlexec_node *scan_node;
    const sqlexec_join_def *join_def;
    const row_source *source;
    dbf_file files[sql_max_sources];
    dbf_field *source_fields[sql_max_sources];
    unsigned short source_offsets[sql_max_sources][sql_max_columns];
    row_source sources[sql_max_sources];
    sql_where where;
    sqlexec_ref project_ref;
    sqlexec_ref scan_ref;
    unsigned short output_count;
    unsigned short field_index;
    int source_field_index;
    unsigned char source_count;
    unsigned char source_index;
    unsigned char opened_count;
    const char *table_name;
    int ret;

    if (program->root == sqlexec_nil) {
        return -1;
    }
    table_name = program->table_name;
    if (!table_name || table_name[0] == '\0') {
        return -1;
    }
    project_ref = program->root;
    project_node = sqlexec_get_const(program, project_ref);
    if (!project_node || project_node->opcode != sqlexec_project
        || resolve_scan_node(program, child_at(program, project_ref, 0),
            &where, &scan_ref) != 0) {
        return -1;
    }
    scan_node = sqlexec_get_const(program, scan_ref);
    if (!scan_node) {
        return -1;
    }

    memset(files, 0, sizeof(files));
    memset(sources, 0, sizeof(sources));
    memset(source_fields, 0, sizeof(source_fields));
    source_count = 1;
    opened_count = 0;

    source_fields[0] = (dbf_field *)malloc(sql_max_columns * sizeof(dbf_field));
    if (!source_fields[0])
        return -1;
    if (open_table_file(root, db, table_name, &files[0], source_fields[0],
        source_offsets[0]) != 0) {
        free_subq_source_fields(source_fields);
        return -1;
    }
    opened_count = 1;
    sources[0].table_name = table_name;
    sources[0].alias = "";
    sources[0].fields = source_fields[0];
    sources[0].offsets = source_offsets[0];
    sources[0].record = NULL;
    sources[0].field_count = files[0].field_count;

    if (scan_node->opcode == sqlexec_join_scan) {
        join_def = &scan_node->data.join;
        source_count = join_source_count(*join_def);
        if (source_count < 2 || source_count > sql_max_sources) {
            free_subq_source_fields(source_fields);
            close_sources(files, opened_count);
            return -1;
        }
        sources[0].table_name = join_table_at(program, *join_def, 0);
        sources[0].alias = join_alias_at(program, *join_def, 0);
        for (source_index = 1; source_index < source_count; source_index++) {
            source_fields[source_index] = (dbf_field *)malloc(
                sql_max_columns * sizeof(dbf_field));
            if (!source_fields[source_index]) {
                free_subq_source_fields(source_fields);
                close_sources(files, opened_count);
                return -1;
            }
            if (open_table_file(root, db,
                join_table_at(program, *join_def, source_index),
                &files[source_index], source_fields[source_index],
                source_offsets[source_index]) != 0) {
                free_subq_source_fields(source_fields);
                close_sources(files, opened_count);
                return -1;
            }
            opened_count++;
            sources[source_index].table_name =
                join_table_at(program, *join_def, source_index);
            sources[source_index].alias =
                join_alias_at(program, *join_def, source_index);
            sources[source_index].fields = source_fields[source_index];
            sources[source_index].offsets = source_offsets[source_index];
            sources[source_index].record = NULL;
            sources[source_index].field_count = files[source_index].field_count;
        }
    }

    output_count = exec_project_output_count(&project_node->data.project,
        sources, source_count);
    if (output_count > sql_max_columns) {
        free_subq_source_fields(source_fields);
        close_sources(files, opened_count);
        return -1;
    }

    *count_out = 0;
    ret = 0;
    for (field_index = 0; field_index < output_count; field_index++) {
        if (project_node->data.project.function_arg_is_star[field_index]) {
            memset(&fields_out[*count_out], 0, sizeof(fields_out[*count_out]));
            fields_out[*count_out].type = 'N';
            fields_out[*count_out].length = 5;
            fields_out[*count_out].decimals = 0;
        } else {
            if (exec_resolve_project_field(program, &project_node->data.project,
                sources, source_count, field_index, &source,
                &source_field_index) != 0) {
                ret = -1;
                break;
            }
            fields_out[*count_out] = source->fields[source_field_index];
        }
        if (!project_node->data.project.select_all) {
            exec_copy_dbf_field_name(fields_out[*count_out].name,
                program->names[project_node->data.project.aliases.first
                    + field_index][0] != '\0'
                    ? program->names[project_node->data.project.aliases.first
                        + field_index]
                    : project_node->data.project.function_arg_is_star[
                        field_index] ? "count"
                    : program->names[project_node->data.project.names.first
                        + field_index]);
            if (project_node->data.project.functions[field_index]
                == sql_function_trim) {
                fields_out[*count_out].type = 'C';
                fields_out[*count_out].decimals = 0;
            } else if (project_node->data.project.functions[field_index]
                    == sql_function_count
                || project_node->data.project.functions[field_index]
                    == sql_function_sum
                || project_node->data.project.functions[field_index]
                    == sql_function_avg) {
                if (!project_node->data.project.function_arg_is_star[field_index]
                    && source->fields[source_field_index].type != 'N'
                    && project_node->data.project.functions[field_index]
                        != sql_function_count) {
                    ret = -1;
                    break;
                }
                fields_out[*count_out].length = 9;
                fields_out[*count_out].type = 'N';
                fields_out[*count_out].decimals = 0;
            }
        }
        (*count_out)++;
    }

    free_subq_source_fields(source_fields);
    if (ret != 0) {
        close_sources(files, opened_count);
        return -1;
    }
    return close_sources(files, opened_count);
}

/* ------------------------------------------------------------------ */
/* Execute inner SQL to temp DBF                                        */
/* ------------------------------------------------------------------ */

/*
 * Parses and executes an inner SELECT, redirecting output to a temp DBF
 * instead of the terminal. The temp DBF is created at temp_path with
 * a schema matching the inner SELECT's project columns.
 */
static int program_uses_materialized_from(const sqlexec_program *program)
{
    return program && program->root != sqlexec_nil
        && program_subquery_text(program)[0] != '\0';
}

static int prepare_inner_sql_context(const sqlexec_env *parent_env,
    const char *sql_text, int allow_nested_subquery, sql_context *inner)
{
    inner->root = parent_env->root;
    inner->current_db[0] = '\0';
    if (parent_env->current_db) {
        copy_name(inner->current_db, parent_env->current_db);
    }
    inner->text = sql_text;
    inner->io.write_char = NULL;
    inner->result = 0;
    inner->schema = NULL;

    if (sql_parse_select_program(sql_text, &inner->program,
        parent_env->root, inner->current_db) != 0) {
        return -1;
    }
    if (!allow_nested_subquery
        && program_uses_materialized_from(&inner->program)) {
        return -1;
    }
    return sqlopt_run(inner);
}

static int exec_sql_to_temp(const sqlexec_env *parent_env,
    const char *sql_text, const char *temp_path, int allow_nested_subquery)
{
    sql_context *inner;
    dbf_field *fields;
    unsigned short field_count;
    sqlexec_env env2;
    exec_temp_ctx tctx;
    dbf_file temp_dbf;
    unsigned short temp_offsets[sql_max_columns];
    int ret;

    inner = (sql_context *)malloc(sizeof(sql_context));
    if (!inner)
        return -1;
    memset(inner, 0, sizeof(*inner));

    fields = (dbf_field *)malloc(sql_max_columns * sizeof(dbf_field));
    if (!fields) {
        free(inner);
        return -1;
    }

    if (prepare_inner_sql_context(parent_env, sql_text,
        allow_nested_subquery, inner) != 0) {
        free(fields);
        free(inner);
        return -1;
    }

    if (determine_inner_schema(&inner->program, parent_env->root,
        parent_env->current_db, fields, &field_count) != 0) {
        free(fields);
        free(inner);
        return -1;
    }

    if (dbf_create(&temp_dbf, temp_path, fields, field_count) != 0) {
        free(fields);
        free(inner);
        return -1;
    }
    build_field_offsets(fields, field_count, temp_offsets);

    tctx.out         = &temp_dbf;
    tctx.fields      = fields;
    tctx.offsets     = temp_offsets;
    tctx.field_count = (unsigned short)field_count;

    memset(&env2, 0, sizeof(env2));
    env2.root        = parent_env->root;
    env2.program     = &inner->program;
    env2.current_db  = inner->current_db;
    env2.io          = &inner->io;
    env2.temp        = &tctx;
    env2.collect     = NULL;
    env2.temp_serial = parent_env->temp_serial;

    ret = sqlexec_execute_env(&env2);
    dbf_close(&temp_dbf);
    free(fields);
    free(inner);
    return ret;
}

static int note_subquery_uses(const sql_where *where,
    const sql_where_node *nodes,
    unsigned char uses[sql_max_predicate_subqueries])
{
    unsigned char stack[sql_where_max_nodes];
    unsigned char depth;
    unsigned char ref;
    const sql_where_node *node;

    if (!where->active) {
        return 0;
    }

    depth = 0;
    stack[depth++] = where->root;
    while (depth > 0) {
        ref = stack[--depth];
        node = &nodes[ref];
        switch (node->type) {
        case sql_where_quantified:
            if (node->subquery_index >= sql_max_predicate_subqueries) {
                return -1;
            }
            uses[node->subquery_index] |= 1u;
            break;
        case sql_where_exists:
            if (node->subquery_index >= sql_max_predicate_subqueries) {
                return -1;
            }
            uses[node->subquery_index] |= 2u;
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

static int collect_predicate_subquery_uses(const sqlexec_program *program,
    unsigned char uses[sql_max_predicate_subqueries])
{
    memset(uses, 0, sql_max_predicate_subqueries);
    if (note_subquery_uses(&program->where, program->where_nodes, uses) != 0
        || note_subquery_uses(&program->having, program_having_nodes(program),
            uses) != 0) {
        return -1;
    }
    return 0;
}

int load_predicate_subqueries(const sqlexec_env *env,
    sql_predicate_subquery_cache *cache)
{
    unsigned char uses[sql_max_predicate_subqueries];
    sql_context *inner;
    sqlexec_env inner_env;
    exec_collect_ctx collect;
    unsigned short index;

    inner = (sql_context *)malloc(sizeof(sql_context));
    if (!inner)
        return -1;

    memset(cache, 0, sizeof(*cache));
    cache->count = env->program->predicate_subquery_count;
    if (collect_predicate_subquery_uses(env->program, uses) != 0) {
        free(inner);
        return -1;
    }
    for (index = 0; index < env->program->predicate_subquery_count; index++) {
        memset(inner, 0, sizeof(*inner));
        if (uses[index] == 0
            || prepare_inner_sql_context(env,
                env->program->predicate_subqueries[index], 0, inner) != 0) {
            free(inner);
            return -1;
        }

        memset(&collect, 0, sizeof(collect));
        collect.mode = (uses[index] & 1u) != 0
            ? exec_collect_values : exec_collect_exists;
        collect.result = &cache->results[index];

        memset(&inner_env, 0, sizeof(inner_env));
        inner_env.root = env->root;
        inner_env.program = &inner->program;
        inner_env.current_db = inner->current_db;
        inner_env.io = &inner->io;
        inner_env.temp = NULL;
        inner_env.collect = &collect;
        inner_env.temp_serial = env->temp_serial;

        if (sqlexec_execute_env(&inner_env) != 0) {
            free(inner);
            return -1;
        }
    }
    free(inner);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public entry points                                                  */
/* ------------------------------------------------------------------ */

int exec_run_subquery(sqlexec_env *env)
{
    char temp_path[path_buffer_size];
    const char *subquery = program_subquery_text(env->program);

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
    return exec_sql_to_temp(env, subquery, temp_path, 1);
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
