/*
 * Provides tiny automated tests for the SQL parser library.
 * The tests exercise the small supported grammar and keep the
 * parser behavior stable as more statements are added later.
 *
 * MIT License (see: LICENSE)
 * Copyright (C) 2026 tomaz stih
 */

#include "sql.h"

#include <stdio.h>
#include <string.h>

#define sql_parse sql_parse_statement

#define stmt_column_count(stmt) ((stmt)->detail.variant.create_table.column_count)
#define stmt_columns(stmt) ((stmt)->detail.variant.create_table.columns)
#define stmt_index_unique(stmt) ((stmt)->detail.variant.create_index.unique)
#define stmt_key_count(stmt) ((stmt)->detail.variant.create_index.key_count)
#define stmt_key_names(stmt) ((stmt)->detail.variant.create_index.key_names)
#define stmt_assignment_count(stmt) ((stmt)->detail.variant.mutate.assignment_count)
#define stmt_assignments(stmt) ((stmt)->detail.variant.mutate.assignments)
#define stmt_from_is_subquery(stmt) ((stmt)->detail.select.from_is_subquery)
#define stmt_from_alias(stmt) ((stmt)->detail.select.from_alias)
#define stmt_join_count(stmt) ((stmt)->detail.select.join_count)
#define stmt_join_table_names(stmt) ((stmt)->detail.select.join_table_names)
#define stmt_join_aliases(stmt) ((stmt)->detail.select.join_aliases)
#define stmt_select_all(stmt) ((stmt)->detail.select.select_all)
#define stmt_select_distinct(stmt) ((stmt)->detail.select.select_distinct)
#define stmt_select_count_star(stmt) ((stmt)->detail.select.select_count_star)
#define stmt_select_has_aggregate(stmt) ((stmt)->detail.select.select_has_aggregate)
#define stmt_select_count(stmt) ((stmt)->detail.select.select_count)
#define stmt_select_items(stmt) ((stmt)->detail.select.select_items)
#define stmt_group_count(stmt) ((stmt)->detail.select.group_count)
#define stmt_group_items(stmt) ((stmt)->detail.select.group_items)

static const sql_where_node *where_root_node(const sql_statement *statement)
{
    if (!statement->where.active
        || statement->where.root == (unsigned char)sql_where_nil
        || statement->where.root >= statement->where.node_count) {
        return NULL;
    }

    return &statement->where_nodes[statement->where.root];
}

static int operand_is_value(const sql_predicate_operand *operand,
    sql_value_type value_type, const char *value_text)
{
    return operand->kind == sql_predicate_operand_value
        && operand->data.value.type == value_type
        && strcmp(operand->data.value.text, value_text) == 0;
}

static int operand_is_column(const sql_predicate_operand *operand,
    const char *qualifier, const char *name)
{
    return operand->kind == sql_predicate_operand_column
        && strcmp(operand->data.column.qualifier, qualifier) == 0
        && strcmp(operand->data.column.name, name) == 0;
}

static int check_simple_where(const sql_statement *statement,
    const char *column_name, sql_compare_operator operator,
    sql_value_type value_type, const char *value_text)
{
    const sql_where_node *node;

    node = where_root_node(statement);
    if (!node || node->type != sql_where_compare
        || strcmp(node->column_name, column_name) != 0
        || node->operator != operator
        || node->value_count != 1
        || node->value_first >= statement->where.value_count) {
        return 1;
    }

    if (!operand_is_value(&statement->where_values[node->value_first],
        value_type, value_text)) {
        return 1;
    }

    return 0;
}

/*
 * Verifies that CREATE DATABASE parses into the expected structure.
 * Returns zero on success and one on failure.
 */
static int test_create_database_parse(void)
{
    sql_statement statement;

    if (sql_parse("CREATE DATABASE demo;", &statement) != 0) {
        return 1;
    }

    if (statement.type != sql_statement_create_database) {
        return 1;
    }

    if (strcmp(statement.name, "demo") != 0) {
        return 1;
    }

    return 0;
}

/*
 * Verifies that SHOW DATABASES parses into the expected structure.
 * Returns zero on success and one on failure.
 */
static int test_show_databases_parse(void)
{
    sql_statement statement;

    /* SHOW DATABASES expands to SELECT * FROM sys_databases at parse time. */
    if (sql_parse("SHOW DATABASES;", &statement) != 0) {
        return 1;
    }

    if (statement.type != sql_statement_select) {
        return 1;
    }

    if (!stmt_select_all(&statement)) {
        return 1;
    }

    if (strcmp(statement.name, "sys_databases") != 0) {
        return 1;
    }

    return 0;
}

/*
 * Verifies that CREATE TABLE parses DBF-compatible columns.
 * Returns zero on success and one on failure.
 */
