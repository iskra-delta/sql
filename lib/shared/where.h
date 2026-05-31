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
 * An empty qualifier always matches.
 */
int qualifier_matches_source(const char *qualifier,
    const row_source *source);

/*
 * Resolves a column reference (qualifier.name or just name) against
 * the left and right row sources. Writes the matching source and field
 * index through the output pointers. Returns zero on success and -1
 * when the reference is ambiguous or not found.
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
 * supplied row sources, 0 otherwise. Used for pre-execution validation.
 */
int where_references_known_fields(const sqlexec_program *program,
    const sql_where *where, const row_source *left_source,
    const row_source *right_source);

/*
 * Evaluates the WHERE tree against the current records in the row
 * sources. Returns 1 when the row matches and 0 when it does not.
 * An inactive WHERE always returns 1.
 */
int where_matches(const sqlexec_program *program, const sql_where *where,
    const row_source *left_source, const row_source *right_source);

#endif
