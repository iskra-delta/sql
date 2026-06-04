/*
 * Exercises the SQL atomic-operation execution tree library.
 * The tests cover low-level tree edits plus parser-built execution-tree
 * shapes for DDL, query, and mutation work.
 *
 * MIT License (see: LICENSE)
 * Copyright (C) 2026 tomaz stih
 */

#include "sql.h"
#include "sqlexec.h"
#include "../lib/sqlexec/exec_impl.h"
#include "../lib/sqlexec/where.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define sqlexec_root_path "../bin/sqlexec_root"
#define sqlexec_catalog_path "../bin/sqlexec_root/sys/db.dbf"

typedef struct output_capture {
    char text[128];
    unsigned short used;
} output_capture;

static output_capture capture;

static void capture_output_char(char c)
{
    if ((unsigned short)(capture.used + 1)
        >= (unsigned short)sizeof(capture.text)) {
        return;
    }
    capture.text[capture.used++] = c;
    capture.text[capture.used] = '\0';
}

static void cleanup_exec_root(void)
{
    unlink(sqlexec_catalog_path);
    rmdir("../bin/sqlexec_root/sys");
    rmdir("../bin/sqlexec_root/1");
    rmdir(sqlexec_root_path);
}

static void set_named(sqlexec_program *program, sqlexec_ref ref,
    const char *name)
{
    sqlexec_node *node;

    node = sqlexec_get(program, ref);
    if (node) {
        strcpy(node->data.named.name, name);
    }
}

static sqlexec_ref test_child_at(const sqlexec_program *program,
    sqlexec_ref parent,
    unsigned char index)
{
    sqlexec_ref child;

    child = program->nodes[parent].first_child;
    while (child != sqlexec_nil && index > 0) {
        child = program->nodes[child].next_sibling;
        index--;
    }
    return child;
}

static const sql_where_node *where_root_node(const sqlexec_program *program,
    const sql_where *where)
{
    if (!where->active || where->root == (unsigned char)sql_where_nil
        || where->root >= where->node_count) {
        return NULL;
    }

    return &program->where_nodes[where->root];
}

static const sql_where_node *having_node_at(const sqlexec_program *program,
    unsigned char ref)
{
    return &program_having_nodes(program)[ref];
}

static const sql_predicate_operand *having_value_at(
    const sqlexec_program *program, unsigned char ref)
{
    return &program_having_values(program)[ref];
}

static int test_where_split_conjuncts(void)
{
    sqlexec_program program;
    dbf_field people_fields[3];
    dbf_field city_fields[2];
    row_source sources[2];
    where_term terms[sql_where_max_nodes];
    unsigned char term_count;

    if (sql_parse("SELECT p.name FROM people p, cities c "
        "WHERE p.age >= 18 AND c.region = 'EU' AND p.city = c.code;",
        &program, NULL, NULL) != 0) {
        return 1;
    }

    strcpy(people_fields[0].name, "name");
    people_fields[0].type = 'C';
    strcpy(people_fields[1].name, "age");
    people_fields[1].type = 'N';
    strcpy(people_fields[2].name, "city");
    people_fields[2].type = 'C';

    strcpy(city_fields[0].name, "code");
    city_fields[0].type = 'C';
    strcpy(city_fields[1].name, "region");
    city_fields[1].type = 'C';

    memset(sources, 0, sizeof(sources));
    sources[0].table_name = "people";
    sources[0].alias = "p";
    sources[0].fields = people_fields;
    sources[0].field_count = 3;
    sources[1].table_name = "cities";
    sources[1].alias = "c";
    sources[1].fields = city_fields;
    sources[1].field_count = 2;

    if (where_split_conjuncts_n(&program, &program.where, sources, 2,
        terms, &term_count) != 0) {
        return 1;
    }
    if (term_count != 3
        || terms[0].source_mask != 0x01u || terms[0].ready_depth != 0
        || terms[1].source_mask != 0x02u || terms[1].ready_depth != 1
        || terms[2].source_mask != 0x03u || terms[2].ready_depth != 1) {
        return 1;
    }

    return 0;
}

