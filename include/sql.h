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

typedef enum sql_statement_type {
    sql_statement_invalid = 0,
    sql_statement_create_database,
    sql_statement_show_databases,
    sql_statement_use,
    sql_statement_drop_database,
    sql_statement_drop_table,
    sql_statement_create_table,
    sql_statement_create_index,
    sql_statement_create_view,
    sql_statement_drop_view,
    sql_statement_show_views,
    sql_statement_select,
    sql_statement_insert,
    sql_statement_update,
    sql_statement_delete
} sql_statement_type;

typedef struct sql_select_item {
    sql_column_ref column;
    char alias[sql_name_size];
} sql_select_item;

typedef struct sql_statement {
    sql_statement_type type;
    char name[sql_name_size];
    char table_name[sql_name_size];
    /* FROM (SELECT ...) or view expansion */
    unsigned char from_is_subquery;
    char subquery_text[sql_subquery_size];
    char from_alias[sql_name_size];
    unsigned char join_active;
    char join_table_name[sql_name_size];
    char join_alias[sql_name_size];
    sql_column_ref join_left;
    sql_column_ref join_right;
    unsigned char column_count;
    sql_column columns[sql_max_columns];
    unsigned char create_index_unique;
    unsigned char key_count;
    char key_names[sql_max_columns][sql_name_size];
    unsigned char select_all;
    unsigned char select_count_star;
    unsigned char select_count;
    sql_select_item select_items[sql_max_columns];
    unsigned char value_count;
    sql_value values[sql_max_columns];
    unsigned char assignment_count;
    sql_assignment assignments[sql_max_columns];
    sql_where where;
    sql_where_node where_nodes[sql_where_max_nodes];
    sql_value where_values[sql_where_max_values];
} sql_statement;

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

#endif
