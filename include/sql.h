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

#define sql_name_size 17
#define sql_max_columns 16
#define sql_value_size 33

typedef struct sql_column {
    char name[sql_name_size];
    char dbf_type;
    unsigned char length;
    unsigned char decimals;
} sql_column;

typedef enum sql_compare_operator {
    sql_compare_invalid = 0,
    sql_compare_equal,
    sql_compare_not_equal,
    sql_compare_less,
    sql_compare_less_equal,
    sql_compare_greater,
    sql_compare_greater_equal
} sql_compare_operator;

typedef enum sql_value_type {
    sql_value_none = 0,
    sql_value_identifier,
    sql_value_number,
    sql_value_string
} sql_value_type;

typedef struct sql_value {
    sql_value_type type;
    char text[sql_value_size];
} sql_value;

typedef struct sql_where {
    unsigned char active;
    char column_name[sql_name_size];
    sql_compare_operator operator;
    sql_value value;
} sql_where;

typedef struct sql_assignment {
    char column_name[sql_name_size];
    sql_value value;
} sql_assignment;

typedef enum sql_statement_type {
    sql_statement_invalid = 0,
    sql_statement_create_database,
    sql_statement_show_databases,
    sql_statement_create_table,
    sql_statement_select,
    sql_statement_insert,
    sql_statement_update,
    sql_statement_delete
} sql_statement_type;

typedef struct sql_statement {
    sql_statement_type type;
    char name[sql_name_size];
    unsigned char column_count;
    sql_column columns[sql_max_columns];
    unsigned char select_all;
    unsigned char select_count;
    char select_names[sql_max_columns][sql_name_size];
    unsigned char value_count;
    sql_value values[sql_max_columns];
    unsigned char assignment_count;
    sql_assignment assignments[sql_max_columns];
    sql_where where;
} sql_statement;

/*
 * Parses one SQL statement from text into a fixed output structure.
 * The parser currently supports CREATE DATABASE, SHOW DATABASES,
 * CREATE TABLE, SELECT, INSERT, UPDATE, and DELETE subsets.
 * Returns zero on success and -1 on failure.
 */
int sql_parse(const char *text, sql_statement *statement);

#endif
