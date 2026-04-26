/*
 * Implements a tiny SQL parser for the project command shell.
 * The code keeps the lexer and recursive descent parser very small
 * and recognizes only the statements needed by the current system.
 *
 * MIT License (see: LICENSE)
 * Copyright (C) 2026 tomaz stih
 */

#include <ctype.h>
#include <string.h>
#include "sql.h"

/*
 * Clears the output statement before parsing begins.
 * All sentinel values (invalid, none) are defined as zero so a plain
 * memset is sufficient.
 */
static void sql_reset(sql_statement *s)
{
    memset(s, 0, sizeof(*s));
}

/*
 * Skips ASCII whitespace and returns the next text position.
 */
static const char *skip_space(const char *text)
{
    while (*text && isspace((unsigned char)*text))
        text++;
    return text;
}

/*
 * Compares one keyword without caring about ASCII letter case.
 * Returns zero when the match fails or is not on a word boundary.
 */
static int keyword_matches(const char *text, const char *keyword)
{
    while (*keyword) {
        if (toupper((unsigned char)*text) != toupper((unsigned char)*keyword))
            return 0;
        text++;
        keyword++;
    }
    return !(isalnum((unsigned char)*text) || *text == '_');
}

/*
 * Reads one SQL identifier into a fixed output buffer.
 */
static const char *read_identifier(const char *text, char *name)
{
    unsigned short i;
    if (!isalpha((unsigned char)*text) && *text != '_')
        return NULL;
    i = 0;
    while (isalnum((unsigned char)*text) || *text == '_') {
        if (i + 1 >= sql_name_size)
            return NULL;
        name[i++] = *text++;
    }
    name[i] = '\0';
    return text;
}

/*
 * Reads one unsigned number from the SQL text.
 */
static const char *read_number(const char *text, unsigned short *value)
{
    unsigned short n;
    if (!isdigit((unsigned char)*text))
        return NULL;
    n = 0;
    while (isdigit((unsigned char)*text))
        n = (unsigned short)(n * 10 + (*text++ - '0'));
    *value = n;
    return text;
}

/*
 * Parses one SQL value and records its inferred type.
 * Handles single-quoted strings, digit sequences, and bare identifiers.
 */
static const char *parse_value(const char *text, sql_value *val)
{
    unsigned short i;
    text = skip_space(text);
    if (*text == '\'') {
        val->type = sql_value_string;
        text++;
        i = 0;
        while (*text && *text != '\'') {
            if (i + 1 >= sql_value_size)
                return NULL;
            val->text[i++] = *text++;
        }
        if (*text != '\'')
            return NULL;
        val->text[i] = '\0';
        return text + 1;
    }
    if (isdigit((unsigned char)*text)) {
        val->type = sql_value_number;
        i = 0;
        while (isdigit((unsigned char)*text)) {
            if (i + 1 >= sql_value_size)
                return NULL;
            val->text[i++] = *text++;
        }
        val->text[i] = '\0';
        return text;
    }
    val->type = sql_value_identifier;
    return read_identifier(text, val->text);
}

/*
 * Skips unsupported column constraints until comma or right paren.
 */
static const char *skip_constraints(const char *text)
{
    while (*text && *text != ',' && *text != ')')
        text++;
    return text;
}

/*
 * Parses CHAR(n), CHARACTER(n), NUMERIC(n[,d]), DATE, or LOGICAL.
 * CHAR and CHARACTER share the same body; klen selects the keyword length.
 */
static const char *parse_column_type(const char *text, sql_column *col)
{
    unsigned short len;
    unsigned short dec;
    unsigned short klen;

    text = skip_space(text);
    klen = 0;
    if (keyword_matches(text, "CHARACTER"))
        klen = 9;
    else if (keyword_matches(text, "CHAR"))
        klen = 4;
    if (klen) {
        text += klen;
        text = skip_space(text);
        if (*text++ != '(')
            return NULL;
        text = skip_space(text);
        if (!(text = read_number(text, &len)) || !len || len > 255)
            return NULL;
        text = skip_space(text);
        if (*text++ != ')')
            return NULL;
        col->dbf_type = 'C';
        col->length = (unsigned char)len;
        col->decimals = 0;
        return text;
    }
    if (keyword_matches(text, "NUMERIC")) {
        text += 7;
        text = skip_space(text);
        if (*text++ != '(')
            return NULL;
        text = skip_space(text);
        if (!(text = read_number(text, &len)) || !len || len > 255)
            return NULL;
        dec = 0;
        text = skip_space(text);
        if (*text == ',') {
            text++;
            text = skip_space(text);
            if (!(text = read_number(text, &dec)) || dec > len)
                return NULL;
        }
        text = skip_space(text);
        if (*text++ != ')')
            return NULL;
        col->dbf_type = 'N';
        col->length = (unsigned char)len;
        col->decimals = (unsigned char)dec;
        return text;
    }
    if (keyword_matches(text, "DATE")) {
        col->dbf_type = 'D';
        col->length = 8;
        col->decimals = 0;
        return text + 4;
    }
    if (keyword_matches(text, "LOGICAL")) {
        col->dbf_type = 'L';
        col->length = 1;
        col->decimals = 0;
        return text + 7;
    }
    return NULL;
}

