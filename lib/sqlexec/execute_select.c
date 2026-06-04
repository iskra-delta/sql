/*
 * Executes SELECT statements, including COUNT(*), table scans, index
 * scans, joins, WHERE filtering, and column projection.
 *
 * MIT License (see: LICENSE)
 * Copyright (C) 2026 tomaz stih
 */

#include "exec_impl.h"
#include "../tran/tran.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Output helpers (local to this module)                               */
/* ------------------------------------------------------------------ */

static void sel_write_char(const sqlexec_env *env, char c)
{
    if (env->io && env->io->write_char) {
        env->io->write_char(c);
    }
}

static void sel_write_nl(const sqlexec_env *env)
{
    sel_write_char(env, '\r');
    sel_write_char(env, '\n');
}

static void sel_write_str(const sqlexec_env *env, const char *s)
{
    while (*s) {
        sel_write_char(env, *s++);
    }
}

static void sel_write_uint(const sqlexec_env *env, unsigned short n)
{
    char buf[6];
    uint_to_str(n, buf);
    sel_write_str(env, buf);
}

static void long_to_text(long value, char *buf)
{
    unsigned short index;
    unsigned short left;
    unsigned short right;
    unsigned long magnitude;
    char tmp;

    if (value == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }

    index = 0;
    magnitude = value < 0 ? (unsigned long)(-value) : (unsigned long)value;
    while (magnitude > 0UL && index + 1 < sql_value_size) {
        buf[index++] = (char)('0' + (magnitude % 10UL));
        magnitude /= 10UL;
    }
    if (value < 0 && index + 1 < sql_value_size) {
        buf[index++] = '-';
    }
    buf[index] = '\0';

    left = 0;
    right = index == 0 ? 0 : (unsigned short)(index - 1);
    while (left < right) {
        tmp = buf[left];
        buf[left] = buf[right];
        buf[right] = tmp;
        left++;
        right--;
    }
}

/* ------------------------------------------------------------------ */
/* Tree navigation helpers                                              */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Row output                                                           */
/* ------------------------------------------------------------------ */

static int write_output_values(const sqlexec_env *env,
    char output_values[sql_max_columns][sql_value_size],
    unsigned short output_count)
{
    unsigned short index;

    for (index = 0; index < output_count; index++) {
        if (index > 0) {
            sel_write_str(env, " | ");
        }
        sel_write_str(env, output_values[index]);
    }
    sel_write_nl(env);
    return 0;
}

static int emit_output_values(const sqlexec_env *env,
    const char output_types[sql_max_columns],
    char output_values[sql_max_columns][sql_value_size],
    unsigned short output_count)
{
    if (env->temp) {
        return exec_append_output_values_to_temp(env->temp, output_values,
            output_count);
    }
    if (env->collect) {
        return exec_collect_output_values(env->collect, output_values,
            output_types, output_count);
    }
    return write_output_values(env, output_values, output_count);
}

static int emit_projected_row(const sqlexec_env *env,
    const exec_project_binding *binding, const row_source *sources)
{
    char output_values[sql_max_columns][sql_value_size];
    unsigned short output_count;

    if (exec_build_project_output_values_bound(binding, sources,
        output_values, &output_count) != 0) {
        sel_write_str(env, "error");
        sel_write_nl(env);
        return -1;
    }
    return emit_output_values(env, binding->output_types, output_values,
        output_count);
}

static int project_has_functions(const sqlexec_project_def *project)
{
    unsigned short index;

    for (index = 0; index < project->names.count; index++) {
        if (project->functions[index] != sql_function_none) {
            return 1;
        }
    }
    return 0;
}

static int project_requires_grouping(const sqlexec_project_def *project)
{
    return project->has_aggregate
        || project->group_names.count > 0;
}