static int test_create_table_parse(void)
{
    sql_statement statement;

    if (sql_parse("CREATE TABLE people (name CHAR(8), "
        "age NUMERIC(3), born DATE, active LOGICAL);",
        &statement) != 0) {
        return 1;
    }

    if (statement.type != sql_statement_create_table) {
        return 1;
    }

    if (strcmp(statement.name, "people") != 0) {
        return 1;
    }

    if (stmt_column_count(&statement) != 4) {
        return 1;
    }

    if (stmt_columns(&statement)[0].dbf_type != 'C'
        || stmt_columns(&statement)[0].length != 8) {
        return 1;
    }

    if (stmt_columns(&statement)[1].dbf_type != 'N'
        || stmt_columns(&statement)[1].length != 3
        || stmt_columns(&statement)[1].decimals != 0) {
        return 1;
    }

    if (stmt_columns(&statement)[2].dbf_type != 'D'
        || stmt_columns(&statement)[2].length != 8) {
        return 1;
    }

    if (stmt_columns(&statement)[3].dbf_type != 'L'
        || stmt_columns(&statement)[3].length != 1) {
        return 1;
    }

    return 0;
}

/*
 * Verifies that CREATE [UNIQUE] INDEX parses cleanly.
 * Returns zero on success and one on failure.
 */
static int test_create_index_parse(void)
{
    sql_statement statement;

    if (sql_parse("CREATE UNIQUE INDEX people_name ON people "
        "(city, name);", &statement) != 0) {
        return 1;
    }

    if (statement.type != sql_statement_create_index) {
        return 1;
    }

    if (strcmp(statement.name, "people_name") != 0) {
        return 1;
    }

    if (strcmp(statement.table_name, "people") != 0) {
        return 1;
    }

    if (!stmt_index_unique(&statement) || stmt_key_count(&statement) != 2) {
        return 1;
    }

    if (strcmp(stmt_key_names(&statement)[0], "city") != 0
        || strcmp(stmt_key_names(&statement)[1], "name") != 0) {
        return 1;
    }

    return 0;
}

/*
 * Verifies that CREATE VIEW validates and stores one SELECT body.
 * Returns zero on success and one on failure.
 */
static int test_create_view_parse(void)
{
    sql_statement statement;

    if (sql_parse("CREATE VIEW adult_people AS "
        "SELECT name, city FROM people WHERE age >= 18;",
        &statement) != 0) {
        return 1;
    }

    if (statement.type != sql_statement_create_view) {
        return 1;
    }

    if (strcmp(statement.name, "adult_people") != 0
        || strcmp(statement.subquery_text,
            "SELECT name, city FROM people WHERE age >= 18") != 0) {
        return 1;
    }

    return 0;
}

/*
 * Verifies that SELECT * parses into the expected structure.
 * Returns zero on success and one on failure.
 */
static int test_select_all_parse(void)
{
    sql_statement statement;

    if (sql_parse("SELECT * FROM people;", &statement) != 0) {
        return 1;
    }

    if (statement.type != sql_statement_select) {
        return 1;
    }

    if (strcmp(statement.name, "people") != 0) {
        return 1;
    }

    if (!stmt_select_all(&statement) || stmt_select_count(&statement) != 0) {
        return 1;
    }

    if (statement.where.active) {
        return 1;
    }

    return 0;
}

/*
 * Verifies that SELECT column lists and WHERE parse cleanly.
 * Returns zero on success and one on failure.
 */
static int test_select_where_parse(void)
{
    sql_statement statement;

    if (sql_parse("SELECT name, age FROM people WHERE age >= 18;",
        &statement) != 0) {
        return 1;
    }

    if (statement.type != sql_statement_select) {
        return 1;
    }

    if (strcmp(statement.name, "people") != 0) {
        return 1;
    }

    if (stmt_select_all(&statement) || stmt_select_count(&statement) != 2) {
        return 1;
    }

    if (strcmp(stmt_select_items(&statement)[0].column.name, "name") != 0
        || stmt_select_items(&statement)[0].alias[0] != '\0'
        || stmt_select_items(&statement)[0].column.qualifier[0] != '\0') {
        return 1;
    }

    if (strcmp(stmt_select_items(&statement)[1].column.name, "age") != 0
        || stmt_select_items(&statement)[1].alias[0] != '\0'
        || stmt_select_items(&statement)[1].column.qualifier[0] != '\0') {
        return 1;
    }

    if (!statement.where.active) {
        return 1;
    }

    if (check_simple_where(&statement, "age", sql_compare_greater_equal,
        sql_value_number, "18") != 0) {
        return 1;
    }

    return 0;
}

/*
 * Verifies that SELECT field aliases and table aliases parse cleanly.
 * Returns zero on success and one on failure.
 */
static int test_select_alias_parse(void)
{
    sql_statement statement;

    if (sql_parse("SELECT p.name AS person, p.age years FROM people AS p "
        "WHERE p.age >= 18;", &statement) != 0) {
        return 1;
    }

    if (statement.type != sql_statement_select
        || strcmp(statement.name, "people") != 0
        || strcmp(stmt_from_alias(&statement), "p") != 0
        || stmt_select_count(&statement) != 2) {
        return 1;
    }

    if (strcmp(stmt_select_items(&statement)[0].column.qualifier, "p") != 0
        || strcmp(stmt_select_items(&statement)[0].column.name, "name") != 0
        || strcmp(stmt_select_items(&statement)[0].alias, "person") != 0) {
        return 1;
    }

    if (strcmp(stmt_select_items(&statement)[1].column.qualifier, "p") != 0
        || strcmp(stmt_select_items(&statement)[1].column.name, "age") != 0
        || strcmp(stmt_select_items(&statement)[1].alias, "years") != 0) {
        return 1;
    }

    if (check_simple_where(&statement, "age", sql_compare_greater_equal,
        sql_value_number, "18") != 0) {
        return 1;
    }

    return 0;
}

