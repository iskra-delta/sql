/*
 * Implements WHERE clause evaluation and row source field resolution.
 * These routines are shared between the SELECT and MUTATE executors.
 *
 * MIT License (see: LICENSE)
 * Copyright (C) 2026 tomaz stih
 */

#include "where.h"

#include <string.h>

int qualifier_matches_source(const char *qualifier,
    const row_source *source)
{
    if (!source || qualifier[0] == '\0') {
        return qualifier[0] == '\0';
    }
    if (source->table_name
        && strcmp(qualifier, source->table_name) == 0) {
        return 1;
    }
    return source->alias && source->alias[0] != '\0'
        && strcmp(qualifier, source->alias) == 0;
}

int resolve_field_ref_n(const row_source *sources, unsigned char count,
    const char *qualifier, const char *name,
    const row_source **source_out, int *field_index_out)
{
    unsigned char i;
    int fi;
    const row_source *found_source = NULL;
    int found_index = -1;
    int ambiguous = 0;

    for (i = 0; i < count; i++) {
        if (!sources[i].fields) {
            continue;
        }
        fi = find_field_index(sources[i].fields, sources[i].field_count,
            name);
        if (fi < 0) {
            continue;
        }
        if (qualifier[0] != '\0') {
            if (!qualifier_matches_source(qualifier, &sources[i])) {
                continue;
            }
            *source_out = &sources[i];
            *field_index_out = fi;
            return 0;
        }
        if (found_source) {
            ambiguous = 1;
        }
        found_source = &sources[i];
        found_index  = fi;
    }

    if (ambiguous || !found_source) {
        return -1;
    }
    *source_out     = found_source;
    *field_index_out = found_index;
    return 0;
}

int resolve_field_ref(const row_source *left_source,
    const row_source *right_source, const char *qualifier,
    const char *name, const row_source **source_out,
    int *field_index_out)
{
    /* Implemented directly rather than via resolve_field_ref_n to
     * ensure *source_out points to the caller's originals, not to
     * local copies that would be invalid after return. */
    int left_index;
    int right_index;

    left_index = left_source
        ? find_field_index(left_source->fields,
            left_source->field_count, name) : -1;
    right_index = right_source
        ? find_field_index(right_source->fields,
            right_source->field_count, name) : -1;

    if (qualifier[0] != '\0') {
        if (left_source
            && qualifier_matches_source(qualifier, left_source)
            && left_index >= 0) {
            *source_out = left_source; *field_index_out = left_index;
            return 0;
        }
        if (right_source
            && qualifier_matches_source(qualifier, right_source)
            && right_index >= 0) {
            *source_out = right_source; *field_index_out = right_index;
            return 0;
        }
        return -1;
    }

    if (left_index >= 0 && right_index >= 0) return -1;
    if (left_index >= 0) {
        *source_out = left_source; *field_index_out = left_index; return 0;
    }
    if (right_index >= 0) {
        *source_out = right_source; *field_index_out = right_index; return 0;
    }
    return -1;
}

int field_matches_value(const char *left, char field_type,
    const sql_value *value, sql_compare_operator op)
{
    int ok_left;
    int ok_right;
    long left_number;
    long right_number;

    if (field_type == 'N' && value->type == sql_value_number) {
        left_number = parse_integer_text(left, &ok_left);
        right_number = parse_integer_text(value->text, &ok_right);
        if (!ok_left || !ok_right) {
            return 0;
        }
        return compare_longs(left_number, right_number, op);
    }
    return compare_strings(left, value->text, op);
}

int field_values_equal(const char *left, char left_type,
    const char *right, char right_type)
{
    int ok_left;
    int ok_right;
    long left_number;
    long right_number;

    if (left_type == 'N' && right_type == 'N') {
        left_number = parse_integer_text(left, &ok_left);
        right_number = parse_integer_text(right, &ok_right);
        if (!ok_left || !ok_right) {
            return 0;
        }
        return left_number == right_number;
    }
    return strcmp(left, right) == 0;
}

