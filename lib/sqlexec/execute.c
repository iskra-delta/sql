/*
 * Thin dispatcher for sqlexec_execute. Reads the root opcode from the
 * validated execution tree and routes to the appropriate executor
 * module (DDL, SELECT, or MUTATE). All heavy logic lives in the three
 * split executor files so each can be loaded as an independent module
 * on memory-constrained targets.
 *
 * MIT License (see: LICENSE)
 * Copyright (C) 2026 tomaz stih
 */

#include "exec_impl.h"
#include "sqlctx.h"

#include <string.h>

static int is_ddl_root(sqlexec_opcode opcode)
{
    return opcode == sqlexec_create_database
        || opcode == sqlexec_use_database
        || opcode == sqlexec_drop_database
        || opcode == sqlexec_create_table
        || opcode == sqlexec_drop_table
        || opcode == sqlexec_create_index
        || opcode == sqlexec_create_view
        || opcode == sqlexec_drop_view;
}

static int is_select_head(sqlexec_opcode opcode)
{
    return opcode == sqlexec_project
        || opcode == sqlexec_table_scan
        || opcode == sqlexec_join_scan;
}

/*
 * Core dispatch: routes an already-constructed env to the right
 * executor. Called by both sqlexec_execute and sqlexec_execute_env so
 * that the redirected-output fields are respected when executing inner
 * queries for temp materialisation or predicate-subquery collection.
 */
static int sqlexec_dispatch(sqlexec_env *env)
{
    const sqlexec_program *program;
    const sqlexec_node *root_node;
    const char *table_name;

    program = env->program;

    root_node = sqlexec_get_const(program, program->root);
    if (!root_node) {
        return -1;
    }

    if (is_ddl_root(root_node->opcode)) {
        return exec_ddl(env);
    }

    table_name = program->table_name;
    if (!table_name || table_name[0] == '\0') {
        return -1;
    }
    if (is_select_head(root_node->opcode)) {
        int sel_ret;

        if (program_subquery_text(program)[0] != '\0') {
            if (exec_run_subquery(env) != 0) {
                return -1;
            }
        }
        sel_ret = exec_select(env, table_name, program->root);
        if (program_subquery_text(program)[0] != '\0') {
            exec_delete_temp(env);
        }
        return sel_ret;
    }

    if (root_node->opcode == sqlexec_append_record) {
        return exec_insert(env, table_name,
            root_node->data.assignments);
    }

    if (root_node->opcode == sqlexec_write_current) {
        return exec_update(env, table_name, program->root);
    }

    if (root_node->opcode == sqlexec_delete_current) {
        return exec_delete(env, table_name, program->root);
    }

    return -1;
}

int sqlexec_execute(const char *root, const sqlexec_program *program,
    char *current_db, const sqlexec_io *io)
{
    sqlexec_env env;
    unsigned short temp_serial;
    int ret;

    if (!root || !program || !current_db
        || program->root == sqlexec_nil) {
        return -1;
    }

    memset(&env, 0, sizeof(env));
    temp_serial = 0;
    env.root       = root;
    env.program    = program;
    env.current_db = current_db;
    env.io         = io;
    env.temp       = NULL;
    env.schema     = NULL;
    env.temp_serial = &temp_serial;

    ret = sqlexec_dispatch(&env);

    /* sqlexec_execute has no sql_context to propagate the cache back
     * through. Free any cache built during this call to avoid leaking. */
    meta_cache_free(env.schema);

    return ret;
}

int sqlexec_run(sql_context *ctx)
{
    sqlexec_env env;
    unsigned short temp_serial;
    int ret;

    if (!ctx || !ctx->root || ctx->program.root == sqlexec_nil) {
        return -1;
    }

    memset(&env, 0, sizeof(env));
    temp_serial = 0;
    env.root       = ctx->root;
    env.program    = &ctx->program;
    env.current_db = ctx->current_db;
    env.io         = &ctx->io;
    env.temp       = NULL;
    env.schema     = ctx->schema;
    env.temp_serial = &temp_serial;

    ret = sqlexec_dispatch(&env);

    /* Propagate schema changes (USE, CREATE TABLE, DROP TABLE) back. */
    ctx->schema = env.schema;

    return ret;
}

int sqlexec_execute_env(sqlexec_env *env)
{
    return sqlexec_dispatch(env);
}
