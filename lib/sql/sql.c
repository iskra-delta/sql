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
    s->where.root = (unsigned char)sql_where_nil;
}

static void copy_name(char *target, const char *source)
{
    unsigned short index;

    for (index = 0; index + 1 < sql_name_size; index++) {
        target[index] = source[index];
        if (source[index] == '\0') {
            return;
        }
    }
    target[sql_name_size - 1] = '\0';
}

static void copy_column_ref(sql_column_ref *target, const char *qualifier,
    const char *name)
{
    copy_name(target->qualifier, qualifier);
    copy_name(target->name, name);
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
 * Reads one column reference with an optional qualifier.
 * Accepts either name or qualifier.name and stores the field part in
 * name while storing any qualifier separately.
 */
static const char *read_field_reference(const char *text, char *qualifier,
    char *name)
{
    char first[sql_name_size];
    const char *next;

    qualifier[0] = '\0';
    next = read_identifier(text, first);
    if (!next) {
        return NULL;
    }

    next = skip_space(next);
    if (*next != '.') {
        copy_name(name, first);
        return next;
    }

    copy_name(qualifier, first);
    next = skip_space(next + 1);
    next = read_identifier(next, name);
    return next;
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
 * Allocates one predicate node inside the statement-local WHERE pool.
 */
static int where_add_node(sql_statement *stmt, sql_where_node_type type,
    unsigned char *ref_out)
{
    sql_where_node *node;

    if (stmt->where.node_count >= sql_where_max_nodes) {
        return -1;
    }

    *ref_out = stmt->where.node_count++;
    node = &stmt->where_nodes[*ref_out];
    memset(node, 0, sizeof(*node));
    node->type = type;
    node->left = (unsigned char)sql_where_nil;
    node->right = (unsigned char)sql_where_nil;
    return 0;
}

/*
 * Appends one SQL value to the statement-local WHERE value pool.
 */
static int where_add_value(sql_statement *stmt, const sql_value *value,
    unsigned char *index_out)
{
    if (stmt->where.value_count >= sql_where_max_values) {
        return -1;
    }

    *index_out = stmt->where.value_count;
    stmt->where_values[stmt->where.value_count++] = *value;
    return 0;
}

/*
 * Resolves one parsed qualifier against the current FROM table.
 * Empty qualifiers are always accepted.
 */
static int qualifier_matches_source(const char *qualifier,
    const char *table_name, const char *table_alias)
{
    if (qualifier[0] == '\0') {
        return 1;
    }
    if (strcmp(qualifier, table_name) == 0) {
        return 1;
    }
    return table_alias[0] != '\0' && strcmp(qualifier, table_alias) == 0;
}

static int qualifier_matches_tables(const char *qualifier,
    const char *table_name, const char *table_alias,
    const char *join_table_name, const char *join_alias)
{
    if (qualifier_matches_source(qualifier, table_name, table_alias)) {
        return 1;
    }
    return join_table_name[0] != '\0'
        && qualifier_matches_source(qualifier, join_table_name, join_alias);
}

/*
 * Reads one WHERE column reference, optionally allowing qualification.
 */
static const char *read_where_column(const char *text, char *qualifier_out,
    char *column_name,
    const char *table_name, const char *table_alias,
    const char *join_table_name, const char *join_alias,
    unsigned char allow_qualifier)
{
    char qualifier[sql_name_size];
    char field_name[sql_name_size];

    text = skip_space(text);
    if (allow_qualifier) {
        text = read_field_reference(text, qualifier, field_name);
        if (!text || !qualifier_matches_tables(qualifier, table_name,
            table_alias, join_table_name, join_alias)) {
            return NULL;
        }
    } else {
        text = read_identifier(text, field_name);
        if (!text) {
            return NULL;
        }
        qualifier[0] = '\0';
    }

    copy_name(qualifier_out, qualifier);
    copy_name(column_name, field_name);
    return text;
}

/*
 * Parses one IN (...) value list into the statement-local WHERE pool.
 */
static const char *parse_in_value_list(const char *text, sql_statement *stmt,
    unsigned char *first_out, unsigned char *count_out)
{
    sql_value value;
    unsigned char first;
    unsigned char count;
    unsigned char index;

    text = skip_space(text);
    if (*text++ != '(') {
        return NULL;
    }

    count = 0;
    first = stmt->where.value_count;
    while (1) {
        text = parse_value(text, &value);
        if (!text || where_add_value(stmt, &value, &index) != 0) {
            return NULL;
        }
        count++;

        text = skip_space(text);
        if (*text == ',') {
            text++;
            continue;
        }
        if (*text == ')') {
            *first_out = first;
            *count_out = count;
            return text + 1;
        }
        return NULL;
    }
}

/*
 * Parses one atomic predicate: comparison, IN, or parenthesised group.
 */
static const char *parse_where_or_expression(const char *text,
    sql_statement *stmt, const char *table_name, const char *table_alias,
    const char *join_table_name, const char *join_alias,
    unsigned char allow_qualifier, unsigned char *ref_out);

static const char *parse_where_primary(const char *text, sql_statement *stmt,
    const char *table_name, const char *table_alias,
    const char *join_table_name, const char *join_alias,
    unsigned char allow_qualifier, unsigned char *ref_out)
{
    sql_value value;
    sql_compare_operator operator;
    sql_where_node *node;
    unsigned char value_first;
    unsigned char value_count;
    unsigned char node_ref;

    text = skip_space(text);
    if (*text == '(') {
        text = parse_where_or_expression(text + 1, stmt, table_name,
            table_alias, join_table_name, join_alias, allow_qualifier,
            ref_out);
        if (!text) {
            return NULL;
        }
        text = skip_space(text);
        return *text == ')' ? text + 1 : NULL;
    }

    if (where_add_node(stmt, sql_where_compare, &node_ref) != 0) {
        return NULL;
    }
    node = &stmt->where_nodes[node_ref];
    text = read_where_column(text, node->qualifier, node->column_name,
        table_name, table_alias, join_table_name, join_alias,
        allow_qualifier);
    if (!text) {
        return NULL;
    }

    text = skip_space(text);
    if (keyword_matches(text, "IN")) {
        node->type = sql_where_in;
        text += 2;
        text = parse_in_value_list(text, stmt, &value_first, &value_count);
        if (!text || value_count == 0) {
            return NULL;
        }
        node->value_first = value_first;
        node->value_count = value_count;
        *ref_out = node_ref;
        return text;
    }

    text = parse_compare_operator(text, &operator);
    if (!text) {
        return NULL;
    }
    text = parse_value(text, &value);
    if (!text || where_add_value(stmt, &value, &value_first) != 0) {
        return NULL;
    }

    node->operator = operator;
    node->value_first = value_first;
    node->value_count = 1;
    *ref_out = node_ref;
    return text;
}

/*
 * Parses one AND-precedence expression.
 */
static const char *parse_where_and_expression(const char *text,
    sql_statement *stmt, const char *table_name, const char *table_alias,
    const char *join_table_name, const char *join_alias,
    unsigned char allow_qualifier, unsigned char *ref_out)
{
    unsigned char left;
    unsigned char right;
    unsigned char node_ref;
    sql_where_node *node;

    text = parse_where_primary(text, stmt, table_name, table_alias,
        join_table_name, join_alias, allow_qualifier, &left);
    if (!text) {
        return NULL;
    }

    while (1) {
        text = skip_space(text);
        if (!keyword_matches(text, "AND")) {
            *ref_out = left;
            return text;
        }

        text += 3;
        text = parse_where_primary(text, stmt, table_name, table_alias,
            join_table_name, join_alias, allow_qualifier, &right);
        if (!text || where_add_node(stmt, sql_where_and, &node_ref) != 0) {
            return NULL;
        }

        node = &stmt->where_nodes[node_ref];
        node->left = left;
        node->right = right;
        left = node_ref;
    }
}

/*
 * Parses one OR-precedence expression.
 */
static const char *parse_where_or_expression(const char *text,
    sql_statement *stmt, const char *table_name, const char *table_alias,
    const char *join_table_name, const char *join_alias,
    unsigned char allow_qualifier, unsigned char *ref_out)
{
    unsigned char left;
    unsigned char right;
    unsigned char node_ref;
    sql_where_node *node;

    text = parse_where_and_expression(text, stmt, table_name, table_alias,
        join_table_name, join_alias, allow_qualifier, &left);
    if (!text) {
        return NULL;
    }

    while (1) {
        text = skip_space(text);
        if (!keyword_matches(text, "OR")) {
            *ref_out = left;
            return text;
        }

        text += 2;
        text = parse_where_and_expression(text, stmt, table_name, table_alias,
            join_table_name, join_alias, allow_qualifier, &right);
        if (!text || where_add_node(stmt, sql_where_or, &node_ref) != 0) {
            return NULL;
        }

        node = &stmt->where_nodes[node_ref];
        node->left = left;
        node->right = right;
        left = node_ref;
    }
}

/*
 * Parses one optional WHERE expression into the statement-local pool.
 * Returns the input pointer unchanged when WHERE is absent.
 */
static const char *parse_where_clause(const char *text, sql_statement *stmt,
    const char *table_name, const char *table_alias,
    const char *join_table_name, const char *join_alias,
    unsigned char allow_qualifier)
{
    unsigned char root;

    text = skip_space(text);
    if (!keyword_matches(text, "WHERE")) {
        return text;
    }

    stmt->where.active = 0;
    stmt->where.root = (unsigned char)sql_where_nil;
    stmt->where.node_count = 0;
    stmt->where.value_count = 0;

    text = parse_where_or_expression(text + 5, stmt, table_name, table_alias,
        join_table_name, join_alias, allow_qualifier, &root);
    if (!text) {
        return NULL;
    }

    stmt->where.active = 1;
    stmt->where.root = root;
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
 * Parses one comma-separated list of index key names inside parentheses.
 */
static const char *parse_key_list(const char *text, sql_statement *stmt)
{
    const char *next;

    text = skip_space(text);
    if (*text++ != '(')
        return NULL;

    while (1) {
        text = skip_space(text);
        if (stmt->key_count >= sql_max_columns)
            return NULL;

        next = read_identifier(text, stmt->key_names[stmt->key_count]);
        if (!next)
            return NULL;
        stmt->key_count++;

        text = skip_space(next);
        if (*text == ',') {
            text++;
            continue;
        }
        if (*text == ')')
            return text + 1;
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
 * Parses one optional alias following a SELECT item or FROM table.
 * Supports both AS alias and bare alias forms.
 */
static const char *parse_optional_alias(const char *text, char *alias,
    const char *stop_keyword)
{
    const char *next;

    alias[0] = '\0';
    text = skip_space(text);
    if (keyword_matches(text, "AS")) {
        text += 2;
        text = skip_space(text);
        return read_identifier(text, alias);
    }
    if (stop_keyword && keyword_matches(text, stop_keyword)) {
        return text;
    }
    next = read_identifier(text, alias);
    if (!next) {
        alias[0] = '\0';
        return text;
    }
    return next;
}

static const char *parse_optional_alias2(const char *text, char *alias,
    const char *stop_keyword1, const char *stop_keyword2)
{
    const char *next;

    alias[0] = '\0';
    text = skip_space(text);
    if (keyword_matches(text, "AS")) {
        text += 2;
        text = skip_space(text);
        return read_identifier(text, alias);
    }
    if ((stop_keyword1 && keyword_matches(text, stop_keyword1))
        || (stop_keyword2 && keyword_matches(text, stop_keyword2))) {
        return text;
    }
    next = read_identifier(text, alias);
    if (!next) {
        alias[0] = '\0';
        return text;
    }
    return next;
}

/*
 * Parses one projected column with an optional alias.
 */
static const char *parse_select_item(const char *text, sql_select_item *item)
{
    text = skip_space(text);
    text = read_field_reference(text, item->column.qualifier,
        item->column.name);
    if (!text) {
        return NULL;
    }
    return parse_optional_alias(text, item->alias, "FROM");
}

/*
 * Validates SELECT item qualifiers against the current table.
 */
static int normalize_select_items(sql_statement *stmt)
{
    unsigned char index;

    for (index = 0; index < stmt->select_count; index++) {
        if (!qualifier_matches_tables(stmt->select_items[index]
            .column.qualifier, stmt->name, stmt->from_alias,
            stmt->join_table_name, stmt->join_alias)) {
            return -1;
        }
    }
    return 0;
}

static const char *parse_join_condition(const char *text, sql_statement *stmt)
{
    char left_qualifier[sql_name_size];
    char left_name[sql_name_size];
    char right_qualifier[sql_name_size];
    char right_name[sql_name_size];
    int left_is_left;
    int left_is_right;
    int right_is_left;
    int right_is_right;

    text = skip_space(text);
    if (!keyword_matches(text, "ON")) {
        return NULL;
    }
    text += 2;
    text = skip_space(text);
    text = read_field_reference(text, left_qualifier, left_name);
    if (!text || left_qualifier[0] == '\0') {
        return NULL;
    }
    text = skip_space(text);
    if (*text++ != '=') {
        return NULL;
    }
    text = skip_space(text);
    text = read_field_reference(text, right_qualifier, right_name);
    if (!text || right_qualifier[0] == '\0') {
        return NULL;
    }

    left_is_left = qualifier_matches_source(left_qualifier, stmt->name,
        stmt->from_alias);
    left_is_right = qualifier_matches_source(left_qualifier,
        stmt->join_table_name, stmt->join_alias);
    right_is_left = qualifier_matches_source(right_qualifier, stmt->name,
        stmt->from_alias);
    right_is_right = qualifier_matches_source(right_qualifier,
        stmt->join_table_name, stmt->join_alias);

    if (left_is_left && right_is_right) {
        copy_column_ref(&stmt->join_left, left_qualifier, left_name);
        copy_column_ref(&stmt->join_right, right_qualifier, right_name);
        return text;
    }
    if (left_is_right && right_is_left) {
        copy_column_ref(&stmt->join_left, right_qualifier, right_name);
        copy_column_ref(&stmt->join_right, left_qualifier, left_name);
        return text;
    }

    return NULL;
}

/*
 * Parses SELECT * or one comma-separated list of projected columns.
 * Also recognises COUNT(*) and sets select_count_star.
 */
static const char *parse_select_list(const char *text, sql_statement *stmt)
{
    const char *next;

    text = skip_space(text);
    if (keyword_matches(text, "COUNT")) {
        text += 5;
        text = skip_space(text);
        if (*text++ != '(') return NULL;
        text = skip_space(text);
        if (*text++ != '*') return NULL;
        text = skip_space(text);
        if (*text++ != ')') return NULL;
        stmt->select_count_star = 1;
        return text;
    }
    if (*text == '*') {
        stmt->select_all = 1;
        return text + 1;
    }
    while (1) {
        if (stmt->select_count >= sql_max_columns)
            return NULL;
        next = parse_select_item(text, &stmt->select_items[stmt->select_count]);
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
 * Parses USE name;
 */
static const char *parse_use(const char *text, sql_statement *stmt)
{
    text = skip_space(text);
    if (!keyword_matches(text, "USE"))
        return NULL;
    text += 3;
    text = skip_space(text);
    text = read_identifier(text, stmt->name);
    if (!text)
        return NULL;
    stmt->type = sql_statement_use;
    return text;
}

/*
 * Parses DROP DATABASE name; or DROP TABLE name;
 */
static const char *parse_drop(const char *text, sql_statement *stmt)
{
    text = skip_space(text);
    if (!keyword_matches(text, "DROP"))
        return NULL;
    text += 4;
    text = skip_space(text);
    if (keyword_matches(text, "DATABASE")) {
        text += 8;
        text = skip_space(text);
        text = read_identifier(text, stmt->name);
        if (!text) return NULL;
        stmt->type = sql_statement_drop_database;
        return text;
    }
    if (keyword_matches(text, "TABLE")) {
        text += 5;
        text = skip_space(text);
        text = read_identifier(text, stmt->name);
        if (!text) return NULL;
        stmt->type = sql_statement_drop_table;
        return text;
    }
    if (keyword_matches(text, "VIEW")) {
        text += 4;
        text = skip_space(text);
        text = read_identifier(text, stmt->name);
        if (!text) return NULL;
        stmt->type = sql_statement_drop_view;
        return text;
    }
    return NULL;
}

/*
 * Parses CREATE DATABASE, CREATE TABLE, or CREATE [UNIQUE] INDEX.
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
    if (keyword_matches(text, "VIEW")) {
        /* CREATE VIEW name AS SELECT ... */
        const char *sel_start;
        unsigned short sel_len;

        text += 4;
        text = skip_space(text);
        text = read_identifier(text, stmt->name);
        if (!text)
            return NULL;
        text = skip_space(text);
        if (!keyword_matches(text, "AS"))
            return NULL;
        text += 2;
        text = skip_space(text);
        if (!keyword_matches(text, "SELECT"))
            return NULL;
        sel_start = text;
        /* Find the end: scan to ';' */
        while (*text && *text != ';')
            text++;
        sel_len = (unsigned short)(text - sel_start);
        if (sel_len == 0 || sel_len >= sql_subquery_size)
            return NULL;
        memcpy(stmt->subquery_text, sel_start, sel_len);
        stmt->subquery_text[sel_len] = '\0';
        stmt->type = sql_statement_create_view;
        return text;
    }
    if (keyword_matches(text, "UNIQUE")) {
        stmt->create_index_unique = 1;
        text += 6;
        text = skip_space(text);
    }
    if (keyword_matches(text, "INDEX")) {
        text += 5;
        text = skip_space(text);
        text = read_identifier(text, stmt->name);
        if (!text)
            return NULL;
        text = skip_space(text);
        if (!keyword_matches(text, "ON"))
            return NULL;
        text += 2;
        text = skip_space(text);
        text = read_identifier(text, stmt->table_name);
        if (!text)
            return NULL;
        text = parse_key_list(text, stmt);
        if (!text || stmt->key_count == 0)
            return NULL;
        stmt->type = sql_statement_create_index;
        return text;
    }
    return NULL;
}

/*
 * Parses SHOW DATABASES; or SHOW VIEWS;
 */
static const char *parse_show(const char *text, sql_statement *stmt)
{
    text = skip_space(text);
    if (!keyword_matches(text, "SHOW"))
        return NULL;
    text += 4;
    text = skip_space(text);
    if (keyword_matches(text, "DATABASES")) {
        stmt->type = sql_statement_show_databases;
        return text + 9;
    }
    if (keyword_matches(text, "VIEWS")) {
        stmt->type = sql_statement_show_views;
        return text + 5;
    }
    return NULL;
}

/*
 * Parses SELECT columns FROM table [JOIN table ON ...] [WHERE ...];
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
    if (*text == '(') {
        /* FROM (SELECT ...) [AS alias] — inline subquery */
        const char *inner_start;
        unsigned short inner_len;
        int depth;
        const char *p;

        text++;  /* skip '(' */
        text = skip_space(text);
        if (!keyword_matches(text, "SELECT"))
            return NULL;
        inner_start = text;
        depth = 1;
        p = text;
        while (*p && depth > 0) {
            if (*p == '(') depth++;
            else if (*p == ')') { depth--; if (depth == 0) break; }
            p++;
        }
        if (depth != 0) return NULL;
        inner_len = (unsigned short)(p - inner_start);
        if (inner_len == 0 || inner_len >= sql_subquery_size)
            return NULL;
        memcpy(stmt->subquery_text, inner_start, inner_len);
        stmt->subquery_text[inner_len] = '\0';
        stmt->from_is_subquery = 1;
        strcpy(stmt->name, "_tmp");
        text = p + 1;  /* skip ')' */
        text = parse_optional_alias2(text, stmt->from_alias, "WHERE", "JOIN");
    } else {
        text = read_identifier(text, stmt->name);
        if (!text)
            return NULL;
        text = parse_optional_alias2(text, stmt->from_alias, "WHERE", "JOIN");
    }
    if (!text)
        return NULL;
    text = skip_space(text);
    if (keyword_matches(text, "JOIN")) {
        stmt->join_active = 1;
        text += 4;
        text = skip_space(text);
        text = read_identifier(text, stmt->join_table_name);
        if (!text) {
            return NULL;
        }
        text = parse_optional_alias(text, stmt->join_alias, "ON");
        if (!text) {
            return NULL;
        }
        text = parse_join_condition(text, stmt);
        if (!text) {
            return NULL;
        }
    }
    if (!stmt->select_all && !stmt->select_count_star
        && normalize_select_items(stmt) != 0) {
        return NULL;
    }
    text = parse_where_clause(text, stmt, stmt->name, stmt->from_alias,
        stmt->join_table_name, stmt->join_alias, 1);
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
    text = parse_where_clause(text, stmt, stmt->name, "", "", "", 0);
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
    text = parse_where_clause(text, stmt, stmt->name, "", "", "", 0);
    if (!text)
        return NULL;
    stmt->type = sql_statement_delete;
    return text;
}

int sql_parse_statement(const char *text, sql_statement *stmt)
{
    const char *next;
    const char *p;

    sql_reset(stmt);
    p = skip_space(text);

    if      (keyword_matches(p, "CREATE")) next = parse_create(text, stmt);
    else if (keyword_matches(p, "SHOW"))   next = parse_show(text, stmt);
    else if (keyword_matches(p, "USE"))    next = parse_use(text, stmt);
    else if (keyword_matches(p, "DROP"))   next = parse_drop(text, stmt);
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
