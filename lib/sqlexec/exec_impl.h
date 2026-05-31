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

/*
 * Execution environment threaded through every executor function.
 * Holds the context that would otherwise require global state.
 */
typedef struct sqlexec_env {
    const char *root;
    const sqlexec_program *program;
    char *current_db;
    const sqlexec_io *io;
    /* When write_to_temp is set, exec_select appends projected rows to
     * temp_out instead of writing text to io. */
    int write_to_temp;
    dbf_file *temp_out;
    const dbf_field *temp_fields;
    const unsigned short *temp_offsets;
    unsigned short temp_field_count;
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

int exec_run_subquery(sqlexec_env *env);
int exec_delete_temp(sqlexec_env *env);

void append_projected_to_temp(const sqlexec_env *env,
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