/*
 * Verifies that one simple INNER JOIN parses cleanly.
 * Returns zero on success and one on failure.
 */
static int test_select_join_parse(void)
{
    sql_statement statement;

    if (sql_parse("SELECT p.name, c.title FROM people AS p JOIN cities c "
        "ON p.city = c.code WHERE c.region = 'EU';", &statement) != 0) {
        return 1;
    }

    if (statement.type != sql_statement_select
        || strcmp(statement.name, "people") != 0
        || strcmp(stmt_from_alias(&statement), "p") != 0
        || stmt_join_count(&statement) != 1
        || strcmp(stmt_join_table_names(&statement)[0], "cities") != 0
        || strcmp(stmt_join_aliases(&statement)[0], "c") != 0) {
        return 1;
    }

    if (stmt_select_count(&statement) != 2
        || strcmp(stmt_select_items(&statement)[0].column.qualifier, "p") != 0
        || strcmp(stmt_select_items(&statement)[0].column.name, "name") != 0
        || strcmp(stmt_select_items(&statement)[1].column.qualifier, "c") != 0
        || strcmp(stmt_select_items(&statement)[1].column.name, "title") != 0) {
        return 1;
    }

    /* The WHERE tree now contains both the ON condition and the regular WHERE,
     * joined by an AND root. Verify the WHERE is active and non-trivial. */
    if (!statement.where.active || statement.where.node_count < 2) {
        return 1;
    }
    /* The ON and WHERE conditions must both be present somewhere. */
    {
        unsigned char n;
        int found_on = 0;
        int found = 0;
        for (n = 0; n < statement.where.node_count; n++) {
            const sql_where_node *wn = &statement.where_nodes[n];
            if (wn->type == sql_where_compare
                && strcmp(wn->qualifier, "p") == 0
                && strcmp(wn->column_name, "city") == 0
                && wn->operator == sql_compare_equal
                && wn->value_count == 1
                && operand_is_column(
                    &statement.where_values[wn->value_first],
                    "c", "code")) {
                found_on = 1;
            }
            if (wn->type == sql_where_compare
                && strcmp(wn->column_name, "region") == 0
                && wn->value_count == 1
                && operand_is_value(
                    &statement.where_values[wn->value_first],
                    sql_value_string, "EU")) {
                found = 1;
            }
        }
        if (!found_on || !found) {
            return 1;
        }
    }

    return 0;
}

/*
 * Verifies that comma-separated FROM table lists parse cleanly.
 * Returns zero on success and one on failure.
 */
static int test_select_table_list_parse(void)
{
    sql_statement statement;

    if (sql_parse("SELECT p.name, c.title FROM people p, cities c "
        "WHERE p.city = c.code AND c.region = 'EU';", &statement) != 0) {
        return 1;
    }

    if (statement.type != sql_statement_select
        || strcmp(statement.name, "people") != 0
        || strcmp(stmt_from_alias(&statement), "p") != 0
        || stmt_join_count(&statement) != 1
        || strcmp(stmt_join_table_names(&statement)[0], "cities") != 0
        || strcmp(stmt_join_aliases(&statement)[0], "c") != 0) {
        return 1;
    }

    if (stmt_select_count(&statement) != 2
        || strcmp(stmt_select_items(&statement)[0].column.qualifier, "p") != 0
        || strcmp(stmt_select_items(&statement)[1].column.qualifier, "c") != 0) {
        return 1;
    }

    return 0;
}

/*
 * Verifies that several WHERE predicates can be chained together.
 * Returns zero on success and one on failure.
 */
static int test_select_long_where_parse(void)
{
    sql_statement statement;
    unsigned char n;
    unsigned char compare_count;

    if (sql_parse("SELECT name FROM people WHERE a = 1 AND b = 2 "
        "AND c = 3 AND d = 4 AND e = 5 AND f = 6 AND g = 7 "
        "AND h = 8 AND i = 9;", &statement) != 0) {
        return 1;
    }

    if (!statement.where.active || statement.where.value_count != 9
        || statement.where.node_count != 17) {
        return 1;
    }

    compare_count = 0;
    for (n = 0; n < statement.where.node_count; n++) {
        if (statement.where_nodes[n].type == sql_where_compare) {
            compare_count++;
        }
    }

    if (compare_count != 9) {
        return 1;
    }

    return 0;
}

/*
 * Verifies that bounded multi-join SELECT statements parse cleanly.
 * Returns zero on success and one on failure.
 */