/*
 * Parses one CREATE TABLE column definition.
 */
static const char *parse_column(const char *text, sql_column *col)
{
    text = skip_space(text);
    text = read_identifier(text, col->name);
    if (!text)
        return NULL;
    text = parse_column_type(text, col);
    if (!text)
        return NULL;
    text = skip_space(text);
    return skip_constraints(text);
}

/*
 * Parses one SQL comparison operator used by WHERE.
 */
static const char *parse_compare_operator(const char *text,
    sql_compare_operator *op)
{
    if (text[0] == '<' && text[1] == '>') { *op = sql_compare_not_equal;     return text + 2; }
    if (text[0] == '!' && text[1] == '=') { *op = sql_compare_not_equal;     return text + 2; }
    if (text[0] == '<' && text[1] == '=') { *op = sql_compare_less_equal;    return text + 2; }
    if (text[0] == '>' && text[1] == '=') { *op = sql_compare_greater_equal; return text + 2; }
    if (text[0] == '=') { *op = sql_compare_equal;   return text + 1; }
    if (text[0] == '<') { *op = sql_compare_less;    return text + 1; }
    if (text[0] == '>') { *op = sql_compare_greater; return text + 1; }
    return NULL;
}

/*
 * Parses one WHERE comparison with a single column and value.
 * Returns the input pointer unchanged when WHERE is absent.
 */
static const char *parse_where_clause(const char *text, sql_where *where)
{
    text = skip_space(text);
    if (!keyword_matches(text, "WHERE"))
        return text;
    text += 5;
    text = skip_space(text);
    text = read_identifier(text, where->column_name);
    if (!text)
        return NULL;
    text = skip_space(text);
    text = parse_compare_operator(text, &where->operator);
    if (!text)
        return NULL;
    text = parse_value(text, &where->value);
    if (!text)
        return NULL;
    where->active = 1;
    return text;
}

/*
 * Parses one comma-separated list of SQL values inside parentheses.
 */
static const char *parse_value_list(const char *text, sql_statement *stmt)
{
    const char *next;
    text = skip_space(text);
    if (*text++ != '(')
        return NULL;
    while (1) {
        if (stmt->value_count >= sql_max_columns)
            return NULL;
        next = parse_value(text, &stmt->values[stmt->value_count]);
        if (!next)
            return NULL;
        stmt->value_count++;
        text = skip_space(next);
        if (*text == ',') { text++; continue; }
        if (*text == ')') return text + 1;
        return NULL;
    }
}

/*
 * Parses one update assignment like column = value.
 */
static const char *parse_assignment(const char *text, sql_assignment *asgn)
{
    text = skip_space(text);
    text = read_identifier(text, asgn->column_name);
    if (!text)
        return NULL;
    text = skip_space(text);
    if (*text++ != '=')
        return NULL;
    return parse_value(text, &asgn->value);
}

/*
 * Parses one comma-separated SET assignment list.
 */
static const char *parse_assignment_list(const char *text, sql_statement *stmt)
{
    const char *next;
    while (1) {
        if (stmt->assignment_count >= sql_max_columns)
            return NULL;
        next = parse_assignment(text, &stmt->assignments[stmt->assignment_count]);
        if (!next)
            return NULL;
        stmt->assignment_count++;
        text = skip_space(next);
        if (*text != ',')
            return text;
        text++;
    }
}

/*
 * Parses SELECT * or one comma-separated list of column names.
 */
static const char *parse_select_list(const char *text, sql_statement *stmt)
{
    const char *next;
    text = skip_space(text);
    if (*text == '*') {
        stmt->select_all = 1;
        return text + 1;
    }
    while (1) {
        if (stmt->select_count >= sql_max_columns)
            return NULL;
        next = read_identifier(text, stmt->select_names[stmt->select_count]);
        if (!next)
            return NULL;
        stmt->select_count++;
        text = skip_space(next);
        if (*text != ',')
            return text;
        text = skip_space(text + 1);
    }
}

/*
 * Parses CREATE DATABASE name; or CREATE TABLE name (...);
 */