static int test_where_binding_match(void)
{
    sqlexec_program program;
    dbf_field people_fields[3];
    dbf_field city_fields[2];
    unsigned short people_offsets[3];
    unsigned short city_offsets[2];
    char people_record[14];
    char city_record[5];
    row_source sources[2];
    where_binding binding;
    where_term terms[sql_where_max_nodes];
    unsigned char term_count;

    if (sql_parse("SELECT p.name FROM people p, cities c "
        "WHERE p.age >= 18 AND c.region = 'EU' AND p.city = c.code;",
        &program, NULL, NULL) != 0) {
        return 1;
    }

    memset(people_fields, 0, sizeof(people_fields));
    memset(city_fields, 0, sizeof(city_fields));
    strcpy(people_fields[0].name, "name");
    people_fields[0].type = 'C';
    people_fields[0].length = 8;
    strcpy(people_fields[1].name, "age");
    people_fields[1].type = 'N';
    people_fields[1].length = 3;
    strcpy(people_fields[2].name, "city");
    people_fields[2].type = 'C';
    people_fields[2].length = 3;

    strcpy(city_fields[0].name, "code");
    city_fields[0].type = 'C';
    city_fields[0].length = 3;
    strcpy(city_fields[1].name, "region");
    city_fields[1].type = 'C';
    city_fields[1].length = 2;

    people_offsets[0] = 0;
    people_offsets[1] = 8;
    people_offsets[2] = 11;
    city_offsets[0] = 0;
    city_offsets[1] = 3;

    memset(people_record, ' ', sizeof(people_record));
    memset(city_record, ' ', sizeof(city_record));
    memcpy(people_record, "Ada", 3);
    memcpy(people_record + people_offsets[1], "20", 2);
    memcpy(people_record + people_offsets[2], "ROM", 3);
    memcpy(city_record + city_offsets[0], "ROM", 3);
    memcpy(city_record + city_offsets[1], "EU", 2);

    memset(sources, 0, sizeof(sources));
    sources[0].table_name = "people";
    sources[0].alias = "p";
    sources[0].fields = people_fields;
    sources[0].offsets = people_offsets;
    sources[0].record = people_record;
    sources[0].field_count = 3;
    sources[1].table_name = "cities";
    sources[1].alias = "c";
    sources[1].fields = city_fields;
    sources[1].offsets = city_offsets;
    sources[1].record = city_record;
    sources[1].field_count = 2;

    if (where_bind_n(&program, &program.where, sources, 2, &binding) != 0
        || where_split_conjuncts_bound(&program, &program.where, &binding,
            terms, &term_count) != 0) {
        return 1;
    }
    if (term_count != 3
        || binding.left[terms[0].ref].source_index != 0
        || binding.left[terms[1].ref].source_index != 1
        || binding.values[
            program.where_nodes[terms[2].ref].value_first].source_index != 1
        || !where_matches_bound_n(&program, &program.where, &binding,
            sources, NULL)) {
        return 1;
    }

    memcpy(city_record + city_offsets[1], "US", 2);
    if (where_matches_bound_n(&program, &program.where, &binding,
        sources, NULL)) {
        return 1;
    }

    return 0;
}

static int test_where_binding_same_source_column_compare(void)
{
    sqlexec_program program;
    dbf_field fields[2];
    unsigned short offsets[2];
    char record[6];
    row_source source;
    where_binding binding;
    const sql_where_node *node;

    if (sql_parse("SELECT name FROM people WHERE age = age;",
        &program, NULL, NULL) != 0) {
        return 1;
    }

    memset(fields, 0, sizeof(fields));
    strcpy(fields[0].name, "name");
    fields[0].type = 'C';
    fields[0].length = 3;
    strcpy(fields[1].name, "age");
    fields[1].type = 'N';
    fields[1].length = 3;
    offsets[0] = 0;
    offsets[1] = 3;
    memcpy(record, "Ada020", 6);

    memset(&source, 0, sizeof(source));
    source.table_name = "people";
    source.alias = "";
    source.fields = fields;
    source.offsets = offsets;
    source.record = record;
    source.field_count = 2;

    if (where_bind_n(&program, &program.where, &source, 1, &binding) != 0) {
        return 1;
    }
    node = where_root_node(&program, &program.where);
    if (!node
        || binding.left[program.where.root].source_index != 0
        || binding.values[node->value_first].source_index != 0
        || !where_matches_bound_n(&program, &program.where, &binding,
            &source, NULL)) {
        return 1;
    }

    return 0;
}

static int test_tree_helpers(void)
{
    sqlexec_program program;
    sqlexec_ref root;
    sqlexec_ref first;
    char dump[512];

    sqlexec_reset(&program);
    root = sqlexec_make_root(&program, sqlexec_sequence);
    if (root == sqlexec_nil) {
        return 1;
    }

    first = sqlexec_append_child(&program, root, sqlexec_create_database);
    if (first == sqlexec_nil) {
        return 1;
    }
    set_named(&program, first, "demo");

    if (sqlexec_replace(&program, first, sqlexec_drop_database) != 0) {
        return 1;
    }
    set_named(&program, first, "old");

    if (sqlexec_validate(&program) != 0) {
        return 1;
    }

    if (sqlexec_dump(&program, dump, sizeof(dump)) != 0) {
        return 1;
    }
    if (strstr(dump, "sequence") == NULL
        || strstr(dump, "drop_database old") == NULL) {
        return 1;
    }

    return 0;
}

