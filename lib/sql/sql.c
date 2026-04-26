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
    unsigned short column_index;
    unsigned short value_index;
    unsigned short assignment_index;

    statement->type = sql_statement_invalid;
    for (index = 0; index < sql_name_size; index++) {
        statement->name[index] = '\0';
    }

    statement->column_count = 0;
    for (column_index = 0; column_index < sql_max_columns;
        column_index++) {
        statement->columns[column_index].name[0] = '\0';
        statement->columns[column_index].dbf_type = '\0';
        statement->columns[column_index].length = 0;
        statement->columns[column_index].decimals = 0;
    }

    statement->select_all = 0;
    statement->select_count = 0;
    for (column_index = 0; column_index < sql_max_columns;
        column_index++) {
        for (index = 0; index < sql_name_size; index++) {
            statement->select_names[column_index][index] = '\0';
        }
    }

    statement->value_count = 0;
    for (column_index = 0; column_index < sql_max_columns;
        column_index++) {
        statement->values[column_index].type = sql_value_none;
        for (value_index = 0; value_index < sql_value_size;
            value_index++) {
            statement->values[column_index].text[value_index] = '\0';
        }
    }

    statement->assignment_count = 0;
    for (assignment_index = 0; assignment_index < sql_max_columns;
        assignment_index++) {
        for (index = 0; index < sql_name_size; index++) {
            statement->assignments[assignment_index]
                .column_name[index] = '\0';
        }
        statement->assignments[assignment_index].value.type
            = sql_value_none;
        for (value_index = 0; value_index < sql_value_size;
            value_index++) {
            statement->assignments[assignment_index]
                .value.text[value_index] = '\0';
        }
    }

    statement->where.active = 0;
    for (index = 0; index < sql_name_size; index++) {
        statement->where.column_name[index] = '\0';
    }
    statement->where.operator = sql_compare_invalid;
    statement->where.value.type = sql_value_none;
    for (value_index = 0; value_index < sql_value_size; value_index++) {
        statement->where.value.text[value_index] = '\0';
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
 * Reads one unsigned number from the SQL text.
 */
static const char *read_number(const char *text, unsigned short *value)
{
    unsigned short number;

    if (!isdigit((unsigned char)*text)) {
        return (const char *)0;
    }

    number = 0;
    while (isdigit((unsigned char)*text)) {
        number = (unsigned short)((number * 10)
            + (unsigned short)(*text - '0'));
        text++;
    }

    *value = number;
    return text;
}

/*
 * Reads one SQL comparison value into a fixed output buffer.
 */
static const char *read_value_text(const char *text, char *value,
    unsigned short size)
{
    unsigned short index;

    if (*text == '\'') {
        text++;
        index = 0;
        while (*text != '\0' && *text != '\'') {
            if (index + 1 >= size) {
                return (const char *)0;
            }
            value[index] = *text;
            index++;
            text++;
        }

        if (*text != '\'') {
            return (const char *)0;
        }

        value[index] = '\0';
        return text + 1;
    }

    if (isdigit((unsigned char)*text)) {
        index = 0;
        while (isdigit((unsigned char)*text)) {
            if (index + 1 >= size) {
                return (const char *)0;
            }
            value[index] = *text;
            index++;
            text++;
        }

        value[index] = '\0';
        return text;
    }

    return read_identifier(text, value);
}

/*
 * Parses one SQL value and records its inferred fixed output type.
 */
static const char *parse_value(const char *text, sql_value *value)
{
    text = skip_space(text);
    if (*text == '\'') {
        value->type = sql_value_string;
        return read_value_text(text, value->text, sql_value_size);
    }

    if (isdigit((unsigned char)*text)) {
        value->type = sql_value_number;
        return read_value_text(text, value->text, sql_value_size);
    }

    value->type = sql_value_identifier;
    return read_value_text(text, value->text, sql_value_size);
}

/*
 * Skips unsupported column constraints until comma or right paren.
 */
static const char *skip_constraints(const char *text)
{
    while (*text != '\0' && *text != ',' && *text != ')') {
        text++;
    }

    return text;
}

/*
 * Parses CHAR(n), CHARACTER(n), NUMERIC(n[,d]), DATE, or LOGICAL.
 */
static const char *parse_column_type(const char *text, sql_column *column)
{
    unsigned short length;
    unsigned short decimals;

    text = skip_space(text);
    if (keyword_matches(text, "CHAR")) {
        text += 4;
        text = skip_space(text);
        if (*text != '(') {
            return (const char *)0;
        }
        text++;
        text = skip_space(text);
        text = read_number(text, &length);
        if (text == (const char *)0 || length == 0 || length > 255) {
            return (const char *)0;
        }
        text = skip_space(text);
        if (*text != ')') {
            return (const char *)0;
        }
        column->dbf_type = 'C';
        column->length = (unsigned char)length;
        column->decimals = 0;
        return text + 1;
    }

    if (keyword_matches(text, "CHARACTER")) {
        text += 9;
        text = skip_space(text);
        if (*text != '(') {
            return (const char *)0;
        }
        text++;
        text = skip_space(text);
        text = read_number(text, &length);
        if (text == (const char *)0 || length == 0 || length > 255) {
            return (const char *)0;
        }
        text = skip_space(text);
        if (*text != ')') {
            return (const char *)0;
        }
        column->dbf_type = 'C';
        column->length = (unsigned char)length;
        column->decimals = 0;
        return text + 1;
    }

    if (keyword_matches(text, "NUMERIC")) {
        text += 7;
        text = skip_space(text);
        if (*text != '(') {
            return (const char *)0;
        }
        text++;
        text = skip_space(text);
        text = read_number(text, &length);
        if (text == (const char *)0 || length == 0 || length > 255) {
            return (const char *)0;
        }

        decimals = 0;
        text = skip_space(text);
        if (*text == ',') {
            text++;
            text = skip_space(text);
            text = read_number(text, &decimals);
            if (text == (const char *)0 || decimals > length) {
                return (const char *)0;
            }
        }

        text = skip_space(text);
        if (*text != ')') {
            return (const char *)0;
        }
        column->dbf_type = 'N';
        column->length = (unsigned char)length;
        column->decimals = (unsigned char)decimals;
        return text + 1;
    }

    if (keyword_matches(text, "DATE")) {
        column->dbf_type = 'D';
        column->length = 8;
        column->decimals = 0;
        return text + 4;
    }

    if (keyword_matches(text, "LOGICAL")) {
        column->dbf_type = 'L';
        column->length = 1;
        column->decimals = 0;
        return text + 7;
    }

    return (const char *)0;
}

/*
 * Parses one CREATE TABLE column definition.
 */
static const char *parse_column(const char *text, sql_column *column)
{
    text = skip_space(text);
    text = read_identifier(text, column->name);
    if (text == (const char *)0) {
        return (const char *)0;
    }

    text = skip_space(text);
    text = parse_column_type(text, column);
    if (text == (const char *)0) {
        return (const char *)0;
    }

    text = skip_space(text);
    return skip_constraints(text);
}

/*
 * Parses one SQL comparison operator used by WHERE.
 */
static const char *parse_compare_operator(const char *text,
    sql_compare_operator *operator)
{
    if (text[0] == '<' && text[1] == '>') {
        *operator = sql_compare_not_equal;
        return text + 2;
    }

    if (text[0] == '!' && text[1] == '=') {
        *operator = sql_compare_not_equal;
        return text + 2;
    }

    if (text[0] == '<' && text[1] == '=') {
        *operator = sql_compare_less_equal;
        return text + 2;
    }

    if (text[0] == '>' && text[1] == '=') {
        *operator = sql_compare_greater_equal;
        return text + 2;
    }

    if (text[0] == '=') {
        *operator = sql_compare_equal;
        return text + 1;
    }

    if (text[0] == '<') {
        *operator = sql_compare_less;
        return text + 1;
    }

    if (text[0] == '>') {
        *operator = sql_compare_greater;
        return text + 1;
    }

    return (const char *)0;
}

/*
 * Parses one WHERE comparison with a single column and value.
 */
static const char *parse_where_clause(const char *text, sql_where *where)
{
    text = skip_space(text);
    if (!keyword_matches(text, "WHERE")) {
        return text;
    }

    text += 5;
    text = skip_space(text);
    text = read_identifier(text, where->column_name);
    if (text == (const char *)0) {
        return (const char *)0;
    }

    text = skip_space(text);
    text = parse_compare_operator(text, &where->operator);
    if (text == (const char *)0) {
        return (const char *)0;
    }

    text = skip_space(text);
    text = parse_value(text, &where->value);
    if (text == (const char *)0) {
        return (const char *)0;
    }

    where->active = 1;
    return text;
}

/*
 * Parses one comma-separated list of SQL values inside parentheses.
 */
static const char *parse_value_list(const char *text, sql_statement *statement)
{
    const char *next;

    text = skip_space(text);
    if (*text != '(') {
        return (const char *)0;
    }
    text++;

    while (1) {
        if (statement->value_count >= sql_max_columns) {
            return (const char *)0;
        }

        next = parse_value(text, &statement->values[statement->value_count]);
        if (next == (const char *)0) {
            return (const char *)0;
        }

        statement->value_count++;
        text = skip_space(next);
        if (*text == ',') {
            text++;
            text = skip_space(text);
            continue;
        }

        if (*text == ')') {
            return text + 1;
        }

        return (const char *)0;
    }
}

/*
 * Parses one update assignment like column = value.
 */
static const char *parse_assignment(const char *text,
    sql_assignment *assignment)
{
    text = skip_space(text);
    text = read_identifier(text, assignment->column_name);
    if (text == (const char *)0) {
        return (const char *)0;
    }

    text = skip_space(text);
    if (*text != '=') {
        return (const char *)0;
    }
    text++;

    text = parse_value(text, &assignment->value);
    return text;
}

/*
 * Parses one comma-separated SET assignment list.
 */
static const char *parse_assignment_list(const char *text,
    sql_statement *statement)
{
    const char *next;

    while (1) {
        if (statement->assignment_count >= sql_max_columns) {
            return (const char *)0;
        }

        next = parse_assignment(text,
            &statement->assignments[statement->assignment_count]);
        if (next == (const char *)0) {
            return (const char *)0;
        }

        statement->assignment_count++;
        text = skip_space(next);
        if (*text != ',') {
            return text;
        }

        text++;
        text = skip_space(text);
    }
}

/*
 * Parses SELECT * or one comma-separated list of column names.
 */
static const char *parse_select_list(const char *text,
    sql_statement *statement)
{
    const char *next;

    text = skip_space(text);
    if (*text == '*') {
        statement->select_all = 1;
        statement->select_count = 0;
        return text + 1;
    }

    while (1) {
        if (statement->select_count >= sql_max_columns) {
            return (const char *)0;
        }

        next = read_identifier(text,
            statement->select_names[statement->select_count]);
        if (next == (const char *)0) {
            return (const char *)0;
        }

        statement->select_count++;
        text = skip_space(next);
        if (*text != ',') {
            return text;
        }

        text++;
        text = skip_space(text);
    }
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

/*
 * Parses CREATE TABLE name (column type ...);
 */
static const char *parse_create_table(const char *text,
    sql_statement *statement)
{
    const char *next;

    text = skip_space(text);
    if (!keyword_matches(text, "CREATE")) {
        return (const char *)0;
    }

    text += 6;
    text = skip_space(text);
    if (!keyword_matches(text, "TABLE")) {
        return (const char *)0;
    }

    text += 5;
    text = skip_space(text);
    text = read_identifier(text, statement->name);
    if (text == (const char *)0) {
        return (const char *)0;
    }

    text = skip_space(text);
    if (*text != '(') {
        return (const char *)0;
    }
    text++;

    while (1) {
        if (statement->column_count >= sql_max_columns) {
            return (const char *)0;
        }

        next = parse_column(text,
            &statement->columns[statement->column_count]);
        if (next == (const char *)0) {
            return (const char *)0;
        }
        statement->column_count++;
        text = skip_space(next);

        if (*text == ',') {
            text++;
            continue;
        }

        if (*text == ')') {
            statement->type = sql_statement_create_table;
            return text + 1;
        }

        return (const char *)0;
    }
}

/*
 * Parses SELECT columns FROM table [WHERE column op value];
 */
static const char *parse_select(const char *text, sql_statement *statement)
{
    text = skip_space(text);
    if (!keyword_matches(text, "SELECT")) {
        return (const char *)0;
    }

    text += 6;
    text = skip_space(text);
    text = parse_select_list(text, statement);
    if (text == (const char *)0) {
        return (const char *)0;
    }

    text = skip_space(text);
    if (!keyword_matches(text, "FROM")) {
        return (const char *)0;
    }

    text += 4;
    text = skip_space(text);
    text = read_identifier(text, statement->name);
    if (text == (const char *)0) {
        return (const char *)0;
    }

    text = skip_space(text);
    text = parse_where_clause(text, &statement->where);
    if (text == (const char *)0) {
        return (const char *)0;
    }

    statement->type = sql_statement_select;
    return text;
}

/*
 * Parses INSERT INTO table VALUES (value[, value ...]);
 */
static const char *parse_insert(const char *text, sql_statement *statement)
{
    text = skip_space(text);
    if (!keyword_matches(text, "INSERT")) {
        return (const char *)0;
    }

    text += 6;
    text = skip_space(text);
    if (!keyword_matches(text, "INTO")) {
        return (const char *)0;
    }

    text += 4;
    text = skip_space(text);
    text = read_identifier(text, statement->name);
    if (text == (const char *)0) {
        return (const char *)0;
    }

    text = skip_space(text);
    if (!keyword_matches(text, "VALUES")) {
        return (const char *)0;
    }

    text += 6;
    text = skip_space(text);
    text = parse_value_list(text, statement);
    if (text == (const char *)0 || statement->value_count == 0) {
        return (const char *)0;
    }

    statement->type = sql_statement_insert;
    return text;
}

/*
 * Parses UPDATE table SET column = value[, ...] [WHERE ...];
 */
static const char *parse_update(const char *text, sql_statement *statement)
{
    text = skip_space(text);
    if (!keyword_matches(text, "UPDATE")) {
        return (const char *)0;
    }

    text += 6;
    text = skip_space(text);
    text = read_identifier(text, statement->name);
    if (text == (const char *)0) {
        return (const char *)0;
    }

    text = skip_space(text);
    if (!keyword_matches(text, "SET")) {
        return (const char *)0;
    }

    text += 3;
    text = skip_space(text);
    text = parse_assignment_list(text, statement);
    if (text == (const char *)0 || statement->assignment_count == 0) {
        return (const char *)0;
    }

    text = skip_space(text);
    text = parse_where_clause(text, &statement->where);
    if (text == (const char *)0) {
        return (const char *)0;
    }

    statement->type = sql_statement_update;
    return text;
}

/*
 * Parses DELETE FROM table [WHERE column op value];
 */
static const char *parse_delete(const char *text, sql_statement *statement)
{
    text = skip_space(text);
    if (!keyword_matches(text, "DELETE")) {
        return (const char *)0;
    }

    text += 6;
    text = skip_space(text);
    if (!keyword_matches(text, "FROM")) {
        return (const char *)0;
    }

    text += 4;
    text = skip_space(text);
    text = read_identifier(text, statement->name);
    if (text == (const char *)0) {
        return (const char *)0;
    }

    text = skip_space(text);
    text = parse_where_clause(text, &statement->where);
    if (text == (const char *)0) {
        return (const char *)0;
    }

    statement->type = sql_statement_delete;
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
        next = parse_create_table(text, statement);
    }
    if (next == (const char *)0) {
        next = parse_select(text, statement);
    }
    if (next == (const char *)0) {
        next = parse_insert(text, statement);
    }
    if (next == (const char *)0) {
        next = parse_update(text, statement);
    }
    if (next == (const char *)0) {
        next = parse_delete(text, statement);
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