static const char *parse_create(const char *text, sql_statement *stmt)
{
    const char *next;
    text = skip_space(text);
    if (!keyword_matches(text, "CREATE"))
        return NULL;
    text += 6;
    text = skip_space(text);
    if (keyword_matches(text, "DATABASE")) {
        text += 8;
        text = skip_space(text);
        text = read_identifier(text, stmt->name);
        if (!text)
            return NULL;
        stmt->type = sql_statement_create_database;
        return text;
    }
    if (keyword_matches(text, "TABLE")) {
        text += 5;
        text = skip_space(text);
        text = read_identifier(text, stmt->name);
        if (!text)
            return NULL;
        text = skip_space(text);
        if (*text++ != '(')
            return NULL;
        while (1) {
            if (stmt->column_count >= sql_max_columns)
                return NULL;
            next = parse_column(text, &stmt->columns[stmt->column_count]);
            if (!next)
                return NULL;
            stmt->column_count++;
            text = skip_space(next);
            if (*text == ',') { text++; continue; }
            if (*text == ')') {
                stmt->type = sql_statement_create_table;
                return text + 1;
            }
            return NULL;
        }
    }
    return NULL;
}

/*
 * Parses SHOW DATABASES;
 */
static const char *parse_show_databases(const char *text, sql_statement *stmt)
{
    text = skip_space(text);
    if (!keyword_matches(text, "SHOW"))
        return NULL;
    text += 4;
    text = skip_space(text);
    if (!keyword_matches(text, "DATABASES"))
        return NULL;
    stmt->type = sql_statement_show_databases;
    return text + 9;
}

/*
 * Parses SELECT columns FROM table [WHERE column op value];
 */
static const char *parse_select(const char *text, sql_statement *stmt)
{
    text = skip_space(text);
    if (!keyword_matches(text, "SELECT"))
        return NULL;
    text += 6;
    text = parse_select_list(text, stmt);
    if (!text)
        return NULL;
    text = skip_space(text);
    if (!keyword_matches(text, "FROM"))
        return NULL;
    text += 4;
    text = skip_space(text);
    text = read_identifier(text, stmt->name);
    if (!text)
        return NULL;
    text = parse_where_clause(text, &stmt->where);
    if (!text)
        return NULL;
    stmt->type = sql_statement_select;
    return text;
}

/*
 * Parses INSERT INTO table VALUES (value[, value ...]);
 */
static const char *parse_insert(const char *text, sql_statement *stmt)
{
    text = skip_space(text);
    if (!keyword_matches(text, "INSERT"))
        return NULL;
    text += 6;
    text = skip_space(text);
    if (!keyword_matches(text, "INTO"))
        return NULL;
    text += 4;
    text = skip_space(text);
    text = read_identifier(text, stmt->name);
    if (!text)
        return NULL;
    text = skip_space(text);
    if (!keyword_matches(text, "VALUES"))
        return NULL;
    text += 6;
    text = parse_value_list(text, stmt);
    if (!text || stmt->value_count == 0)
        return NULL;
    stmt->type = sql_statement_insert;
    return text;
}

/*
 * Parses UPDATE table SET column = value[, ...] [WHERE ...];
 */
static const char *parse_update(const char *text, sql_statement *stmt)
{
    text = skip_space(text);
    if (!keyword_matches(text, "UPDATE"))
        return NULL;
    text += 6;
    text = skip_space(text);
    text = read_identifier(text, stmt->name);
    if (!text)
        return NULL;
    text = skip_space(text);
    if (!keyword_matches(text, "SET"))
        return NULL;
    text += 3;
    text = skip_space(text);
    text = parse_assignment_list(text, stmt);
    if (!text || stmt->assignment_count == 0)
        return NULL;
    text = parse_where_clause(text, &stmt->where);
    if (!text)
        return NULL;
    stmt->type = sql_statement_update;
    return text;
}

/*
 * Parses DELETE FROM table [WHERE column op value];
 */
static const char *parse_delete(const char *text, sql_statement *stmt)
{
    text = skip_space(text);
    if (!keyword_matches(text, "DELETE"))
        return NULL;
    text += 6;
    text = skip_space(text);
    if (!keyword_matches(text, "FROM"))
        return NULL;
    text += 4;
    text = skip_space(text);
    text = read_identifier(text, stmt->name);
    if (!text)
        return NULL;
    text = parse_where_clause(text, &stmt->where);
    if (!text)
        return NULL;
    stmt->type = sql_statement_delete;
    return text;
}

int sql_parse(const char *text, sql_statement *stmt)
{
    const char *next;
    const char *p;

    sql_reset(stmt);
    p = skip_space(text);

    if      (keyword_matches(p, "CREATE")) next = parse_create(text, stmt);
    else if (keyword_matches(p, "SHOW"))   next = parse_show_databases(text, stmt);
    else if (keyword_matches(p, "SELECT")) next = parse_select(text, stmt);
    else if (keyword_matches(p, "INSERT")) next = parse_insert(text, stmt);
    else if (keyword_matches(p, "UPDATE")) next = parse_update(text, stmt);
    else if (keyword_matches(p, "DELETE")) next = parse_delete(text, stmt);
    else                                   return -1;

    if (!next) return -1;
    next = skip_space(next);
    if (*next != ';') return -1;
    next = skip_space(next + 1);
    return *next != '\0' ? -1 : 0;
}
