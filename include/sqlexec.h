/*
 * Declares a tiny physical-plan layer for atomic SQL operations.
 * The code stores one statement as an execution tree inside a bounded
 * arena of fixed-size nodes so later generator, optimizer, and
 * executor stages can share one compact representation.
 *
 * MIT License (see: LICENSE)
 * Copyright (C) 2026 tomaz stih
 */

#ifndef sqlexec_h
#define sqlexec_h

#include "sqltypes.h"

#define sqlexec_max_nodes 20
#define sqlexec_max_names 24

typedef unsigned short sqlexec_ref;

#ifndef sqlexec_program_declared
#define sqlexec_program_declared
struct sqlexec_program;
typedef struct sqlexec_program sqlexec_program;
#endif

#define sqlexec_nil ((sqlexec_ref)0xffffu)

typedef enum sqlexec_opcode {
    sqlexec_invalid = 0,
    sqlexec_sequence,
    sqlexec_create_database,
    sqlexec_show_databases,
    sqlexec_use_database,
    sqlexec_drop_database,
    sqlexec_create_table,
    sqlexec_drop_table,
    sqlexec_build_index,
    sqlexec_register_index,
    sqlexec_unregister_table_indexes,
    sqlexec_unregister_database_indexes,
    sqlexec_open_table,
    sqlexec_close_table,
    sqlexec_table_scan,
    sqlexec_join_scan,
    sqlexec_index_scan_eq,
    sqlexec_index_scan_range,
    sqlexec_filter,
    sqlexec_project,
    sqlexec_make_record,
    sqlexec_append_record,
    sqlexec_apply_assignments,
    sqlexec_write_current,
    sqlexec_delete_current,
    sqlexec_count_rows,
    sqlexec_count_affected,
    sqlexec_emit_rows,
    sqlexec_emit_count,
    sqlexec_rebuild_table_indexes,
    sqlexec_create_view,
    sqlexec_drop_view,
    sqlexec_run_subquery,
    sqlexec_delete_temp
} sqlexec_opcode;

typedef struct sqlexec_span {
    unsigned char first;
    unsigned char count;
} sqlexec_span;

typedef struct sqlexec_named {
    char name[sql_name_size];
} sqlexec_named;

typedef struct sqlexec_table_def {
    char name[sql_name_size];
    sqlexec_span columns;
} sqlexec_table_def;

typedef struct sqlexec_index_def {
    char index_name[sql_name_size];
    char table_name[sql_name_size];
    unsigned char unique;
    sqlexec_span key_names;
} sqlexec_index_def;

typedef struct sqlexec_project_def {
    unsigned char select_all;
    sqlexec_span qualifiers;
    sqlexec_span names;
    sqlexec_span aliases;
} sqlexec_project_def;

/* Accessor macros for join name spans in program->names[]. */
#define join_left_table(p, j)   (p)->names[(j).tables.first + 0]
#define join_left_alias(p, j)   (p)->names[(j).tables.first + 1]
#define join_right_table(p, j)  (p)->names[(j).tables.first + 2]
#define join_right_alias(p, j)  (p)->names[(j).tables.first + 3]

/*
 * Join definition stored as spans into the program's names[] pool.
 * tables: names[first]=left_table, names[first+1]=left_alias,
 *                        names[first+2]=right_table, names[first+3]=right_alias
 * The ON condition is stored in the WHERE tree as a regular compare
 * node with the right-hand column encoded as a sql_value_identifier
 * "qualifier.name" string — no separate keys storage needed.
 */
typedef struct sqlexec_join_def {
    sqlexec_span tables;   /* 4 names: left_table, left_alias, right_table, right_alias */
} sqlexec_join_def;

typedef struct sqlexec_index_probe {
    char index_name[sql_name_size];
    sql_value value;
} sqlexec_index_probe;

typedef struct sqlexec_index_range {
    char index_name[sql_name_size];
    sql_compare_operator lower_operator;
    sql_compare_operator upper_operator;
    sql_value lower_value;
    sql_value upper_value;
} sqlexec_index_range;

typedef union sqlexec_payload {
    sqlexec_named named;
    sqlexec_table_def table;
    sqlexec_index_def index;
    sqlexec_project_def project;
    sqlexec_join_def join;
    sqlexec_span values;
    sqlexec_span assignments;
    sql_where where;
    sqlexec_index_probe index_probe;
    sqlexec_index_range index_range;
} sqlexec_payload;

typedef struct sqlexec_node {
    sqlexec_opcode opcode;
    sqlexec_ref first_child;
    sqlexec_ref next_sibling;
    sqlexec_payload data;
} sqlexec_node;

typedef void (*sqlexec_write_char_fn)(char c);

typedef struct sqlexec_io {
    sqlexec_write_char_fn write_char;
} sqlexec_io;

