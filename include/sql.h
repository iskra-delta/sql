/*
 * Declares a tiny SQL parser for a small CP/M-style database shell.
 * The parser uses a minimal hand-written lexer and recursive descent
 * logic for a very small subset of SQL statements.
 *
 * MIT License (see: LICENSE)
 * Copyright (C) 2026 tomaz stih
 */

#ifndef sql_h
#define sql_h

#include "sqltypes.h"

#ifndef sqlexec_program_declared
#define sqlexec_program_declared
struct sqlexec_program;
typedef struct sqlexec_program sqlexec_program;
#endif

typedef unsigned char sql_statement_type;
enum {
    sql_statement_invalid = 0,
    sql_statement_create_database,
    sql_statement_use,
    sql_statement_drop_database,
    sql_statement_drop_table,
    sql_statement_create_table,
    sql_statement_create_index,
    sql_statement_create_view,
    sql_statement_drop_view,
    sql_statement_select,
    sql_statement_insert,
    sql_statement_update,
    sql_statement_delete,
    sql_statement_begin,
    sql_statement_commit,
    sql_statement_rollback
};

typedef struct sql_select_item {
    sql_select_function function;
    unsigned char argument_is_star;
    sql_column_ref column;
    char alias[sql_name_size];
} sql_select_item;

typedef union sql_statement_variant {
    struct {
        unsigned char column_count;
        sql_column columns[sql_max_columns];
    } create_table;
    struct {
        unsigned char unique;
        unsigned char key_count;
        char key_names[sql_max_columns][sql_name_size];
    } create_index;
    struct {
        unsigned char assignment_count;
        sql_assignment assignments[sql_max_columns];
    } mutate;
} sql_statement_variant;

typedef union sql_statement_detail {
    struct {
        unsigned char from_is_subquery;
        char from_alias[sql_name_size];
        unsigned char join_count;
        char join_table_names[sql_max_joins][sql_name_size];
        char join_aliases[sql_max_joins][sql_name_size];
        unsigned char select_all;
        unsigned char select_distinct;
        unsigned char select_count_star;
        unsigned char select_has_aggregate;
        unsigned char select_count;
        sql_select_item select_items[sql_max_columns];
        unsigned char group_count;
        sql_column_ref group_items[sql_max_columns];
    } select;
    sql_statement_variant variant;
} sql_statement_detail;

typedef struct sql_statement {
    sql_statement_type type;
    char name[sql_name_size];
    char table_name[sql_name_size];
    char subquery_text[sql_subquery_size];
    sql_statement_detail detail;
    unsigned char predicate_subquery_count;
    char predicate_subqueries[sql_max_predicate_subqueries]
        [sql_subquery_size];
    unsigned char predicate_node_count;
    unsigned char predicate_value_count;
    sql_where where;
    sql_where_node where_nodes[sql_where_max_nodes];
    sql_predicate_operand where_values[sql_where_max_values];
    sql_where having;
    unsigned char having_node_first;
    unsigned char having_value_first;
} sql_statement;

#define statement_having_nodes(stmt) \
    ((stmt)->where_nodes + (stmt)->having_node_first)
#define statement_having_values(stmt) \
    ((stmt)->where_values + (stmt)->having_value_first)

/*
 * Parses one SQL statement from text into one sqlexec execution tree.
 * Supports CREATE DATABASE, SHOW DATABASES, CREATE TABLE, CREATE INDEX,
 * CREATE VIEW, DROP VIEW, SHOW VIEWS, SELECT (including FROM subqueries),
 * INSERT, UPDATE, and DELETE subsets.
 * root and current_db are used for view name resolution; pass NULL for
 * both when catalog access is not needed (e.g. in tests).
 * Returns zero on success and -1 on failure.
 */
int sql_parse(const char *text, sqlexec_program *program,
    const char *root, const char *current_db);

/*
 * Parses one SQL statement from text into the fixed syntax structure
 * used by the parser internals and low-level syntax tests.
 * Returns zero on success and -1 on failure.
 */
int sql_parse_statement(const char *text, sql_statement *statement);

/*
 * Parses one SELECT body from text into the fixed syntax structure
 * used by the parser internals and view/subquery validation paths.
 * The text must begin with SELECT and must not include a trailing
 * semicolon. Returns zero on success and -1 on failure.
 */
int sql_parse_select_body(const char *text, sql_statement *statement);

/*
 * Parses one SELECT body from text directly into one sqlexec program.
 * The text must begin with SELECT and must not include a trailing
 * semicolon. root/current_db are used for view name resolution and may
 * be NULL when catalog access is not needed.
 * Returns zero on success and -1 on failure.
 */
int sql_parse_select_program(const char *text, sqlexec_program *program,
    const char *root, const char *current_db);

#endif