static int test_lower_select_where(void)
{
    sqlexec_program program;
    const sqlexec_node *project;
    char dump[512];
    sqlexec_ref root;

    if (sql_parse("SELECT name, age FROM people WHERE age >= 18;",
        &program, NULL, NULL) != 0) {
        return 1;
    }

    root = program.root;
    if (!sqlexec_get_const(&program, root)
        || program.nodes[root].opcode != sqlexec_project) {
        return 1;
    }
    if (strcmp(program.table_name, "people") != 0) {
        return 1;
    }

    if (test_child_at(&program, root, 1) != sqlexec_nil) {
        return 1;
    }

    project = sqlexec_get_const(&program, root);
    if (!project || project->opcode != sqlexec_project
        || project->data.project.select_all
        || project->data.project.names.count != 2
        || project->data.project.aliases.count != 2) {
        return 1;
    }
    if (strcmp(program.names[project->data.project.names.first], "name") != 0
        || strcmp(program.names[project->data.project.names.first + 1],
            "age") != 0) {
        return 1;
    }
    if (program.names[project->data.project.aliases.first][0] != '\0'
        || program.names[project->data.project.aliases.first + 1][0] != '\0') {
        return 1;
    }

    if (program.where.node_count != 1
        || program.where.value_count != 1
        || where_root_node(&program, &program.where) == NULL
        || strcmp(where_root_node(&program, &program.where)
                ->column_name, "age") != 0
        || where_root_node(&program, &program.where)->operator
            != sql_compare_greater_equal
        || program.where_values[where_root_node(&program,
                &program.where)->value_first].kind
            != sql_predicate_operand_value
        || strcmp(program.where_values[where_root_node(&program,
                &program.where)->value_first].data.value.text,
            "18") != 0) {
        return 1;
    }
    if (program.nodes[test_child_at(&program, root, 0)]
        .opcode != sqlexec_table_scan) {
        return 1;
    }

    if (sqlexec_dump(&program, dump, sizeof(dump)) != 0) {
        return 1;
    }
    if (strstr(dump, "project name, age where age >= 18") == NULL) {
        return 1;
    }

    return 0;
}

static int test_lower_select_logic(void)
{
    sqlexec_program program;
    const sql_where_node *root_where;
    char dump[512];

    if (sql_parse("SELECT name FROM people WHERE city = 'LON' OR age IN "
        "(18, 21);", &program, NULL, NULL) != 0) {
        return 1;
    }

    root_where = where_root_node(&program, &program.where);
    if (!root_where || program.where.node_count != 3
        || program.where.value_count != 3
        || root_where->type != sql_where_or) {
        return 1;
    }

    if (sqlexec_dump(&program, dump, sizeof(dump)) != 0) {
        return 1;
    }
    if (strstr(dump, "where (city = 'LON' OR age IN (18, 21))") == NULL) {
        return 1;
    }

    return 0;
}

static int test_lower_select_aliases(void)
{
    sqlexec_program program;
    const sqlexec_node *project;
    char dump[512];
    sqlexec_ref root;
    sqlexec_ref emit;

    if (sql_parse("SELECT p.name AS person, p.age years FROM people p "
        "WHERE p.age >= 18;", &program, NULL, NULL) != 0) {
        return 1;
    }

    root = program.root;
    emit = root;
    project = sqlexec_get_const(&program, emit);
    if (!project || project->opcode != sqlexec_project
        || project->data.project.select_all
        || project->data.project.names.count != 2
        || project->data.project.aliases.count != 2) {
        return 1;
    }

    if (strcmp(program.names[project->data.project.names.first], "name") != 0
        || strcmp(program.names[project->data.project.names.first + 1],
            "age") != 0) {
        return 1;
    }
    if (strcmp(program.names[project->data.project.aliases.first],
        "person") != 0
        || strcmp(program.names[project->data.project.aliases.first + 1],
            "years") != 0) {
        return 1;
    }

    if (sqlexec_dump(&program, dump, sizeof(dump)) != 0) {
        return 1;
    }
    if (strstr(dump, "project name as person, age as years") == NULL) {
        return 1;
    }

    return 0;
}

