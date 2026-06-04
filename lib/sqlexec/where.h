/*
 * Declares the row_source type and WHERE clause evaluation functions
 * used by the sqlexec module.
 * This code is executor-local because it depends on sqlexec_program,
 * runtime row sources, and predicate-subquery execution state.
 *
 * MIT License (see: LICENSE)
 * Copyright (C) 2026 tomaz stih
 */

#ifndef where_h
#define where_h

#include "sqlexec.h"
#include "../common/common.h"

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
 * One top-level conjunct from a WHERE tree plus the source mask it
 * depends on. ready_depth is the highest source index referenced by
 * the term, so nested-loop scans can evaluate it as soon as that
 * source has been bound.
 */
typedef struct where_term {
    unsigned char ref;
    unsigned char source_mask;
    unsigned char ready_depth;
} where_term;

/*
 * One executor-bound field reference inside a WHERE tree. The binding
 * replaces repeated qualifier/name lookups with direct source and field
 * slots.
 */
typedef struct where_bound_field {
    unsigned char source_index;
    unsigned char field_index;
} where_bound_field;

/*
 * Bound descriptor for one WHERE tree. left[] stores the bound field for
 * each node's left operand. values[] stores one bound field per predicate
 * operand slot when that slot is a column reference.
 */
typedef struct where_binding {
    where_bound_field left[sql_where_max_nodes];
    where_bound_field values[sql_where_max_values];
} where_binding;

/*
 * Returns 1 when the optimized program already carries exact packed
 * field-slot bindings for where, so bound evaluation may pass a NULL
 * binding pointer and consume the program slots directly.
 */
int where_can_use_program_binding(const sqlexec_program *program,
    const sql_where *where);

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
 * Returns SQL three-valued NOT semantics.
 */
sql_truth_value truth_not_value(sql_truth_value value);

/*
 * Returns SQL three-valued AND semantics.
 */
sql_truth_value truth_and_value(sql_truth_value left,
    sql_truth_value right);

/*
 * Returns SQL three-valued OR semantics.
 */
sql_truth_value truth_or_value(sql_truth_value left,
    sql_truth_value right);

/*
 * Compares two trimmed text values with numeric comparison when both
 * logical types are numeric. Blank values produce UNKNOWN.
 */
sql_truth_value compare_text_values_truth(const char *left, char left_type,
    const char *right, char right_type, sql_compare_operator op);

/*
 * Returns SQL LIKE truth semantics for one trimmed text value against
 * one SQL string pattern value. Blank values or NULL produce UNKNOWN.
 */
sql_truth_value text_matches_like_truth(const char *left,
    const sql_value *value);

/*
 * Returns SQL LIKE truth semantics for one trimmed text value against
 * one already-trimmed pattern text.
 */
sql_truth_value text_matches_like_text_truth(const char *left,
    const char *pattern);

/*
 * Returns three-valued comparison semantics for one field/value test.
 * The result is TRUE, FALSE, or UNKNOWN when NULL participates.
 */
sql_truth_value field_matches_value_truth(const char *left, char field_type,
    const sql_value *value, sql_compare_operator op);

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
 * Binds every column reference in where to direct source and field
 * indexes. Returns zero on success and -1 on invalid references.
 */
int where_bind_n(const sqlexec_program *program, const sql_where *where,
    const row_source *sources, unsigned char source_count,
    where_binding *binding);

/*
 * Returns 1 when every column referenced by where is present in the
 * supplied row sources, 0 otherwise. Legacy convenience wrapper around
 * where_bind_n().
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
 * Returns 1 when the WHERE tree root is the constant FALSE node.
 * Returns 0 for every other shape, including inactive WHERE clauses.
 */
int where_is_constant_false(const sql_where *where,
    const sql_where_node *nodes);

/*
 * Evaluates the WHERE tree against N row sources. Returns 1 only when
 * the final truth value is TRUE. UNKNOWN behaves as non-match. binding
 * may be NULL when where_can_use_program_binding() is true.
 */
int where_matches_bound_n(const sqlexec_program *program,
    const sql_where *where, const where_binding *binding,
    const row_source *sources,
    const sql_predicate_subquery_cache *subqueries);

/*
 * Evaluates one bound WHERE subtree by ref against N row sources.
 * Returns 1 only when the subtree truth value is TRUE. binding may be
 * NULL when where_can_use_program_binding() is true.
 */
int where_matches_ref_bound_n(const sqlexec_program *program,
    const sql_where *where, unsigned char ref,
    const where_binding *binding, const row_source *sources,
    const sql_predicate_subquery_cache *subqueries);

/*
 * Evaluates the WHERE tree against N row sources. Returns 1 only when
 * the final truth value is TRUE. UNKNOWN behaves as non-match.
 * Legacy convenience wrapper around where_bind_n() and the bound path.
 */
int where_matches_n(const sqlexec_program *program, const sql_where *where,
    const row_source *sources, unsigned char source_count,
    const sql_predicate_subquery_cache *subqueries);

/*
 * Evaluates one WHERE subtree by ref against N row sources. Returns 1
 * only when the subtree truth value is TRUE.
 */
int where_matches_ref_n(const sqlexec_program *program, const sql_where *where,
    unsigned char ref, const row_source *sources, unsigned char source_count,
    const sql_predicate_subquery_cache *subqueries);

/*
 * Flattens the top-level AND structure of where into conjunct terms and
 * computes the row-source mask for each term.
 * Returns zero on success and -1 on invalid references or overflow.
 * binding may be NULL when the optimized program already carries the
 * exact source masks for where.
 */
int where_split_conjuncts_bound(const sqlexec_program *program,
    const sql_where *where, const where_binding *binding,
    where_term *terms, unsigned char *term_count_out);

/*
 * Flattens the top-level AND structure of where into conjunct terms and
 * computes the row-source mask for each term.
 * Returns zero on success and -1 on invalid references or overflow.
 * Legacy convenience wrapper around where_bind_n().
 */
int where_split_conjuncts_n(const sqlexec_program *program,
    const sql_where *where, const row_source *sources,
    unsigned char source_count, where_term *terms,
    unsigned char *term_count_out);

/*
 * Two-source convenience wrapper around where_matches_n.
 */
int where_matches(const sqlexec_program *program, const sql_where *where,
    const row_source *left_source, const row_source *right_source,
    const sql_predicate_subquery_cache *subqueries);

#endif