static sql_truth_value having_node_matches(const sqlexec_env *env,
    const exec_project_binding *binding, unsigned char ref,
    char output_values[sql_max_columns][sql_value_size],
    const sql_predicate_subquery_cache *subqueries)
{
    const sql_where_node *having_nodes;
    const sql_predicate_operand *having_values;
    const sql_where_node *node;
    const sql_predicate_operand *operand;
    const sql_predicate_subquery_result *subquery_result;
    int output_index;
    int right_index;
    sql_truth_value truth;
    int saw_unknown;
    unsigned short value_index;

    having_nodes = program_having_nodes(env->program);
    having_values = program_having_values(env->program);
    node = &having_nodes[ref];
    switch (node->type) {
    case sql_where_false:
        return sql_truth_false;
    case sql_where_compare:
        output_index = binding->having_left_output[ref];
        if (output_index < 0) {
            return 0;
        }
        operand = &having_values[node->value_first];
        if (operand->kind == sql_predicate_operand_column) {
            right_index = binding->having_value_output[node->value_first];
            if (right_index >= 0) {
                return compare_text_values_truth(
                    output_values[output_index],
                    binding->output_types[output_index],
                    output_values[right_index],
                    binding->output_types[right_index],
                    node->operator);
            }
            return sql_truth_false;
        }
        return field_matches_value_truth(output_values[output_index],
            binding->output_types[output_index], &operand->data.value,
            node->operator);
    case sql_where_in:
        output_index = binding->having_left_output[ref];
        if (output_index < 0) {
            return 0;
        }
        saw_unknown = 0;
        for (right_index = 0; right_index < node->value_count; right_index++) {
            operand = &having_values[node->value_first
                + right_index];
            if (operand->kind == sql_predicate_operand_column) {
                value_index = (unsigned short)
                    binding->having_value_output[node->value_first
                        + right_index];
                if (value_index >= binding->output_count) {
                    return sql_truth_false;
                }
                truth = compare_text_values_truth(
                    output_values[output_index],
                    binding->output_types[output_index],
                    output_values[value_index],
                    binding->output_types[value_index],
                    sql_compare_equal);
            } else {
                truth = field_matches_value_truth(output_values[output_index],
                    binding->output_types[output_index],
                    &operand->data.value, sql_compare_equal);
            }
            if (truth == sql_truth_true) {
                return sql_truth_true;
            }
            if (truth == sql_truth_unknown) {
                saw_unknown = 1;
            }
        }
        return saw_unknown ? sql_truth_unknown : sql_truth_false;
    case sql_where_like:
        output_index = binding->having_left_output[ref];
        if (output_index < 0) {
            return sql_truth_false;
        }
        operand = &having_values[node->value_first];
        if (operand->kind == sql_predicate_operand_column) {
            value_index = (unsigned short)
                binding->having_value_output[node->value_first];
            if (value_index >= binding->output_count
                || output_values[output_index][0] == '\0'
                || output_values[value_index][0] == '\0') {
                return sql_truth_unknown;
            }
            return text_matches_like_text_truth(
                output_values[output_index],
                output_values[value_index]);
        }
        return text_matches_like_truth(output_values[output_index],
            &operand->data.value);
    case sql_where_is_null:
        output_index = binding->having_left_output[ref];
        if (output_index < 0) {
            return sql_truth_false;
        }
        if (node->operator == sql_compare_not_equal) {
            return output_values[output_index][0] == '\0'
                ? sql_truth_false : sql_truth_true;
        }
        return output_values[output_index][0] == '\0'
            ? sql_truth_true : sql_truth_false;
    case sql_where_quantified:
        output_index = binding->having_left_output[ref];
        if (output_index < 0 || !subqueries
            || node->subquery_index >= subqueries->count) {
            return sql_truth_false;
        }
        subquery_result = &subqueries->results[node->subquery_index];
        if (node->quantifier == sql_quantifier_any
            && subquery_result->row_count == 0) {
            return sql_truth_false;
        }
        if (node->quantifier == sql_quantifier_all
            && subquery_result->row_count == 0) {
            return sql_truth_true;
        }
        saw_unknown = 0;
        for (value_index = 0; value_index < subquery_result->row_count;
            value_index++) {
            truth = field_matches_value_truth(output_values[output_index],
                binding->output_types[output_index],
                &subquery_result->values[value_index], node->operator);
            if (node->quantifier == sql_quantifier_any) {
                if (truth == sql_truth_true) {
                    return sql_truth_true;
                }
            } else if (truth == sql_truth_false) {
                return sql_truth_false;
            }
            if (truth == sql_truth_unknown) {
                saw_unknown = 1;
            }
        }
        if (saw_unknown) {
            return sql_truth_unknown;
        }
        return node->quantifier == sql_quantifier_any
            ? sql_truth_false : sql_truth_true;
    case sql_where_exists:
        if (!subqueries || node->subquery_index >= subqueries->count) {
            return sql_truth_false;
        }
        return subqueries->results[node->subquery_index].row_count > 0
            ? sql_truth_true : sql_truth_false;
    case sql_where_not:
        return truth_not_value(having_node_matches(env, binding,
            node->left, output_values, subqueries));
    case sql_where_and:
        return truth_and_value(
            having_node_matches(env, binding, node->left,
                output_values, subqueries),
            having_node_matches(env, binding, node->right,
                output_values, subqueries));
    case sql_where_or:
        return truth_or_value(
            having_node_matches(env, binding, node->left,
                output_values, subqueries),
            having_node_matches(env, binding, node->right,
                output_values, subqueries));
    default:
        return sql_truth_false;
    }
}

static int having_matches_output(const sqlexec_env *env,
    const exec_project_binding *binding,
    char output_values[sql_max_columns][sql_value_size],
    const sql_predicate_subquery_cache *subqueries)
{
    if (!env->program->having.active) {
        return 1;
    }
    return having_node_matches(env, binding, env->program->having.root,
        output_values, subqueries)
        == sql_truth_true;
}

typedef struct select_group {
    unsigned char used;
    char key_values[sql_max_columns][sql_value_size];
    char min_values[sql_max_columns][sql_value_size];
    char max_values[sql_max_columns][sql_value_size];
    long accum_values[sql_max_columns];
    unsigned short count_values[sql_max_columns];
    unsigned short avg_counts[sql_max_columns];
    unsigned char min_seen[sql_max_columns];
    unsigned char max_seen[sql_max_columns];
    unsigned char sum_seen[sql_max_columns];
} select_group;

