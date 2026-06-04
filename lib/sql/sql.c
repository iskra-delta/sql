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
#include "../common/common.h"

/*
 * Clears the output statement before parsing begins.
 * All sentinel values (invalid, none) are defined as zero so a plain
 * memset is sufficient.
 */
static void sql_reset(sql_statement *s)
{
    memset(s, 0, sizeof(*s));
    s->where.root = (unsigned char)sql_where_nil;
    s->having.root = (unsigned char)sql_where_nil;
}

static void copy_column_ref_name(sql_column_ref *target, const char *qualifier,
    const char *name)
{
    copy_name(target->qualifier, qualifier);
    copy_name(target->name, name);
}

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

static int column_ref_matches(const sql_column_ref *left,
    const sql_column_ref *right)
{
    if (strcmp(left->name, right->name) != 0) {
        return 0;
    }
    return left->qualifier[0] == '\0' || right->qualifier[0] == '\0'
        || strcmp(left->qualifier, right->qualifier) == 0;
}

static int select_item_is_aggregate(const sql_select_item *item)
{
    if (item->function == sql_function_count) {
        return 1;
    }
    if (item->function == sql_function_min) {
        return 1;
    }
    return item->function == sql_function_max
        || item->function == sql_function_sum
        || item->function == sql_function_avg;
}

static const char *select_item_output_name(const sql_select_item *item)
{
    if (item->alias[0] != '\0') {
        return item->alias;
    }
    if (item->function == sql_function_count && item->argument_is_star) {
        return "count";
    }
    return item->alias[0] != '\0' ? item->alias : item->column.name;
}

static unsigned char statement_source_count(const sql_statement *stmt)
{
    return (unsigned char)(1u + stmt_join_count(stmt));
}

static const char *statement_source_name(const sql_statement *stmt,
    unsigned char index)
{
    if (index == 0) {
        return stmt->name;
    }
    return stmt_join_table_names(stmt)[index - 1u];
}

static const char *statement_source_alias(const sql_statement *stmt,
    unsigned char index)
{
    if (index == 0) {
        return stmt_from_alias(stmt);
    }
    return stmt_join_aliases(stmt)[index - 1u];
}

typedef unsigned char sql_token_kind;
enum {
    sql_token_invalid = 0,
    sql_token_eof,
    sql_token_identifier,
    sql_token_number,
    sql_token_string,
    sql_token_lparen,
    sql_token_rparen,
    sql_token_comma,
    sql_token_dot,
    sql_token_star,
    sql_token_semicolon,
    sql_token_equal,
    sql_token_not_equal,
    sql_token_less,
    sql_token_less_equal,
    sql_token_greater,
    sql_token_greater_equal
};

typedef unsigned char sql_keyword;
enum {
    sql_keyword_none = 0,
    sql_keyword_all,
    sql_keyword_and,
    sql_keyword_any,
    sql_keyword_avg,
    sql_keyword_as,
    sql_keyword_between,
    sql_keyword_by,
    sql_keyword_char,
    sql_keyword_character,
    sql_keyword_count,
    sql_keyword_create,
    sql_keyword_database,
    sql_keyword_databases,
    sql_keyword_date,
    sql_keyword_delete,
    sql_keyword_distinct,
    sql_keyword_drop,
    sql_keyword_exists,
    sql_keyword_from,
    sql_keyword_group,
    sql_keyword_having,
    sql_keyword_in,
    sql_keyword_index,
    sql_keyword_insert,
    sql_keyword_into,
    sql_keyword_is,
    sql_keyword_join,
    sql_keyword_like,
    sql_keyword_logical,
    sql_keyword_max,
    sql_keyword_min,
    sql_keyword_not,
    sql_keyword_null,
    sql_keyword_numeric,
    sql_keyword_on,
    sql_keyword_or,
    sql_keyword_select,
    sql_keyword_set,
    sql_keyword_show,
    sql_keyword_sum,
    sql_keyword_table,
    sql_keyword_trim,
    sql_keyword_unique,
    sql_keyword_update,
    sql_keyword_use,
    sql_keyword_values,
    sql_keyword_view,
    sql_keyword_views,
    sql_keyword_where
};

typedef struct sql_keyword_entry {
    const char *name;
    sql_keyword keyword;
} sql_keyword_entry;

static const sql_keyword_entry sql_keywords[] = {
    { "ALL", sql_keyword_all },
    { "AND", sql_keyword_and },
    { "ANY", sql_keyword_any },
    { "AVG", sql_keyword_avg },
    { "AS", sql_keyword_as },
    { "BETWEEN", sql_keyword_between },
    { "BY", sql_keyword_by },
    { "CHAR", sql_keyword_char },
    { "CHARACTER", sql_keyword_character },
    { "COUNT", sql_keyword_count },
    { "CREATE", sql_keyword_create },
    { "DATABASE", sql_keyword_database },
    { "DATABASES", sql_keyword_databases },
    { "DATE", sql_keyword_date },
    { "DELETE", sql_keyword_delete },
    { "DISTINCT", sql_keyword_distinct },
    { "DROP", sql_keyword_drop },
    { "EXISTS", sql_keyword_exists },
    { "FROM", sql_keyword_from },
    { "GROUP", sql_keyword_group },
    { "HAVING", sql_keyword_having },
    { "IN", sql_keyword_in },
    { "INDEX", sql_keyword_index },
    { "INSERT", sql_keyword_insert },
    { "INTO", sql_keyword_into },
    { "IS", sql_keyword_is },
    { "JOIN", sql_keyword_join },
    { "LIKE", sql_keyword_like },
    { "LOGICAL", sql_keyword_logical },
    { "MAX", sql_keyword_max },
    { "MIN", sql_keyword_min },
    { "NOT", sql_keyword_not },
    { "NULL", sql_keyword_null },
    { "NUMERIC", sql_keyword_numeric },
    { "ON", sql_keyword_on },
    { "OR", sql_keyword_or },
    { "SELECT", sql_keyword_select },
    { "SET", sql_keyword_set },
    { "SHOW", sql_keyword_show },
    { "SUM", sql_keyword_sum },
    { "TABLE", sql_keyword_table },
    { "TRIM", sql_keyword_trim },
    { "UNIQUE", sql_keyword_unique },
    { "UPDATE", sql_keyword_update },
    { "USE", sql_keyword_use },
    { "VALUES", sql_keyword_values },
    { "VIEW", sql_keyword_view },
    { "VIEWS", sql_keyword_views },
    { "WHERE", sql_keyword_where }
};

typedef struct sql_token {
    sql_token_kind kind;
    sql_keyword keyword;
    const char *text;
    const char *next;
    unsigned short length;
} sql_token;

typedef struct sql_lexer {
    sql_token token;
} sql_lexer;

typedef unsigned char sql_scalar_expr_kind;
enum {
    sql_scalar_expr_invalid = 0,
    sql_scalar_expr_value,
    sql_scalar_expr_column,
    sql_scalar_expr_function
};

typedef struct sql_scalar_expr {
    sql_scalar_expr_kind kind;
    sql_value value;
    sql_column_ref column;
    sql_select_function function;
    unsigned char argument_is_star;
} sql_scalar_expr;

static int keyword_text_matches(const char *text, unsigned short length,
    const char *keyword)
{
    unsigned short index;

    index = 0;
    while (keyword[index] != '\0') {
        if (index >= length
            || toupper((unsigned char)text[index])
                != toupper((unsigned char)keyword[index])) {
            return 0;
        }
        index++;
    }
    return index == length;
}