static int test_select_multi_join_parse(void)
{
    sql_statement statement;

    if (sql_parse("SELECT p.name, c.title, r.name FROM people p "
        "JOIN cities c ON p.city = c.code "
        "JOIN regions r ON c.region = r.code "
        "WHERE r.zone = 'west';", &statement) != 0) {
        return 1;
    }

    if (statement.type != sql_statement_select
        || strcmp(statement.name, "people") != 0
        || strcmp(stmt_from_alias(&statement), "p") != 0
        || stmt_join_count(&statement) != 2) {
        return 1;
    }

    if (strcmp(stmt_join_table_names(&statement)[0], "cities") != 0
        || strcmp(stmt_join_aliases(&statement)[0], "c") != 0
        || strcmp(stmt_join_table_names(&statement)[1], "regions") != 0
        || strcmp(stmt_join_aliases(&statement)[1], "r") != 0) {
        return 1;
    }

    if (stmt_select_count(&statement) != 3
        || strcmp(stmt_select_items(&statement)[0].column.qualifier, "p") != 0
        || strcmp(stmt_select_items(&statement)[1].column.qualifier, "c") != 0
        || strcmp(stmt_select_items(&statement)[2].column.qualifier, "r") != 0) {
        return 1;
    }

    {
        unsigned char n;
        int found_city = 0;
        int found_region = 0;

        for (n = 0; n < statement.where.node_count; n++) {
            const sql_where_node *wn = &statement.where_nodes[n];

            if (wn->type != sql_where_compare || wn->value_count != 1) {
                continue;
            }
            if (strcmp(wn->qualifier, "p") == 0
                && strcmp(wn->column_name, "city") == 0
                && wn->operator == sql_compare_equal
                && operand_is_column(
                    &statement.where_values[wn->value_first],
                    "c", "code")) {
                found_city = 1;
            }
            if (strcmp(wn->qualifier, "c") == 0
                && strcmp(wn->column_name, "region") == 0
                && wn->operator == sql_compare_equal
                && operand_is_column(
                    &statement.where_values[wn->value_first],
                    "r", "code")) {
                found_region = 1;
            }
        }
        if (!found_city || !found_region) {
            return 1;
        }
    }

    return 0;
}

/*
 * Verifies that SELECT rejects more joins than the fixed plan supports.
 * Returns zero on success and one on failure.
 */
static int test_select_too_many_joins_parse(void)
{
    sql_statement statement;

    if (sql_parse("SELECT p.name FROM people p "
        "JOIN cities c ON p.city = c.code "
        "JOIN regions r ON c.region = r.code "
        "JOIN zones z ON r.zone = z.code "
        "JOIN sectors s ON z.sector = s.code;", &statement) == 0) {
        return 1;
    }

    return 0;
}

/*
 * Verifies that scalar projection functions parse cleanly.
 * Returns zero on success and one on failure.
 */
static int test_select_trim_parse(void)
{
    sql_statement statement;

    if (sql_parse("SELECT TRIM(name) AS clean FROM people;",
        &statement) != 0) {
        return 1;
    }

    if (statement.type != sql_statement_select
        || stmt_select_count(&statement) != 1
        || stmt_select_items(&statement)[0].function != sql_function_trim
        || strcmp(stmt_select_items(&statement)[0].column.name, "name") != 0
        || strcmp(stmt_select_items(&statement)[0].alias, "clean") != 0) {
        return 1;
    }

    return 0;
}

/*
 * Verifies that DISTINCT and the broader aggregate family parse cleanly.
 * Returns zero on success and one on failure.
 */
static int test_select_distinct_aggregate_parse(void)
{
    sql_statement statement;

    if (sql_parse("SELECT DISTINCT city, COUNT(*) AS total, MIN(age) min_age, "
        "SUM(age) sum_age FROM people GROUP BY city;", &statement) != 0) {
        return 1;
    }

    if (statement.type != sql_statement_select
        || !stmt_select_distinct(&statement)
        || !stmt_select_has_aggregate(&statement)
        || stmt_select_count(&statement) != 4
        || stmt_group_count(&statement) != 1) {
        return 1;
    }

    if (stmt_select_items(&statement)[0].function != sql_function_none
        || stmt_select_items(&statement)[1].function != sql_function_count
        || !stmt_select_items(&statement)[1].argument_is_star
        || strcmp(stmt_select_items(&statement)[1].alias, "total") != 0
        || stmt_select_items(&statement)[2].function != sql_function_min
        || strcmp(stmt_select_items(&statement)[2].column.name, "age") != 0
        || stmt_select_items(&statement)[3].function != sql_function_sum
        || strcmp(stmt_select_items(&statement)[3].alias, "sum_age") != 0) {
        return 1;
    }

    return 0;
}

/*
 * Verifies that GROUP BY, HAVING, and aggregate functions parse cleanly.
 * Returns zero on success and one on failure.
 */
