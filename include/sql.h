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

typedef enum sql_statement_type {
    sql_statement_invalid = 0,
    sql_statement_create_database,
    sql_statement_show_databases
} sql_statement_type;

typedef struct sql_statement {
    sql_statement_type type;
    char name[sql_name_size];
} sql_statement;

/*
 * Parses one SQL statement from text into a fixed output structure.
 * The parser currently supports CREATE DATABASE and SHOW DATABASES.
 * Returns zero on success and -1 on failure.
 */
int sql_parse(const char *text, sql_statement *statement);

#endif