static sql_keyword keyword_from_identifier(const char *text,
    unsigned short length)
{
    unsigned short index;

    for (index = 0;
        index < sizeof(sql_keywords) / sizeof(sql_keywords[0]);
        index++) {
        if (keyword_text_matches(text, length, sql_keywords[index].name)) {
            return sql_keywords[index].keyword;
        }
    }
    return sql_keyword_none;
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
 * Reads one token from the SQL text with one-token lookahead.
 * The token skips leading whitespace and reports its source span.
 * Returns the next input position or NULL for malformed string tokens.
 */
static const char *read_token(const char *text, sql_token *token)
{
    const char *start;
    const char *p;

    start = skip_space(text);
    token->text = start;
    token->next = start;
    token->length = 0;
    token->keyword = sql_keyword_none;
    if (*start == '\0') {
        token->kind = sql_token_eof;
        return start;
    }

    if (isalpha((unsigned char)*start) || *start == '_') {
        p = start + 1;
        while (isalnum((unsigned char)*p) || *p == '_') {
            p++;
        }
        token->kind = sql_token_identifier;
        token->next = p;
        token->length = (unsigned short)(p - start);
        token->keyword = keyword_from_identifier(start, token->length);
        return p;
    }

    if (isdigit((unsigned char)*start)) {
        p = start + 1;
        while (isdigit((unsigned char)*p)) {
            p++;
        }
        token->kind = sql_token_number;
        token->next = p;
        token->length = (unsigned short)(p - start);
        return p;
    }

    if (*start == '\'') {
        p = start + 1;
        while (*p && *p != '\'') {
            p++;
        }
        if (*p != '\'') {
            return NULL;
        }
        token->kind = sql_token_string;
        token->next = p + 1;
        token->length = (unsigned short)((p + 1) - start);
        return p + 1;
    }

    token->length = 1;
    token->next = start + 1;
    if (*start == '(') {
        token->kind = sql_token_lparen;
        return token->next;
    }
    if (*start == ')') {
        token->kind = sql_token_rparen;
        return token->next;
    }
    if (*start == ',') {
        token->kind = sql_token_comma;
        return token->next;
    }
    if (*start == '.') {
        token->kind = sql_token_dot;
        return token->next;
    }
    if (*start == '*') {
        token->kind = sql_token_star;
        return token->next;
    }
    if (*start == ';') {
        token->kind = sql_token_semicolon;
        return token->next;
    }
    if (*start == '=' && start[1] == '\0') {
        token->kind = sql_token_equal;
        return token->next;
    }
    if (*start == '<' && start[1] == '>') {
        token->kind = sql_token_not_equal;
        token->next = start + 2;
        token->length = 2;
        return token->next;
    }
    if (*start == '!' && start[1] == '=') {
        token->kind = sql_token_not_equal;
        token->next = start + 2;
        token->length = 2;
        return token->next;
    }
    if (*start == '<' && start[1] == '=') {
        token->kind = sql_token_less_equal;
        token->next = start + 2;
        token->length = 2;
        return token->next;
    }
    if (*start == '>' && start[1] == '=') {
        token->kind = sql_token_greater_equal;
        token->next = start + 2;
        token->length = 2;
        return token->next;
    }
    if (*start == '=') {
        token->kind = sql_token_equal;
        return token->next;
    }
    if (*start == '<') {
        token->kind = sql_token_less;
        return token->next;
    }
    if (*start == '>') {
        token->kind = sql_token_greater;
        return token->next;
    }

    token->kind = sql_token_invalid;
    return token->next;
}

static int lexer_reposition(sql_lexer *lexer, const char *text)
{
    return read_token(text, &lexer->token) ? 0 : -1;
}

static int lexer_init(sql_lexer *lexer, const char *text)
{
    return lexer_reposition(lexer, text);
}

static int lexer_advance(sql_lexer *lexer)
{
    return lexer_reposition(lexer, lexer->token.next);
}

static const char *lexer_position(const sql_lexer *lexer)
{
    return lexer->token.text;
}

static int lexer_peek_kind(const sql_lexer *lexer, sql_token_kind kind)
{
    return lexer->token.kind == kind;
}

static int lexer_peek_keyword(const sql_lexer *lexer, sql_keyword keyword)
{
    return lexer->token.kind == sql_token_identifier
        && lexer->token.keyword == keyword;
}

static int lexer_accept_kind(sql_lexer *lexer, sql_token_kind kind)
{
    if (!lexer_peek_kind(lexer, kind)) {
        return 0;
    }
    return lexer_advance(lexer) == 0;
}

static int lexer_accept_keyword(sql_lexer *lexer, sql_keyword keyword)
{
    if (!lexer_peek_keyword(lexer, keyword)) {
        return 0;
    }
    return lexer_advance(lexer) == 0;
}

static int lexer_accept_identifier(sql_lexer *lexer, char *name)
{
    if (lexer->token.kind != sql_token_identifier
        || lexer->token.length + 1 > sql_name_size) {
        return 0;
    }
    memcpy(name, lexer->token.text, lexer->token.length);
    name[lexer->token.length] = '\0';
    return lexer_advance(lexer) == 0;
}

static int lexer_accept_number(sql_lexer *lexer, unsigned short *value)
{
    unsigned short index;
    unsigned short number;

    if (lexer->token.kind != sql_token_number) {
        return 0;
    }
    number = 0;
    for (index = 0; index < lexer->token.length; index++) {
        number = (unsigned short)(number * 10
            + (unsigned short)(lexer->token.text[index] - '0'));
    }
    *value = number;
    return lexer_advance(lexer) == 0;
}

static int lexer_accept_field_reference(sql_lexer *lexer, char *qualifier,
    char *name)
{
    char first[sql_name_size];

    qualifier[0] = '\0';
    if (!lexer_accept_identifier(lexer, first)) {
        return 0;
    }
    if (!lexer_accept_kind(lexer, sql_token_dot)) {
        copy_name(name, first);
        return 1;
    }
    copy_name(qualifier, first);
    return lexer_accept_identifier(lexer, name);
}

static int lexer_peek_parenthesized_select(const sql_lexer *lexer)
{
    sql_token token;

    if (!lexer_peek_kind(lexer, sql_token_lparen)) {
        return 0;
    }
    if (!read_token(lexer->token.next, &token)) {
        return 0;
    }
    return token.kind == sql_token_identifier
        && token.keyword == sql_keyword_select;
}

static int scalar_expr_to_value(const sql_scalar_expr *expr,
    sql_value *value);

static int encode_column_ref_value(const sql_column_ref *column,
    sql_value *value)
{
    unsigned short qlen;
    unsigned short nlen;

    value->type = sql_value_identifier;
    if (column->qualifier[0] == '\0') {
        if ((unsigned short)(strlen(column->name) + 1) > sql_value_size) {
            return -1;
        }
        copy_name(value->text, column->name);
        return 0;
    }

    qlen = (unsigned short)strlen(column->qualifier);
    nlen = (unsigned short)strlen(column->name);
    if ((unsigned short)(qlen + 1 + nlen + 1) > sql_value_size) {
        return -1;
    }
    memcpy(value->text, column->qualifier, qlen);
    value->text[qlen] = '.';
    memcpy(value->text + qlen + 1, column->name, nlen);
    value->text[qlen + 1 + nlen] = '\0';
    return 0;
}

static int keyword_to_select_function(sql_keyword keyword,
    sql_select_function *function)
{
    switch (keyword) {
    case sql_keyword_trim:
        *function = sql_function_trim;
        return 1;
    case sql_keyword_count:
        *function = sql_function_count;
        return 1;
    case sql_keyword_min:
        *function = sql_function_min;
        return 1;
    case sql_keyword_max:
        *function = sql_function_max;
        return 1;
    case sql_keyword_sum:
        *function = sql_function_sum;
        return 1;
    case sql_keyword_avg:
        *function = sql_function_avg;
        return 1;
    default:
        return 0;
    }
}

/*
 * Extracts one parenthesized SELECT body without the outer parens.
 * Returns the text position just after the closing ')'.
 */
static const char *parse_parenthesized_select_body(sql_lexer *lexer,
    char *subquery_out)
{
    const char *inner_start;
    const char *p;
    unsigned short inner_len;
    int depth;

    if (!lexer_accept_kind(lexer, sql_token_lparen)) {
        return NULL;
    }
    if (!lexer_peek_keyword(lexer, sql_keyword_select)) {
        return NULL;
    }
    inner_start = lexer_position(lexer);

    depth = 1;
    p = inner_start;
    while (*p && depth > 0) {
        if (*p == '(') {
            depth++;
        } else if (*p == ')') {
            depth--;
            if (depth == 0) {
                break;
            }
        }
        p++;
    }
    if (depth != 0) {
        return NULL;
    }

    inner_len = (unsigned short)(p - inner_start);
    if (inner_len == 0 || inner_len >= sql_subquery_size) {
        return NULL;
    }
    memcpy(subquery_out, inner_start, inner_len);
    subquery_out[inner_len] = '\0';
    return lexer_reposition(lexer, p + 1) == 0
        ? lexer_position(lexer) : NULL;
}

static int find_predicate_subquery(const sql_statement *stmt,
    const char *sql_text)
{
    unsigned char index;

    for (index = 0; index < stmt->predicate_subquery_count; index++) {
        if (strcmp(stmt->predicate_subqueries[index], sql_text) == 0) {
            return (int)index;
        }
    }
    return -1;
}

static int add_predicate_subquery(sql_statement *stmt, const char *sql_text,
    unsigned char *index_out)
{
    int existing;

    existing = find_predicate_subquery(stmt, sql_text);
    if (existing >= 0) {
        *index_out = (unsigned char)existing;
        return 0;
    }
    if (stmt->predicate_subquery_count >= sql_max_predicate_subqueries) {
        return -1;
    }
    *index_out = stmt->predicate_subquery_count++;
    copy_subquery(stmt->predicate_subqueries[*index_out], sql_text);
    return 0;
}

static const char *skip_constraints_lexer(sql_lexer *lexer)
{
    while (lexer->token.kind != sql_token_eof
        && lexer->token.kind != sql_token_comma
        && lexer->token.kind != sql_token_rparen) {
        if (lexer_advance(lexer) != 0) {
            return NULL;
        }
    }
    return lexer_position(lexer);
}

/*
 * Parses CHAR(n), CHARACTER(n), NUMERIC(n[,d]), DATE, or LOGICAL.
 */
static const char *parse_column_type_lexer(sql_lexer *lexer, sql_column *col)
{
    unsigned short len;
    unsigned short dec;

    if (lexer_accept_keyword(lexer, sql_keyword_character)
        || lexer_accept_keyword(lexer, sql_keyword_char)) {
        if (!lexer_accept_kind(lexer, sql_token_lparen)
            || !lexer_accept_number(lexer, &len) || !len || len > 255
            || !lexer_accept_kind(lexer, sql_token_rparen)) {
            return NULL;
        }
        col->dbf_type = 'C';
        col->length = (unsigned char)len;
        col->decimals = 0;
        return lexer_position(lexer);
    }
    if (lexer_accept_keyword(lexer, sql_keyword_numeric)) {
        if (!lexer_accept_kind(lexer, sql_token_lparen)
            || !lexer_accept_number(lexer, &len) || !len || len > 255) {
            return NULL;
        }
        dec = 0;
        if (lexer_accept_kind(lexer, sql_token_comma)) {
            if (!lexer_accept_number(lexer, &dec) || dec > len) {
                return NULL;
            }
        }
        if (!lexer_accept_kind(lexer, sql_token_rparen)) {
            return NULL;
        }
        col->dbf_type = 'N';
        col->length = (unsigned char)len;
        col->decimals = (unsigned char)dec;
        return lexer_position(lexer);
    }
    if (lexer_accept_keyword(lexer, sql_keyword_date)) {
        col->dbf_type = 'D';
        col->length = 8;
        col->decimals = 0;
        return lexer_position(lexer);
    }
    if (lexer_accept_keyword(lexer, sql_keyword_logical)) {
        col->dbf_type = 'L';
        col->length = 1;
        col->decimals = 0;
        return lexer_position(lexer);
    }
    return NULL;
}

/*
 * Parses one CREATE TABLE column definition.
 */
static const char *parse_column_lexer(sql_lexer *lexer, sql_column *col)
{
    if (!lexer_accept_identifier(lexer, col->name)
        || !parse_column_type_lexer(lexer, col)) {
        return NULL;
    }
    return skip_constraints_lexer(lexer);
}

static const char *parse_column_list_lexer(sql_lexer *lexer,
    sql_statement *stmt)
{
    if (!lexer_accept_kind(lexer, sql_token_lparen)) {
        return NULL;
    }
    while (1) {
        if (stmt_column_count(stmt) >= sql_max_columns) {
            return NULL;
        }
        if (!parse_column_lexer(lexer,
            &stmt_columns(stmt)[stmt_column_count(stmt)])) {
            return NULL;
        }
        stmt_column_count(stmt)++;
        if (lexer_accept_kind(lexer, sql_token_comma)) {
            continue;
        }
        return lexer_accept_kind(lexer, sql_token_rparen)
            ? lexer_position(lexer) : NULL;
    }
}

/*
 * Allocates one predicate node inside the statement-local WHERE pool.
 */
typedef struct sql_where_target {
    sql_where *where;
    sql_where_node *nodes;
    sql_predicate_operand *values;
} sql_where_target;

static int where_add_node(sql_where_target *target,
    sql_where_node_type type, unsigned char *ref_out)
{
    sql_where_node *node;

    if (target->where->node_count >= sql_where_max_nodes) {
        return -1;
    }

    *ref_out = target->where->node_count++;
    node = &target->nodes[*ref_out];
    memset(node, 0, sizeof(*node));
    node->type = type;
    node->left = (unsigned char)sql_where_nil;
    node->right = (unsigned char)sql_where_nil;
    return 0;
}

/*
 * Appends one SQL value to the statement-local WHERE value pool.
 */
static int where_add_operand(sql_where_target *target,
    const sql_predicate_operand *operand, unsigned char *index_out)
{
    if (target->where->value_count >= sql_where_max_values) {
        return -1;
    }

    *index_out = target->where->value_count;
    target->values[target->where->value_count++] = *operand;
    return 0;
}

/*
 * Resolves one parsed qualifier against the current FROM table.
 * Empty qualifiers are always accepted.
 */
static int qualifier_matches_pair(const char *qualifier,
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

static int qualifier_matches_sources(const char *qualifier,
    const sql_statement *stmt, unsigned char source_count)
{
    unsigned char index;

    if (qualifier[0] == '\0') {
        return 1;
    }
    for (index = 0; index < source_count; index++) {
        if (qualifier_matches_pair(qualifier,
            statement_source_name(stmt, index),
            statement_source_alias(stmt, index))) {
            return 1;
        }
    }
    return 0;
}

static int scalar_expr_column_matches_sources(const sql_scalar_expr *expr,
    const sql_statement *stmt, unsigned char source_count,
    unsigned char allow_qualifier)
{
    if (expr->kind != sql_scalar_expr_column) {
        return 0;
    }
    if (!allow_qualifier) {
        return expr->column.qualifier[0] == '\0';
    }
    return qualifier_matches_sources(expr->column.qualifier, stmt,
        source_count);
}

static const char *parse_predicate_operand_lexer(sql_lexer *lexer,
    sql_predicate_operand *operand);

/*
 * Parses one IN (...) value list into the statement-local WHERE pool.
 */
static const char *parse_in_value_list_lexer(sql_lexer *lexer,
    sql_where_target *target, unsigned char *first_out,
    unsigned char *count_out)
{
    sql_predicate_operand operand;
    unsigned char index;

    if (!lexer_accept_kind(lexer, sql_token_lparen)) {
        return NULL;
    }

    *first_out = target->where->value_count;
    *count_out = 0;
    while (1) {
        if (!parse_predicate_operand_lexer(lexer, &operand)
            || where_add_operand(target, &operand, &index) != 0) {
            return NULL;
        }
        (*count_out)++;
        if (lexer_accept_kind(lexer, sql_token_comma)) {
            continue;
        }
        return lexer_accept_kind(lexer, sql_token_rparen)
            ? lexer_position(lexer) : NULL;
    }
}

/*
 * Parses one atomic predicate: comparison, IN, or parenthesised group.
 */
static const char *parse_scalar_expr_lexer(sql_lexer *lexer,
    unsigned char allow_functions, sql_scalar_expr *expr);
static const char *parse_scalar_column_expr_lexer(sql_lexer *lexer,
    const sql_statement *stmt, unsigned char source_count,
    unsigned char allow_qualifier, sql_scalar_expr *expr);
static const char *parse_where_or_expression_lexer(sql_lexer *lexer,
    sql_statement *stmt, unsigned char source_count,
    unsigned char allow_qualifier, sql_where_target *target,
    unsigned char *ref_out);

static int make_compare_node(sql_where_target *target, const char *qualifier,
    const char *column_name, sql_compare_operator operator,
    const sql_predicate_operand *operand, unsigned char *ref_out)
{
    sql_where_node *node;
    unsigned char value_first;

    if (where_add_node(target, sql_where_compare, ref_out) != 0
        || where_add_operand(target, operand, &value_first) != 0) {
        return -1;
    }
    node = &target->nodes[*ref_out];
    copy_name(node->qualifier, qualifier);
    copy_name(node->column_name, column_name);
    node->operator = operator;
    node->value_first = value_first;
    node->value_count = 1;
    return 0;
}

static int make_column_node(sql_where_target *target, sql_where_node_type type,
    const sql_column_ref *column, unsigned char *ref_out)
{
    sql_where_node *node;

    if (where_add_node(target, type, ref_out) != 0) {
        return -1;
    }
    node = &target->nodes[*ref_out];
    copy_name(node->qualifier, column->qualifier);
    copy_name(node->column_name, column->name);
    return 0;
}

static int make_binary_node(sql_where_target *target, sql_where_node_type type,
    unsigned char left, unsigned char right, unsigned char *ref_out)
{
    sql_where_node *node;

    if (where_add_node(target, type, ref_out) != 0) {
        return -1;
    }
    node = &target->nodes[*ref_out];
    node->left = left;
    node->right = right;
    return 0;
}

static const char *parse_predicate_subquery_lexer(sql_lexer *lexer,
    sql_statement *stmt, unsigned char *index_out)
{
    char subquery_text[sql_subquery_size];

    if (!parse_parenthesized_select_body(lexer, subquery_text)
        || add_predicate_subquery(stmt, subquery_text,
        index_out) != 0) {
        return NULL;
    }
    return lexer_position(lexer);
}

static const char *parse_compare_operator_lexer(sql_lexer *lexer,
    sql_compare_operator *op)
{
    switch (lexer->token.kind) {
    case sql_token_not_equal:
        *op = sql_compare_not_equal;
        return lexer_advance(lexer) == 0 ? lexer_position(lexer) : NULL;
    case sql_token_less_equal:
        *op = sql_compare_less_equal;
        return lexer_advance(lexer) == 0 ? lexer_position(lexer) : NULL;
    case sql_token_greater_equal:
        *op = sql_compare_greater_equal;
        return lexer_advance(lexer) == 0 ? lexer_position(lexer) : NULL;
    case sql_token_equal:
        *op = sql_compare_equal;
        return lexer_advance(lexer) == 0 ? lexer_position(lexer) : NULL;
    case sql_token_less:
        *op = sql_compare_less;
        return lexer_advance(lexer) == 0 ? lexer_position(lexer) : NULL;
    case sql_token_greater:
        *op = sql_compare_greater;
        return lexer_advance(lexer) == 0 ? lexer_position(lexer) : NULL;
    default:
        return NULL;
    }
}

static const char *parse_where_primary_lexer(sql_lexer *lexer,
    sql_statement *stmt, unsigned char source_count,
    unsigned char allow_qualifier, sql_where_target *target,
    unsigned char *ref_out)
{
    sql_scalar_expr expr;
    sql_predicate_operand operand;
    sql_predicate_operand lower_operand;
    sql_predicate_operand upper_operand;
    sql_compare_operator operator;
    unsigned char left_ref;
    unsigned char right_ref;
    unsigned char value_count;
    unsigned char node_ref;
    unsigned char subquery_index;
    sql_where_node *node;

    if (lexer_accept_keyword(lexer, sql_keyword_exists)) {
        if (where_add_node(target, sql_where_exists, &node_ref) != 0) {
            return NULL;
        }
        if (!parse_predicate_subquery_lexer(lexer, stmt, &subquery_index)) {
            return NULL;
        }
        target->nodes[node_ref].subquery_index = subquery_index;
        *ref_out = node_ref;
        return lexer_position(lexer);
    }
    if (lexer_accept_kind(lexer, sql_token_lparen)) {
        if (!parse_where_or_expression_lexer(lexer, stmt, source_count,
            allow_qualifier, target, ref_out)
            || !lexer_accept_kind(lexer, sql_token_rparen)) {
            return NULL;
        }
        return lexer_position(lexer);
    }

    if (!parse_scalar_column_expr_lexer(lexer, stmt, source_count,
        allow_qualifier, &expr)) {
        return NULL;
    }

    if (lexer_accept_keyword(lexer, sql_keyword_is)) {
        if (make_column_node(target, sql_where_is_null, &expr.column,
            &node_ref) != 0) {
            return NULL;
        }
        node = &target->nodes[node_ref];
        node->operator = sql_compare_equal;
        if (lexer_accept_keyword(lexer, sql_keyword_not)) {
            node->operator = sql_compare_not_equal;
        }
        if (!lexer_accept_keyword(lexer, sql_keyword_null)) {
            return NULL;
        }
        *ref_out = node_ref;
        return lexer_position(lexer);
    }
    if (lexer_accept_keyword(lexer, sql_keyword_in)) {
        if (lexer_peek_parenthesized_select(lexer)) {
            if (make_column_node(target, sql_where_quantified,
                &expr.column, &node_ref) != 0) {
                return NULL;
            }
            node = &target->nodes[node_ref];
            node->operator = sql_compare_equal;
            node->quantifier = sql_quantifier_any;
            if (!parse_predicate_subquery_lexer(lexer, stmt,
                &subquery_index)) {
                return NULL;
            }
            node->subquery_index = subquery_index;
            *ref_out = node_ref;
            return lexer_position(lexer);
        }
        if (make_column_node(target, sql_where_in, &expr.column,
            &node_ref) != 0) {
            return NULL;
        }
        node = &target->nodes[node_ref];
        if (!parse_in_value_list_lexer(lexer, target, &node->value_first,
            &value_count) || value_count == 0) {
            return NULL;
        }
        node->value_count = value_count;
        *ref_out = node_ref;
        return lexer_position(lexer);
    }

    if (lexer_accept_keyword(lexer, sql_keyword_between)) {
        if (!parse_predicate_operand_lexer(lexer, &lower_operand)) {
            return NULL;
        }
        if (!lexer_accept_keyword(lexer, sql_keyword_and)) {
            return NULL;
        }
        if (!parse_predicate_operand_lexer(lexer, &upper_operand)) {
            return NULL;
        }
        if (make_compare_node(target, expr.column.qualifier, expr.column.name,
            sql_compare_greater_equal, &lower_operand, &left_ref) != 0
            || make_compare_node(target, expr.column.qualifier,
                expr.column.name,
                sql_compare_less_equal, &upper_operand, &right_ref) != 0
            || make_binary_node(target, sql_where_and, left_ref, right_ref,
                ref_out) != 0) {
            return NULL;
        }
        return lexer_position(lexer);
    }

    if (lexer_accept_keyword(lexer, sql_keyword_like)) {
        unsigned char value_first;

        if (!parse_predicate_operand_lexer(lexer, &operand)
            || make_column_node(target, sql_where_like, &expr.column,
            &node_ref) != 0
            || where_add_operand(target, &operand, &value_first) != 0) {
            return NULL;
        }
        node = &target->nodes[node_ref];
        node->value_first = value_first;
        node->value_count = 1;
        *ref_out = node_ref;
        return lexer_position(lexer);
    }

    if (!parse_compare_operator_lexer(lexer, &operator)) {
        return NULL;
    }
    if (lexer_peek_keyword(lexer, sql_keyword_any)
        || lexer_peek_keyword(lexer, sql_keyword_all)) {
        if (make_column_node(target, sql_where_quantified, &expr.column,
            &node_ref) != 0) {
            return NULL;
        }
        node = &target->nodes[node_ref];
        node->operator = operator;
        if (lexer_accept_keyword(lexer, sql_keyword_any)) {
            node->quantifier = sql_quantifier_any;
        } else if (lexer_accept_keyword(lexer, sql_keyword_all)) {
            node->quantifier = sql_quantifier_all;
        } else {
            return NULL;
        }
        if (!parse_predicate_subquery_lexer(lexer, stmt, &subquery_index)) {
            return NULL;
        }
        node->subquery_index = subquery_index;
        *ref_out = node_ref;
        return lexer_position(lexer);
    }
    if (!parse_predicate_operand_lexer(lexer, &operand)) {
        return NULL;
    }
    if (make_compare_node(target, expr.column.qualifier, expr.column.name,
        operator, &operand, ref_out) != 0) {
        return NULL;
    }
    return lexer_position(lexer);
}

static const char *parse_where_not_expression_lexer(sql_lexer *lexer,
    sql_statement *stmt, unsigned char source_count,
    unsigned char allow_qualifier, sql_where_target *target,
    unsigned char *ref_out)
{
    unsigned char child_ref;
    sql_where_node *node;

    if (!lexer_accept_keyword(lexer, sql_keyword_not)) {
        return parse_where_primary_lexer(lexer, stmt, source_count,
            allow_qualifier, target, ref_out);
    }
    if (!parse_where_not_expression_lexer(lexer, stmt, source_count,
        allow_qualifier, target, &child_ref)
        || where_add_node(target, sql_where_not, ref_out) != 0) {
        return NULL;
    }
    node = &target->nodes[*ref_out];
    node->left = child_ref;
    return lexer_position(lexer);
}

/*
 * Parses one AND-precedence expression.
 */
static const char *parse_where_and_expression_lexer(sql_lexer *lexer,
    sql_statement *stmt, unsigned char source_count,
    unsigned char allow_qualifier, sql_where_target *target,
    unsigned char *ref_out)
{
    unsigned char left;
    unsigned char right;
    unsigned char node_ref;
    sql_where_node *node;

    if (!parse_where_not_expression_lexer(lexer, stmt, source_count,
        allow_qualifier, target, &left)) {
        return NULL;
    }

    while (1) {
        if (!lexer_accept_keyword(lexer, sql_keyword_and)) {
            *ref_out = left;
            return lexer_position(lexer);
        }

        if (!parse_where_not_expression_lexer(lexer, stmt, source_count,
            allow_qualifier, target, &right)
            || where_add_node(target, sql_where_and, &node_ref) != 0) {
            return NULL;
        }

        node = &target->nodes[node_ref];
        node->left = left;
        node->right = right;
        left = node_ref;
    }
}

/*
 * Parses one OR-precedence expression.
 */
static const char *parse_where_or_expression_lexer(sql_lexer *lexer,
    sql_statement *stmt, unsigned char source_count,
    unsigned char allow_qualifier, sql_where_target *target,
    unsigned char *ref_out)
{
    unsigned char left;
    unsigned char right;
    unsigned char node_ref;
    sql_where_node *node;

    if (!parse_where_and_expression_lexer(lexer, stmt, source_count,
        allow_qualifier, target, &left)) {
        return NULL;
    }

    while (1) {
        if (!lexer_accept_keyword(lexer, sql_keyword_or)) {
            *ref_out = left;
            return lexer_position(lexer);
        }

        if (!parse_where_and_expression_lexer(lexer, stmt, source_count,
            allow_qualifier, target, &right)
            || where_add_node(target, sql_where_or, &node_ref) != 0) {
            return NULL;
        }

        node = &target->nodes[node_ref];
        node->left = left;
        node->right = right;
        left = node_ref;
    }
}

/*
 * Parses one optional WHERE expression into the statement-local pool.
 * Returns the input pointer unchanged when WHERE is absent.
 */
static const char *parse_predicate_clause_lexer(sql_lexer *lexer,
    sql_keyword keyword,
    sql_statement *stmt, unsigned char source_count,
    unsigned char allow_qualifier, sql_where_target *target)
{
    unsigned char old_active;
    unsigned char old_root;
    unsigned char root;

    if (!lexer_accept_keyword(lexer, keyword)) {
        return lexer_position(lexer);
    }

    old_active = target->where->active;
    old_root = target->where->root;
    if (!old_active) {
        target->where->root = (unsigned char)sql_where_nil;
        target->where->node_count = 0;
        target->where->value_count = 0;
    }

    if (!parse_where_or_expression_lexer(lexer, stmt, source_count,
        allow_qualifier, target, &root)) {
        return NULL;
    }

    if (old_active) {
        if (make_binary_node(target, sql_where_and, old_root, root,
            &target->where->root) != 0) {
            return NULL;
        }
    } else {
        target->where->root = root;
    }
    target->where->active = 1;
    return lexer_position(lexer);
}

static const char *parse_where_clause_lexer(sql_lexer *lexer,
    sql_statement *stmt, unsigned char source_count,
    unsigned char allow_qualifier)
{
    sql_where_target target;
    const char *next;

    target.where = &stmt->where;
    target.nodes = stmt->where_nodes;
    target.values = stmt->where_values;
    next = parse_predicate_clause_lexer(lexer, sql_keyword_where, stmt,
        source_count, allow_qualifier, &target);
    if (!next) {
        return NULL;
    }
    if (stmt->where.active) {
        stmt->predicate_node_count = stmt->where.node_count;
        stmt->predicate_value_count = stmt->where.value_count;
    }
    return next;
}

static const char *parse_having_clause_lexer(sql_lexer *lexer,
    sql_statement *stmt)
{
    sql_where_target target;
    const char *next;

    target.where = &stmt->having;
    stmt->having_node_first = stmt->predicate_node_count;
    stmt->having_value_first = stmt->predicate_value_count;
    target.nodes = statement_having_nodes(stmt);
    target.values = statement_having_values(stmt);
    next = parse_predicate_clause_lexer(lexer, sql_keyword_having, stmt,
        0, 0, &target);
    if (!next) {
        return NULL;
    }
    if (stmt->having.active) {
        stmt->predicate_node_count = (unsigned char)(
            stmt->having_node_first + stmt->having.node_count);
        stmt->predicate_value_count = (unsigned char)(
            stmt->having_value_first + stmt->having.value_count);
    }
    return next;
}

static const char *parse_value_lexer(sql_lexer *lexer, sql_value *value);

static const char *parse_insert_value_list_lexer(sql_lexer *lexer,
    sql_statement *stmt)
{
    sql_value value;
    unsigned char index;

    if (!lexer_accept_kind(lexer, sql_token_lparen)) {
        return NULL;
    }
    while (1) {
        if (stmt_assignment_count(stmt) >= sql_max_columns) {
            return NULL;
        }
        if (!parse_value_lexer(lexer, &value)) {
            return NULL;
        }
        if (stmt_assignments(stmt)[0].column_name[0] == '\0'
            && stmt_assignment_count(stmt) == 0) {
            stmt_assignments(stmt)[0].value = value;
            stmt_assignment_count(stmt) = 1;
        } else if (stmt_assignments(stmt)[0].column_name[0] == '\0') {
            stmt_assignments(stmt)[stmt_assignment_count(stmt)]
                .column_name[0] = '\0';
            stmt_assignments(stmt)[stmt_assignment_count(stmt)].value = value;
            stmt_assignment_count(stmt)++;
        } else {
            index = 0;
            while (index < stmt_assignment_count(stmt)
                && stmt_assignments(stmt)[index].value.type
                    != sql_value_none) {
                index++;
            }
            if (index >= stmt_assignment_count(stmt)) {
                return NULL;
            }
            stmt_assignments(stmt)[index].value = value;
        }
        if (lexer_accept_kind(lexer, sql_token_comma)) {
            continue;
        }
        if (!lexer_accept_kind(lexer, sql_token_rparen)) {
            return NULL;
        }
        if (stmt_assignments(stmt)[0].column_name[0] != '\0') {
            for (index = 0; index < stmt_assignment_count(stmt); index++) {
                if (stmt_assignments(stmt)[index].value.type
                    == sql_value_none) {
                    return NULL;
                }
            }
        }
        return lexer_position(lexer);
    }
}

static const char *parse_insert_column_list_lexer(sql_lexer *lexer,
    sql_statement *stmt)
{
    unsigned char index;

    if (!lexer_peek_kind(lexer, sql_token_lparen)) {
        return lexer_position(lexer);
    }
    if (!lexer_accept_kind(lexer, sql_token_lparen)) {
        return NULL;
    }
    while (1) {
        if (stmt_assignment_count(stmt) >= sql_max_columns) {
            return NULL;
        }
        if (!lexer_accept_identifier(lexer,
            stmt_assignments(stmt)[stmt_assignment_count(stmt)].column_name)) {
            return NULL;
        }
        for (index = 0; index < stmt_assignment_count(stmt); index++) {
            if (strcmp(stmt_assignments(stmt)[index].column_name,
                stmt_assignments(stmt)[stmt_assignment_count(stmt)]
                    .column_name) == 0) {
                return NULL;
            }
        }
        stmt_assignment_count(stmt)++;
        if (lexer_accept_kind(lexer, sql_token_comma)) {
            continue;
        }
        return lexer_accept_kind(lexer, sql_token_rparen)
            ? lexer_position(lexer) : NULL;
    }
}

static const char *parse_key_list_lexer(sql_lexer *lexer,
    sql_statement *stmt)
{
    if (!lexer_accept_kind(lexer, sql_token_lparen)) {
        return NULL;
    }
    while (1) {
        if (stmt_key_count(stmt) >= sql_max_columns) {
            return NULL;
        }
        if (!lexer_accept_identifier(lexer,
            stmt_key_names(stmt)[stmt_key_count(stmt)])) {
            return NULL;
        }
        stmt_key_count(stmt)++;
        if (lexer_accept_kind(lexer, sql_token_comma)) {
            continue;
        }
        return lexer_accept_kind(lexer, sql_token_rparen)
            ? lexer_position(lexer) : NULL;
    }
}

static const char *parse_assignment_lexer(sql_lexer *lexer,
    sql_assignment *asgn)
{
    if (!lexer_accept_identifier(lexer, asgn->column_name)
        || !lexer_accept_kind(lexer, sql_token_equal)) {
        return NULL;
    }
    return parse_value_lexer(lexer, &asgn->value);
}

static const char *parse_assignment_list_lexer(sql_lexer *lexer,
    sql_statement *stmt)
{
    while (1) {
        if (stmt_assignment_count(stmt) >= sql_max_columns) {
            return NULL;
        }
        if (!parse_assignment_lexer(lexer,
            &stmt_assignments(stmt)[stmt_assignment_count(stmt)])) {
            return NULL;
        }
        stmt_assignment_count(stmt)++;
        if (!lexer_accept_kind(lexer, sql_token_comma)) {
            return lexer_position(lexer);
        }
    }
}

/*
 * Parses one optional alias following a SELECT item or FROM table.
 * Supports both AS alias and bare alias forms.
 */
static int keyword_matches_any(const sql_lexer *lexer,
    const sql_keyword *keywords, unsigned char keyword_count)
{
    unsigned char index;

    for (index = 0; index < keyword_count; index++) {
        if (keywords[index] != sql_keyword_none
            && lexer_peek_keyword(lexer, keywords[index])) {
            return 1;
        }
    }
    return 0;
}

static const char *parse_optional_alias(sql_lexer *lexer, char *alias,
    const sql_keyword *stop_keywords, unsigned char stop_keyword_count)
{
    alias[0] = '\0';
    if (lexer_accept_keyword(lexer, sql_keyword_as)) {
        return lexer_accept_identifier(lexer, alias)
            ? lexer_position(lexer) : NULL;
    }
    if (keyword_matches_any(lexer, stop_keywords, stop_keyword_count)) {
        return lexer_position(lexer);
    }
    if (!lexer_accept_identifier(lexer, alias)) {
        alias[0] = '\0';
        return lexer_position(lexer);
    }
    return lexer_position(lexer);
}

static const sql_keyword from_alias_stop_keywords[] = {
    sql_keyword_where, sql_keyword_join, sql_keyword_group,
    sql_keyword_having
};

static const sql_keyword select_alias_stop_keywords[] = {
    sql_keyword_from
};

static const sql_keyword join_alias_stop_keywords[] = {
    sql_keyword_on
};

static const char *parse_select(sql_lexer *lexer, sql_statement *stmt);

static const char *complete_named_statement(sql_lexer *lexer,
    sql_statement *stmt, sql_statement_type type)
{
    if (!lexer_accept_identifier(lexer, stmt->name)) {
        return NULL;
    }
    stmt->type = type;
    return lexer_position(lexer);
}

static const char *parse_keyword_named_statement(sql_lexer *lexer,
    sql_statement *stmt, sql_keyword keyword, sql_statement_type type)
{
    if (!lexer_accept_keyword(lexer, keyword)) {
        return NULL;
    }
    return complete_named_statement(lexer, stmt, type);
}

static const char *complete_show_select(sql_lexer *lexer,
    sql_statement *stmt, const char *table_name)
{
    stmt->type = sql_statement_select;
    stmt_select_all(stmt) = 1;
    copy_name(stmt->name, table_name);
    return lexer_position(lexer);
}

static const char *parse_named_source_lexer(sql_lexer *lexer, char *table_name,
    char *alias, const sql_keyword *stop_keywords,
    unsigned char stop_keyword_count)
{
    if (!lexer_accept_identifier(lexer, table_name)) {
        return NULL;
    }
    return parse_optional_alias(lexer, alias, stop_keywords,
        stop_keyword_count);
}

static const char *parse_from_source_lexer(sql_lexer *lexer,
    sql_statement *stmt)
{
    if (lexer_peek_parenthesized_select(lexer)) {
        if (!parse_parenthesized_select_body(lexer, stmt->subquery_text)) {
            return NULL;
        }
        stmt_from_is_subquery(stmt) = 1;
        strcpy(stmt->name, "_tmp");
        return parse_optional_alias(lexer, stmt_from_alias(stmt),
            from_alias_stop_keywords, 4);
    }
    return parse_named_source_lexer(lexer, stmt->name, stmt_from_alias(stmt),
        from_alias_stop_keywords, 4);
}

static void init_scalar_expr(sql_scalar_expr *expr)
{
    memset(expr, 0, sizeof(*expr));
    expr->kind = sql_scalar_expr_invalid;
}

static const char *parse_scalar_column_expr_lexer(sql_lexer *lexer,
    const sql_statement *stmt, unsigned char source_count,
    unsigned char allow_qualifier, sql_scalar_expr *expr);

static const char *parse_scalar_function_expr_lexer(sql_lexer *lexer,
    sql_select_function function, sql_scalar_expr *expr)
{
    sql_scalar_expr argument;

    init_scalar_expr(expr);
    expr->kind = sql_scalar_expr_function;
    expr->function = function;
    expr->argument_is_star = 0;
    if (!lexer_accept_kind(lexer, sql_token_lparen)) {
        return NULL;
    }
    if (function == sql_function_count
        && lexer_accept_kind(lexer, sql_token_star)) {
        expr->argument_is_star = 1;
        expr->column.qualifier[0] = '\0';
        expr->column.name[0] = '\0';
    } else {
        if (!parse_scalar_column_expr_lexer(lexer, NULL, 0, 0, &argument)) {
            return NULL;
        }
        copy_column_ref_name(&expr->column, argument.column.qualifier,
            argument.column.name);
    }
    if (!lexer_accept_kind(lexer, sql_token_rparen)) {
        return NULL;
    }
    return lexer_position(lexer);
}

static const char *parse_scalar_expr_lexer(sql_lexer *lexer,
    unsigned char allow_functions, sql_scalar_expr *expr)
{
    sql_select_function function;

    init_scalar_expr(expr);
    if (allow_functions
        && lexer->token.kind == sql_token_identifier
        && keyword_to_select_function(lexer->token.keyword, &function)) {
        if (lexer_advance(lexer) != 0) {
            return NULL;
        }
        return parse_scalar_function_expr_lexer(lexer, function, expr);
    }

    if (lexer->token.kind == sql_token_string) {
        if (lexer->token.length <= 1
            || lexer->token.length - 1 > sql_value_size) {
            return NULL;
        }
        expr->kind = sql_scalar_expr_value;
        expr->value.type = sql_value_string;
        memcpy(expr->value.text, lexer->token.text + 1,
            lexer->token.length - 2);
        expr->value.text[lexer->token.length - 2] = '\0';
        return lexer_advance(lexer) == 0 ? lexer_position(lexer) : NULL;
    }
    if (lexer->token.kind == sql_token_number) {
        if (lexer->token.length + 1 > sql_value_size) {
            return NULL;
        }
        expr->kind = sql_scalar_expr_value;
        expr->value.type = sql_value_number;
        memcpy(expr->value.text, lexer->token.text, lexer->token.length);
        expr->value.text[lexer->token.length] = '\0';
        return lexer_advance(lexer) == 0 ? lexer_position(lexer) : NULL;
    }
    if (lexer_peek_keyword(lexer, sql_keyword_null)) {
        expr->kind = sql_scalar_expr_value;
        expr->value.type = sql_value_null;
        expr->value.text[0] = '\0';
        return lexer_advance(lexer) == 0 ? lexer_position(lexer) : NULL;
    }

    expr->kind = sql_scalar_expr_column;
    return lexer_accept_field_reference(lexer, expr->column.qualifier,
        expr->column.name) ? lexer_position(lexer) : NULL;
}

static int scalar_expr_to_value(const sql_scalar_expr *expr,
    sql_value *value)
{
    if (expr->kind == sql_scalar_expr_value) {
        *value = expr->value;
        return 0;
    }
    if (expr->kind == sql_scalar_expr_column) {
        return encode_column_ref_value(&expr->column, value);
    }
    return -1;
}

static const char *parse_value_lexer(sql_lexer *lexer, sql_value *value)
{
    sql_scalar_expr expr;

    if (!parse_scalar_expr_lexer(lexer, 0, &expr)
        || scalar_expr_to_value(&expr, value) != 0) {
        return NULL;
    }
    return lexer_position(lexer);
}

static int scalar_expr_to_predicate_operand(const sql_scalar_expr *expr,
    sql_predicate_operand *operand)
{
    memset(operand, 0, sizeof(*operand));
    if (expr->kind == sql_scalar_expr_value) {
        operand->kind = sql_predicate_operand_value;
        operand->data.value = expr->value;
        return 0;
    }
    if (expr->kind == sql_scalar_expr_column) {
        operand->kind = sql_predicate_operand_column;
        copy_column_ref_name(&operand->data.column,
            expr->column.qualifier, expr->column.name);
        return 0;
    }
    return -1;
}

static const char *parse_predicate_operand_lexer(sql_lexer *lexer,
    sql_predicate_operand *operand)
{
    sql_scalar_expr expr;

    if (!parse_scalar_expr_lexer(lexer, 0, &expr)
        || scalar_expr_to_predicate_operand(&expr, operand) != 0) {
        return NULL;
    }
    return lexer_position(lexer);
}

static const char *parse_scalar_column_expr_lexer(sql_lexer *lexer,
    const sql_statement *stmt, unsigned char source_count,
    unsigned char allow_qualifier, sql_scalar_expr *expr)
{
    if (!parse_scalar_expr_lexer(lexer, 0, expr)
        || expr->kind != sql_scalar_expr_column) {
        return NULL;
    }
    if (stmt && !scalar_expr_column_matches_sources(expr, stmt,
        source_count, allow_qualifier)) {
        return NULL;
    }
    return lexer_position(lexer);
}

static int scalar_expr_to_select_item(const sql_scalar_expr *expr,
    sql_select_item *item)
{
    memset(item, 0, sizeof(*item));
    if (expr->kind == sql_scalar_expr_column) {
        item->function = sql_function_none;
        item->argument_is_star = 0;
        copy_column_ref_name(&item->column, expr->column.qualifier,
            expr->column.name);
        return 0;
    }
    if (expr->kind == sql_scalar_expr_function) {
        item->function = expr->function;
        item->argument_is_star = expr->argument_is_star;
        copy_column_ref_name(&item->column, expr->column.qualifier,
            expr->column.name);
        return 0;
    }
    return -1;
}

/*
 * Parses one projected column with an optional alias.
 */
static const char *parse_select_item_lexer(sql_lexer *lexer,
    sql_select_item *item)
{
    sql_scalar_expr expr;

    if (!parse_scalar_expr_lexer(lexer, 1, &expr)
        || scalar_expr_to_select_item(&expr, item) != 0) {
        return NULL;
    }
    return parse_optional_alias(lexer, item->alias,
        select_alias_stop_keywords, 1);
}

/*
 * Validates SELECT item qualifiers against the current table.
 */
static int normalize_select_items(sql_statement *stmt)
{
    unsigned char index;

    for (index = 0; index < stmt_select_count(stmt); index++) {
        if (stmt_select_items(stmt)[index].argument_is_star) {
            continue;
        }
        if (!qualifier_matches_sources(stmt_select_items(stmt)[index]
            .column.qualifier, stmt, statement_source_count(stmt))) {
            return -1;
        }
    }
    return 0;
}

static int group_item_present(const sql_statement *stmt,
    const sql_column_ref *column)
{
    unsigned char index;

    for (index = 0; index < stmt_group_count(stmt); index++) {
        if (column_ref_matches(column, &stmt_group_items(stmt)[index])) {
            return 1;
        }
    }
    return 0;
}

static int output_name_present(const sql_statement *stmt, const char *name)
{
    unsigned char index;

    for (index = 0; index < stmt_select_count(stmt); index++) {
        if (strcmp(select_item_output_name(&stmt_select_items(stmt)[index]),
            name) == 0) {
            return 1;
        }
    }
    return 0;
}

static int validate_having_names(const sql_statement *stmt)
{
    unsigned char stack[sql_where_max_nodes];
    unsigned char depth;
    unsigned char ref;
    const sql_where_node *node;

    if (!stmt->having.active) {
        return 0;
    }

    depth = 0;
    stack[depth++] = stmt->having.root;
    while (depth > 0) {
        ref = stack[--depth];
        node = &statement_having_nodes(stmt)[ref];
        switch (node->type) {
        case sql_where_false:
            break;
        case sql_where_compare:
        case sql_where_in:
        case sql_where_like:
        case sql_where_is_null:
        case sql_where_quantified:
            if (node->qualifier[0] != '\0'
                || !output_name_present(stmt, node->column_name)) {
                return -1;
            }
            break;
        case sql_where_exists:
            break;
        case sql_where_not:
            stack[depth++] = node->left;
            break;
        case sql_where_and:
        case sql_where_or:
            stack[depth++] = node->right;
            stack[depth++] = node->left;
            break;
        default:
            return -1;
        }
    }

    return 0;
}

static int validate_grouped_select(const sql_statement *stmt)
{
    unsigned char index;
    int grouped;

    grouped = stmt_group_count(stmt) > 0
        || stmt_select_has_aggregate(stmt)
        || stmt->having.active;
    if (!grouped) {
        return 0;
    }

    if (stmt_select_all(stmt) || stmt_select_count_star(stmt)) {
        return -1;
    }
    if (stmt->having.active
        && stmt_group_count(stmt) == 0
        && !stmt_select_has_aggregate(stmt)) {
        return -1;
    }
    for (index = 0; index < stmt_select_count(stmt); index++) {
        if (select_item_is_aggregate(&stmt_select_items(stmt)[index])) {
            continue;
        }
        if (!group_item_present(stmt, &stmt_select_items(stmt)[index].column)) {
            return -1;
        }
    }

    return validate_having_names(stmt);
}

static const char *parse_group_by_clause_lexer(sql_lexer *lexer,
    sql_statement *stmt)
{
    sql_scalar_expr expr;

    if (!lexer_accept_keyword(lexer, sql_keyword_group)) {
        return lexer_position(lexer);
    }
    if (!lexer_accept_keyword(lexer, sql_keyword_by)) {
        return NULL;
    }
    while (1) {
        if (stmt_group_count(stmt) >= sql_max_columns) {
            return NULL;
        }
        if (!parse_scalar_column_expr_lexer(lexer, stmt,
            statement_source_count(stmt), 1, &expr)) {
            return NULL;
        }
        copy_column_ref_name(&stmt_group_items(stmt)[stmt_group_count(stmt)],
            expr.column.qualifier, expr.column.name);
        stmt_group_count(stmt)++;
        if (!lexer_accept_kind(lexer, sql_token_comma)) {
            return lexer_position(lexer);
        }
    }
}

/*
 * Adds the JOIN ON condition to the statement's WHERE tree as a regular
 * compare node. The right-hand column stays as a structured column
 * operand so later flattening, optimization, and binding do not need
 * to reconstruct it from text.
 */
static const char *add_on_to_where(sql_statement *stmt,
    const char *left_qualifier, const char *left_name,
    const char *right_qualifier, const char *right_name)
{
    sql_where_target target;
    sql_predicate_operand operand;
    unsigned char on_idx;
    unsigned char and_idx;
    unsigned char old_root;

    /* Need room for one compare node + possibly one AND node. */
    if ((unsigned short)stmt->where.node_count + 2 > sql_where_max_nodes) {
        return NULL;
    }
    if ((unsigned short)stmt->where.value_count + 1 > sql_where_max_values) {
        return NULL;
    }

    target.where = &stmt->where;
    target.nodes = stmt->where_nodes;
    target.values = stmt->where_values;
    memset(&operand, 0, sizeof(operand));
    operand.kind = sql_predicate_operand_column;
    copy_name(operand.data.column.qualifier, right_qualifier);
    copy_name(operand.data.column.name, right_name);
    if (make_compare_node(&target, left_qualifier, left_name,
        sql_compare_equal, &operand, &on_idx) != 0) {
        return NULL;
    }

    /* AND the ON condition with any existing WHERE clause. */
    if (stmt->where.active) {
        old_root = stmt->where.root;
        if (make_binary_node(&target, sql_where_and, old_root, on_idx,
            &and_idx) != 0) {
            return NULL;
        }
        stmt->where.root = and_idx;
    } else {
        stmt->where.active = 1;
        stmt->where.root = on_idx;
    }

    return ""; /* non-NULL = success */
}

static const char *parse_join_condition(sql_lexer *lexer, sql_statement *stmt,
    unsigned char join_index)
{
    char left_qualifier[sql_name_size];
    char left_name[sql_name_size];
    char right_qualifier[sql_name_size];
    char right_name[sql_name_size];
    int left_is_existing;
    int left_is_new;
    int right_is_existing;
    int right_is_new;
    unsigned char existing_source_count;

    if (!lexer_accept_keyword(lexer, sql_keyword_on)) {
        return NULL;
    }
    if (!lexer_accept_field_reference(lexer, left_qualifier, left_name)
        || left_qualifier[0] == '\0') {
        return NULL;
    }
    if (!lexer_accept_kind(lexer, sql_token_equal)) {
        return NULL;
    }
    if (!lexer_accept_field_reference(lexer, right_qualifier, right_name)
        || right_qualifier[0] == '\0') {
        return NULL;
    }

    existing_source_count = (unsigned char)(1u + join_index);
    left_is_existing = qualifier_matches_sources(left_qualifier, stmt,
        existing_source_count);
    left_is_new = qualifier_matches_pair(left_qualifier,
        stmt_join_table_names(stmt)[join_index],
        stmt_join_aliases(stmt)[join_index]);
    right_is_existing = qualifier_matches_sources(right_qualifier, stmt,
        existing_source_count);
    right_is_new = qualifier_matches_pair(right_qualifier,
        stmt_join_table_names(stmt)[join_index],
        stmt_join_aliases(stmt)[join_index]);

    /* Normalise so left side always belongs to the existing source set. */
    if (left_is_existing && right_is_new) {
        return add_on_to_where(stmt, left_qualifier, left_name,
            right_qualifier, right_name) ? lexer_position(lexer) : NULL;
    }
    if (left_is_new && right_is_existing) {
        return add_on_to_where(stmt, right_qualifier, right_name,
            left_qualifier, left_name) ? lexer_position(lexer) : NULL;
    }

    return NULL;
}

/*
 * Parses SELECT * or one comma-separated list of projected columns.
 */
static const char *parse_select_list(sql_lexer *lexer, sql_statement *stmt)
{
    if (lexer_accept_kind(lexer, sql_token_star)) {
        stmt_select_all(stmt) = 1;
        return lexer_position(lexer);
    }
    while (1) {
        if (stmt_select_count(stmt) >= sql_max_columns) {
            return NULL;
        }
        if (!parse_select_item_lexer(lexer,
            &stmt_select_items(stmt)[stmt_select_count(stmt)])) {
            return NULL;
        }
        if (select_item_is_aggregate(
            &stmt_select_items(stmt)[stmt_select_count(stmt)])) {
            stmt_select_has_aggregate(stmt) = 1;
        }
        stmt_select_count(stmt)++;
        if (!lexer_accept_kind(lexer, sql_token_comma)) {
            return lexer_position(lexer);
        }
    }
}

/*
 * Detects the special COUNT(*)-only form that still lowers to the
 * compact count-only scan tree.
 */
static const char *try_parse_count_only(sql_lexer *lexer)
{
    sql_select_item item;
    sql_lexer look;

    memset(&item, 0, sizeof(item));
    look = *lexer;
    if (!parse_select_item_lexer(&look, &item)
        || item.function != sql_function_count
        || !item.argument_is_star
        || item.alias[0] != '\0') {
        return NULL;
    }
    if (!lexer_peek_keyword(&look, sql_keyword_from)) {
        return NULL;
    }
    *lexer = look;
    return lexer_position(lexer);
}

/*
 * Parses USE name;
 */
static const char *parse_use(sql_lexer *lexer, sql_statement *stmt)
{
    return parse_keyword_named_statement(lexer, stmt, sql_keyword_use,
        sql_statement_use);
}

/*
 * Parses DROP DATABASE name; or DROP TABLE name;
 */
static const char *parse_drop(sql_lexer *lexer, sql_statement *stmt)
{
    const char *text;

    if (!lexer_accept_keyword(lexer, sql_keyword_drop)) {
        return NULL;
    }
    text = parse_keyword_named_statement(lexer, stmt, sql_keyword_database,
        sql_statement_drop_database);
    if (text) {
        return text;
    }
    text = parse_keyword_named_statement(lexer, stmt, sql_keyword_table,
        sql_statement_drop_table);
    if (text) {
        return text;
    }
    return parse_keyword_named_statement(lexer, stmt, sql_keyword_view,
        sql_statement_drop_view);
}

/*
 * Parses CREATE DATABASE, CREATE TABLE, or CREATE [UNIQUE] INDEX.
 */
static const char *parse_create(sql_lexer *lexer, sql_statement *stmt)
{
    const char *text;
    const char *sel_start;
    char view_name[sql_name_size];
    char view_text[sql_subquery_size];
    unsigned short sel_len;

    if (!lexer_accept_keyword(lexer, sql_keyword_create)) {
        return NULL;
    }
    text = parse_keyword_named_statement(lexer, stmt, sql_keyword_database,
        sql_statement_create_database);
    if (text) {
        return text;
    }
    if (lexer_accept_keyword(lexer, sql_keyword_table)) {
        if (!lexer_accept_identifier(lexer, stmt->name)) {
            return NULL;
        }
        stmt->type = sql_statement_create_table;
        return parse_column_list_lexer(lexer, stmt);
    }
    if (lexer_accept_keyword(lexer, sql_keyword_view)) {
        if (!lexer_accept_identifier(lexer, stmt->name)
            || !lexer_accept_keyword(lexer, sql_keyword_as)
            || !lexer_peek_keyword(lexer, sql_keyword_select)) {
            return NULL;
        }
        copy_name(view_name, stmt->name);
        sel_start = lexer_position(lexer);
        text = parse_select(lexer, stmt);
        if (!text) {
            return NULL;
        }
        sel_len = (unsigned short)(text - sel_start);
        if (sel_len == 0 || sel_len >= sql_subquery_size)
            return NULL;
        memcpy(view_text, sel_start, sel_len);
        view_text[sel_len] = '\0';
        sql_reset(stmt);
        copy_name(stmt->name, view_name);
        copy_subquery(stmt->subquery_text, view_text);
        stmt->type = sql_statement_create_view;
        return text;
    }
    if (lexer_accept_keyword(lexer, sql_keyword_unique)) {
        stmt_index_unique(stmt) = 1;
    }
    if (lexer_accept_keyword(lexer, sql_keyword_index)) {
        if (!lexer_accept_identifier(lexer, stmt->name)
            || !lexer_accept_keyword(lexer, sql_keyword_on)
            || !lexer_accept_identifier(lexer, stmt->table_name)) {
            return NULL;
        }
        if (!parse_key_list_lexer(lexer, stmt) || stmt_key_count(stmt) == 0) {
            return NULL;
        }
        stmt->type = sql_statement_create_index;
        return lexer_position(lexer);
    }
    return NULL;
}

/*
 * Parses SHOW DATABASES; or SHOW VIEWS; by expanding them into
 * SELECT * FROM sys_databases / sys_views at parse time.
 * This removes dedicated opcodes and lets the view pipeline handle output.
 */
static const char *parse_show(sql_lexer *lexer, sql_statement *stmt)
{
    if (!lexer_accept_keyword(lexer, sql_keyword_show)) {
        return NULL;
    }
    if (lexer_accept_keyword(lexer, sql_keyword_databases)) {
        return complete_show_select(lexer, stmt, "sys_databases");
    }
    if (lexer_accept_keyword(lexer, sql_keyword_views)) {
        return complete_show_select(lexer, stmt, "sys_views");
    }
    return NULL;
}

/*
 * Parses SELECT columns FROM table [JOIN table ON ...] [WHERE ...];
 */
static const char *parse_select(sql_lexer *lexer, sql_statement *stmt)
{
    const char *count_only_text;
    const char *text;

    if (!lexer_accept_keyword(lexer, sql_keyword_select)) {
        return NULL;
    }
    if (lexer_accept_keyword(lexer, sql_keyword_distinct)) {
        stmt_select_distinct(stmt) = 1;
    } else {
        (void)lexer_accept_keyword(lexer, sql_keyword_all);
    }
    count_only_text = try_parse_count_only(lexer);
    if (count_only_text != NULL) {
        stmt_select_count_star(stmt) = 1;
    } else {
        text = parse_select_list(lexer, stmt);
        if (!text) {
            return NULL;
        }
    }
    if (!lexer_accept_keyword(lexer, sql_keyword_from)) {
        return NULL;
    }
    text = parse_from_source_lexer(lexer, stmt);
    if (!text) {
        return NULL;
    }
    while (lexer_peek_kind(lexer, sql_token_comma)
        || lexer_peek_keyword(lexer, sql_keyword_join)) {
        unsigned char join_index;

        if (stmt_join_count(stmt) >= sql_max_joins) {
            return NULL;
        }
        join_index = stmt_join_count(stmt);
        if (lexer_accept_kind(lexer, sql_token_comma)) {
            text = parse_named_source_lexer(lexer,
                stmt_join_table_names(stmt)[join_index],
                stmt_join_aliases(stmt)[join_index],
                from_alias_stop_keywords, 4);
            if (!text) {
                return NULL;
            }
        } else {
            if (!lexer_accept_keyword(lexer, sql_keyword_join)) {
                return NULL;
            }
            text = parse_named_source_lexer(lexer,
                stmt_join_table_names(stmt)[join_index],
                stmt_join_aliases(stmt)[join_index],
                join_alias_stop_keywords, 1);
            if (!text) {
                return NULL;
            }
            text = parse_join_condition(lexer, stmt, join_index);
            if (!text) {
                return NULL;
            }
        }
        stmt_join_count(stmt)++;
    }
    if (!stmt_select_all(stmt) && !stmt_select_count_star(stmt)
        && normalize_select_items(stmt) != 0) {
        return NULL;
    }
    text = parse_where_clause_lexer(lexer, stmt, statement_source_count(stmt),
        1);
    if (!text) {
        return NULL;
    }
    text = parse_group_by_clause_lexer(lexer, stmt);
    if (!text) {
        return NULL;
    }
    text = parse_having_clause_lexer(lexer, stmt);
    if (!text) {
        return NULL;
    }
    if (validate_grouped_select(stmt) != 0) {
        return NULL;
    }
    stmt->type = sql_statement_select;
    return lexer_position(lexer);
}

/*
 * Parses INSERT INTO table VALUES (value[, value ...]);
 */
static const char *parse_insert(sql_lexer *lexer, sql_statement *stmt)
{
    const char *text;

    if (!lexer_accept_keyword(lexer, sql_keyword_insert)
        || !lexer_accept_keyword(lexer, sql_keyword_into)
        || !lexer_accept_identifier(lexer, stmt->name)) {
        return NULL;
    }
    text = parse_insert_column_list_lexer(lexer, stmt);
    if (!text) {
        return NULL;
    }
    if (!lexer_accept_keyword(lexer, sql_keyword_values)) {
        return NULL;
    }
    text = parse_insert_value_list_lexer(lexer, stmt);
    if (!text || stmt_assignment_count(stmt) == 0) {
        return NULL;
    }
    stmt->type = sql_statement_insert;
    return lexer_position(lexer);
}

/*
 * Parses UPDATE table SET column = value[, ...] [WHERE ...];
 */
static const char *parse_update(sql_lexer *lexer, sql_statement *stmt)
{
    const char *text;

    if (!lexer_accept_keyword(lexer, sql_keyword_update)
        || !lexer_accept_identifier(lexer, stmt->name)
        || !lexer_accept_keyword(lexer, sql_keyword_set)) {
        return NULL;
    }
    text = parse_assignment_list_lexer(lexer, stmt);
    if (!text || stmt_assignment_count(stmt) == 0) {
        return NULL;
    }
    text = parse_where_clause_lexer(lexer, stmt, 1, 0);
    if (!text) {
        return NULL;
    }
    stmt->type = sql_statement_update;
    return lexer_position(lexer);
}

/*
 * Parses DELETE FROM table [WHERE column op value];
 */
static const char *parse_delete(sql_lexer *lexer, sql_statement *stmt)
{
    const char *text;

    if (!lexer_accept_keyword(lexer, sql_keyword_delete)
        || !lexer_accept_keyword(lexer, sql_keyword_from)
        || !lexer_accept_identifier(lexer, stmt->name)) {
        return NULL;
    }
    text = parse_where_clause_lexer(lexer, stmt, 1, 0);
    if (!text) {
        return NULL;
    }
    stmt->type = sql_statement_delete;
    return lexer_position(lexer);
}

static const char *parse_statement_lexer(sql_lexer *lexer,
    sql_statement *stmt)
{
    switch (lexer->token.keyword) {
    case sql_keyword_create:
        return parse_create(lexer, stmt);
    case sql_keyword_show:
        return parse_show(lexer, stmt);
    case sql_keyword_use:
        return parse_use(lexer, stmt);
    case sql_keyword_drop:
        return parse_drop(lexer, stmt);
    case sql_keyword_select:
        return parse_select(lexer, stmt);
    case sql_keyword_insert:
        return parse_insert(lexer, stmt);
    case sql_keyword_update:
        return parse_update(lexer, stmt);
    case sql_keyword_delete:
        return parse_delete(lexer, stmt);
    default:
        break;
    }
    return NULL;
}

int sql_parse_statement(const char *text, sql_statement *stmt)
{
    sql_lexer lexer;
    const char *next;

    sql_reset(stmt);
    if (lexer_init(&lexer, text) != 0
        || lexer.token.kind != sql_token_identifier) {
        return -1;
    }

    next = parse_statement_lexer(&lexer, stmt);
    if (!next) {
        return -1;
    }
    if (lexer_reposition(&lexer, next) != 0
        || !lexer_accept_kind(&lexer, sql_token_semicolon)) {
        return -1;
    }
    return lexer.token.kind == sql_token_eof ? 0 : -1;
}

int sql_parse_select_body(const char *text, sql_statement *stmt)
{
    sql_lexer lexer;
    const char *next;

    sql_reset(stmt);
    if (lexer_init(&lexer, text) != 0
        || lexer.token.keyword != sql_keyword_select) {
        return -1;
    }

    next = parse_statement_lexer(&lexer, stmt);
    if (!next || stmt->type != sql_statement_select) {
        return -1;
    }
    if (lexer_reposition(&lexer, next) != 0) {
        return -1;
    }
    return lexer.token.kind == sql_token_eof ? 0 : -1;
}