static int test_select_group_parse(void)
{
    sql_statement statement;
    const sql_where_node *having_root;

    if (sql_parse("SELECT city, MAX(age) AS max_age, AVG(age) avg_age "
        "FROM people GROUP BY city HAVING max_age > 18;",
        &statement) != 0) {
        return 1;
    }

    if (statement.type != sql_statement_select
        || !stmt_select_has_aggregate(&statement)
        || stmt_select_count(&statement) != 3
        || stmt_group_count(&statement) != 1) {
        return 1;
    }

    if (stmt_select_items(&statement)[0].function != sql_function_none
        || strcmp(stmt_select_items(&statement)[0].column.name, "city") != 0
        || stmt_select_items(&statement)[1].function != sql_function_max
        || strcmp(stmt_select_items(&statement)[1].column.name, "age") != 0
        || strcmp(stmt_select_items(&statement)[1].alias, "max_age") != 0
        || stmt_select_items(&statement)[2].function != sql_function_avg
        || strcmp(stmt_select_items(&statement)[2].column.name, "age") != 0
        || strcmp(stmt_select_items(&statement)[2].alias, "avg_age") != 0) {
        return 1;
    }

    if (strcmp(stmt_group_items(&statement)[0].name, "city") != 0) {
        return 1;
    }

    if (!statement.having.active || statement.having.node_count != 1
        || statement.having.value_count != 1) {
        return 1;
    }
    having_root = &statement_having_nodes(&statement)[statement.having.root];
    if (having_root->type != sql_where_compare
        || strcmp(having_root->column_name, "max_age") != 0
        || having_root->operator != sql_compare_greater
        || !operand_is_value(
            &statement_having_values(&statement)[having_root->value_first],
            sql_value_number, "18")) {
        return 1;
    }

    return 0;
}

/*
 * Verifies that string WHERE values are parsed as strings.
 * Returns zero on success and one on failure.
 */
static int test_select_string_where_parse(void)
{
    sql_statement statement;

    if (sql_parse("SELECT name FROM people WHERE city = 'London';",
        &statement) != 0) {
        return 1;
    }

    if (check_simple_where(&statement, "city", sql_compare_equal,
        sql_value_string, "London") != 0) {
        return 1;
    }

    return 0;
}

/*
 * Verifies that unqualified right-hand column predicates stay
 * structured as column references instead of string literals.
 */
static int test_select_column_compare_parse(void)
{
    sql_statement statement;
    const sql_where_node *node;

    if (sql_parse("SELECT name FROM people WHERE age = age;",
        &statement) != 0) {
        return 1;
    }

    node = where_root_node(&statement);
    if (!node || node->type != sql_where_compare
        || strcmp(node->column_name, "age") != 0
        || node->operator != sql_compare_equal
        || node->value_count != 1
        || statement.where_values[node->value_first].kind
            != sql_predicate_operand_column
        || statement.where_values[node->value_first].data.column.qualifier[0]
            != '\0'
        || strcmp(statement.where_values[node->value_first]
            .data.column.name, "age") != 0) {
        return 1;
    }

    return 0;
}

/*
 * Verifies that AND, OR, and IN parse into the expected predicate tree.
 * Returns zero on success and one on failure.
 */
static int test_select_logic_parse(void)
{
    sql_statement statement;
    const sql_where_node *root;
    const sql_where_node *left;
    const sql_where_node *right;
    const sql_where_node *right_left;
    const sql_where_node *right_right;

    if (sql_parse("SELECT name FROM people WHERE city = 'LON' OR age IN "
        "(18, 21) AND name = 'amy';", &statement) != 0) {
        return 1;
    }

    if (!statement.where.active || statement.where.node_count != 5
        || statement.where.value_count != 4) {
        return 1;
    }

    root = where_root_node(&statement);
    if (!root || root->type != sql_where_or) {
        return 1;
    }

    left = &statement.where_nodes[root->left];
    right = &statement.where_nodes[root->right];
    if (left->type != sql_where_compare || right->type != sql_where_and) {
        return 1;
    }

    if (strcmp(left->column_name, "city") != 0
        || left->operator != sql_compare_equal
        || !operand_is_value(&statement.where_values[left->value_first],
            sql_value_string, "LON")) {
        return 1;
    }

    right_left = &statement.where_nodes[right->left];
    right_right = &statement.where_nodes[right->right];
    if (right_left->type != sql_where_in
        || strcmp(right_left->column_name, "age") != 0
        || right_left->value_count != 2
        || !operand_is_value(
            &statement.where_values[right_left->value_first],
            sql_value_number, "18")
        || !operand_is_value(
            &statement.where_values[right_left->value_first + 1],
            sql_value_number, "21")) {
        return 1;
    }
    if (right_right->type != sql_where_compare
        || strcmp(right_right->column_name, "name") != 0
        || right_right->operator != sql_compare_equal
        || !operand_is_value(
            &statement.where_values[right_right->value_first],
            sql_value_string, "amy")) {
        return 1;
    }

    return 0;
}

/*
 * Verifies that NOT, BETWEEN, and LIKE parse into the expected tree.
 * Returns zero on success and one on failure.
 */
