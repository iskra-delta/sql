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
#define sql_max_sources 4
#define sql_max_joins (sql_max_sources - 1)
#define sql_max_groups 8
#define sql_value_size 34   /* 33 chars + null for one literal value */
#define sql_where_max_nodes 48
#define sql_where_max_values 32
#define sql_where_nil 0xffu
#define sql_subquery_size 241
#define sql_max_predicate_subqueries 8
#define sql_predicate_subquery_rows 32

typedef struct sql_column {
    char name[sql_name_size];
    char dbf_type;
    unsigned char length;
    unsigned char decimals;
} sql_column;

typedef unsigned char sql_compare_operator;
enum {
    sql_compare_invalid = 0,
    sql_compare_equal,
    sql_compare_not_equal,
    sql_compare_less,
    sql_compare_less_equal,
    sql_compare_greater,
    sql_compare_greater_equal
};

typedef unsigned char sql_value_type;
enum {
    sql_value_none = 0,
    sql_value_identifier,
    sql_value_number,
    sql_value_string,
    sql_value_null
};

typedef unsigned char sql_quantifier;
enum {
    sql_quantifier_none = 0,
    sql_quantifier_any,
    sql_quantifier_all
};

typedef unsigned char sql_truth_value;
enum {
    sql_truth_false = 0,
    sql_truth_true,
    sql_truth_unknown
};

typedef unsigned char sql_select_function;
enum {
    sql_function_none = 0,
    sql_function_trim,
    sql_function_count,
    sql_function_min,
    sql_function_max,
    sql_function_sum,
    sql_function_avg
};

typedef struct sql_value {
    sql_value_type type;
    char text[sql_value_size];
} sql_value;

typedef struct sql_column_ref {
    char qualifier[sql_name_size];
    char name[sql_name_size];
} sql_column_ref;

typedef unsigned char sql_predicate_operand_kind;
enum {
    sql_predicate_operand_invalid = 0,
    sql_predicate_operand_value,
    sql_predicate_operand_column
};

typedef struct sql_predicate_operand {
    sql_predicate_operand_kind kind;
    union {
        sql_value value;
        sql_column_ref column;
    } data;
} sql_predicate_operand;

typedef unsigned char sql_where_node_type;
enum {
    sql_where_invalid = 0,
    sql_where_false,
    sql_where_compare,
    sql_where_in,
    sql_where_like,
    sql_where_is_null,
    sql_where_quantified,
    sql_where_exists,
    sql_where_not,
    sql_where_and,
    sql_where_or
};

typedef struct sql_where_node {
    sql_where_node_type type;
    char qualifier[sql_name_size];
    char column_name[sql_name_size];
    sql_compare_operator operator;
    unsigned char left;
    unsigned char right;
    unsigned char value_first;
    unsigned char value_count;
    unsigned char subquery_index;
    unsigned char quantifier;
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

typedef struct sql_predicate_subquery_result {
    unsigned char row_count;
    unsigned char overflow;
    sql_value values[sql_predicate_subquery_rows];
} sql_predicate_subquery_result;

typedef struct sql_predicate_subquery_cache {
    unsigned char count;
    sql_predicate_subquery_result
        results[sql_max_predicate_subqueries];
} sql_predicate_subquery_cache;

#endif