struct sqlexec_program {
    sqlexec_ref root;
    sqlexec_ref free_head;
    unsigned short node_count;
    sqlexec_node nodes[sqlexec_max_nodes];
    unsigned char name_count;
    char names[sqlexec_max_names][sql_name_size];
    unsigned char column_count;
    sql_column columns[sql_max_columns];
    unsigned char value_count;
    sql_value values[sql_max_columns];
    unsigned char assignment_count;
    sql_assignment assignments[sql_max_columns];
    unsigned char where_node_count;
    sql_where_node where_nodes[sql_where_max_nodes];
    unsigned char where_value_count;
    sql_value where_values[sql_where_max_values];
    /* SQL text for one subquery or view expansion per program. */
    char subquery_text[sql_subquery_size];
};

/*
 * Resets the plan arena and returns every node to the free list.
 * Any previous nodes, payload spans, or tree links are discarded.
 */
void sqlexec_reset(sqlexec_program *program);

/*
 * Returns the printable ASCII name for one opcode.
 * The pointer is always valid and never NULL.
 */
const char *sqlexec_opcode_name(sqlexec_opcode opcode);

/*
 * Creates the root node for one execution tree.
 * Returns the new root reference or sqlexec_nil on failure.
 */
sqlexec_ref sqlexec_make_root(sqlexec_program *program,
    sqlexec_opcode opcode);

/*
 * Appends one new child node to the given parent node.
 * Returns the new child reference or sqlexec_nil on failure.
 */
sqlexec_ref sqlexec_append_child(sqlexec_program *program,
    sqlexec_ref parent, sqlexec_opcode opcode);

/*
 * Inserts one new sibling node after the given node reference.
 * Returns the new sibling reference or sqlexec_nil on failure.
 */
sqlexec_ref sqlexec_insert_sibling_after(sqlexec_program *program,
    sqlexec_ref after, sqlexec_opcode opcode);

/*
 * Removes one subtree from the active execution tree and returns every
 * node in that subtree to the free list. Returns zero on success and
 * -1 on failure.
 */
int sqlexec_remove(sqlexec_program *program, sqlexec_ref target);

/*
 * Replaces one node opcode and clears its payload while preserving the
 * current child and sibling links. Returns zero on success and -1 on
 * failure.
 */
int sqlexec_replace(sqlexec_program *program, sqlexec_ref target,
    sqlexec_opcode opcode);

/*
 * Returns a writable pointer to one active node or NULL on failure.
 */
sqlexec_node *sqlexec_get(sqlexec_program *program, sqlexec_ref ref);

/*
 * Returns a read-only pointer to one active node or NULL on failure.
 */
const sqlexec_node *sqlexec_get_const(const sqlexec_program *program,
    sqlexec_ref ref);

/*
 * Copies one name list into the program-owned pool and returns the
 * first slot through first_out. Returns zero on success and -1 on
 * overflow or invalid input.
 */
int sqlexec_add_names(sqlexec_program *program,
    char names[][sql_name_size], unsigned char count,
    unsigned char *first_out);

/*
 * Copies one CREATE TABLE column list into the program-owned pool.
 * Returns zero on success and -1 on overflow or invalid input.
 */
int sqlexec_add_columns(sqlexec_program *program,
    const sql_column *columns,
    unsigned char count, unsigned char *first_out);

/*
 * Copies one VALUES list into the program-owned pool.
 * Returns zero on success and -1 on overflow or invalid input.
 */
int sqlexec_add_values(sqlexec_program *program, const sql_value *values,
    unsigned char count, unsigned char *first_out);

/*
 * Copies one SET assignment list into the program-owned pool.
 * Returns zero on success and -1 on overflow or invalid input.
 */
int sqlexec_add_assignments(sqlexec_program *program,
    const sql_assignment *assignments, unsigned char count,
    unsigned char *first_out);

/*
 * Validates tree links and opcode payload spans inside one program.
 * Returns zero when the tree is self-consistent and -1 otherwise.
 * In production builds (SQLEXEC_DEBUG not defined) this is a no-op
 * that always returns zero.
 */
#ifdef SQLEXEC_DEBUG
int sqlexec_validate(const sqlexec_program *program);
#else
#define sqlexec_validate(p) 0
#endif

#ifdef SQLEXEC_DEBUG
/*
 * Renders one human-readable plan dump into the caller buffer.
 * Returns zero on success and -1 when the buffer is too small or the
 * program is invalid.
 */
int sqlexec_dump(const sqlexec_program *program, char *text,
    unsigned short size);
#endif

/*
 * Extracts the source table name and SELECT column information from a
 * compiled SELECT program. Used by exec_sql_to_temp to determine the
 * temp table schema without re-walking the tree at every call site.
 * Returns zero on success and -1 when the program is not a SELECT.
 */
int sqlexec_get_output_info(const sqlexec_program *program,
    const char **table_name_out,
    unsigned char *select_all_out,
    sqlexec_span *names_out);

/*
 * Executes one validated execution tree against the DBF/NDX storage
 * root. The executor may update current_db for database-selection
 * statements and writes any user-visible output through io. When io is
 * NULL or io->write_char is NULL, execution stays silent. Returns zero
 * on success and -1 on failure.
 */
int sqlexec_execute(const char *root, const sqlexec_program *program,
    char *current_db, const sqlexec_io *io);

#endif
