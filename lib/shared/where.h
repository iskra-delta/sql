/*
 * Declares the row_source type and WHERE clause evaluation functions
 * shared between the SELECT and MUTATE executor modules.
 *
 * MIT License (see: LICENSE)
 * Copyright (C) 2026 tomaz stih
 */

#ifndef where_h
#define where_h

#include "sqlexec.h"
#include "shared.h"

/*
 * Describes one open table's fields, offsets, and current record
 * for use in WHERE evaluation and row projection.
 */
typedef struct row_source {
    const char *table_name;
    const char *alias;
    const dbf_field *fields;
    const unsigned short *offsets;
    const char *record;
    unsigned short field_count;
} row_source;

/*
 * Returns 1 when qualifier matches the source by table name or alias.
 * An empty qualifier always matches any source when source count is 1.
 */
int qualifier_matches_source(const char *qualifier,
    const row_source *source);

/*
 * Resolves a column reference against an array of N row sources.
 * For an unqualified name, returns the first source that has the field;
 * returns -1 if ambiguous (found in more than one source).
 * For a qualified name, matches by table_name or alias.
 */
int resolve_field_ref_n(const row_source *sources, unsigned char count,
    const char *qualifier, const char *name,
    const row_source **source_out, int *field_index_out);

/*
 * Two-source convenience wrapper around resolve_field_ref_n.
 * Passes right_source only when non-NULL.
 */
int resolve_field_ref(const row_source *left_source,
    const row_source *right_source, const char *qualifier,
    const char *name, const row_source **source_out,
    int *field_index_out);

/*
 * Returns 1 when the trimmed field value satisfies the comparison
 * against value using the given SQL operator.
 */
int field_matches_value(const char *left, char field_type,
    const sql_value *value, sql_compare_operator op);

/*
 * Returns 1 when two trimmed field values are equal, comparing
 * numerically when both fields are numeric.
 */
int field_values_equal(const char *left, char left_type,
    const char *right, char right_type);

/*
 * Returns 1 when every column referenced by where is present in the
 * supplied row sources, 0 otherwise.
 */
int where_references_known_fields_n(const sqlexec_program *program,
    const sql_where *where,
    const row_source *sources, unsigned char source_count);

/*
 * Two-source convenience wrapper.
 */
int where_references_known_fields(const sqlexec_program *program,
    const sql_where *where, const row_source *left_source,
    const row_source *right_source);

/*
 * Evaluates the WHERE tree against N row sources. Returns 1 on match.
 * An inactive WHERE always returns 1.
 */
int where_matches_n(const sqlexec_program *program, const sql_where *where,
    const row_source *sources, unsigned char source_count);

/*
 * Two-source convenience wrapper around where_matches_n.
 */
int where_matches(const sqlexec_program *program, const sql_where *where,
    const row_source *left_source, const row_source *right_source);

#endif
