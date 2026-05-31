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

static sqlexec_ref child_at(const sqlexec_program *program, sqlexec_ref parent,
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

static int test_tree_helpers(void)
{
    sqlexec_program program;
    sqlexec_ref root;
    sqlexec_ref first;
    sqlexec_ref second;
    sqlexec_ref third;
    char dump[512];

    sqlexec_reset(&program);
    root = sqlexec_make_root(&program, sqlexec_sequence);
    if (root == sqlexec_nil) {
        return 1;
    }

    first = sqlexec_append_child(&program, root, sqlexec_create_database);
    second = sqlexec_append_child(&program, root, sqlexec_show_databases);
    if (first == sqlexec_nil || second == sqlexec_nil) {
        return 1;
    }
    set_named(&program, first, "demo");

    third = sqlexec_insert_sibling_after(&program, first, sqlexec_use_database);
    if (third == sqlexec_nil) {
        return 1;
    }
    set_named(&program, third, "demo");

    if (sqlexec_replace(&program, second, sqlexec_drop_database) != 0) {
        return 1;
    }
    set_named(&program, second, "old");

    if (sqlexec_validate(&program) != 0) {
        return 1;
    }

    if (sqlexec_remove(&program, first) != 0) {
        return 1;
    }
    if (sqlexec_validate(&program) != 0) {
        return 1;
    }

    if (child_at(&program, root, 0) != third
        || child_at(&program, root, 1) != second
        || child_at(&program, root, 2) != sqlexec_nil) {
        return 1;
    }

    if (sqlexec_dump(&program, dump, sizeof(dump)) != 0) {
        return 1;
    }
    if (strstr(dump, "sequence") == NULL
        || strstr(dump, "use_database demo") == NULL
        || strstr(dump, "drop_database old") == NULL) {
        return 1;
    }

    return 0;
}

static int test_lower_select_where(void)
{
    sqlexec_program program;
    const sqlexec_node *project;
    const sqlexec_node *filter;
    char dump[512];
    sqlexec_ref root;
    sqlexec_ref emit;
    sqlexec_ref close_table;

    if (sql_parse("SELECT name, age FROM people WHERE age >= 18;",
        &program, NULL, NULL) != 0) {
        return 1;
    }

    root = program.root;
    if (!sqlexec_get_const(&program, root)
        || program.nodes[root].opcode != sqlexec_sequence) {
        return 1;
    }
    if (strcmp(program.nodes[child_at(&program, root, 0)].data.named.name,
        "people") != 0) {
        return 1;
    }

    emit = child_at(&program, root, 1);
    close_table = child_at(&program, root, 2);
    if (emit == sqlexec_nil || close_table == sqlexec_nil
        || child_at(&program, root, 3) != sqlexec_nil) {
        return 1;
    }
    if (program.nodes[emit].opcode != sqlexec_emit_rows
        || program.nodes[close_table].opcode != sqlexec_close_table) {
        return 1;
    }

    project = sqlexec_get_const(&program, child_at(&program, emit, 0));
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

    filter = sqlexec_get_const(&program, child_at(&program, emit, 0));
    filter = sqlexec_get_const(&program, child_at(&program,
        child_at(&program, emit, 0), 0));
    if (!filter || filter->opcode != sqlexec_filter
        || filter->data.where.node_count != 1
        || filter->data.where.value_count != 1
        || where_root_node(&program, &filter->data.where) == NULL
        || strcmp(where_root_node(&program, &filter->data.where)
                ->column_name, "age") != 0
        || where_root_node(&program, &filter->data.where)->operator
            != sql_compare_greater_equal
        || strcmp(program.where_values[where_root_node(&program,
                &filter->data.where)->value_first].text, "18") != 0) {
        return 1;
    }
    if (program.nodes[child_at(&program, child_at(&program,
        child_at(&program, emit, 0), 0), 0)].opcode != sqlexec_table_scan) {
        return 1;
    }

    if (sqlexec_dump(&program, dump, sizeof(dump)) != 0) {
        return 1;
    }
    if (strstr(dump, "project name, age") == NULL
        || strstr(dump, "filter age >= 18") == NULL) {
        return 1;
    }

    return 0;
}

static int test_lower_select_logic(void)
{
    sqlexec_program program;
    const sqlexec_node *filter;
    const sql_where_node *root_where;
    char dump[512];
    sqlexec_ref root;
    sqlexec_ref emit;

    if (sql_parse("SELECT name FROM people WHERE city = 'LON' OR age IN "
        "(18, 21);", &program, NULL, NULL) != 0) {
        return 1;
    }

    root = program.root;
    emit = child_at(&program, root, 1);
    filter = sqlexec_get_const(&program, child_at(&program,
        child_at(&program, emit, 0), 0));
    if (!filter || filter->opcode != sqlexec_filter) {
        return 1;
    }

    root_where = where_root_node(&program, &filter->data.where);
    if (!root_where || filter->data.where.node_count != 3
        || filter->data.where.value_count != 3
        || root_where->type != sql_where_or) {
        return 1;
    }

    if (sqlexec_dump(&program, dump, sizeof(dump)) != 0) {
        return 1;
    }
    if (strstr(dump, "filter (city = 'LON' OR age IN (18, 21))") == NULL) {
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
    emit = child_at(&program, root, 1);
    project = sqlexec_get_const(&program, child_at(&program, emit, 0));
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
    emit = child_at(&program, root, 1);
    project = sqlexec_get_const(&program, child_at(&program, emit, 0));
    scan = sqlexec_get_const(&program, child_at(&program,
        child_at(&program, emit, 0), 0));
    scan = sqlexec_get_const(&program, child_at(&program,
        child_at(&program, emit, 0), 0));
    scan = sqlexec_get_const(&program, child_at(&program,
        child_at(&program, child_at(&program, emit, 0), 0), 0));
    if (!project || project->opcode != sqlexec_project
        || project->data.project.qualifiers.count != 2
        || strcmp(program.names[project->data.project.qualifiers.first],
            "p") != 0
        || strcmp(program.names[project->data.project.qualifiers.first + 1],
            "c") != 0
        || !scan || scan->opcode != sqlexec_join_scan
        || strcmp(scan->data.join.right_table_name, "cities") != 0
        || strcmp(scan->data.join.left_key_name, "city") != 0
        || strcmp(scan->data.join.right_key_name, "code") != 0) {
        return 1;
    }

    if (sqlexec_dump(&program, dump, sizeof(dump)) != 0) {
        return 1;
    }
    if (strstr(dump, "project p.name, c.title") == NULL
        || strstr(dump, "join_scan people as p join cities as c on p.city = c.code")
            == NULL) {
        return 1;
    }

    return 0;
}

static int test_lower_select_count(void)
{
    sqlexec_program program;
    sqlexec_ref root;
    sqlexec_ref emit;

    if (sql_parse("SELECT COUNT(*) FROM people;", &program, NULL, NULL) != 0) {
        return 1;
    }

    root = program.root;
    emit = child_at(&program, root, 1);
    if (program.nodes[root].opcode != sqlexec_sequence
        || program.nodes[emit].opcode != sqlexec_emit_count
        || program.nodes[child_at(&program, emit, 0)].opcode
            != sqlexec_count_rows
        || program.nodes[child_at(&program,
            child_at(&program, emit, 0), 0)].opcode != sqlexec_table_scan) {
        return 1;
    }

    return 0;
}

static int test_lower_insert(void)
{
    sqlexec_program program;
    sqlexec_ref root;
    sqlexec_ref count_node;
    sqlexec_ref make_record;
    const sqlexec_node *record_node;

    if (sql_parse("INSERT INTO people VALUES ('alice', 18, active);",
        &program, NULL, NULL) != 0) {
        return 1;
    }

    root = program.root;
    count_node = child_at(&program, root, 1);
    make_record = child_at(&program, child_at(&program, count_node, 0), 0);
    record_node = sqlexec_get_const(&program, make_record);
    if (!record_node || record_node->opcode != sqlexec_make_record
        || record_node->data.values.count != 3) {
        return 1;
    }
    if (strcmp(program.nodes[child_at(&program, root, 2)].data.named.name,
        "people") != 0) {
        return 1;
    }

    return 0;
}

static int test_lower_update(void)
{
    sqlexec_program program;
    sqlexec_ref root;
    sqlexec_ref apply;
    const sqlexec_node *node;

    if (sql_parse("UPDATE people SET name = 'alice', age = 19 "
        "WHERE age = 18;", &program, NULL, NULL) != 0) {
        return 1;
    }

    root = program.root;
    apply = child_at(&program, child_at(&program,
        child_at(&program, root, 1), 0), 0);
    node = sqlexec_get_const(&program, apply);
    if (!node || node->opcode != sqlexec_apply_assignments
        || node->data.assignments.count != 2) {
        return 1;
    }
    if (program.nodes[child_at(&program, apply, 0)].opcode != sqlexec_filter
        || program.nodes[child_at(&program,
            child_at(&program, apply, 0), 0)].opcode != sqlexec_table_scan) {
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

    node = sqlexec_get_const(&program, child_at(&program, program.root, 0));
    if (!node || node->opcode != sqlexec_build_index
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
    if (program.nodes[child_at(&program, program.root, 1)].opcode
        != sqlexec_register_index) {
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

    if (program.nodes[program.root].opcode != sqlexec_sequence
        || program.nodes[child_at(&program, program.root, 0)].opcode
            != sqlexec_unregister_table_indexes
        || program.nodes[child_at(&program, program.root, 1)].opcode
            != sqlexec_drop_table) {
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

    if (test_lower_select_count() != 0) {
        printf("test_sqlexec: select count fail\n");
        return 1;
    }

    if (test_lower_insert() != 0) {
        printf("test_sqlexec: insert fail\n");
        return 1;
    }

    if (test_lower_update() != 0) {
        printf("test_sqlexec: update fail\n");
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