int where_references_known_fields_n(const sqlexec_program *program,
    const sql_where *where,
    const row_source *sources, unsigned char source_count)
{
    unsigned char stack[sql_where_max_nodes];
    unsigned char depth;
    unsigned char ref;
    const row_source *source;
    const sql_where_node *node;
    int field_index;

    if (!where->active) {
        return 1;
    }
    depth = 0;
    stack[depth++] = where->root;
    while (depth > 0) {
        ref = stack[--depth];
        node = &program->where_nodes[ref];
        switch (node->type) {
        case sql_where_compare:
            if (resolve_field_ref_n(sources, source_count,
                node->qualifier, node->column_name,
                &source, &field_index) != 0) {
                return 0;
            }
            /* Also validate right-hand column ref for JOIN ON nodes. */
            if (node->value_count > 0) {
                const sql_value *v =
                    &program->where_values[node->value_first];
                if (v->type == sql_value_identifier) {
                    const char *dot = strchr(v->text, '.');
                    if (dot) {
                        char rq[sql_name_size];
                        unsigned short qlen =
                            (unsigned short)(dot - v->text);
                        if (qlen < sql_name_size) {
                            memcpy(rq, v->text, qlen);
                            rq[qlen] = '\0';
                            if (resolve_field_ref_n(sources, source_count,
                                rq, dot + 1, &source, &field_index) != 0) {
                                return 0;
                            }
                        }
                    }
                }
            }
            break;
        case sql_where_in:
            if (resolve_field_ref_n(sources, source_count,
                node->qualifier, node->column_name,
                &source, &field_index) != 0) {
                return 0;
            }
            break;
        case sql_where_and:
        case sql_where_or:
            stack[depth++] = node->right;
            stack[depth++] = node->left;
            break;
        default:
            return 0;
        }
    }
    return 1;
}

int where_references_known_fields(const sqlexec_program *program,
    const sql_where *where, const row_source *left_source,
    const row_source *right_source)
{
    unsigned char stack[sql_where_max_nodes];
    unsigned char depth;
    unsigned char ref;
    const row_source *source;
    const sql_where_node *node;
    int field_index;

    if (!where->active) return 1;
    depth = 0;
    stack[depth++] = where->root;
    while (depth > 0) {
        ref = stack[--depth];
        node = &program->where_nodes[ref];
        switch (node->type) {
        case sql_where_compare:
        case sql_where_in:
            if (resolve_field_ref(left_source, right_source,
                node->qualifier, node->column_name,
                &source, &field_index) != 0) {
                return 0;
            }
            break;
        case sql_where_and:
        case sql_where_or:
            stack[depth++] = node->right;
            stack[depth++] = node->left;
            break;
        default:
            return 0;
        }
    }
    return 1;
}