static int test_select_extended_predicate_parse(void)
{
    sql_statement statement;
    const sql_where_node *root;
    const sql_where_node *left;
    const sql_where_node *right;
    const sql_where_node *between_left;
    const sql_where_node *between_right;

    if (sql_parse("SELECT name FROM people WHERE NOT city LIKE 'N%' "
        "AND age BETWEEN 18 AND 24;", &statement) != 0) {
        return 1;
    }

    if (!statement.where.active || statement.where.node_count != 6
        || statement.where.value_count != 3) {
        return 1;
    }

    root = where_root_node(&statement);
    if (!root || root->type != sql_where_and) {
        return 1;
    }

    left = &statement.where_nodes[root->left];
    right = &statement.where_nodes[root->right];
    if (left->type != sql_where_not || right->type != sql_where_and) {
        return 1;
    }
    if (statement.where_nodes[left->left].type != sql_where_like
        || strcmp(statement.where_nodes[left->left].column_name, "city") != 0
        || !operand_is_value(
            &statement.where_values[
                statement.where_nodes[left->left].value_first],
            sql_value_string, "N%")) {
        return 1;
    }

    between_left = &statement.where_nodes[right->left];
    between_right = &statement.where_nodes[right->right];
    if (between_left->type != sql_where_compare
        || between_left->operator != sql_compare_greater_equal
        || !operand_is_value(
            &statement.where_values[between_left->value_first],
            sql_value_number, "18")
        || between_right->type != sql_where_compare
        || between_right->operator != sql_compare_less_equal
        || !operand_is_value(
            &statement.where_values[between_right->value_first],
            sql_value_number, "24")) {
        return 1;
    }

    return 0;
}

/*
 * Verifies that IS NULL and IS NOT NULL parse into dedicated nodes.
 * Returns zero on success and one on failure.
 */
static int test_select_null_predicate_parse(void)
{
    sql_statement statement;
    const sql_where_node *root;
    const sql_where_node *left;
    const sql_where_node *right;

    if (sql_parse("SELECT name FROM people WHERE city IS NULL "
        "OR age IS NOT NULL;", &statement) != 0) {
        return 1;
    }

    root = where_root_node(&statement);
    if (!root || root->type != sql_where_or
        || statement.where.node_count != 3
        || statement.where.value_count != 0) {
        return 1;
    }

    left = &statement.where_nodes[root->left];
    right = &statement.where_nodes[root->right];
    if (left->type != sql_where_is_null
        || strcmp(left->column_name, "city") != 0
        || left->operator != sql_compare_equal
        || right->type != sql_where_is_null
        || strcmp(right->column_name, "age") != 0
        || right->operator != sql_compare_not_equal) {
        return 1;
    }

    return 0;
}

/*
 * Verifies that EXISTS, IN (SELECT ...), and quantified comparisons
 * parse into dedicated bounded predicate-subquery nodes.
 * Returns zero on success and one on failure.
 */
static int test_select_subquery_predicate_parse(void)
{
    sql_statement statement;
    const sql_where_node *root;
    const sql_where_node *left;
    const sql_where_node *right;

    if (sql_parse("SELECT name FROM people WHERE EXISTS "
        "(SELECT code FROM cities) AND city IN "
        "(SELECT code FROM cities) AND age >= ALL "
        "(SELECT age FROM people);", &statement) != 0) {
        return 1;
    }

    if (statement.predicate_subquery_count != 2
        || strcmp(statement.predicate_subqueries[0],
            "SELECT code FROM cities") != 0
        || strcmp(statement.predicate_subqueries[1],
            "SELECT age FROM people") != 0) {
        return 1;
    }

    root = where_root_node(&statement);
    if (!root || root->type != sql_where_and) {
        return 1;
    }

    left = &statement.where_nodes[root->left];
    right = &statement.where_nodes[root->right];
    if (left->type != sql_where_and
        || right->type != sql_where_quantified) {
        return 1;
    }
    if (statement.where_nodes[left->left].type != sql_where_exists
        || statement.where_nodes[left->left].subquery_index != 0
        || statement.where_nodes[left->right].type != sql_where_quantified
        || statement.where_nodes[left->right].operator != sql_compare_equal
        || statement.where_nodes[left->right].quantifier
            != sql_quantifier_any
        || statement.where_nodes[left->right].subquery_index != 0) {
        return 1;
    }
    if (strcmp(statement.where_nodes[left->right].column_name, "city") != 0
        || strcmp(right->column_name, "age") != 0
        || right->operator != sql_compare_greater_equal
        || right->quantifier != sql_quantifier_all
        || right->subquery_index != 1) {
        return 1;
    }

    return 0;
}

/*
 * Verifies that INSERT ... VALUES parses into the expected structure.
 * Returns zero on success and one on failure.
 */
static int test_insert_parse(void)
{
    sql_statement statement;

    if (sql_parse("INSERT INTO people VALUES ('alice', 18, active);",
        &statement) != 0) {
        return 1;
    }

    if (statement.type != sql_statement_insert) {
        return 1;
    }

    if (strcmp(statement.name, "people") != 0) {
        return 1;
    }

    if (stmt_assignment_count(&statement) != 3) {
        return 1;
    }

    if (stmt_assignments(&statement)[0].column_name[0] != '\0'
        || stmt_assignments(&statement)[0].value.type != sql_value_string
        || strcmp(stmt_assignments(&statement)[0].value.text, "alice") != 0) {
        return 1;
    }

    if (stmt_assignments(&statement)[1].column_name[0] != '\0'
        || stmt_assignments(&statement)[1].value.type != sql_value_number
        || strcmp(stmt_assignments(&statement)[1].value.text, "18") != 0) {
        return 1;
    }

    if (stmt_assignments(&statement)[2].column_name[0] != '\0'
        || stmt_assignments(&statement)[2].value.type
            != sql_value_identifier
        || strcmp(stmt_assignments(&statement)[2].value.text, "active") != 0) {
        return 1;
    }

    return 0;
}

