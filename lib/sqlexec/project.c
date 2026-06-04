/*
 * Implements shared projection and temp-materialization helpers for
 * the executor modules.
 *
 * This keeps column resolution and projected-row copying in one place
 * so SELECT, index-scan output, and subquery materialization do not
 * carry diverging copies of the same logic.
 *
 * MIT License (see: LICENSE)
 * Copyright (C) 2026 tomaz stih
 */

#include "exec_impl.h"

#include <stdlib.h>

static void init_project_binding(exec_project_binding *binding)
{
    unsigned short index;

    memset(binding, 0, sizeof(*binding));
    for (index = 0; index < sql_max_columns; index++) {
        binding->outputs[index].source_index = 0xffu;
        binding->outputs[index].field_index = 0xffu;
        binding->groups[index].source_index = 0xffu;
        binding->groups[index].field_index = 0xffu;
        binding->aggregates[index].kind = exec_group_output_invalid;
        binding->aggregates[index].output_index = 0xffu;
        binding->aggregates[index].input.source_index = 0xffu;
        binding->aggregates[index].input.field_index = 0xffu;
        binding->aggregates[index].input_type = '\0';
        binding->group_lookup[index] = -1;
        binding->group_output_slots[index] = -1;
        binding->group_output_kinds[index] = exec_group_output_invalid;
    }
    for (index = 0; index < sql_where_max_nodes; index++) {
        binding->having_left_output[index] = -1;
    }
    for (index = 0; index < sql_where_max_values; index++) {
        binding->having_value_output[index] = -1;
    }
}

static int project_requires_grouping(const sqlexec_project_def *project)
{
    return project->has_aggregate || project->group_names.count > 0;
}

static const char *project_output_name(const sqlexec_program *program,
    const sqlexec_project_def *project, const row_source *sources,
    unsigned short index)
{
    const char *alias;

    if (project->select_all) {
        unsigned short source_offset;
        unsigned char source_index;

        source_offset = index;
        for (source_index = 0; source_index < sql_max_sources; source_index++) {
            if (!sources[source_index].fields) {
                break;
            }
            if (source_offset < sources[source_index].field_count) {
                return sources[source_index].fields[source_offset].name;
            }
            source_offset = (unsigned short)(source_offset
                - sources[source_index].field_count);
        }
        return NULL;
    }
    alias = program->names[project->aliases.first + index];
    if (alias[0] != '\0') {
        return alias;
    }
    if (project->function_arg_is_star[index]) {
        return "count";
    }
    return program->names[project->names.first + index];
}

static int find_output_index(const sqlexec_program *program,
    const sqlexec_project_def *project, const row_source *sources,
    unsigned short output_count, const char *name)
{
    unsigned short index;
    const char *output_name;

    for (index = 0; index < output_count; index++) {
        output_name = project_output_name(program, project, sources, index);
        if (output_name && strcmp(output_name, name) == 0) {
            return (int)index;
        }
    }
    return -1;
}