static int extract_group_key_values(const exec_project_binding *binding,
    const row_source *sources,
    char key_values[sql_max_columns][sql_value_size])
{
    unsigned short index;
    const row_source *source;
    unsigned char field_index;

    for (index = 0; index < binding->group_count; index++) {
        if (binding->groups[index].source_index == 0xffu
            || binding->groups[index].field_index == 0xffu) {
            return -1;
        }
        source = &sources[binding->groups[index].source_index];
        field_index = binding->groups[index].field_index;
        trim_field_value(key_values[index], sql_value_size,
            source->record + source->offsets[field_index],
            source->fields[field_index].length);
    }
    return 0;
}

static int extract_bound_input_value(const exec_bound_field *field,
    char input_type, const row_source *sources,
    char value_out[sql_value_size], char *type_out)
{
    const row_source *source;

    if (field->source_index == 0xffu || field->field_index == 0xffu) {
        return -1;
    }
    source = &sources[field->source_index];
    if (field->field_index >= source->field_count) {
        return -1;
    }
    trim_field_value(value_out, sql_value_size,
        source->record + source->offsets[field->field_index],
        source->fields[field->field_index].length);
    *type_out = input_type;
    return 0;
}

static int select_greater_than(const char *left, char type,
    const char *right)
{
    return compare_text_values_truth(left, type, right, type,
        sql_compare_greater) == sql_truth_true;
}

static int select_less_than(const char *left, char type, const char *right)
{
    return compare_text_values_truth(left, type, right, type,
        sql_compare_less) == sql_truth_true;
}

static int find_or_create_group(const exec_project_binding *binding,
    const row_source *sources,
    select_group groups[sql_max_groups])
{
    char key_values[sql_max_columns][sql_value_size];
    unsigned short group_index;
    unsigned short key_index;
    int same;

    if (binding->group_count == 0) {
        if (!groups[0].used) {
            memset(&groups[0], 0, sizeof(groups[0]));
            groups[0].used = 1;
        }
        return 0;
    }

    if (extract_group_key_values(binding, sources, key_values) != 0) {
        return -1;
    }

    for (group_index = 0; group_index < sql_max_groups; group_index++) {
        if (!groups[group_index].used) {
            continue;
        }
        same = 1;
        for (key_index = 0; key_index < binding->group_count;
            key_index++) {
            if (strcmp(groups[group_index].key_values[key_index],
                key_values[key_index]) != 0) {
                same = 0;
                break;
            }
        }
        if (same) {
            return (int)group_index;
        }
    }

    for (group_index = 0; group_index < sql_max_groups; group_index++) {
        if (!groups[group_index].used) {
            memset(&groups[group_index], 0, sizeof(groups[group_index]));
            groups[group_index].used = 1;
            for (key_index = 0; key_index < binding->group_count;
                key_index++) {
                exec_copy_value_text(
                    groups[group_index].key_values[key_index],
                    key_values[key_index]);
            }
            return (int)group_index;
        }
    }

    return -1;
}

static int update_group_from_row(const exec_project_binding *binding,
    const row_source *sources, select_group *group)
{
    const exec_group_aggregate *aggregate;
    char value[sql_value_size];
    char value_type;
    unsigned short aggregate_index;
    unsigned char output_index;
    int ok;
    long number;

    for (aggregate_index = 0;
        aggregate_index < binding->aggregate_count; aggregate_index++) {
        aggregate = &binding->aggregates[aggregate_index];
        output_index = aggregate->output_index;
        if (aggregate->kind == exec_group_output_count) {
            group->count_values[output_index]++;
            continue;
        }
        if (extract_bound_input_value(&aggregate->input,
            aggregate->input_type, sources, value, &value_type) != 0) {
            return -1;
        }
        if (aggregate->kind == exec_group_output_min) {
            if (!group->min_seen[output_index]
                || select_less_than(value, value_type,
                    group->min_values[output_index])) {
                exec_copy_value_text(group->min_values[output_index], value);
                group->min_seen[output_index] = 1;
            }
            continue;
        }
        if (aggregate->kind == exec_group_output_max) {
            if (!group->max_seen[output_index]
                || select_greater_than(value, value_type,
                    group->max_values[output_index])) {
                exec_copy_value_text(group->max_values[output_index], value);
                group->max_seen[output_index] = 1;
            }
            continue;
        }
        number = parse_integer_text(value, &ok);
        if (!ok) {
            return -1;
        }
        if (aggregate->kind == exec_group_output_sum) {
            group->accum_values[output_index] += number;
            group->sum_seen[output_index] = 1;
            continue;
        }
        if (aggregate->kind != exec_group_output_avg) {
            return -1;
        }
        group->accum_values[output_index] += number;
        group->avg_counts[output_index]++;
    }
    return 0;
}