/*
 * Verifies that INSERT column lists parse into named assignments.
 * Returns zero on success and one on failure.
 */
static int test_insert_column_list_parse(void)
{
    sql_statement statement;

    if (sql_parse("INSERT INTO people (city, name) VALUES ('LON', 'zoe');",
        &statement) != 0) {
        return 1;
    }

    if (statement.type != sql_statement_insert
        || strcmp(statement.name, "people") != 0
        || stmt_assignment_count(&statement) != 2) {
        return 1;
    }

    if (strcmp(stmt_assignments(&statement)[0].column_name, "city") != 0
        || stmt_assignments(&statement)[0].value.type != sql_value_string
        || strcmp(stmt_assignments(&statement)[0].value.text, "LON") != 0
        || strcmp(stmt_assignments(&statement)[1].column_name, "name") != 0
        || strcmp(stmt_assignments(&statement)[1].value.text, "zoe") != 0) {
        return 1;
    }

    return 0;
}

/*
 * Verifies that UPDATE accepts NULL assignments.
 * Returns zero on success and one on failure.
 */
static int test_update_null_parse(void)
{
    sql_statement statement;

    if (sql_parse("UPDATE people SET city = NULL WHERE name = 'zoe';",
        &statement) != 0) {
        return 1;
    }

    if (statement.type != sql_statement_update
        || stmt_assignment_count(&statement) != 1
        || strcmp(stmt_assignments(&statement)[0].column_name, "city") != 0
        || stmt_assignments(&statement)[0].value.type != sql_value_null) {
        return 1;
    }

    return 0;
}

/*
 * Verifies that UPDATE ... SET ... WHERE parses cleanly.
 * Returns zero on success and one on failure.
 */
static int test_update_parse(void)
{
    sql_statement statement;

    if (sql_parse("UPDATE people SET name = 'alice', age = 19 "
        "WHERE age = 18;", &statement) != 0) {
        return 1;
    }

    if (statement.type != sql_statement_update) {
        return 1;
    }

    if (strcmp(statement.name, "people") != 0) {
        return 1;
    }

    if (stmt_assignment_count(&statement) != 2) {
        return 1;
    }

    if (strcmp(stmt_assignments(&statement)[0].column_name, "name") != 0) {
        return 1;
    }

    if (stmt_assignments(&statement)[0].value.type != sql_value_string
        || strcmp(stmt_assignments(&statement)[0].value.text, "alice") != 0) {
        return 1;
    }

    if (strcmp(stmt_assignments(&statement)[1].column_name, "age") != 0) {
        return 1;
    }

    if (stmt_assignments(&statement)[1].value.type != sql_value_number
        || strcmp(stmt_assignments(&statement)[1].value.text, "19") != 0) {
        return 1;
    }

    if (check_simple_where(&statement, "age", sql_compare_equal,
        sql_value_number, "18") != 0) {
        return 1;
    }

    return 0;
}

/*
 * Verifies that DELETE ... WHERE parses into the expected structure.
 * Returns zero on success and one on failure.
 */
static int test_delete_parse(void)
{
    sql_statement statement;

    if (sql_parse("DELETE FROM people WHERE age < 18;",
        &statement) != 0) {
        return 1;
    }

    if (statement.type != sql_statement_delete) {
        return 1;
    }

    if (strcmp(statement.name, "people") != 0) {
        return 1;
    }

    if (check_simple_where(&statement, "age", sql_compare_less,
        sql_value_number, "18") != 0) {
        return 1;
    }

    return 0;
}

/*
 * Verifies that invalid SQL is rejected cleanly.
 * Returns zero on success and one on failure.
 */