static int test_lower_select_join(void)
{
    sqlexec_program program;
    const sqlexec_node *project;
    const sqlexec_node *scan;
    char dump[512];
    sqlexec_ref root;
    sqlexec_ref emit;

    if (sql_parse("SELECT p.name, c.title FROM people AS p JOIN cities c "
        "ON p.city = c.code WHERE c.region = 'EU';", &program, NULL, NULL) != 0) {
        return 1;
    }

    root = program.root;
    emit = root;
    project = sqlexec_get_const(&program, emit);
    scan = sqlexec_get_const(&program, test_child_at(&program, emit, 0));
    if (!project || project->opcode != sqlexec_project
        || project->data.project.qualifiers.count != 2
        || strcmp(program.names[project->data.project.qualifiers.first],
            "p") != 0
        || strcmp(program.names[project->data.project.qualifiers.first + 1],
            "c") != 0
        || !scan || scan->opcode != sqlexec_join_scan
        || strcmp(join_right_table(&program, scan->data.join), "cities") != 0) {
        return 1;
    }
    /* The ON condition is now in the WHERE tree as a compare node with a
     * structured right-hand column operand for "c.code". */
    {
        int found_on = 0;
        unsigned char n;
        for (n = 0; n < program.where.node_count && !found_on; n++) {
            const sql_where_node *wn = &program.where_nodes[n];
            if (wn->type == sql_where_compare
                && strcmp(wn->qualifier, "p") == 0
                && strcmp(wn->column_name, "city") == 0
                && wn->value_count == 1) {
                const sql_predicate_operand *operand =
                    &program.where_values[wn->value_first];
                if (operand->kind == sql_predicate_operand_column
                    && strcmp(operand->data.column.qualifier, "c") == 0
                    && strcmp(operand->data.column.name, "code") == 0) {
                    found_on = 1;
                }
            }
        }
        if (!found_on) {
            return 1;
        }
    }

    if (sqlexec_dump(&program, dump, sizeof(dump)) != 0) {
        return 1;
    }
    if (strstr(dump, "project p.name, c.title") == NULL
        || strstr(dump, "join_scan people as p join cities") == NULL) {
        return 1;
    }

    return 0;
}

static int test_lower_select_table_list(void)
{
    sqlexec_program program;
    const sqlexec_node *scan;
    char dump[512];
    sqlexec_ref root;
    sqlexec_ref emit;

    if (sql_parse("SELECT p.name, c.title FROM people AS p, cities c "
        "WHERE p.city = c.code;", &program, NULL, NULL) != 0) {
        return 1;
    }

    root = program.root;
    emit = root;
    scan = sqlexec_get_const(&program, test_child_at(&program, emit, 0));
    if (!scan || scan->opcode != sqlexec_join_scan
        || join_source_count(scan->data.join) != 2
        || strcmp(join_table_at(&program, scan->data.join, 1), "cities") != 0
        || strcmp(join_alias_at(&program, scan->data.join, 1), "c") != 0) {
        return 1;
    }

    if (sqlexec_dump(&program, dump, sizeof(dump)) != 0) {
        return 1;
    }
    if (strstr(dump, "join_scan people as p join cities as c") == NULL
        || strstr(dump, "p.city = c.code") == NULL) {
        return 1;
    }

    return 0;
}

static int test_lower_select_multi_join(void)
{
    sqlexec_program program;
    const sqlexec_node *project;
    const sqlexec_node *scan;
    char dump[512];
    sqlexec_ref root;
    sqlexec_ref emit;

    if (sql_parse("SELECT p.name, c.title, r.name FROM people p "
        "JOIN cities c ON p.city = c.code "
        "JOIN regions r ON c.region = r.code "
        "WHERE r.zone = 'west';", &program, NULL, NULL) != 0) {
        return 1;
    }

    root = program.root;
    emit = root;
    project = sqlexec_get_const(&program, emit);
    scan = sqlexec_get_const(&program, test_child_at(&program, emit, 0));
    if (!project || project->opcode != sqlexec_project
        || project->data.project.qualifiers.count != 3
        || strcmp(program.names[project->data.project.qualifiers.first],
            "p") != 0
        || strcmp(program.names[project->data.project.qualifiers.first + 1],
            "c") != 0
        || strcmp(program.names[project->data.project.qualifiers.first + 2],
            "r") != 0
        || !scan || scan->opcode != sqlexec_join_scan
        || join_source_count(scan->data.join) != 3
        || strcmp(join_table_at(&program, scan->data.join, 1), "cities") != 0
        || strcmp(join_alias_at(&program, scan->data.join, 1), "c") != 0
        || strcmp(join_table_at(&program, scan->data.join, 2), "regions") != 0
        || strcmp(join_alias_at(&program, scan->data.join, 2), "r") != 0) {
        return 1;
    }

    if (sqlexec_dump(&program, dump, sizeof(dump)) != 0) {
        return 1;
    }
    if (strstr(dump, "project p.name, c.title, r.name") == NULL
        || strstr(dump, "join_scan people as p join cities as c "
            "join regions as r") == NULL) {
        return 1;
    }

    return 0;
}

