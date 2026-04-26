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

    if (sql_parse("SHOW DATABASES;", &statement) != 0) {
        return 1;
    }

    if (statement.type != sql_statement_show_databases) {
        return 1;
    }

    if (statement.name[0] != '\0') {
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

    if (strcmp(statement.select_names[0], "name") != 0) {
        return 1;
    }

    if (strcmp(statement.select_names[1], "age") != 0) {
        return 1;
    }

    if (!statement.where.active) {
        return 1;
    }

    if (strcmp(statement.where.column_name, "age") != 0) {
        return 1;
    }

    if (statement.where.operator != sql_compare_greater_equal) {
        return 1;
    }

    if (statement.where.value.type != sql_value_number) {
        return 1;
    }

    if (strcmp(statement.where.value.text, "18") != 0) {
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

    if (!statement.where.active) {
        return 1;
    }

    if (statement.where.operator != sql_compare_equal) {
        return 1;
    }

    if (statement.where.value.type != sql_value_string) {
        return 1;
    }

    if (strcmp(statement.where.value.text, "London") != 0) {
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

    if (!statement.where.active
        || strcmp(statement.where.column_name, "age") != 0
        || statement.where.operator != sql_compare_equal
        || strcmp(statement.where.value.text, "18") != 0) {
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

    if (!statement.where.active) {
        return 1;
    }

    if (strcmp(statement.where.column_name, "age") != 0) {
        return 1;
    }

    if (statement.where.operator != sql_compare_less) {
        return 1;
    }

    if (statement.where.value.type != sql_value_number
        || strcmp(statement.where.value.text, "18") != 0) {
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
