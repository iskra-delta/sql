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

static int is_ddl_root(sqlexec_opcode opcode)
{
    return opcode == sqlexec_create_database
        || opcode == sqlexec_show_databases
        || opcode == sqlexec_use_database
        || opcode == sqlexec_create_table
        || opcode == sqlexec_create_view
        || opcode == sqlexec_drop_view
        || opcode == sqlexec_show_views;
}

static int is_ddl_sequence_first(sqlexec_opcode opcode)
{
    return opcode == sqlexec_build_index
        || opcode == sqlexec_unregister_table_indexes
        || opcode == sqlexec_unregister_database_indexes;
}

/*
 * Core dispatch: routes an already-constructed env to the right
 * executor. Called by both sqlexec_execute and sqlexec_execute_env so
 * that the write_to_temp field is respected when executing inner
 * queries for subquery materialisation.
 */
static int sqlexec_dispatch(sqlexec_env *env)
{
    const sqlexec_program *program;
    const sqlexec_node *root_node;
    const sqlexec_node *first_node;
    const sqlexec_node *second_node;
    const sqlexec_node *node;
    sqlexec_ref first_ref;
    sqlexec_ref second_ref;
    sqlexec_ref ref;

    program = env->program;

    root_node = sqlexec_get_const(program, program->root);
    if (!root_node) {
        return -1;
    }

    if (is_ddl_root(root_node->opcode)) {
        return exec_ddl(env);
    }

    if (root_node->opcode != sqlexec_sequence) {
        return -1;
    }

    first_ref = child_at(program, program->root, 0);
    second_ref = child_at(program, program->root, 1);
    if (first_ref == sqlexec_nil) {
        return -1;
    }
    first_node = sqlexec_get_const(program, first_ref);
    second_node = second_ref != sqlexec_nil
        ? sqlexec_get_const(program, second_ref) : NULL;
    if (!first_node) {
        return -1;
    }

    if (is_ddl_sequence_first(first_node->opcode)) {
        return exec_ddl(env);
    }

    if (first_node->opcode == sqlexec_run_subquery) {
        if (exec_run_subquery(env) != 0) {
            return -1;
        }
        first_ref = second_ref;
        second_ref = child_at(program, program->root, 2);
        first_node = second_node;
        second_node = second_ref != sqlexec_nil
            ? sqlexec_get_const(program, second_ref) : NULL;
        if (!first_node) {
            return -1;
        }
    }

    if (first_node->opcode != sqlexec_open_table || !second_node) {
        return -1;
    }

    if (second_node->opcode == sqlexec_emit_rows
        || second_node->opcode == sqlexec_emit_count) {
        int sel_ret;
        sqlexec_ref sib;
        sqlexec_ref last;

        sel_ret = exec_select(env, first_node->data.named.name,
            second_ref);
        /* Clean up temp table if the last sequence child is delete_temp. */
        sib = program->nodes[program->root].first_child;
        last = sqlexec_nil;
        while (sib != sqlexec_nil) {
            last = sib;
            sib = program->nodes[sib].next_sibling;
        }
        if (last != sqlexec_nil
            && program->nodes[last].opcode == sqlexec_delete_temp) {
            exec_delete_temp(env);
        }
        return sel_ret;
    }

    if (second_node->opcode != sqlexec_count_affected) {
        return -1;
    }

    ref = child_at(program, second_ref, 0);
    if (ref == sqlexec_nil) {
        return -1;
    }
    node = sqlexec_get_const(program, ref);
    if (!node) {
        return -1;
    }

    if (node->opcode == sqlexec_append_record) {
        ref = child_at(program, ref, 0);
        node = sqlexec_get_const(program, ref);
        if (!node || node->opcode != sqlexec_make_record) {
            return -1;
        }
        return exec_insert(env, first_node->data.named.name,
            node->data.values);
    }

    if (node->opcode == sqlexec_write_current) {
        return exec_update(env, first_node->data.named.name,
            second_ref);
    }

    if (node->opcode == sqlexec_delete_current) {
        return exec_delete(env, first_node->data.named.name,
            second_ref);
    }

    return -1;
}

int sqlexec_execute(const char *root, const sqlexec_program *program,
    char *current_db, const sqlexec_io *io)
{
    sqlexec_env env;

    if (!root || !program || !current_db
        || program->root == sqlexec_nil) {
        return -1;
    }

    memset(&env, 0, sizeof(env));
    env.root = root;
    env.program = program;
    env.current_db = current_db;
    env.io = io;

    return sqlexec_dispatch(&env);
}

int sqlexec_run(sql_context *ctx)
{
    return sqlexec_execute(ctx->root, &ctx->program,
        ctx->current_db, &ctx->io);
}

int sqlexec_execute_env(sqlexec_env *env)
{
    /* Use the provided env directly so write_to_temp is preserved. */
    return sqlexec_dispatch(env);
}