static int test_lower_select_trim(void)
{
    sqlexec_program program;
    const sqlexec_node *project;
    char dump[512];
    sqlexec_ref root;
    sqlexec_ref emit;

    if (sql_parse("SELECT TRIM(name) AS clean FROM people;",
        &program, NULL, NULL) != 0) {
        return 1;
    }

    root = program.root;
    emit = root;
    project = sqlexec_get_const(&program, emit);
    if (!project || project->opcode != sqlexec_project
        || project->data.project.names.count != 1
        || project->data.project.functions[0] != sql_function_trim
        || strcmp(program.names[project->data.project.names.first],
            "name") != 0
        || strcmp(program.names[project->data.project.aliases.first],
            "clean") != 0) {
        return 1;
    }

    if (sqlexec_dump(&program, dump, sizeof(dump)) != 0) {
        return 1;
    }
    if (strstr(dump, "project TRIM(name) as clean") == NULL) {
        return 1;
    }

    return 0;
}

static int test_lower_select_group(void)
{
    sqlexec_program program;
    const sqlexec_node *project;
    char dump[512];
    sqlexec_ref root;
    sqlexec_ref emit;

    if (sql_parse("SELECT city, MAX(age) AS max_age, AVG(age) avg_age "
        "FROM people GROUP BY city HAVING max_age > 18;",
        &program, NULL, NULL) != 0) {
        return 1;
    }

    root = program.root;
    emit = root;
    project = sqlexec_get_const(&program, emit);
    if (!project || project->opcode != sqlexec_project
        || !project->data.project.has_aggregate
        || project->data.project.names.count != 3
        || project->data.project.group_names.count != 1
        || project->data.project.functions[0] != sql_function_none
        || project->data.project.functions[1] != sql_function_max
        || project->data.project.functions[2] != sql_function_avg
        || strcmp(program.names[project->data.project.group_names.first],
            "city") != 0
        || !program.having.active) {
        return 1;
    }

    if (sqlexec_dump(&program, dump, sizeof(dump)) != 0) {
        return 1;
    }
    if (strstr(dump, "project city, MAX(age) as max_age, AVG(age) as avg_age "
        "group_by city having max_age > 18") == NULL) {
        return 1;
    }

    return 0;
}

static int test_lower_select_distinct_and_extended_predicates(void)
{
    sqlexec_program program;
    const sqlexec_node *project;
    char dump[512];
    sqlexec_ref root;
    sqlexec_ref emit;

    if (sql_parse("SELECT DISTINCT city, COUNT(*) AS total, MIN(age) min_age, "
        "SUM(age) sum_age FROM people WHERE NOT city LIKE 'N%' "
        "AND age BETWEEN 18 AND 24 GROUP BY city HAVING total > 1;",
        &program, NULL, NULL) != 0) {
        return 1;
    }

    root = program.root;
    emit = root;
    project = sqlexec_get_const(&program, emit);
    if (!project || project->opcode != sqlexec_project
        || !project->data.project.distinct
        || !project->data.project.has_aggregate
        || project->data.project.functions[1] != sql_function_count
        || !project->data.project.function_arg_is_star[1]
        || project->data.project.functions[2] != sql_function_min
        || project->data.project.functions[3] != sql_function_sum
        || !program.having.active) {
        return 1;
    }

    if (sqlexec_dump(&program, dump, sizeof(dump)) != 0) {
        return 1;
    }
    if (strstr(dump, "project DISTINCT city, COUNT(*) as total, MIN(age) as "
        "min_age, SUM(age) as sum_age") == NULL
        || strstr(dump, "NOT city LIKE 'N%'") == NULL
        || strstr(dump, "age >= 18") == NULL
        || strstr(dump, "age <= 24") == NULL) {
        return 1;
    }

    return 0;
}

