/*
 * Declares the shared SQL value and schema types used across the
 * parser, optimizer, and executor phases.
 * The goal is to keep the common in-memory contract separate from any
 * one phase-specific API so modules can be loaded independently later.
 *
 * MIT License (see: LICENSE)
 * Copyright (C) 2026 tomaz stih
 */

#ifndef sqltypes_h
#define sqltypes_h

#define sql_name_size 17
#define sql_max_columns 16
#define sql_value_size 33
#define sql_where_max_nodes 16
#define sql_where_max_values 16
#define sql_where_nil 0xffu
#define sql_subquery_size 241

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

typedef struct sql_column_ref {
    char qualifier[sql_name_size];
    char name[sql_name_size];
} sql_column_ref;

typedef enum sql_where_node_type {
    sql_where_invalid = 0,
    sql_where_compare,
    sql_where_in,
    sql_where_and,
    sql_where_or
} sql_where_node_type;

typedef struct sql_where_node {
    sql_where_node_type type;
    char qualifier[sql_name_size];
    char column_name[sql_name_size];
    sql_compare_operator operator;
    unsigned char left;
    unsigned char right;
    unsigned char value_first;
    unsigned char value_count;
} sql_where_node;

typedef struct sql_where {
    unsigned char active;
    unsigned char root;
    unsigned char node_count;
    unsigned char value_count;
} sql_where;

typedef struct sql_assignment {
    char column_name[sql_name_size];
    sql_value value;
} sql_assignment;

#endif