static int build_group_output_values(const exec_project_binding *binding,
    const select_group *group,
    char output_values[sql_max_columns][sql_value_size])
{
    unsigned short index;
    int group_index;

    for (index = 0; index < binding->output_count; index++) {
        output_values[index][0] = '\0';
        switch (binding->group_output_kinds[index]) {
        case exec_group_output_key:
            group_index = binding->group_output_slots[index];
            if (group_index < 0) {
                return -1;
            }
            exec_copy_value_text(output_values[index],
                group->key_values[group_index]);
            break;
        case exec_group_output_count:
            long_to_text((long)group->count_values[index], output_values[index]);
            break;
        case exec_group_output_min:
            if (!group->min_seen[index]) {
                output_values[index][0] = '\0';
                break;
            }
            exec_copy_value_text(output_values[index],
                group->min_values[index]);
            break;
        case exec_group_output_max:
            if (!group->max_seen[index]) {
                output_values[index][0] = '\0';
                break;
            }
            exec_copy_value_text(output_values[index],
                group->max_values[index]);
            break;
        case exec_group_output_sum:
            if (!group->sum_seen[index]) {
                output_values[index][0] = '\0';
                break;
            }
            long_to_text(group->accum_values[index], output_values[index]);
            break;
        case exec_group_output_avg:
            if (group->avg_counts[index] == 0) {
                output_values[index][0] = '\0';
                break;
            }
            long_to_text(group->accum_values[index]
                / (long)group->avg_counts[index], output_values[index]);
            break;
        default:
            return -1;
        }
    }
    return 0;
}

typedef struct select_distinct_row {
    unsigned char used;
    char values[sql_max_columns][sql_value_size];
} select_distinct_row;

static int output_rows_equal(
    char left[sql_max_columns][sql_value_size],
    char right[sql_max_columns][sql_value_size], unsigned short output_count)
{
    unsigned short index;

    for (index = 0; index < output_count; index++) {
        if (strcmp(left[index], right[index]) != 0) {
            return 0;
        }
    }
    return 1;
}

static int distinct_row_is_new(
    select_distinct_row seen_rows[sql_max_groups],
    char output_values[sql_max_columns][sql_value_size],
    unsigned short output_count)
{
    unsigned short row_index;
    unsigned short value_index;

    for (row_index = 0; row_index < sql_max_groups; row_index++) {
        if (!seen_rows[row_index].used) {
            continue;
        }
        if (output_rows_equal(seen_rows[row_index].values, output_values,
            output_count)) {
            return 0;
        }
    }

    for (row_index = 0; row_index < sql_max_groups; row_index++) {
        if (!seen_rows[row_index].used) {
            seen_rows[row_index].used = 1;
            for (value_index = 0; value_index < output_count; value_index++) {
                exec_copy_value_text(seen_rows[row_index].values[value_index],
                    output_values[value_index]);
            }
            return 1;
        }
    }

    return -1;
}

static int where_terms_match_depth(const sqlexec_env *env,
    const sql_where *where, const row_source *sources,
    const where_binding *where_binding, unsigned char source_count,
    const where_term *terms,
    unsigned char term_count, unsigned char depth,
    const sql_predicate_subquery_cache *subqueries)
{
    unsigned char index;

    for (index = 0; index < term_count; index++) {
        if (terms[index].ready_depth != depth) {
            continue;
        }
        if (!where_matches_ref_bound_n(env->program, where,
            terms[index].ref, where_binding, sources, subqueries)) {
            return 0;
        }
    }
    (void)source_count;
    return 1;
}

static int accumulate_group_row(const sqlexec_env *env,
    const exec_project_binding *binding,
    const sql_where *where, const where_binding *where_binding,
    const row_source *sources,
    unsigned char source_count,
    select_group groups[sql_max_groups],
    const sql_predicate_subquery_cache *subqueries)
{
    int group_index;

    if (!where_matches_bound_n(env->program, where, where_binding, sources,
        subqueries)) {
        return 0;
    }
    group_index = find_or_create_group(binding, sources, groups);
    if (group_index < 0) {
        return -1;
    }
    (void)source_count;
    return update_group_from_row(binding, sources, &groups[group_index]);
}

static int scan_group_rows(const sqlexec_env *env,
    const exec_project_binding *binding,
    const sql_where *where, const where_binding *where_binding,
    dbf_file *files, row_source *sources, unsigned char source_count,
    char **records, unsigned char depth,
    select_group *groups,
    const where_term *where_terms, unsigned char where_term_count,
    const sql_predicate_subquery_cache *subqueries)
{
    unsigned long index;
    int state;

    for (index = 0; index < files[depth].record_count; index++) {
        int ov;
        state = dbf_read(&files[depth], index, records[depth]);
        if (state < 0) return -1;
        if (state == 1) continue;
        ov = txn_overlay_record(env, sources[depth].table_name, index,
            records[depth], files[depth].record_length);
        if (ov < 0) return -1;
        if (ov == 1) continue;
        if (!where_terms_match_depth(env, where, sources, where_binding,
            source_count, where_terms, where_term_count, depth,
            subqueries)) {
            continue;
        }
        if (depth + 1u == source_count) {
            if (accumulate_group_row(env, binding, where,
                where_binding, sources, source_count, groups,
                subqueries) != 0) {
                return -1;
            }
        } else if (scan_group_rows(env, binding, where,
            where_binding, files, sources, source_count, records,
            (unsigned char)(depth + 1u), groups, where_terms,
            where_term_count, subqueries) != 0) {
            return -1;
        }
    }
    return 0;
}