static int test_bind_having_output_values(void)
{
    sqlexec_program program;
    const sqlexec_node *project;
    row_source source;
    dbf_field fields[2];
    unsigned short offsets[2];
    exec_project_binding binding;
    unsigned char ref;
    unsigned char value_index;
    int saw_in;
    int saw_like;

    if (sql_parse("SELECT city, MAX(age) AS max_age, COUNT(*) AS total "
        "FROM people GROUP BY city "
        "HAVING total IN (max_age, total) AND city LIKE city;",
        &program, NULL, NULL) != 0) {
        return 1;
    }

    memset(fields, 0, sizeof(fields));
    strcpy(fields[0].name, "city");
    fields[0].type = 'C';
    fields[0].length = 3;
    strcpy(fields[1].name, "age");
    fields[1].type = 'N';
    fields[1].length = 3;
    offsets[0] = 0;
    offsets[1] = 3;

    memset(&source, 0, sizeof(source));
    source.table_name = "people";
    source.alias = "";
    source.fields = fields;
    source.offsets = offsets;
    source.field_count = 2;

    project = sqlexec_get_const(&program, program.root);
    if (!project || project->opcode != sqlexec_project
        || exec_bind_project(&program, &project->data.project,
            &source, 1, &binding) != 0) {
        return 1;
    }

    saw_in = 0;
    saw_like = 0;
    for (ref = 0; ref < program.having.node_count; ref++) {
        const sql_where_node *node = having_node_at(&program, ref);

        if (node->type == sql_where_in) {
            if (binding.having_left_output[ref] < 0) {
                return 1;
            }
            for (value_index = 0; value_index < node->value_count;
                value_index++) {
                if (having_value_at(&program,
                        (unsigned char)(node->value_first + value_index))->kind
                    == sql_predicate_operand_column
                    && binding.having_value_output[node->value_first
                        + value_index] < 0) {
                    return 1;
                }
            }
            saw_in = 1;
        } else if (node->type == sql_where_like) {
            if (binding.having_left_output[ref] < 0
                || binding.having_value_output[node->value_first] < 0) {
                return 1;
            }
            saw_like = 1;
        }
    }

    return saw_in && saw_like ? 0 : 1;
}

static int test_lower_select_subquery_predicates(void)
{
    sqlexec_program program;
    char dump[768];

    if (sql_parse("SELECT name FROM people WHERE city IS NULL "
        "OR EXISTS (SELECT code FROM cities) "
        "OR age >= ALL (SELECT age FROM people) "
        "OR city IN (SELECT code FROM cities);",
        &program, NULL, NULL) != 0) {
        return 1;
    }

    if (program.predicate_subquery_count != 2
        || strcmp(program.predicate_subqueries[0],
            "SELECT code FROM cities") != 0
        || strcmp(program.predicate_subqueries[1],
            "SELECT age FROM people") != 0) {
        return 1;
    }

    if (sqlexec_dump(&program, dump, sizeof(dump)) != 0) {
        return 1;
    }
    if (strstr(dump, "city IS NULL") == NULL
        || strstr(dump, "EXISTS (SELECT code FROM cities)") == NULL
        || strstr(dump, "age >= ALL (SELECT age FROM people)") == NULL
        || strstr(dump, "city = ANY (SELECT code FROM cities)") == NULL) {
        return 1;
    }

    return 0;
}

static int test_lower_select_inline_subquery_flatten(void)
{
    sqlexec_program program;
    const sqlexec_node *scan;
    char dump[768];
    sqlexec_ref root;
    sqlexec_ref emit;

    if (sql_parse("SELECT a.name, c.title FROM "
        "(SELECT * FROM people p WHERE p.age >= 18) a "
        "JOIN cities c ON a.city = c.code WHERE c.region = 'EU';",
        &program, NULL, NULL) != 0) {
        return 1;
    }

    root = program.root;
    emit = root;
    scan = sqlexec_get_const(&program, test_child_at(&program, emit, 0));
    if (program_subquery_text(&program)[0] != '\0'
        || strcmp(program.table_name, "people") != 0
        || !scan || scan->opcode != sqlexec_join_scan
        || strcmp(join_left_alias(&program, scan->data.join), "a") != 0) {
        return 1;
    }

    if (sqlexec_dump(&program, dump, sizeof(dump)) != 0) {
        return 1;
    }
    if (strstr(dump, "join_scan people as a join cities as c") == NULL
        || strstr(dump, "a.age >= 18") == NULL
        || strstr(dump, "a.city = c.code") == NULL
        || strstr(dump, "c.region = 'EU'") == NULL) {
        return 1;
    }

    return 0;
}

