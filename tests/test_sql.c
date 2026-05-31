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

static const sql_where_node *where_root_node(const sql_statement *statement)
{
    if (!statement->where.active
        || statement->where.root == (unsigned char)sql_where_nil
        || statement->where.root >= statement->where.node_count) {
        return NULL;
    }

    return &statement->where_nodes[statement->where.root];
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

    if (statement->where_values[node->value_first].type != value_type
        || strcmp(statement->where_values[node->value_first].text,
            value_text) != 0) {
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

    if (!statement.select_all) {
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

    if (statement.column_count != 4) {
        return 1;
    }

    if (statement.columns[0].dbf_type != 'C'
        || statement.columns[0].length != 8) {
        return 1;
    }

    if (statement.columns[1].dbf_type != 'N'
        || statement.columns[1].length != 3
        || statement.columns[1].decimals != 0) {
        return 1;
    }

    if (statement.columns[2].dbf_type != 'D'
        || statement.columns[2].length != 8) {
        return 1;
    }

    if (statement.columns[3].dbf_type != 'L'
        || statement.columns[3].length != 1) {
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

    if (!statement.create_index_unique || statement.key_count != 2) {
        return 1;
    }

    if (strcmp(statement.key_names[0], "city") != 0
        || strcmp(statement.key_names[1], "name") != 0) {
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

    if (!statement.select_all || statement.select_count != 0) {
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

    if (statement.select_all || statement.select_count != 2) {
        return 1;
    }

    if (strcmp(statement.select_items[0].column.name, "name") != 0
        || statement.select_items[0].alias[0] != '\0'
        || statement.select_items[0].column.qualifier[0] != '\0') {
        return 1;
    }

    if (strcmp(statement.select_items[1].column.name, "age") != 0
        || statement.select_items[1].alias[0] != '\0'
        || statement.select_items[1].column.qualifier[0] != '\0') {
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
        || strcmp(statement.from_alias, "p") != 0
        || statement.select_count != 2) {
        return 1;
    }

    if (strcmp(statement.select_items[0].column.qualifier, "p") != 0
        || strcmp(statement.select_items[0].column.name, "name") != 0
        || strcmp(statement.select_items[0].alias, "person") != 0) {
        return 1;
    }

    if (strcmp(statement.select_items[1].column.qualifier, "p") != 0
        || strcmp(statement.select_items[1].column.name, "age") != 0
        || strcmp(statement.select_items[1].alias, "years") != 0) {
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
        || strcmp(statement.from_alias, "p") != 0
        || !statement.join_active
        || strcmp(statement.join_table_name, "cities") != 0
        || strcmp(statement.join_alias, "c") != 0) {
        return 1;
    }

    /* join_left/right are temporary fields holding the ON condition. */
    if (strcmp(statement.join_left.qualifier, "p") != 0
        || strcmp(statement.join_left.name, "city") != 0
        || strcmp(statement.join_right.qualifier, "c") != 0
        || strcmp(statement.join_right.name, "code") != 0) {
        return 1;
    }

    if (statement.select_count != 2
        || strcmp(statement.select_items[0].column.qualifier, "p") != 0
        || strcmp(statement.select_items[0].column.name, "name") != 0
        || strcmp(statement.select_items[1].column.qualifier, "c") != 0
        || strcmp(statement.select_items[1].column.name, "title") != 0) {
        return 1;
    }

    /* The WHERE tree now contains both the ON condition and the regular WHERE,
     * joined by an AND root. Verify the WHERE is active and non-trivial. */
    if (!statement.where.active || statement.where.node_count < 2) {
        return 1;
    }
    /* The region condition must be present somewhere in the tree. */
    {
        unsigned char n;
        int found = 0;
        for (n = 0; n < statement.where.node_count && !found; n++) {
            const sql_where_node *wn = &statement.where_nodes[n];
            if (wn->type == sql_where_compare
                && strcmp(wn->column_name, "region") == 0
                && wn->value_count == 1
                && statement.where_values[wn->value_first].type
                    == sql_value_string
                && strcmp(statement.where_values[wn->value_first].text,
                    "EU") == 0) {
                found = 1;
            }
        }
        if (!found) return 1;
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
        || strcmp(statement.where_values[left->value_first].text, "LON")
            != 0) {
        return 1;
    }

    right_left = &statement.where_nodes[right->left];
    right_right = &statement.where_nodes[right->right];
    if (right_left->type != sql_where_in
        || strcmp(right_left->column_name, "age") != 0
        || right_left->value_count != 2
        || strcmp(statement.where_values[right_left->value_first].text, "18")
            != 0
        || strcmp(statement.where_values[right_left->value_first + 1].text,
            "21") != 0) {
        return 1;
    }
    if (right_right->type != sql_where_compare
        || strcmp(right_right->column_name, "name") != 0
        || right_right->operator != sql_compare_equal
        || strcmp(statement.where_values[right_right->value_first].text,
            "amy") != 0) {
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

    if (statement.value_count != 3) {
        return 1;
    }

    if (statement.values[0].type != sql_value_string
        || strcmp(statement.values[0].text, "alice") != 0) {
        return 1;
    }

    if (statement.values[1].type != sql_value_number
        || strcmp(statement.values[1].text, "18") != 0) {
        return 1;
    }

    if (statement.values[2].type != sql_value_identifier
        || strcmp(statement.values[2].text, "active") != 0) {
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

    if (statement.assignment_count != 2) {
        return 1;
    }

    if (strcmp(statement.assignments[0].column_name, "name") != 0) {
        return 1;
    }

    if (statement.assignments[0].value.type != sql_value_string
        || strcmp(statement.assignments[0].value.text, "alice") != 0) {
        return 1;
    }

    if (strcmp(statement.assignments[1].column_name, "age") != 0) {
        return 1;
    }

    if (statement.assignments[1].value.type != sql_value_number
        || strcmp(statement.assignments[1].value.text, "19") != 0) {
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

    if (test_select_logic_parse() != 0) {
        printf("test_sql: select logic parse fail\n");
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

    if (test_insert_parse() != 0) {
        printf("test_sql: insert parse fail\n");
        return 1;
    }

    if (test_update_parse() != 0) {
        printf("test_sql: update parse fail\n");
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