static int where_node_matches_n(const sqlexec_program *program,
    const sql_where *where, unsigned char ref,
    const row_source *sources, unsigned char source_count)
{
    const sql_where_node *node;
    const row_source *source;
    char left[sql_value_size];
    int field_index;
    unsigned char index;

    node = &program->where_nodes[ref];
    switch (node->type) {
    case sql_where_compare: {
        const sql_value *val = &program->where_values[node->value_first];
        if (resolve_field_ref_n(sources, source_count,
            node->qualifier, node->column_name,
            &source, &field_index) != 0) {
            return 0;
        }
        trim_field_value(left, sizeof(left),
            source->record + source->offsets[field_index],
            source->fields[field_index].length);
        /* If the value is an identifier containing a dot, it encodes a
         * right-hand column reference (from a JOIN ON condition). Resolve
         * it and compare the two column values directly. */
        if (val->type == sql_value_identifier) {
            const char *dot = strchr(val->text, '.');
            if (dot) {
                char rq[sql_name_size];
                const char *rn;
                const row_source *rsrc;
                int rfi;
                char right[sql_value_size];
                unsigned short qlen = (unsigned short)(dot - val->text);
                if (qlen >= sql_name_size) return 0;
                memcpy(rq, val->text, qlen);
                rq[qlen] = '\0';
                rn = dot + 1;
                if (resolve_field_ref_n(sources, source_count, rq, rn,
                    &rsrc, &rfi) != 0) return 0;
                trim_field_value(right, sizeof(right),
                    rsrc->record + rsrc->offsets[rfi],
                    rsrc->fields[rfi].length);
                return field_values_equal(left, source->fields[field_index].type,
                    right, rsrc->fields[rfi].type);
            }
        }
        return field_matches_value(left,
            source->fields[field_index].type, val, node->operator);
    }
    case sql_where_in:
        if (resolve_field_ref_n(sources, source_count,
            node->qualifier, node->column_name,
            &source, &field_index) != 0) {
            return 0;
        }
        trim_field_value(left, sizeof(left),
            source->record + source->offsets[field_index],
            source->fields[field_index].length);
        for (index = 0; index < node->value_count; index++) {
            if (field_matches_value(left,
                source->fields[field_index].type,
                &program->where_values[node->value_first + index],
                sql_compare_equal)) {
                return 1;
            }
        }
        return 0;
    case sql_where_and:
        return where_node_matches_n(program, where, node->left,
                sources, source_count)
            && where_node_matches_n(program, where, node->right,
                sources, source_count);
    case sql_where_or:
        return where_node_matches_n(program, where, node->left,
                sources, source_count)
            || where_node_matches_n(program, where, node->right,
                sources, source_count);
    default:
        return 0;
    }
}

int where_matches_n(const sqlexec_program *program, const sql_where *where,
    const row_source *sources, unsigned char source_count)
{
    if (!where->active) {
        return 1;
    }
    return where_node_matches_n(program, where, where->root,
        sources, source_count);
}

static int where_node_matches(const sqlexec_program *program,
    const sql_where *where, unsigned char ref,
    const row_source *left_source, const row_source *right_source);

int where_matches(const sqlexec_program *program, const sql_where *where,
    const row_source *left_source, const row_source *right_source)
{
    if (!where->active) return 1;
    return where_node_matches(program, where, where->root,
        left_source, right_source);
}

static int where_node_matches(const sqlexec_program *program,
    const sql_where *where, unsigned char ref,
    const row_source *left_source, const row_source *right_source)
{
    const sql_where_node *node;
    const row_source *source;
    char left[sql_value_size];
    int field_index;
    unsigned char index;

    node = &program->where_nodes[ref];
    switch (node->type) {
    case sql_where_compare:
        if (resolve_field_ref(left_source, right_source,
            node->qualifier, node->column_name,
            &source, &field_index) != 0) return 0;
        trim_field_value(left, sizeof(left),
            source->record + source->offsets[field_index],
            source->fields[field_index].length);
        return field_matches_value(left, source->fields[field_index].type,
            &program->where_values[node->value_first], node->operator);
    case sql_where_in:
        if (resolve_field_ref(left_source, right_source,
            node->qualifier, node->column_name,
            &source, &field_index) != 0) return 0;
        trim_field_value(left, sizeof(left),
            source->record + source->offsets[field_index],
            source->fields[field_index].length);
        for (index = 0; index < node->value_count; index++) {
            if (field_matches_value(left, source->fields[field_index].type,
                &program->where_values[node->value_first + index],
                sql_compare_equal)) return 1;
        }
        return 0;
    case sql_where_and:
        return where_node_matches(program, where, node->left,
                left_source, right_source)
            && where_node_matches(program, where, node->right,
                left_source, right_source);
    case sql_where_or:
        return where_node_matches(program, where, node->left,
                left_source, right_source)
            || where_node_matches(program, where, node->right,
                left_source, right_source);
    default:
        return 0;
    }
}