static int bind_having_outputs(const sqlexec_program *program,
    const sqlexec_project_def *project, const row_source *sources,
    exec_project_binding *binding)
{
    const sql_where_node *having_nodes;
    const sql_predicate_operand *having_values;
    unsigned char stack[sql_where_max_nodes];
    unsigned char depth;
    unsigned char ref;
    unsigned char index;
    const sql_where_node *node;
    const sql_predicate_operand *operand;

    if (!program->having.active) {
        return 0;
    }
    having_nodes = program_having_nodes(program);
    having_values = program_having_values(program);

    depth = 0;
    stack[depth++] = program->having.root;
    while (depth > 0) {
        ref = stack[--depth];
        node = &having_nodes[ref];
        switch (node->type) {
        case sql_where_false:
            break;
        case sql_where_compare:
        case sql_where_in:
        case sql_where_like:
        case sql_where_is_null:
        case sql_where_quantified:
            binding->having_left_output[ref] = (signed char)
                find_output_index(program, project, sources,
                    binding->output_count, node->column_name);
            if (binding->having_left_output[ref] < 0) {
                return -1;
            }
            for (index = 0; index < node->value_count; index++) {
                if ((unsigned short)(node->value_first + index)
                    >= program->having.value_count) {
                    return -1;
                }
                operand = &having_values[node->value_first + index];
                if (operand->kind != sql_predicate_operand_column) {
                    continue;
                }
                binding->having_value_output[node->value_first + index] =
                    (signed char)find_output_index(program, project, sources,
                        binding->output_count, operand->data.column.name);
                if (binding->having_value_output[node->value_first + index]
                    < 0) {
                    return -1;
                }
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

static int bind_select_all_outputs(const row_source *sources,
    unsigned char source_count, exec_project_binding *binding)
{
    unsigned char source_index;
    unsigned short field_index;
    unsigned short output_index;

    output_index = 0;
    for (source_index = 0; source_index < source_count; source_index++) {
        for (field_index = 0; field_index < sources[source_index].field_count;
            field_index++) {
            if (output_index >= sql_max_columns) {
                return -1;
            }
            binding->outputs[output_index].source_index = source_index;
            binding->outputs[output_index].field_index =
                (unsigned char)field_index;
            binding->output_types[output_index] =
                sources[source_index].fields[field_index].type;
            output_index++;
        }
    }
    binding->output_count = output_index;
    return 0;
}

static int bind_named_outputs(const sqlexec_program *program,
    const sqlexec_project_def *project, const row_source *sources,
    unsigned char source_count, exec_project_binding *binding)
{
    const row_source *source;
    unsigned short index;
    int field_index;
    int group_index;
    int grouped_select;

    binding->output_count = project->names.count;
    if (binding->output_count > sql_max_columns) {
        return -1;
    }

    for (index = 0; index < project->names.count; index++) {
        if (project->function_arg_is_star[index]) {
            if (project->functions[index] != sql_function_count) {
                return -1;
            }
            binding->output_types[index] = 'N';
            continue;
        }
        if (exec_resolve_project_field(program, project, sources,
            source_count, index, &source, &field_index) != 0) {
            return -1;
        }
        binding->outputs[index].source_index = (unsigned char)(source - sources);
        binding->outputs[index].field_index = (unsigned char)field_index;
        if ((project->functions[index] == sql_function_avg
                || project->functions[index] == sql_function_sum)
            && source->fields[field_index].type != 'N') {
            return -1;
        }
        if (project->functions[index] == sql_function_avg
            || project->functions[index] == sql_function_sum
            || project->functions[index] == sql_function_count) {
            binding->output_types[index] = 'N';
        } else if (project->functions[index] == sql_function_trim) {
            binding->output_types[index] = 'C';
        } else {
            binding->output_types[index] = source->fields[field_index].type;
        }
    }

    binding->group_count = project->group_names.count;
    for (index = 0; index < project->group_names.count; index++) {
        if (exec_resolve_group_field(program, project, sources,
            source_count, index, &source, &field_index) != 0) {
            return -1;
        }
        binding->groups[index].source_index = (unsigned char)(source - sources);
        binding->groups[index].field_index = (unsigned char)field_index;
    }

    grouped_select = project_requires_grouping(project);
    for (index = 0; index < project->names.count; index++) {
        if (project->functions[index] != sql_function_none
            && project->functions[index] != sql_function_trim) {
            continue;
        }
        for (group_index = 0; group_index < project->group_names.count;
            group_index++) {
            if (binding->outputs[index].source_index
                    == binding->groups[group_index].source_index
                && binding->outputs[index].field_index
                    == binding->groups[group_index].field_index) {
                binding->group_lookup[index] = (signed char)group_index;
                break;
            }
        }
        if (grouped_select && binding->group_lookup[index] < 0) {
            return -1;
        }
    }

    return 0;
}

static exec_group_output_kind group_output_kind_for_function(
    sql_select_function function)
{
    switch (function) {
    case sql_function_none:
    case sql_function_trim:
        return exec_group_output_key;
    case sql_function_count:
        return exec_group_output_count;
    case sql_function_min:
        return exec_group_output_min;
    case sql_function_max:
        return exec_group_output_max;
    case sql_function_sum:
        return exec_group_output_sum;
    case sql_function_avg:
        return exec_group_output_avg;
    default:
        return exec_group_output_invalid;
    }
}

static int bind_group_runtime(const sqlexec_project_def *project,
    const row_source *sources, exec_project_binding *binding)
{
    exec_group_aggregate *aggregate;
    exec_group_output_kind kind;
    unsigned short index;
    unsigned char source_index;
    unsigned char field_index;

    if (!project_requires_grouping(project)) {
        return 0;
    }

    for (index = 0; index < binding->output_count; index++) {
        kind = group_output_kind_for_function(project->functions[index]);
        if (kind == exec_group_output_invalid) {
            return -1;
        }
        binding->group_output_kinds[index] = kind;
        if (kind == exec_group_output_key) {
            if (binding->group_lookup[index] < 0) {
                return -1;
            }
            binding->group_output_slots[index] = binding->group_lookup[index];
            continue;
        }
        if (binding->aggregate_count >= sql_max_columns) {
            return -1;
        }
        aggregate = &binding->aggregates[binding->aggregate_count++];
        aggregate->kind = kind;
        aggregate->output_index = (unsigned char)index;
        if (kind == exec_group_output_count) {
            continue;
        }
        source_index = binding->outputs[index].source_index;
        field_index = binding->outputs[index].field_index;
        if (source_index == 0xffu || field_index == 0xffu) {
            return -1;
        }
        aggregate->input = binding->outputs[index];
        aggregate->input_type =
            sources[source_index].fields[field_index].type;
    }

    return 0;
}

unsigned short exec_project_output_count(const sqlexec_project_def *project,
    const row_source *sources, unsigned char source_count)
{
    unsigned short count;
    unsigned char index;

    if (!project->select_all) {
        return project->names.count;
    }
    count = 0;
    for (index = 0; index < source_count; index++) {
        count = (unsigned short)(count + sources[index].field_count);
    }
    return count;
}

int exec_resolve_project_field(const sqlexec_program *program,
    const sqlexec_project_def *project, const row_source *sources,
    unsigned char source_count, unsigned short index,
    const row_source **source_out, int *field_index_out)
{
    unsigned char source_index;
    unsigned short source_offset;

    if (project->select_all) {
        source_offset = index;
        for (source_index = 0; source_index < source_count; source_index++) {
            if (source_offset < sources[source_index].field_count) {
                *source_out = &sources[source_index];
                *field_index_out = (int)source_offset;
                return 0;
            }
            source_offset = (unsigned short)(source_offset
                - sources[source_index].field_count);
        }
        return -1;
    }
    return resolve_field_ref_n(sources, source_count,
        program->names[project->qualifiers.first + index],
        program->names[project->names.first + index],
        source_out, field_index_out);
}

int exec_resolve_group_field(const sqlexec_program *program,
    const sqlexec_project_def *project, const row_source *sources,
    unsigned char source_count, unsigned short index,
    const row_source **source_out, int *field_index_out)
{
    return resolve_field_ref_n(sources, source_count,
        program->names[project->group_qualifiers.first + index],
        program->names[project->group_names.first + index],
        source_out, field_index_out);
}

int exec_bind_project(const sqlexec_program *program,
    const sqlexec_project_def *project, const row_source *sources,
    unsigned char source_count, exec_project_binding *binding)
{
    init_project_binding(binding);
    if (project->select_all) {
        if (bind_select_all_outputs(sources, source_count, binding) != 0) {
            return -1;
        }
    } else if (bind_named_outputs(program, project, sources,
        source_count, binding) != 0) {
        return -1;
    }
    if (bind_group_runtime(project, sources, binding) != 0) {
        return -1;
    }
    return bind_having_outputs(program, project, sources, binding);
}

void exec_copy_dbf_field_name(char *target, const char *source)
{
    unsigned short index;

    for (index = 0; index < 11u; index++) {
        target[index] = source[index];
        if (source[index] == '\0') {
            return;
        }
    }
    target[11] = '\0';
}

void exec_copy_value_text(char *target, const char *source)
{
    unsigned short index;

    for (index = 0; index + 1 < sql_value_size; index++) {
        target[index] = source[index];
        if (source[index] == '\0') {
            return;
        }
    }
    target[sql_value_size - 1] = '\0';
}

int exec_append_output_values_to_temp(exec_temp_ctx *ctx,
    char output_values[sql_max_columns][sql_value_size],
    unsigned short output_count)
{
    char *record;
    sql_value stored_value;
    unsigned short offset;
    unsigned short index;
    unsigned short record_len;
    int ret;

    if (output_count != ctx->field_count) {
        return -1;
    }

    record_len = ctx->field_count > 0
        ? (unsigned short)(ctx->offsets[ctx->field_count - 1]
            + ctx->fields[ctx->field_count - 1].length)
        : 0;

    record = (char *)malloc(record_len > 0 ? record_len : 1);
    if (!record) {
        return -1;
    }
    clear_record(record, record_len);

    offset = 0;
    for (index = 0; index < output_count; index++) {
        memset(&stored_value, 0, sizeof(stored_value));
        if (output_values[index][0] == '\0') {
            stored_value.type = sql_value_null;
        } else {
            stored_value.type = ctx->fields[index].type == 'N'
                ? sql_value_number : sql_value_string;
            exec_copy_value_text(stored_value.text, output_values[index]);
        }
        if (store_value_in_field(record + offset, &ctx->fields[index],
            &stored_value) != 0) {
            free(record);
            return -1;
        }
        offset = (unsigned short)(offset + ctx->fields[index].length);
    }

    ret = dbf_append(ctx->out, record);
    free(record);
    return ret;
}

int exec_collect_output_values(exec_collect_ctx *collect,
    char output_values[sql_max_columns][sql_value_size],
    const char output_types[sql_max_columns], unsigned short output_count)
{
    sql_value *value;

    if (!collect || !collect->result) {
        return -1;
    }
    if (collect->mode == exec_collect_exists) {
        if (collect->result->row_count == 0) {
            collect->result->row_count = 1;
        }
        return 0;
    }
    if (output_count != 1) {
        return -1;
    }
    if (collect->result->row_count >= sql_predicate_subquery_rows) {
        collect->result->overflow = 1;
        return -1;
    }

    value = &collect->result->values[collect->result->row_count];
    if (output_values[0][0] == '\0') {
        value->type = sql_value_null;
        value->text[0] = '\0';
    } else {
        value->type = output_types[0] == 'N'
            ? sql_value_number : sql_value_string;
        exec_copy_value_text(value->text, output_values[0]);
    }
    collect->result->row_count++;
    return 0;
}

int exec_build_project_output_values_bound(
    const exec_project_binding *binding, const row_source *sources,
    char output_values[sql_max_columns][sql_value_size],
    unsigned short *output_count_out)
{
    const row_source *source;
    unsigned short index;

    *output_count_out = binding->output_count;
    for (index = 0; index < binding->output_count; index++) {
        if (binding->outputs[index].source_index == 0xffu
            || binding->outputs[index].field_index == 0xffu) {
            return -1;
        }
        source = &sources[binding->outputs[index].source_index];
        if (binding->outputs[index].field_index >= source->field_count) {
            return -1;
        }
        trim_field_value(output_values[index], sql_value_size,
            source->record + source->offsets[binding->outputs[index].field_index],
            source->fields[binding->outputs[index].field_index].length);
    }
    return 0;
}

int append_projected_to_temp_bound(exec_temp_ctx *ctx,
    const exec_project_binding *binding, const row_source *sources)
{
    char output_values[sql_max_columns][sql_value_size];
    unsigned short output_count;

    if (exec_build_project_output_values_bound(binding, sources,
        output_values, &output_count) != 0
        || output_count != ctx->field_count) {
        return -1;
    }
    return exec_append_output_values_to_temp(ctx, output_values,
        output_count);
}
