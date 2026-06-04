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
#include "where.h"
#include "../catalog/catalog.h"
#include "metacache.h"

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

typedef unsigned char exec_collect_mode;
enum {
    exec_collect_values = 0,
    exec_collect_exists
};

typedef struct exec_collect_ctx {
    exec_collect_mode mode;
    sql_predicate_subquery_result *result;
} exec_collect_ctx;

typedef struct exec_bound_field {
    unsigned char source_index;
    unsigned char field_index;
} exec_bound_field;

typedef unsigned char exec_group_output_kind;
enum {
    exec_group_output_invalid = 0,
    exec_group_output_key,
    exec_group_output_count,
    exec_group_output_min,
    exec_group_output_max,
    exec_group_output_sum,
    exec_group_output_avg
};

typedef struct exec_group_aggregate {
    exec_group_output_kind kind;
    unsigned char output_index;
    exec_bound_field input;
    char input_type;
} exec_group_aggregate;

typedef struct exec_project_binding {
    unsigned short output_count;
    unsigned short group_count;
    unsigned short aggregate_count;
    exec_bound_field outputs[sql_max_columns];
    exec_bound_field groups[sql_max_columns];
    exec_group_aggregate aggregates[sql_max_columns];
    signed char group_lookup[sql_max_columns];
    signed char group_output_slots[sql_max_columns];
    exec_group_output_kind group_output_kinds[sql_max_columns];
    char output_types[sql_max_columns];
    signed char having_left_output[sql_where_max_nodes];
    signed char having_value_output[sql_where_max_values];
} exec_project_binding;

/* Pull in sql_context (and with it sql_txn) for the env fields below. */
#include "sqlctx.h"

typedef struct sqlexec_env {
    const char *root;
    const sqlexec_program *program;
    char *current_db;
    const sqlexec_io *io;
    /* Non-NULL only when materialising a subquery to a temp table. */
    exec_temp_ctx *temp;
    /* Non-NULL only when collecting predicate subquery rows in memory. */
    exec_collect_ctx *collect;
    /* Schema cache: non-NULL after USE, freed on schema change. */
    meta_cache *schema;
    /* Shared counter for allocating unique temp files in nested work. */
    unsigned short *temp_serial;
    /* Transaction state — NULL when called outside sql_context. */
    struct sql_txn *txn;
    /* Original SQL text for statement recording inside a transaction. */
    const char *source_text;
    /* Full context pointer for COMMIT conflict replay. */
    struct sql_context *ctx;
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
    unsigned char   eq_key[ndx_max_key_length];     /* equality access     */
    unsigned char   lower_key[ndx_max_key_length];  /* range access        */
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
    /* May be NULL when the program carries packed WHERE bindings. */
    const where_binding    *where_binding;
    /* Action-specific payload */
    exec_scan_action action;
    /* SELECT */
    const exec_project_binding *binding;
    int             count_only;
    unsigned short *row_count;
    const sql_predicate_subquery_cache *subqueries;
    /* UPDATE */
    const sqlexec_node *write_node;
    const unsigned short *assignment_fields;
    unsigned short *changed_count;
    /* DELETE */
    unsigned short *deleted_count;
    /* Error flag set by callback on I/O failure */
    int             failed;
} exec_index_scan_ctx;

/*
 * ndx_scan callback for equality access on a table_scan.
 * Stops when the current key is past the target; processes records
 * whose key exactly equals eq_key.
 */
int exec_eq_scan_callback(unsigned long record_number,
    const ndx_scan_entry *entry);

/*
 * ndx_scan callback for range access on a table_scan.
 * Skips records below the lower bound; stops when past the upper bound.
 */
int exec_range_scan_callback(unsigned long record_number,
    const ndx_scan_entry *entry);

/*
 * Runs one index-backed scan node through the shared callback path.
 * Returns zero on success and -1 on NDX open/build/scan failure.
 */
int exec_run_index_scan(const sqlexec_env *env, const sqlexec_node *scan_node,
    exec_index_scan_ctx *ctx);

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
    *where = program->where;
    node = sqlexec_get_const(program, input_ref);
    if (!node) return -1;
    if (node->opcode != sqlexec_table_scan
        && node->opcode != sqlexec_join_scan) {
        return -1;
    }
    *scan_ref_out = input_ref;
    return 0;
}

static inline int scan_uses_index(const sqlexec_node *node)
{
    return node
        && node->opcode == sqlexec_table_scan
        && node->data.scan.access_kind != sqlexec_scan_full;
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

int load_predicate_subqueries(const sqlexec_env *env,
    sql_predicate_subquery_cache *cache);

unsigned short exec_project_output_count(const sqlexec_project_def *project,
    const row_source *sources, unsigned char source_count);

int exec_resolve_project_field(const sqlexec_program *program,
    const sqlexec_project_def *project, const row_source *sources,
    unsigned char source_count, unsigned short index,
    const row_source **source_out, int *field_index_out);

int exec_resolve_group_field(const sqlexec_program *program,
    const sqlexec_project_def *project, const row_source *sources,
    unsigned char source_count, unsigned short index,
    const row_source **source_out, int *field_index_out);

int exec_bind_project(const sqlexec_program *program,
    const sqlexec_project_def *project, const row_source *sources,
    unsigned char source_count, exec_project_binding *binding);

void exec_copy_dbf_field_name(char *target, const char *source);

void exec_copy_value_text(char *target, const char *source);

int exec_build_project_output_values_bound(
    const exec_project_binding *binding, const row_source *sources,
    char output_values[sql_max_columns][sql_value_size],
    unsigned short *output_count_out);

int exec_append_output_values_to_temp(exec_temp_ctx *temp,
    char output_values[sql_max_columns][sql_value_size],
    unsigned short output_count);

int exec_collect_output_values(exec_collect_ctx *collect,
    char output_values[sql_max_columns][sql_value_size],
    const char output_types[sql_max_columns], unsigned short output_count);

int append_projected_to_temp_bound(exec_temp_ctx *temp,
    const exec_project_binding *binding, const row_source *sources);

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
    sqlexec_ref plan_ref);

int exec_insert(sqlexec_env *env, const char *table_name,
    sqlexec_span assignments);

int exec_update(sqlexec_env *env, const char *table_name,
    sqlexec_ref action_ref);

int exec_delete(sqlexec_env *env, const char *table_name,
    sqlexec_ref action_ref);

#endif
