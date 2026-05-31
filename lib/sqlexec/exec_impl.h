/*
 * Internal header shared across the executor split files.
 * Declares the execution environment and the entry points for each
 * executor module. Not part of the public API.
 *
 * MIT License (see: LICENSE)
 * Copyright (C) 2026 tomaz stih
 */

#ifndef exec_impl_h
#define exec_impl_h

#include "sqlexec.h"
#include "ndx.h"
#include "../shared/where.h"
#include "../shared/catalog.h"
#include "../shared/metacache.h"

#include <string.h>

/*
 * Execution environment threaded through every executor function.
 * Holds the context that would otherwise require global state.
 */
/*
 * Context for materialising an inner query into a temp DBF.
 * Passed to exec_select only when building a subquery result;
 * NULL in all normal execution paths.
 */
typedef struct exec_temp_ctx {
    dbf_file       *out;
    const dbf_field *fields;
    const unsigned short *offsets;
    unsigned short  field_count;
} exec_temp_ctx;

typedef struct sqlexec_env {
    const char *root;
    const sqlexec_program *program;
    char *current_db;
    const sqlexec_io *io;
    /* Non-NULL only when materialising a subquery to a temp table. */
    exec_temp_ctx *temp;
    /* Schema cache: non-NULL after USE, freed on schema change. */
    meta_cache *schema;
} sqlexec_env;

/* ------------------------------------------------------------------ */
/* Index scan context shared by SELECT and MUTATE scanners             */
/* ------------------------------------------------------------------ */

/*
 * Context threaded through ndx_scan callbacks.
 * Holds everything needed to read a DBF record, evaluate WHERE, and
 * decide the scan action. The action field selects the operation that
 * runs for each passing record.
 */
typedef enum exec_scan_action {
    exec_scan_select = 0,
    exec_scan_update,
    exec_scan_delete
} exec_scan_action;

typedef struct exec_index_scan_ctx {
    /* NDX file and encoded target keys */
    ndx_file       *ndx;
    unsigned char   eq_key[ndx_max_key_length];     /* for index_scan_eq   */
    unsigned char   lower_key[ndx_max_key_length];  /* for index_scan_range */
    unsigned char   upper_key[ndx_max_key_length];
    unsigned char   has_lower;
    unsigned char   has_upper;
    unsigned char   lower_incl; /* 1 = >=, 0 = > */
    unsigned char   upper_incl; /* 1 = <=, 0 = < */
    /* DBF state */
    dbf_file       *file;
    const dbf_field        *fields;
    const unsigned short   *offsets;
    char           *record;
    row_source     *source;
    /* WHERE evaluation */
    const sqlexec_env      *env;
    const sql_where        *where;
    /* Action-specific payload */
    exec_scan_action action;
    /* SELECT */
    const sqlexec_project_def *project;
    int             count_only;
    unsigned short *row_count;
    /* UPDATE */
    const sqlexec_node *apply_node;
    unsigned short *changed_count;
    /* DELETE */
    unsigned short *deleted_count;
    /* Error flag set by callback on I/O failure */
    int             failed;
} exec_index_scan_ctx;

/*
 * ndx_scan callback for index_scan_eq.
 * Stops when the current key is past the target; processes records
 * whose key exactly equals eq_key.
 */
int exec_eq_scan_callback(unsigned long record_number,
    const unsigned char *key, unsigned short key_length, void *vctx);

/*
 * ndx_scan callback for index_scan_range.
 * Skips records below the lower bound; stops when past the upper bound.
 */
int exec_range_scan_callback(unsigned long record_number,
    const unsigned char *key, unsigned short key_length, void *vctx);

/* ------------------------------------------------------------------ */
/* Subquery execution entry points                                      */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Shared inline helpers used by multiple executor files               */
/* ------------------------------------------------------------------ */

static inline sqlexec_ref child_at(const sqlexec_program *program,
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

static inline int resolve_scan_node(const sqlexec_program *program,
    sqlexec_ref input_ref, sql_where *where, sqlexec_ref *scan_ref_out)
{
    const sqlexec_node *node;
    if (!where || !scan_ref_out || input_ref == sqlexec_nil) return -1;
    memset(where, 0, sizeof(*where));
    node = sqlexec_get_const(program, input_ref);
    if (!node) return -1;
    if (node->opcode == sqlexec_filter) {
        *where = node->data.where;
        input_ref = child_at(program, input_ref, 0);
        if (input_ref == sqlexec_nil) return -1;
        node = sqlexec_get_const(program, input_ref);
        if (!node) return -1;
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

static inline int open_ndx(const sqlexec_env *env,
    const char *index_name, ndx_file *ndx)
{
    char index_path[path_buffer_size];
    if (build_index_path(env->root, env->current_db,
        index_name, index_path) != 0) {
        return -1;
    }
    return ndx_open(ndx, index_path);
}

static inline int build_eq_key(ndx_file *ndx, const sql_value *value,
    unsigned char *key_out)
{
    if (ndx->key_type == ndx_key_type_numeric)
        return ndx_encode_number_key(key_out, value->text);
    return ndx_encode_text_key(key_out, ndx->key_length, value->text);
}

static inline int build_bound_key(ndx_file *ndx, const sql_value *value,
    unsigned char *key_out)
{
    if (value->type == sql_value_none || value->text[0] == '\0')
        return -1;
    return build_eq_key(ndx, value, key_out);
}

/* Implemented in execute_sysviews.c; called by execute_subquery.c. */
int dispatch_builtin_view(const sqlexec_env *env,
    const char *view_name, const char *temp_path);

int exec_run_subquery(sqlexec_env *env);
int exec_delete_temp(sqlexec_env *env);

void append_projected_to_temp(exec_temp_ctx *temp,
    const sqlexec_env *env,
    const sqlexec_project_def *project, const row_source *source);

/*
 * Executes a program using an already-constructed sqlexec_env.
 * Used by execute_subquery.c to run inner queries.
 */
int sqlexec_execute_env(sqlexec_env *env);

/* ------------------------------------------------------------------ */
/* Module entry points called by the dispatcher in execute.c           */
/* ------------------------------------------------------------------ */

int exec_ddl(sqlexec_env *env);

int exec_select(sqlexec_env *env, const char *table_name,
    sqlexec_ref emit_ref);

int exec_insert(sqlexec_env *env, const char *table_name,
    sqlexec_span values);

int exec_update(sqlexec_env *env, const char *table_name,
    sqlexec_ref count_ref);

int exec_delete(sqlexec_env *env, const char *table_name,
    sqlexec_ref count_ref);

#endif