static int emit_grouped_rows(const sqlexec_env *env,
    const exec_project_binding *binding, const sqlexec_project_def *project,
    const sql_where *where, const where_binding *where_binding,
    dbf_file *files, row_source *sources, unsigned char source_count,
    char **records,
    const where_term *where_terms, unsigned char where_term_count,
    unsigned short *row_count,
    const sql_predicate_subquery_cache *subqueries)
{
    select_group *groups;
    select_distinct_row *seen_rows;
    char output_values[sql_max_columns][sql_value_size];
    unsigned short group_index;
    int distinct_state;

    groups = (select_group *)calloc(sql_max_groups, sizeof(select_group));
    if (!groups)
        return -1;
    seen_rows = (select_distinct_row *)calloc(
        sql_max_groups, sizeof(select_distinct_row));
    if (!seen_rows) {
        free(groups);
        return -1;
    }

    if (scan_group_rows(env, binding, where, where_binding, files,
        sources, source_count, records, 0, groups, where_terms,
        where_term_count, subqueries) != 0) {
        free(seen_rows);
        free(groups);
        return -1;
    }
    if (!groups[0].used && project->has_aggregate
        && project->group_names.count == 0) {
        groups[0].used = 1;
    }

    *row_count = 0;
    for (group_index = 0; group_index < sql_max_groups; group_index++) {
        if (!groups[group_index].used) {
            continue;
        }
        if (build_group_output_values(binding, &groups[group_index],
            output_values) != 0) {
            free(seen_rows);
            free(groups);
            return -1;
        }
        if (!having_matches_output(env, binding, output_values,
            subqueries)) {
            continue;
        }
        if (project->distinct) {
            distinct_state = distinct_row_is_new(seen_rows, output_values,
                binding->output_count);
            if (distinct_state < 0) {
                free(seen_rows);
                free(groups);
                return -1;
            }
            if (!distinct_state) {
                continue;
            }
        }
        if (emit_output_values(env, binding->output_types, output_values,
            binding->output_count) != 0) {
            free(seen_rows);
            free(groups);
            return -1;
        }
        (*row_count)++;
    }

    free(seen_rows);
    free(groups);
    return 0;
}

static int emit_distinct_match(const sqlexec_env *env,
    const exec_project_binding *binding, const sql_where *where,
    const where_binding *where_binding,
    const row_source *sources, unsigned char source_count,
    select_distinct_row seen_rows[sql_max_groups],
    unsigned short *row_count,
    const sql_predicate_subquery_cache *subqueries)
{
    char output_values[sql_max_columns][sql_value_size];
    unsigned short output_count;
    int distinct_state;

    if (!where_matches_bound_n(env->program, where, where_binding, sources,
        subqueries)) {
        return 0;
    }
    if (exec_build_project_output_values_bound(binding, sources,
        output_values, &output_count) != 0) {
        return -1;
    }
    distinct_state = distinct_row_is_new(seen_rows, output_values,
        output_count);
    if (distinct_state < 0) {
        return -1;
    }
    if (!distinct_state) {
        return 0;
    }
    (void)source_count;
    if (emit_projected_row(env, binding, sources) != 0) {
        return -1;
    }
    (*row_count)++;
    return 0;
}

static int scan_distinct_rows(const sqlexec_env *env,
    const exec_project_binding *binding, const sql_where *where,
    const where_binding *where_binding,
    dbf_file *files, row_source *sources, unsigned char source_count,
    char **records, unsigned char depth,
    select_distinct_row *seen_rows, unsigned short *row_count,
    const where_term *where_terms, unsigned char where_term_count,
    const sql_predicate_subquery_cache *subqueries)
{
    unsigned long index;
    int state;

    for (index = 0; index < files[depth].record_count; index++) {
        int ov;
        state = dbf_read(&files[depth], index, records[depth]);
        if (state < 0) return -1;
        if (state == 1) continue;
        ov = txn_overlay_record(env, sources[depth].table_name, index,
            records[depth], files[depth].record_length);
        if (ov < 0) return -1;
        if (ov == 1) continue;
        if (!where_terms_match_depth(env, where, sources, where_binding,
            source_count, where_terms, where_term_count, depth,
            subqueries)) {
            continue;
        }
        if (depth + 1u == source_count) {
            if (emit_distinct_match(env, binding, where, where_binding,
                sources,
                source_count, seen_rows, row_count, subqueries) != 0) {
                return -1;
            }
        } else if (scan_distinct_rows(env, binding, where, where_binding,
            files, sources, source_count, records,
            (unsigned char)(depth + 1u), seen_rows, row_count, where_terms,
            where_term_count, subqueries) != 0) {
            return -1;
        }
    }
    return 0;
}

