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

    if (test_invalid_parse() != 0) {
        printf("test_sql: invalid parse fail\n");
        return 1;
    }

    printf("test_sql: ok\n");
    return 0;
}