static int test_lower_select_projected_subquery_flatten(void)
{
    sqlexec_program program;
    char dump[768];

    if (sql_parse("SELECT person FROM "
        "(SELECT name AS person, city AS home FROM people p "
        "WHERE p.age >= 18) adults WHERE home = 'LON';",
        &program, NULL, NULL) != 0) {
        return 1;
    }

    if (program_subquery_text(&program)[0] != '\0'
        || strcmp(program.table_name, "people") != 0) {
        return 1;
    }

    memset(dump, 0, sizeof(dump));
    if (sqlexec_dump(&program, dump, sizeof(dump)) != 0) {
        return 1;
    }
    if (strstr(dump, "project name as person") == NULL
        || strstr(dump, "age >= 18") == NULL
        || (strstr(dump, "city = 'LON'") == NULL
            && strstr(dump, "city = LON") == NULL)
        || strstr(dump, "home") != NULL
        || strstr(dump, "adults") != NULL) {
        return 1;
    }

    return 0;
}

static int test_lower_select_count(void)
{
    sqlexec_program program;
    sqlexec_ref root;

    if (sql_parse("SELECT COUNT(*) FROM people;", &program, NULL, NULL) != 0) {
        return 1;
    }

    root = program.root;
    if (program.nodes[root].opcode != sqlexec_table_scan
        || strcmp(program.table_name, "people") != 0) {
        return 1;
    }

    return 0;
}

static int test_lower_insert(void)
{
    sqlexec_program program;
    sqlexec_ref root;
    sqlexec_ref append_record;
    const sqlexec_node *append_node;

    if (sql_parse("INSERT INTO people VALUES ('alice', 18, active);",
        &program, NULL, NULL) != 0) {
        return 1;
    }

    root = program.root;
    append_record = root;
    append_node = sqlexec_get_const(&program, append_record);
    if (!append_node || append_node->opcode != sqlexec_append_record
        || append_node->data.assignments.count != 3) {
        return 1;
    }
    if (strcmp(program.table_name, "people") != 0) {
        return 1;
    }

    return 0;
}

static int test_lower_insert_column_list(void)
{
    sqlexec_program program;
    sqlexec_ref root;
    sqlexec_ref append_record;
    const sqlexec_node *append_node;

    if (sql_parse("INSERT INTO people (city, name) VALUES ('LON', 'zoe');",
        &program, NULL, NULL) != 0) {
        return 1;
    }

    root = program.root;
    append_record = root;
    append_node = sqlexec_get_const(&program, append_record);
    if (!append_node || append_node->opcode != sqlexec_append_record
        || append_node->data.assignments.count != 2
        || strcmp(program_assignments(&program)[append_node->data.assignments.first]
            .column_name, "city") != 0
        || strcmp(program_assignments(&program)[append_node->data.assignments.first + 1]
            .column_name, "name") != 0) {
        return 1;
    }

    return 0;
}

static int test_lower_update(void)
{
    sqlexec_program program;
    sqlexec_ref root;
    sqlexec_ref write_current;
    const sqlexec_node *node;

    if (sql_parse("UPDATE people SET name = 'alice', age = 19 "
        "WHERE age = 18;", &program, NULL, NULL) != 0) {
        return 1;
    }

    root = program.root;
    write_current = root;
    node = sqlexec_get_const(&program, write_current);
    if (!node || node->opcode != sqlexec_write_current
        || node->data.assignments.count != 2) {
        return 1;
    }
    if (program.nodes[test_child_at(&program, write_current, 0)].opcode
            != sqlexec_table_scan
        || !program.where.active) {
        return 1;
    }

    return 0;
}

static int test_lower_update_null(void)
{
    sqlexec_program program;
    sqlexec_ref root;
    sqlexec_ref write_current;
    const sqlexec_node *node;

    if (sql_parse("UPDATE people SET city = NULL WHERE name = 'zoe';",
        &program, NULL, NULL) != 0) {
        return 1;
    }

    root = program.root;
    write_current = root;
    node = sqlexec_get_const(&program, write_current);
    if (!node || node->opcode != sqlexec_write_current
        || node->data.assignments.count != 1
        || program_assignments(&program)[node->data.assignments.first].value.type
            != sql_value_null) {
        return 1;
    }

    return 0;
}

static int test_lower_create_index(void)
{
    sqlexec_program program;
    const sqlexec_node *node;

    if (sql_parse("CREATE UNIQUE INDEX people_name ON people "
        "(city, name);", &program, NULL, NULL) != 0) {
        return 1;
    }

    node = sqlexec_get_const(&program, program.root);
    if (!node || node->opcode != sqlexec_create_index
        || !node->data.index.unique
        || strcmp(node->data.index.index_name, "people_name") != 0
        || strcmp(node->data.index.table_name, "people") != 0
        || node->data.index.key_names.count != 2) {
        return 1;
    }
    if (strcmp(program.names[node->data.index.key_names.first], "city") != 0
        || strcmp(program.names[node->data.index.key_names.first + 1],
            "name") != 0) {
        return 1;
    }

    return 0;
}