static int emit_distinct_rows(const sqlexec_env *env,
    const exec_project_binding *binding, const sql_where *where,
    const where_binding *where_binding,
    dbf_file *files, row_source *sources, unsigned char source_count,
    char **records,
    const where_term *where_terms, unsigned char where_term_count,
    unsigned short *row_count,
    const sql_predicate_subquery_cache *subqueries)
{
    select_distinct_row *seen_rows;
    int ret;

    seen_rows = (select_distinct_row *)calloc(
        sql_max_groups, sizeof(select_distinct_row));
    if (!seen_rows)
        return -1;
    *row_count = 0;
    ret = scan_distinct_rows(env, binding, where, where_binding, files,
        sources, source_count, records, 0, seen_rows, row_count,
        where_terms, where_term_count, subqueries);
    free(seen_rows);
    return ret;
}

static int emit_matching_row(const sqlexec_env *env,
    const exec_project_binding *binding, int count_only,
    const sql_where *where, const where_binding *where_binding,
    const row_source *sources,
    unsigned char source_count, unsigned short *row_count,
    const sql_predicate_subquery_cache *subqueries)
{
    if (!where_matches_bound_n(env->program, where, where_binding, sources,
        subqueries)) {
        return 0;
    }
    if (!count_only) {
        if (emit_projected_row(env, binding, sources) != 0) {
            return -1;
        }
    }
    (void)source_count;
    (*row_count)++;
    return 0;
}

static int scan_join_rows(const sqlexec_env *env,
    const exec_project_binding *binding, int count_only,
    const sql_where *where, const where_binding *where_binding,
    dbf_file *files,
    row_source *sources, unsigned char source_count,
    char **records, unsigned char depth,
    unsigned short *row_count,
    const where_term *where_terms, unsigned char where_term_count,
    const sql_predicate_subquery_cache *subqueries)
{
    unsigned long index;
    int state;

    for (index = 0; index < files[depth].record_count; index++) {
        int ov;
        state = dbf_read(&files[depth], index, records[depth]);
        if (state < 0) return -1;
        if (state == 1) continue;
        ov = txn_overlay_record(env, sources[depth].table_name, index,
            records[depth], files[depth].record_length);
        if (ov < 0) return -1;
        if (ov == 1) continue;
        if (!where_terms_match_depth(env, where, sources, where_binding,
            source_count, where_terms, where_term_count, depth,
            subqueries)) {
            continue;
        }
        if (depth + 1u == source_count) {
            if (emit_matching_row(env, binding, count_only, where,
                where_binding, sources, source_count, row_count,
                subqueries) != 0) {
                return -1;
            }
        } else if (scan_join_rows(env, binding, count_only, where,
            where_binding, files, sources, source_count,
            records, (unsigned char)(depth + 1u), row_count, where_terms,
            where_term_count, subqueries) != 0) {
            return -1;
        }
    }
    return 0;
}

static int close_sources(dbf_file *files, unsigned char count)
{
    unsigned char index;

    for (index = count; index > 0; index--) {
        if (dbf_close(&files[index - 1u]) != 0) {
            return -1;
        }
    }
    return 0;
}

static int close_and_free_sources(dbf_file *files, char **records,
    dbf_field **fields, unsigned char count)
{
    unsigned char index;

    for (index = 0; index < count; index++) {
        free(records[index]);
        free(fields[index]);
    }
    return close_sources(files, count);
}

/* ------------------------------------------------------------------ */
/* Transaction INSERT visibility callback                               */
/* ------------------------------------------------------------------ */

typedef struct sel_insert_ctx {
    const sqlexec_env     *env;
    const exec_project_binding *binding;
    const sql_where       *where;
    const where_binding   *where_binding;
    const row_source      *source;
    unsigned short        *row_count;
    const sql_predicate_subquery_cache *subqueries;
    unsigned short         record_length;
    int                    count_only;
} sel_insert_ctx;

static int sel_insert_cb(unsigned long virtual_recno,
    const char *record, unsigned short record_length, void *cb_ctx)
{
    sel_insert_ctx *c = (sel_insert_ctx *)cb_ctx;
    row_source vsrc;
    (void)virtual_recno;
    (void)record_length;

    /* Temporarily point the source record at the virtual row. */
    memcpy((void *)&vsrc, c->source, sizeof(row_source));
    vsrc.record = record;

    if (!where_matches_bound_n(c->env->program, c->where,
        c->where_binding, &vsrc, c->subqueries))
        return 0;

    if (!c->count_only) {
        if (emit_projected_row(c->env, c->binding, &vsrc) != 0)
            return -1;
    }
    (*c->row_count)++;
    return 0;
}

/* ------------------------------------------------------------------ */
/* SELECT executor                                                      */
/* ------------------------------------------------------------------ */

