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

#define sqlexec_max_nodes 2
#define sqlexec_max_names 64

typedef unsigned char sqlexec_ref;

#ifndef sqlexec_program_declared
#define sqlexec_program_declared
struct sqlexec_program;
typedef struct sqlexec_program sqlexec_program;
#endif

#define sqlexec_nil ((sqlexec_ref)0xffu)

typedef unsigned char sqlexec_opcode;
enum {
    sqlexec_invalid = 0,
    sqlexec_sequence,
    sqlexec_create_database,
    sqlexec_use_database,
    sqlexec_drop_database,
    sqlexec_create_table,
    sqlexec_drop_table,
    sqlexec_create_index,
    sqlexec_table_scan,
    sqlexec_join_scan,
    sqlexec_project,
    sqlexec_append_record,
    sqlexec_write_current,
    sqlexec_delete_current,
    sqlexec_create_view,
    sqlexec_drop_view
};

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

typedef unsigned char sqlexec_scan_access_kind;
enum {
    sqlexec_scan_full = 0,
    sqlexec_scan_index_eq,
    sqlexec_scan_index_range
};

typedef struct sqlexec_scan_def {
    sqlexec_scan_access_kind access_kind;
    char index_name[sql_name_size];
    sql_compare_operator lower_operator;
    sql_compare_operator upper_operator;
    sql_value lower_value;
    sql_value upper_value;
} sqlexec_scan_def;

typedef struct sqlexec_index_def {
    char index_name[sql_name_size];
    char table_name[sql_name_size];
    unsigned char unique;
    sqlexec_span key_names;
} sqlexec_index_def;

typedef struct sqlexec_project_def {
    unsigned char select_all;
    unsigned char distinct;
    unsigned char has_aggregate;
    sql_select_function functions[sql_max_columns];
    unsigned char function_arg_is_star[sql_max_columns];
    sqlexec_span qualifiers;
    sqlexec_span names;
    sqlexec_span aliases;
    sqlexec_span group_qualifiers;
    sqlexec_span group_names;
} sqlexec_project_def;

/* Accessor macros for join name spans in program->names[]. */
#define join_source_count(j) ((unsigned char)((j).tables.count / 2u))
#define join_table_at(p, j, i) \
    (p)->names[(j).tables.first + ((unsigned char)(i) * 2u)]
#define join_alias_at(p, j, i) \
    (p)->names[(j).tables.first + ((unsigned char)(i) * 2u) + 1u]
#define join_left_table(p, j)   join_table_at(p, j, 0)
#define join_left_alias(p, j)   join_alias_at(p, j, 0)
#define join_right_table(p, j)  join_table_at(p, j, 1)
#define join_right_alias(p, j)  join_alias_at(p, j, 1)

/*
 * Join definition stored as spans into the program's names[] pool.
 * tables: names[first + 2*i] = table_i, names[first + 2*i + 1] = alias_i
 * for every source table in the join order, starting with the base FROM
 * table and followed by every JOIN source.
 * The ON condition is stored in the WHERE tree as a regular compare
 * node with a structured right-hand column operand — no separate keys
 * storage needed.
 */
typedef struct sqlexec_join_def {
    sqlexec_span tables;   /* 2 * source_count names: table_0, alias_0, ... */
} sqlexec_join_def;

typedef union sqlexec_payload {
    sqlexec_named named;
    sqlexec_table_def table;
    sqlexec_scan_def scan;
    sqlexec_index_def index;
    sqlexec_project_def project;
    sqlexec_join_def join;
    sqlexec_span assignments;
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

typedef union sqlexec_program_storage {
    struct {
        unsigned char column_count;
        sql_column columns[sql_max_columns];
    } table;
    struct {
        unsigned char assignment_count;
        sql_assignment assignments[sql_max_columns];
    } mutate;
    char subquery_text[sql_subquery_size];
} sqlexec_program_storage;

#define program_column_count(p) ((p)->storage.table.column_count)
#define program_columns(p) ((p)->storage.table.columns)
#define program_assignment_count(p) ((p)->storage.mutate.assignment_count)
#define program_assignments(p) ((p)->storage.mutate.assignments)
#define program_subquery_text(p) ((p)->storage.subquery_text)

struct sqlexec_program {
    sqlexec_ref root;
    sqlexec_ref free_head;
    unsigned char node_count;
    sqlexec_node nodes[sqlexec_max_nodes];
    /* Target table for SELECT / INSERT / UPDATE / DELETE plans. */
    char table_name[sql_name_size];
    unsigned char name_count;
    char names[sqlexec_max_names][sql_name_size];
    sqlexec_program_storage storage;
    unsigned char predicate_subquery_count;
    char predicate_subqueries[sql_max_predicate_subqueries]
        [sql_subquery_size];
    unsigned char predicate_node_count;
    unsigned char predicate_value_count;
    sql_where where;
    unsigned char having_node_first;
    unsigned char having_value_first;
    unsigned char where_source_mask_count;
    unsigned char where_source_masks[sql_where_max_nodes];
    unsigned char where_bound_slot_count;
    unsigned char where_left_slots[sql_where_max_nodes];
    sql_where_node where_nodes[sql_where_max_nodes];
    unsigned char where_value_slots[sql_where_max_values];
    sql_predicate_operand where_values[sql_where_max_values];
    sql_where having;
};

static inline const sql_where_node *program_having_nodes(
    const sqlexec_program *program)
{
    return program->where_nodes + program->having_node_first;
}

static inline const sql_predicate_operand *program_having_values(
    const sqlexec_program *program)
{
    return program->where_values + program->having_value_first;
}

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
 * Executes one validated execution tree against the DBF/NDX storage
 * root. The executor may update current_db for database-selection
 * statements and writes any user-visible output through io. When io is
 * NULL or io->write_char is NULL, execution stays silent. Returns zero
 * on success and -1 on failure.
 */
int sqlexec_execute(const char *root, const sqlexec_program *program,
    char *current_db, const sqlexec_io *io);

#endif