static int test_invalid_parse(void)
{
    sql_statement statement;

    if (sql_parse("CREATE demo;", &statement) == 0) {
        return 1;
    }

    if (sql_parse("SHOW DATABASE demo;", &statement) == 0) {
        return 1;
    }

    if (sql_parse("CREATE DATABASE demo", &statement) == 0) {
        return 1;
    }

    if (sql_parse("CREATE TABLE people (name TEXT);", &statement) == 0) {
        return 1;
    }

    if (sql_parse("CREATE TABLE people (value NUMERIC);",
        &statement) == 0) {
        return 1;
    }

    if (sql_parse("CREATE INDEX idx people (name);", &statement) == 0) {
        return 1;
    }

    if (sql_parse("CREATE INDEX idx ON people ();", &statement) == 0) {
        return 1;
    }

    if (sql_parse("CREATE INDEX idx ON people (name,);",
        &statement) == 0) {
        return 1;
    }

    if (sql_parse("CREATE VIEW adults AS SELECT FROM people;",
        &statement) == 0) {
        return 1;
    }

    if (sql_parse("SELECT FROM people;", &statement) == 0) {
        return 1;
    }

    if (sql_parse("SELECT name, FROM people;", &statement) == 0) {
        return 1;
    }

    if (sql_parse("SELECT name people;", &statement) == 0) {
        return 1;
    }

    if (sql_parse("SELECT name FROM people WHERE;", &statement) == 0) {
        return 1;
    }

    if (sql_parse("SELECT name FROM people WHERE age;",
        &statement) == 0) {
        return 1;
    }

    if (sql_parse("SELECT name FROM people WHERE age IN ();",
        &statement) == 0) {
        return 1;
    }

    if (sql_parse("SELECT name FROM people WHERE age = 1 AND;",
        &statement) == 0) {
        return 1;
    }

    if (sql_parse("SELECT name FROM people WHERE OR age = 1;",
        &statement) == 0) {
        return 1;
    }

    if (sql_parse("INSERT people VALUES (1);", &statement) == 0) {
        return 1;
    }

    if (sql_parse("INSERT INTO people VALUES ();", &statement) == 0) {
        return 1;
    }

    if (sql_parse("INSERT INTO people VALUES (1,);", &statement) == 0) {
        return 1;
    }

    if (sql_parse("INSERT INTO people (name, city) VALUES ('zoe');",
        &statement) == 0) {
        return 1;
    }

    if (sql_parse("INSERT INTO people (name, name) VALUES ('zoe', 'amy');",
        &statement) == 0) {
        return 1;
    }

    if (sql_parse("UPDATE people name = 'alice';", &statement) == 0) {
        return 1;
    }

    if (sql_parse("UPDATE people SET WHERE age = 1;", &statement) == 0) {
        return 1;
    }

    if (sql_parse("UPDATE people SET name 'alice';", &statement) == 0) {
        return 1;
    }

    if (sql_parse("DELETE people WHERE age = 1;", &statement) == 0) {
        return 1;
    }

    if (sql_parse("DELETE FROM WHERE age = 1;", &statement) == 0) {
        return 1;
    }

    if (sql_parse("SELECT city, MAX(age) FROM people;", &statement) == 0) {
        return 1;
    }

    return 0;
}

int main(void)
{
    if (test_create_database_parse() != 0) {
        printf("test_sql: create parse fail\n");
        return 1;
    }

    if (test_show_databases_parse() != 0) {
        printf("test_sql: show parse fail\n");
        return 1;
    }

    if (test_create_table_parse() != 0) {
        printf("test_sql: create table parse fail\n");
        return 1;
    }

    if (test_create_index_parse() != 0) {
        printf("test_sql: create index parse fail\n");
        return 1;
    }

    if (test_create_view_parse() != 0) {
        printf("test_sql: create view parse fail\n");
        return 1;
    }

    if (test_select_all_parse() != 0) {
        printf("test_sql: select all parse fail\n");
        return 1;
    }

    if (test_select_where_parse() != 0) {
        printf("test_sql: select where parse fail\n");
        return 1;
    }

    if (test_select_string_where_parse() != 0) {
        printf("test_sql: select string where parse fail\n");
        return 1;
    }

    if (test_select_column_compare_parse() != 0) {
        printf("test_sql: select column compare parse fail\n");
        return 1;
    }

    if (test_select_logic_parse() != 0) {
        printf("test_sql: select logic parse fail\n");
        return 1;
    }

    if (test_select_extended_predicate_parse() != 0) {
        printf("test_sql: select extended predicate parse fail\n");
        return 1;
    }

    if (test_select_null_predicate_parse() != 0) {
        printf("test_sql: select null predicate parse fail\n");
        return 1;
    }

    if (test_select_subquery_predicate_parse() != 0) {
        printf("test_sql: select subquery predicate parse fail\n");
        return 1;
    }

    if (test_select_alias_parse() != 0) {
        printf("test_sql: select alias parse fail\n");
        return 1;
    }

    if (test_select_join_parse() != 0) {
        printf("test_sql: select join parse fail\n");
        return 1;
    }

    if (test_select_table_list_parse() != 0) {
        printf("test_sql: select table list parse fail\n");
        return 1;
    }

    if (test_select_long_where_parse() != 0) {
        printf("test_sql: select long where parse fail\n");
        return 1;
    }

    if (test_select_multi_join_parse() != 0) {
        printf("test_sql: select multi join parse fail\n");
        return 1;
    }

    if (test_select_too_many_joins_parse() != 0) {
        printf("test_sql: select join limit parse fail\n");
        return 1;
    }

    if (test_select_trim_parse() != 0) {
        printf("test_sql: select trim parse fail\n");
        return 1;
    }

    if (test_select_distinct_aggregate_parse() != 0) {
        printf("test_sql: select distinct aggregate parse fail\n");
        return 1;
    }

    if (test_select_group_parse() != 0) {
        printf("test_sql: select group parse fail\n");
        return 1;
    }

    if (test_insert_parse() != 0) {
        printf("test_sql: insert parse fail\n");
        return 1;
    }

    if (test_insert_column_list_parse() != 0) {
        printf("test_sql: insert column list parse fail\n");
        return 1;
    }

    if (test_update_parse() != 0) {
        printf("test_sql: update parse fail\n");
        return 1;
    }

    if (test_update_null_parse() != 0) {
        printf("test_sql: update null parse fail\n");
        return 1;
    }

    if (test_delete_parse() != 0) {
        printf("test_sql: delete parse fail\n");
        return 1;
    }

    if (test_invalid_parse() != 0) {
        printf("test_sql: invalid parse fail\n");
        return 1;
    }

    printf("test_sql: ok\n");
    return 0;
}