static int test_lower_drop_table(void)
{
    sqlexec_program program;

    if (sql_parse("DROP TABLE people;", &program, NULL, NULL) != 0) {
        return 1;
    }

    if (program.nodes[program.root].opcode != sqlexec_drop_table
        || strcmp(program.nodes[program.root].data.named.name, "people")
            != 0) {
        return 1;
    }

    return 0;
}

static int test_execute_smoke(void)
{
    sqlexec_program program;
    sqlexec_io io;
    char current_db[sql_name_size];
    FILE *file;

    cleanup_exec_root();
    current_db[0] = '\0';
    capture.used = 0;
    capture.text[0] = '\0';

    if (sql_parse("CREATE DATABASE demo;", &program, NULL, NULL) != 0) {
        cleanup_exec_root();
        return 1;
    }

    io.write_char = capture_output_char;
    if (sqlexec_execute(sqlexec_root_path, &program, current_db, &io) != 0) {
        cleanup_exec_root();
        return 1;
    }

    if (strcmp(current_db, "demo") != 0
        || strstr(capture.text, "created demo") == NULL) {
        cleanup_exec_root();
        return 1;
    }

    file = fopen(sqlexec_catalog_path, "rb");
    if (!file) {
        cleanup_exec_root();
        return 1;
    }
    fclose(file);
    cleanup_exec_root();
    return 0;
}

int main(void)
{
    if (test_where_split_conjuncts() != 0) {
        printf("test_sqlexec: where split fail\n");
        return 1;
    }

    if (test_where_binding_match() != 0) {
        printf("test_sqlexec: where binding fail\n");
        return 1;
    }

    if (test_where_binding_same_source_column_compare() != 0) {
        printf("test_sqlexec: where same-source column compare fail\n");
        return 1;
    }

    if (test_tree_helpers() != 0) {
        printf("test_sqlexec: tree helper fail\n");
        return 1;
    }

    if (test_lower_select_where() != 0) {
        printf("test_sqlexec: select where fail\n");
        return 1;
    }

    if (test_lower_select_logic() != 0) {
        printf("test_sqlexec: select logic fail\n");
        return 1;
    }

    if (test_lower_select_aliases() != 0) {
        printf("test_sqlexec: select alias fail\n");
        return 1;
    }

    if (test_lower_select_join() != 0) {
        printf("test_sqlexec: select join fail\n");
        return 1;
    }

    if (test_lower_select_table_list() != 0) {
        printf("test_sqlexec: select table list fail\n");
        return 1;
    }

    if (test_lower_select_multi_join() != 0) {
        printf("test_sqlexec: select multi join fail\n");
        return 1;
    }

    if (test_lower_select_trim() != 0) {
        printf("test_sqlexec: select trim fail\n");
        return 1;
    }

    if (test_lower_select_group() != 0) {
        printf("test_sqlexec: select group fail\n");
        return 1;
    }

    if (test_lower_select_distinct_and_extended_predicates() != 0) {
        printf("test_sqlexec: select distinct fail\n");
        return 1;
    }

    if (test_bind_having_output_values() != 0) {
        printf("test_sqlexec: bind having values fail\n");
        return 1;
    }

    if (test_lower_select_subquery_predicates() != 0) {
        printf("test_sqlexec: select subquery predicates fail\n");
        return 1;
    }

    if (test_lower_select_inline_subquery_flatten() != 0) {
        printf("test_sqlexec: select inline subquery flatten fail\n");
        return 1;
    }

    if (test_lower_select_projected_subquery_flatten() != 0) {
        printf("test_sqlexec: projected subquery flatten fail\n");
        return 1;
    }

    if (test_lower_select_count() != 0) {
        printf("test_sqlexec: select count fail\n");
        return 1;
    }

    if (test_lower_insert() != 0) {
        printf("test_sqlexec: insert fail\n");
        return 1;
    }

    if (test_lower_insert_column_list() != 0) {
        printf("test_sqlexec: insert column list fail\n");
        return 1;
    }

    if (test_lower_update() != 0) {
        printf("test_sqlexec: update fail\n");
        return 1;
    }

    if (test_lower_update_null() != 0) {
        printf("test_sqlexec: update null fail\n");
        return 1;
    }

    if (test_lower_create_index() != 0) {
        printf("test_sqlexec: create index fail\n");
        return 1;
    }

    if (test_lower_drop_table() != 0) {
        printf("test_sqlexec: drop table fail\n");
        return 1;
    }

    if (test_execute_smoke() != 0) {
        printf("test_sqlexec: execute smoke fail\n");
        return 1;
    }

    printf("test_sqlexec: ok\n");
    return 0;
}