int exec_select(sqlexec_env *env, const char *table_name,
    sqlexec_ref plan_ref)
{
    const sqlexec_node *plan_node;
    const sqlexec_node *project_node;
    const sqlexec_node *scan_node;
    const sqlexec_join_def *join_def;
    dbf_file files[sql_max_sources];
    dbf_field *source_fields[sql_max_sources];
    unsigned short source_offsets[sql_max_sources][sql_max_columns];
    char *source_records[sql_max_sources];
    sql_where where;
    row_source sources[sql_max_sources];
    where_binding *wb_store;
    const where_binding *where_binding;
    where_term *where_terms;
    unsigned short row_count;
    sqlexec_ref scan_ref;
    sqlexec_ref child_ref;
    unsigned char source_count;
    unsigned char source_index;
    unsigned char opened_source_count;
    unsigned char where_term_count;
    exec_project_binding *binding;
    exec_index_scan_ctx scan_ctx;
    sql_predicate_subquery_cache *psc;
    int count_only;
    int grouped_select;
    int special_projection;

    plan_node = sqlexec_get_const(env->program, plan_ref);
    if (!plan_node) {
        return -1;
    }
    memset(files, 0, sizeof(files));
    memset(sources, 0, sizeof(sources));
    memset(source_records, 0, sizeof(source_records));
    memset(source_fields, 0, sizeof(source_fields));
    wb_store = NULL;
    where_binding = NULL;
    where_terms = NULL;
    binding = NULL;
    psc = NULL;
    source_count = 1;
    opened_source_count = 0;
    where_term_count = 0;

    source_fields[0] = (dbf_field *)malloc(sql_max_columns * sizeof(dbf_field));
    if (!source_fields[0]) {
        return -1;
    }
    if (open_table_file(env->root, env->current_db, table_name, &files[0],
        source_fields[0], source_offsets[0]) != 0) {
        free(source_fields[0]);
        source_fields[0] = NULL;
        return -1;
    }
    source_records[0] = (char *)malloc(files[0].record_length);
    if (!source_records[0]) {
        close_sources(files, 1);
        free(source_fields[0]);
        return -1;
    }
    opened_source_count = 1;

/* Free all heap-allocated select buffers and close sources. */
#define SEL_FAIL(n) do { \
    free(where_terms); free(binding); free(wb_store); free(psc); \
    close_and_free_sources(files, source_records, source_fields, (n)); \
    return -1; \
} while (0)

    count_only = plan_node->opcode != sqlexec_project;
    if (!count_only) {
        project_node = plan_node;
        child_ref = child_at(env->program, plan_ref, 0);
        if (resolve_scan_node(env->program, child_ref,
            &where, &scan_ref) != 0) {
            SEL_FAIL(opened_source_count);
        }
    } else {
        if (resolve_scan_node(env->program, plan_ref, &where,
            &scan_ref) != 0) {
            SEL_FAIL(opened_source_count);
        }
        project_node = NULL;
    }

    scan_node = sqlexec_get_const(env->program, scan_ref);
    if (!scan_node || scan_ref == sqlexec_nil) {
        SEL_FAIL(opened_source_count);
    }

    sources[0].table_name = table_name;
    sources[0].alias = "";
    sources[0].fields = source_fields[0];
    sources[0].offsets = source_offsets[0];
    sources[0].record = source_records[0];
    sources[0].field_count = files[0].field_count;
    join_def = NULL;
    if (scan_node->opcode == sqlexec_join_scan) {
        join_def = &scan_node->data.join;
        source_count = join_source_count(*join_def);
        if (source_count < 2 || source_count > sql_max_sources) {
            SEL_FAIL(opened_source_count);
        }
        sources[0].table_name = join_table_at(env->program, *join_def, 0);
        sources[0].alias = join_alias_at(env->program, *join_def, 0);
        for (source_index = 1; source_index < source_count; source_index++) {
            source_fields[source_index] = (dbf_field *)malloc(
                sql_max_columns * sizeof(dbf_field));
            if (!source_fields[source_index]) {
                SEL_FAIL(opened_source_count);
            }
            if (open_table_file(env->root, env->current_db,
                join_table_at(env->program, *join_def, source_index),
                &files[source_index], source_fields[source_index],
                source_offsets[source_index]) != 0) {
                /* source_fields[source_index] freed by SEL_FAIL */
                SEL_FAIL(opened_source_count);
            }
            opened_source_count++;
            source_records[source_index] = (char *)malloc(
                files[source_index].record_length);
            if (!source_records[source_index]) {
                SEL_FAIL(opened_source_count);
            }
            sources[source_index].table_name = join_table_at(env->program,
                *join_def, source_index);
            sources[source_index].alias = join_alias_at(env->program,
                *join_def, source_index);
            sources[source_index].fields = source_fields[source_index];
            sources[source_index].offsets = source_offsets[source_index];
            sources[source_index].record = source_records[source_index];
            sources[source_index].field_count = files[source_index].field_count;
        }
    }

    if (where_is_constant_false(&where, env->program->where_nodes)) {
        if (close_and_free_sources(files, source_records, source_fields,
            opened_source_count) != 0) {
            return -1;
        }
        sel_write_uint(env, 0);
        if (!count_only) {
            sel_write_str(env, " rows");
        }
        sel_write_nl(env);
        return 0;
    }

    if (env->program->predicate_subquery_count > 0) {
        psc = (sql_predicate_subquery_cache *)malloc(
            sizeof(sql_predicate_subquery_cache));
        if (!psc) {
            SEL_FAIL(opened_source_count);
        }
        memset(psc, 0, sizeof(*psc));
        if (load_predicate_subqueries(env, psc) != 0) {
            SEL_FAIL(opened_source_count);
        }
    }

    if (!count_only) {
        binding = (exec_project_binding *)malloc(sizeof(exec_project_binding));
        if (!binding) {
            SEL_FAIL(opened_source_count);
        }
        if (exec_bind_project(env->program, &project_node->data.project,
            sources, source_count, binding) != 0) {
            SEL_FAIL(opened_source_count);
        }
    }
    if (!where_can_use_program_binding(env->program, &where)) {
        wb_store = malloc(sizeof(*wb_store));
        if (!wb_store) {
            SEL_FAIL(opened_source_count);
        }
        if (where_bind_n(env->program, &where, sources, source_count,
            wb_store) != 0) {
            SEL_FAIL(opened_source_count);
        }
        where_binding = wb_store;
    }
    where_terms = (where_term *)calloc(sql_where_max_nodes, sizeof(where_term));
    if (!where_terms) {
        SEL_FAIL(opened_source_count);
    }
    if (where_split_conjuncts_bound(env->program, &where, where_binding,
        where_terms, &where_term_count) != 0) {
        SEL_FAIL(opened_source_count);
    }
    grouped_select = !count_only
        && project_requires_grouping(&project_node->data.project);
    special_projection = !count_only
        && (project_has_functions(&project_node->data.project)
            || project_node->data.project.distinct);

    row_count = 0;
    if (grouped_select) {
        if (emit_grouped_rows(env, binding, &project_node->data.project,
            &where, where_binding, files, sources, source_count,
            source_records, where_terms, where_term_count, &row_count,
            psc) != 0) {
            SEL_FAIL(opened_source_count);
        }
    } else if (!count_only && project_node->data.project.distinct) {
        if (emit_distinct_rows(env, binding, &where, where_binding, files,
            sources, source_count, source_records, where_terms,
            where_term_count, &row_count, psc) != 0) {
            SEL_FAIL(opened_source_count);
        }
    } else if (source_count == 1 && scan_uses_index(scan_node)
        && !special_projection) {
        /* Index scan: use NDX to drive record retrieval. */
        memset(&scan_ctx, 0, sizeof(scan_ctx));
        scan_ctx.file = &files[0];
        scan_ctx.fields = source_fields[0];
        scan_ctx.offsets = source_offsets[0];
        scan_ctx.record = source_records[0];
        scan_ctx.source = &sources[0];
        scan_ctx.env = env;
        scan_ctx.where = &where;
        scan_ctx.where_binding = where_binding;
        scan_ctx.action = exec_scan_select;
        scan_ctx.binding = binding;
        scan_ctx.count_only = count_only;
        scan_ctx.row_count = &row_count;
        scan_ctx.subqueries = psc;
        if (exec_run_index_scan(env, scan_node, &scan_ctx) != 0) {
            SEL_FAIL(opened_source_count);
        }
    } else if (scan_join_rows(env, binding,
        count_only, &where, where_binding, files, sources,
        source_count, source_records, 0, &row_count, where_terms,
        where_term_count, psc) != 0) {
        SEL_FAIL(opened_source_count);
    }
    /* Yield pending INSERT rows for single-source non-grouped queries. */
    if (source_count == 1 && !grouped_select
        && env->txn && env->txn->active) {
        sel_insert_ctx ins_ctx;
        ins_ctx.env           = env;
        ins_ctx.binding       = binding;
        ins_ctx.where         = &where;
        ins_ctx.where_binding = where_binding;
        ins_ctx.source        = &sources[0];
        ins_ctx.row_count     = &row_count;
        ins_ctx.subqueries    = psc;
        ins_ctx.record_length = files[0].record_length;
        ins_ctx.count_only    = count_only;
        if (txn_scan_inserts(env, table_name, sel_insert_cb, &ins_ctx) != 0) {
            SEL_FAIL(opened_source_count);
        }
    }
    free(where_terms);
    free(binding);
    free(wb_store);
    free(psc);
    if (close_and_free_sources(files, source_records, source_fields,
        opened_source_count) != 0) {
        return -1;
    }

    sel_write_uint(env, row_count);
    if (!count_only) {
        sel_write_str(env, " row");
        if (row_count != 1) {
            sel_write_char(env, 's');
        }
    }
    sel_write_nl(env);
    return 0;
}
#undef SEL_FAIL
