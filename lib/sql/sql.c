/*
 * Implements a tiny SQL parser for the project command shell.
 * The code keeps the lexer and recursive descent parser very small
 * and recognizes only the statements needed by the current system.
 *
 * MIT License (see: LICENSE)
 * Copyright (C) 2026 tomaz stih
 */

#include "sql.h"

#include <ctype.h>

/*
 * Clears the output statement before parsing begins.
 */
static void sql_reset(sql_statement *statement)
{
    unsigned short index;

    statement->type = sql_statement_invalid;
    for (index = 0; index < sql_name_size; index++) {
        statement->name[index] = '\0';
    }
}

/*
 * Skips ASCII whitespace and returns the next text position.
 */
static const char *skip_space(const char *text)
{
    while (*text != '\0' && isspace((unsigned char)*text)) {
        text++;
    }

    return text;
}

/*
 * Compares one keyword without caring about ASCII letter case.
 */
static int keyword_matches(const char *text, const char *keyword)
{
    while (*keyword != '\0') {
        if (toupper((unsigned char)*text)
            != toupper((unsigned char)*keyword)) {
            return 0;
        }
        text++;
        keyword++;
    }

    if (isalnum((unsigned char)*text) || *text == '_') {
        return 0;
    }

    return 1;
}

/*
 * Reads one SQL identifier into a fixed output buffer.
 */
static const char *read_identifier(const char *text, char *name)
{
    unsigned short index;

    if (!isalpha((unsigned char)*text) && *text != '_') {
        return (const char *)0;
    }

    index = 0;
    while (isalnum((unsigned char)*text) || *text == '_') {
        if (index + 1 >= sql_name_size) {
            return (const char *)0;
        }
        name[index] = *text;
        index++;
        text++;
    }

    name[index] = '\0';
    return text;
}

/*
 * Parses CREATE DATABASE name;
 */
static const char *parse_create_database(const char *text,
    sql_statement *statement)
{
    text = skip_space(text);
    if (!keyword_matches(text, "CREATE")) {
        return (const char *)0;
    }

    text += 6;
    text = skip_space(text);
    if (!keyword_matches(text, "DATABASE")) {
        return (const char *)0;
    }

    text += 8;
    text = skip_space(text);
    text = read_identifier(text, statement->name);
    if (text == (const char *)0) {
        return (const char *)0;
    }

    statement->type = sql_statement_create_database;
    return text;
}

/*
 * Parses SHOW DATABASES;
 */
static const char *parse_show_databases(const char *text,
    sql_statement *statement)
{
    text = skip_space(text);
    if (!keyword_matches(text, "SHOW")) {
        return (const char *)0;
    }

    text += 4;
    text = skip_space(text);
    if (!keyword_matches(text, "DATABASES")) {
        return (const char *)0;
    }

    text += 9;
    statement->type = sql_statement_show_databases;
    return text;
}

int sql_parse(const char *text, sql_statement *statement)
{
    const char *next;

    sql_reset(statement);

    next = parse_create_database(text, statement);
    if (next == (const char *)0) {
        next = parse_show_databases(text, statement);
    }

    if (next == (const char *)0) {
        return -1;
    }

    next = skip_space(next);
    if (*next != ';') {
        return -1;
    }

    next++;
    next = skip_space(next);
    if (*next != '\0') {
        return -1;
    }

    return 0;
}
