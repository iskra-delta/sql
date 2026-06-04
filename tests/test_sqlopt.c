/*
 * Exercises the SQL optimizer against real on-disk catalog fixtures.
 * The tests build small db.dbf, ndx.dbf, and table files, then verify
 * that conservative optimizer rewrites happen only for safe matches.
 *
 * MIT License (see: LICENSE)
 * Copyright (C) 2026 tomaz stih
 */

#include "dbf.h"
#include "sql.h"
#include "sqlexec.h"
#include "sqlopt.h"
#include "../lib/sqlexec/where.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define optimizer_root_path "../bin/sqlopt_root"
#define optimizer_sys_path "../bin/sqlopt_root/sys"
#define optimizer_db_path "../bin/sqlopt_root/1"
#define optimizer_db_catalog_path "../bin/sqlopt_root/sys/db.dbf"
#define optimizer_index_catalog_path "../bin/sqlopt_root/sys/ndx.dbf"
#define optimizer_people_table_path "../bin/sqlopt_root/1/people.dbf"

#define catalog_name_length   16
#define catalog_path_length   48
#define catalog_slot_length    2
#define catalog_record_length 66
#define index_catalog_db_length     16
#define index_catalog_name_length   16
#define index_catalog_table_length  16
#define index_catalog_fields_length ((sql_max_columns * 11) \
    + (sql_max_columns - 1))
#define index_catalog_unique_length  1
#define index_catalog_record_length (index_catalog_db_length \
    + index_catalog_name_length + index_catalog_table_length \
    + index_catalog_fields_length + index_catalog_unique_length)

static int ensure_directory(const char *path)
{
    struct stat st;

    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 0 : -1;
    }

    return mkdir(path, 0775);
}

static void cleanup_optimizer_fixture(void)
{
    unlink(optimizer_people_table_path);
    unlink(optimizer_index_catalog_path);
    unlink(optimizer_db_catalog_path);
    rmdir(optimizer_db_path);
    rmdir(optimizer_sys_path);
    rmdir(optimizer_root_path);
}

static void set_field_value(char *target, unsigned short length,
    const char *value)
{
    unsigned short value_length;

    value_length = (unsigned short)strlen(value);
    if (value_length > length) {
        value_length = length;
    }

    memset(target, ' ', length);
    memcpy(target, value, value_length);
}

static void set_optimizer_slot_field(char *target, unsigned short slot)
{
    target[0] = slot >= 10 ? (char)('0' + slot / 10) : ' ';
    target[1] = (char)('0' + slot % 10);
}

static int create_database_catalog(void)
{
    dbf_file file;
    dbf_field fields[3];
    char record[catalog_record_length];

    strcpy(fields[0].name, "name");
    fields[0].type = 'C';
    fields[0].length = catalog_name_length;
    fields[0].decimals = 0;
    strcpy(fields[1].name, "path");
    fields[1].type = 'C';
    fields[1].length = catalog_path_length;
    fields[1].decimals = 0;
    strcpy(fields[2].name, "slot");
    fields[2].type = 'N';
    fields[2].length = catalog_slot_length;
    fields[2].decimals = 0;

    if (dbf_create(&file, optimizer_db_catalog_path, fields, 3) != 0) {
        return 1;
    }

    memset(record, ' ', sizeof(record));
    set_field_value(record, catalog_name_length, "demo");
    set_field_value(record + catalog_name_length, catalog_path_length,
        optimizer_db_path);
    set_optimizer_slot_field(record + catalog_name_length
        + catalog_path_length, 1);
    if (dbf_append(&file, record) != 0) {
        dbf_close(&file);
        return 1;
    }

    return dbf_close(&file) != 0;
}

static void fill_index_catalog_record(char *record, const char *index_name,
    const char *field_list)
{
    memset(record, ' ', index_catalog_record_length);
    set_field_value(record, index_catalog_db_length, "demo");
    set_field_value(record + index_catalog_db_length,
        index_catalog_name_length, index_name);
    set_field_value(record + index_catalog_db_length
        + index_catalog_name_length, index_catalog_table_length, "people");
    set_field_value(record + index_catalog_db_length
        + index_catalog_name_length + index_catalog_table_length,
        index_catalog_fields_length, field_list);
    set_field_value(record + index_catalog_db_length
        + index_catalog_name_length + index_catalog_table_length
        + index_catalog_fields_length, index_catalog_unique_length, "N");
}

static int create_index_catalog(void)
{
    dbf_file file;
    dbf_field fields[5];
    char record[index_catalog_record_length];

    strcpy(fields[0].name, "db_name");
    fields[0].type = 'C';
    fields[0].length = index_catalog_db_length;
    fields[0].decimals = 0;
    strcpy(fields[1].name, "name");
    fields[1].type = 'C';
    fields[1].length = index_catalog_name_length;
    fields[1].decimals = 0;
    strcpy(fields[2].name, "table_name");
    fields[2].type = 'C';
    fields[2].length = index_catalog_table_length;
    fields[2].decimals = 0;
    strcpy(fields[3].name, "key_fields");
    fields[3].type = 'C';
    fields[3].length = (unsigned char)index_catalog_fields_length;
    fields[3].decimals = 0;
    strcpy(fields[4].name, "unique");
    fields[4].type = 'C';
    fields[4].length = index_catalog_unique_length;
    fields[4].decimals = 0;

    if (dbf_create(&file, optimizer_index_catalog_path, fields, 5) != 0) {
        return 1;
    }

    fill_index_catalog_record(record, "people_age", "age");
    if (dbf_append(&file, record) != 0) {
        dbf_close(&file);
        return 1;
    }

    fill_index_catalog_record(record, "people_city_name", "city,name");
    if (dbf_append(&file, record) != 0) {
        dbf_close(&file);
        return 1;
    }

    return dbf_close(&file) != 0;
}

static int create_people_table(void)
{
    dbf_file file;
    dbf_field fields[3];

    strcpy(fields[0].name, "name");
    fields[0].type = 'C';
    fields[0].length = 8;
    fields[0].decimals = 0;
    strcpy(fields[1].name, "city");
    fields[1].type = 'C';
    fields[1].length = 3;
    fields[1].decimals = 0;
    strcpy(fields[2].name, "age");
    fields[2].type = 'N';
    fields[2].length = 3;
    fields[2].decimals = 0;

    if (dbf_create(&file, optimizer_people_table_path, fields, 3) != 0) {
        return 1;
    }

    return dbf_close(&file) != 0;
}

static int prepare_optimizer_fixture(void)
{
    cleanup_optimizer_fixture();

    if (ensure_directory(optimizer_root_path) != 0
        || ensure_directory(optimizer_sys_path) != 0
        || ensure_directory(optimizer_db_path) != 0) {
        cleanup_optimizer_fixture();
        return 1;
    }
    if (create_database_catalog() != 0
        || create_index_catalog() != 0
        || create_people_table() != 0) {
        cleanup_optimizer_fixture();
        return 1;
    }

    return 0;
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

static sqlexec_ref select_scan_ref(const sqlexec_program *program)
{
    sqlexec_ref ref;

    ref = program->root;
    if (program->nodes[ref].opcode == sqlexec_project) {
        ref = child_at(program, ref, 0);
        if (ref == sqlexec_nil) {
            return sqlexec_nil;
        }
    }
    return ref;
}

static int test_optimize_select_eq(void)
{
    sqlexec_program program;
    const sqlexec_node *scan;
    sqlexec_ref scan_ref;
    char dump[512];

    if (prepare_optimizer_fixture() != 0) {
        return 1;
    }
    if (sql_parse("SELECT name FROM people WHERE age = 18;",
        &program, NULL, NULL) != 0) {
        return 1;
    }
    if (sqlopt_optimize(&program, optimizer_root_path, "demo") != 0) {
        return 1;
    }

    scan_ref = select_scan_ref(&program);
    scan = sqlexec_get_const(&program, scan_ref);
    if (!scan || scan->opcode != sqlexec_table_scan
        || scan->data.scan.access_kind != sqlexec_scan_index_eq
        || strcmp(scan->data.scan.index_name, "people_age") != 0
        || strcmp(scan->data.scan.lower_value.text, "18") != 0) {
        return 1;
    }

    if (sqlexec_dump(&program, dump, sizeof(dump)) != 0
        || strstr(dump, "table_scan people_age = 18") == NULL) {
        return 1;
    }
    if (program.where.active) {
        return 1;
    }

    return 0;
}

static int test_optimize_select_range(void)
{
    sqlexec_program program;
    const sqlexec_node *scan;
    sqlexec_ref scan_ref;

    if (prepare_optimizer_fixture() != 0) {
        return 1;
    }
    if (sql_parse("SELECT name FROM people WHERE age >= 18;",
        &program, NULL, NULL) != 0) {
        return 1;
    }
    if (sqlopt_optimize(&program, optimizer_root_path, "demo") != 0) {
        return 1;
    }

    scan_ref = select_scan_ref(&program);
    scan = sqlexec_get_const(&program, scan_ref);
    if (!scan || scan->opcode != sqlexec_table_scan
        || scan->data.scan.access_kind != sqlexec_scan_index_range
        || strcmp(scan->data.scan.index_name, "people_age") != 0
        || scan->data.scan.lower_operator
            != sql_compare_greater_equal
        || strcmp(scan->data.scan.lower_value.text, "18") != 0
        || scan->data.scan.upper_operator != sql_compare_invalid) {
        return 1;
    }
    if (program.where.active) {
        return 1;
    }

    return 0;
}

static int test_optimize_select_conjunctive_eq(void)
{
    sqlexec_program program;
    const sqlexec_node *scan;
    sqlexec_ref scan_ref;

    if (prepare_optimizer_fixture() != 0) {
        return 1;
    }
    if (sql_parse("SELECT name FROM people WHERE city = 'LON' "
        "AND age = 18;", &program, NULL, NULL) != 0) {
        return 1;
    }
    if (sqlopt_optimize(&program, optimizer_root_path, "demo") != 0) {
        return 1;
    }

    scan_ref = select_scan_ref(&program);
    scan = sqlexec_get_const(&program, scan_ref);
    if (!scan || scan->opcode != sqlexec_table_scan
        || scan->data.scan.access_kind != sqlexec_scan_index_eq
        || strcmp(scan->data.scan.index_name, "people_age") != 0
        || strcmp(scan->data.scan.lower_value.text, "18") != 0) {
        return 1;
    }
    if (!program.where.active
        || program.where_nodes[program.where.root].type != sql_where_compare
        || strcmp(program.where_nodes[program.where.root].column_name,
            "city") != 0) {
        return 1;
    }

    return 0;
}

static int test_optimize_select_single_value_in(void)
{
    sqlexec_program program;
    const sqlexec_node *scan;
    sqlexec_ref scan_ref;

    if (prepare_optimizer_fixture() != 0) {
        return 1;
    }
    if (sql_parse("SELECT name FROM people WHERE age IN (18);",
        &program, NULL, NULL) != 0) {
        return 1;
    }
    if (sqlopt_optimize(&program, optimizer_root_path, "demo") != 0) {
        return 1;
    }

    scan_ref = select_scan_ref(&program);
    scan = sqlexec_get_const(&program, scan_ref);
    if (!scan || scan->opcode != sqlexec_table_scan
        || scan->data.scan.access_kind != sqlexec_scan_index_eq
        || strcmp(scan->data.scan.index_name, "people_age") != 0
        || strcmp(scan->data.scan.lower_value.text, "18") != 0) {
        return 1;
    }
    if (program.where.active) {
        return 1;
    }

    return 0;
}

static int test_optimize_select_merged_range(void)
{
    sqlexec_program program;
    const sqlexec_node *scan;
    sqlexec_ref scan_ref;

    if (prepare_optimizer_fixture() != 0) {
        return 1;
    }
    if (sql_parse("SELECT name FROM people WHERE age >= 18 "
        "AND age <= 24;", &program, NULL, NULL) != 0) {
        return 1;
    }
    if (sqlopt_optimize(&program, optimizer_root_path, "demo") != 0) {
        return 1;
    }

    scan_ref = select_scan_ref(&program);
    scan = sqlexec_get_const(&program, scan_ref);
    if (!scan || scan->opcode != sqlexec_table_scan
        || scan->data.scan.access_kind != sqlexec_scan_index_range
        || strcmp(scan->data.scan.index_name, "people_age") != 0
        || scan->data.scan.lower_operator
            != sql_compare_greater_equal
        || strcmp(scan->data.scan.lower_value.text, "18") != 0
        || scan->data.scan.upper_operator
            != sql_compare_less_equal
        || strcmp(scan->data.scan.upper_value.text, "24") != 0) {
        return 1;
    }
    if (program.where.active) {
        return 1;
    }

    return 0;
}

static int test_optimize_select_range_keeps_other_conjunct(void)
{
    sqlexec_program program;
    const sqlexec_node *scan;
    sqlexec_ref scan_ref;

    if (prepare_optimizer_fixture() != 0) {
        return 1;
    }
    if (sql_parse("SELECT name FROM people WHERE age >= 18 "
        "AND age <= 24 AND city = 'LON';",
        &program, NULL, NULL) != 0) {
        return 1;
    }
    if (sqlopt_optimize(&program, optimizer_root_path, "demo") != 0) {
        return 1;
    }

    scan_ref = select_scan_ref(&program);
    scan = sqlexec_get_const(&program, scan_ref);
    if (!scan || scan->opcode != sqlexec_table_scan
        || scan->data.scan.access_kind != sqlexec_scan_index_range
        || strcmp(scan->data.scan.index_name, "people_age") != 0
        || !program.where.active
        || program.where_nodes[program.where.root].type != sql_where_compare
        || strcmp(program.where_nodes[program.where.root].column_name,
            "city") != 0) {
        return 1;
    }
    return 0;
}

static int test_optimize_skips_composite_index(void)
{
    sqlexec_program program;
    sqlexec_ref scan_ref;

    if (prepare_optimizer_fixture() != 0) {
        return 1;
    }
    if (sql_parse("SELECT name FROM people WHERE city = 'LON';",
        &program, NULL, NULL) != 0) {
        return 1;
    }
    if (sqlopt_optimize(&program, optimizer_root_path, "demo") != 0) {
        return 1;
    }

    scan_ref = select_scan_ref(&program);
    return program.nodes[scan_ref].opcode == sqlexec_table_scan
        && program.nodes[scan_ref].data.scan.access_kind == sqlexec_scan_full
        ? 0 : 1;
}

static int test_optimize_skips_complex_where(void)
{
    sqlexec_program program;
    sqlexec_ref scan_ref;

    if (prepare_optimizer_fixture() != 0) {
        return 1;
    }
    if (sql_parse("SELECT name FROM people WHERE age = 18 OR city = 'LON';",
        &program, NULL, NULL) != 0) {
        return 1;
    }
    if (sqlopt_optimize(&program, optimizer_root_path, "demo") != 0) {
        return 1;
    }

    scan_ref = select_scan_ref(&program);
    return program.nodes[scan_ref].opcode == sqlexec_table_scan
        && program.nodes[scan_ref].data.scan.access_kind == sqlexec_scan_full
        ? 0 : 1;
}

static int test_optimize_duplicate_conjuncts(void)
{
    sqlexec_program program;

    if (prepare_optimizer_fixture() != 0) {
        return 1;
    }
    if (sql_parse("SELECT city FROM people WHERE name = 'ANN' "
        "AND name = 'ANN';",
        &program, NULL, NULL) != 0) {
        return 1;
    }
    if (sqlopt_optimize(&program, optimizer_root_path, "demo") != 0) {
        return 1;
    }
    if (!program.where.active
        || program.where_nodes[program.where.root].type != sql_where_compare) {
        return 1;
    }
    if (program.where_nodes[program.where.root].value_first >= 2
        || program.where_values[
            program.where_nodes[program.where.root].value_first].kind
            != sql_predicate_operand_value
        || strcmp(program.where_values[
            program.where_nodes[program.where.root].value_first]
                .data.value.text, "ANN") != 0) {
        return 1;
    }
    return 0;
}

static int test_optimize_redundant_bounds(void)
{
    sqlexec_program program;
    const sqlexec_node *scan;
    sqlexec_ref scan_ref;

    if (prepare_optimizer_fixture() != 0) {
        return 1;
    }
    if (sql_parse("SELECT name FROM people WHERE age >= 18 "
        "AND age > 18 AND age <= 24;",
        &program, NULL, NULL) != 0) {
        return 1;
    }
    if (sqlopt_optimize(&program, optimizer_root_path, "demo") != 0) {
        return 1;
    }

    scan_ref = select_scan_ref(&program);
    scan = sqlexec_get_const(&program, scan_ref);
    if (!scan || scan->opcode != sqlexec_table_scan
        || scan->data.scan.access_kind != sqlexec_scan_index_range
        || strcmp(scan->data.scan.index_name, "people_age") != 0
        || scan->data.scan.lower_operator != sql_compare_greater
        || strcmp(scan->data.scan.lower_value.text, "18") != 0
        || scan->data.scan.upper_operator != sql_compare_less_equal
        || strcmp(scan->data.scan.upper_value.text, "24") != 0) {
        return 1;
    }
    return program.where.active ? 1 : 0;
}

static int test_optimize_equal_bounds_to_eq(void)
{
    sqlexec_program program;
    const sqlexec_node *scan;
    sqlexec_ref scan_ref;

    if (prepare_optimizer_fixture() != 0) {
        return 1;
    }
    if (sql_parse("SELECT name FROM people WHERE age >= 18 "
        "AND age <= 18;",
        &program, NULL, NULL) != 0) {
        return 1;
    }
    if (sqlopt_optimize(&program, optimizer_root_path, "demo") != 0) {
        return 1;
    }

    scan_ref = select_scan_ref(&program);
    scan = sqlexec_get_const(&program, scan_ref);
    if (!scan || scan->opcode != sqlexec_table_scan
        || scan->data.scan.access_kind != sqlexec_scan_index_eq
        || strcmp(scan->data.scan.index_name, "people_age") != 0
        || strcmp(scan->data.scan.lower_value.text, "18") != 0) {
        return 1;
    }
    return program.where.active ? 1 : 0;
}

static int test_optimize_contradictory_conjuncts(void)
{
    sqlexec_program program;

    if (prepare_optimizer_fixture() != 0) {
        return 1;
    }
    if (sql_parse("SELECT city FROM people WHERE name = 'ANN' "
        "AND name = 'BOB';",
        &program, NULL, NULL) != 0) {
        return 1;
    }
    if (sqlopt_optimize(&program, optimizer_root_path, "demo") != 0) {
        return 1;
    }
    if (!program.where.active
        || program.where_nodes[program.where.root].type != sql_where_false) {
        return 1;
    }
    return 0;
}

static int test_optimize_not_compare(void)
{
    sqlexec_program program;
    sql_where_node *root;

    if (prepare_optimizer_fixture() != 0) {
        return 1;
    }
    if (sql_parse("SELECT city FROM people WHERE NOT name < 'M';",
        &program, NULL, NULL) != 0) {
        return 1;
    }
    if (sqlopt_optimize(&program, optimizer_root_path, "demo") != 0) {
        return 1;
    }
    root = &program.where_nodes[program.where.root];
    if (root->type != sql_where_compare
        || root->operator != sql_compare_greater_equal) {
        return 1;
    }
    if (root->value_first >= 1
        || program.where_values[root->value_first].kind
            != sql_predicate_operand_value
        || strcmp(program.where_values[root->value_first].data.value.text,
            "M") != 0) {
        return 1;
    }
    return 0;
}

static int test_optimize_same_column_compare(void)
{
    sqlexec_program program;
    sql_where_node *root;

    if (prepare_optimizer_fixture() != 0) {
        return 1;
    }
    if (sql_parse("SELECT city FROM people WHERE age = age;",
        &program, NULL, NULL) != 0) {
        return 1;
    }
    if (sqlopt_optimize(&program, optimizer_root_path, "demo") != 0) {
        return 1;
    }
    root = &program.where_nodes[program.where.root];
    if (root->type != sql_where_is_null
        || root->operator != sql_compare_not_equal) {
        return 1;
    }

    if (sql_parse("SELECT city FROM people WHERE age < age;",
        &program, NULL, NULL) != 0) {
        return 1;
    }
    if (sqlopt_optimize(&program, optimizer_root_path, "demo") != 0) {
        return 1;
    }
    root = &program.where_nodes[program.where.root];
    return root->type == sql_where_false ? 0 : 1;
}

static int test_optimize_where_source_masks(void)
{
    sqlexec_program program;
    unsigned char age_mask;
    unsigned char city_mask;
    unsigned char name_mask;
    unsigned char index;
    const sql_where_node *node;

    if (sql_parse("SELECT p.name FROM people p, people q "
        "WHERE p.age = 18 AND q.city = 'LON' AND p.name = q.name;",
        &program, NULL, NULL) != 0) {
        return 1;
    }
    if (sqlopt_optimize(&program, NULL, NULL) != 0) {
        return 1;
    }
    if (program.where_source_mask_count != program.where.node_count) {
        return 1;
    }

    age_mask = 0;
    city_mask = 0;
    name_mask = 0;
    for (index = 0; index < program.where.node_count; index++) {
        node = &program.where_nodes[index];
        if (node->type != sql_where_compare) {
            continue;
        }
        if (strcmp(node->column_name, "age") == 0) {
            age_mask = program.where_source_masks[index];
        } else if (strcmp(node->column_name, "city") == 0) {
            city_mask = program.where_source_masks[index];
        } else if (strcmp(node->column_name, "name") == 0
            && node->value_count == 1
            && program.where_values[node->value_first].kind
                == sql_predicate_operand_column) {
            name_mask = program.where_source_masks[index];
        }
    }

    return age_mask == 1u && city_mask == 2u && name_mask == 3u ? 0 : 1;
}

static int test_optimize_where_bound_slots(void)
{
    sqlexec_program program;
    const sql_where_node *node;
    unsigned char value_slot;

    if (prepare_optimizer_fixture() != 0) {
        return 1;
    }
    if (sql_parse("SELECT p.name FROM people p, people q "
        "WHERE p.age = q.age;", &program, NULL, NULL) != 0) {
        return 1;
    }
    if (sqlopt_optimize(&program, optimizer_root_path, "demo") != 0) {
        return 1;
    }
    if (!program.where.active
        || program.where_bound_slot_count != program.where.node_count) {
        return 1;
    }
    node = &program.where_nodes[program.where.root];
    if (node->type != sql_where_compare || node->value_count != 1u) {
        return 1;
    }
    value_slot = program.where_value_slots[node->value_first];
    return program.where_left_slots[program.where.root] == 0x02u
        && value_slot == 0x12u ? 0 : 1;
}

static int test_optimize_preserves_having(void)
{
    sqlexec_program program;
    char dump[512];
    const sql_where_node *having_root;

    if (prepare_optimizer_fixture() != 0) {
        return 1;
    }
    if (sql_parse("SELECT city, MAX(age) AS max_age "
        "FROM people WHERE age >= 18 "
        "GROUP BY city HAVING max_age > 18;",
        &program, NULL, NULL) != 0) {
        return 1;
    }
    if (sqlopt_optimize(&program, optimizer_root_path, "demo") != 0) {
        return 1;
    }
    if (!program.having.active
        || program.having.root == (unsigned char)sql_where_nil
        || program.having.root >= program.having.node_count) {
        return 1;
    }

    having_root = &program_having_nodes(&program)[program.having.root];
    if (having_root->type != sql_where_compare
        || strcmp(having_root->column_name, "max_age") != 0
        || having_root->operator != sql_compare_greater) {
        return 1;
    }
    if (sqlexec_dump(&program, dump, sizeof(dump)) != 0
        || strstr(dump, "having max_age > 18") == NULL) {
        return 1;
    }
    return 0;
}

static int test_optimize_where_runtime_without_binding(void)
{
    sqlexec_program program;
    dbf_field fields[3];
    unsigned short offsets[3];
    char left_record[14];
    char right_record[14];
    row_source sources[2];
    where_term terms[sql_where_max_nodes];
    unsigned char term_count;
    unsigned char mask_a;
    unsigned char mask_b;

    if (prepare_optimizer_fixture() != 0) {
        return 1;
    }
    if (sql_parse("SELECT p.name FROM people p, people q "
        "WHERE p.age = q.age AND q.city = 'LON';",
        &program, NULL, NULL) != 0) {
        return 1;
    }
    if (sqlopt_optimize(&program, optimizer_root_path, "demo") != 0) {
        return 1;
    }
    if (!where_can_use_program_binding(&program, &program.where)) {
        return 1;
    }

    memset(fields, 0, sizeof(fields));
    strcpy(fields[0].name, "name");
    fields[0].type = 'C';
    fields[0].length = 8;
    strcpy(fields[1].name, "city");
    fields[1].type = 'C';
    fields[1].length = 3;
    strcpy(fields[2].name, "age");
    fields[2].type = 'N';
    fields[2].length = 3;
    offsets[0] = 0;
    offsets[1] = 8;
    offsets[2] = 11;

    memset(left_record, ' ', sizeof(left_record));
    memset(right_record, ' ', sizeof(right_record));
    memcpy(left_record, "Ada", 3);
    memcpy(left_record + offsets[1], "ROM", 3);
    memcpy(left_record + offsets[2], "20", 2);
    memcpy(right_record, "Bob", 3);
    memcpy(right_record + offsets[1], "LON", 3);
    memcpy(right_record + offsets[2], "20", 2);

    memset(sources, 0, sizeof(sources));
    sources[0].table_name = "people";
    sources[0].alias = "p";
    sources[0].fields = fields;
    sources[0].offsets = offsets;
    sources[0].record = left_record;
    sources[0].field_count = 3;
    sources[1].table_name = "people";
    sources[1].alias = "q";
    sources[1].fields = fields;
    sources[1].offsets = offsets;
    sources[1].record = right_record;
    sources[1].field_count = 3;

    if (where_split_conjuncts_bound(&program, &program.where, NULL,
        terms, &term_count) != 0) {
        return 1;
    }
    mask_a = terms[0].source_mask;
    mask_b = terms[1].source_mask;
    if (term_count != 2u
        || !((mask_a == 0x03u && mask_b == 0x02u)
            || (mask_a == 0x02u && mask_b == 0x03u))
        || terms[0].ready_depth != 1u
        || terms[1].ready_depth != 1u
        || !where_matches_bound_n(&program, &program.where, NULL,
            sources, NULL)) {
        return 1;
    }

    memcpy(right_record + offsets[1], "ROM", 3);
    return where_matches_bound_n(&program, &program.where, NULL,
        sources, NULL) ? 1 : 0;
}

int main(void)
{
    cleanup_optimizer_fixture();

    if (test_optimize_select_eq() != 0) {
        printf("test_sqlopt: optimize select eq fail\n");
        return 1;
    }

    if (test_optimize_select_range() != 0) {
        printf("test_sqlopt: optimize select range fail\n");
        return 1;
    }

    if (test_optimize_select_conjunctive_eq() != 0) {
        printf("test_sqlopt: optimize conjunctive eq fail\n");
        return 1;
    }

    if (test_optimize_select_single_value_in() != 0) {
        printf("test_sqlopt: optimize single value in fail\n");
        return 1;
    }

    if (test_optimize_select_merged_range() != 0) {
        printf("test_sqlopt: optimize merged range fail\n");
        return 1;
    }

    if (test_optimize_select_range_keeps_other_conjunct() != 0) {
        printf("test_sqlopt: optimize range prune fail\n");
        return 1;
    }

    if (test_optimize_skips_composite_index() != 0) {
        printf("test_sqlopt: optimize composite skip fail\n");
        return 1;
    }

    if (test_optimize_skips_complex_where() != 0) {
        printf("test_sqlopt: optimize complex skip fail\n");
        return 1;
    }

    if (test_optimize_duplicate_conjuncts() != 0) {
        printf("test_sqlopt: optimize duplicate conjuncts fail\n");
        return 1;
    }

    if (test_optimize_redundant_bounds() != 0) {
        printf("test_sqlopt: optimize redundant bounds fail\n");
        return 1;
    }

    if (test_optimize_equal_bounds_to_eq() != 0) {
        printf("test_sqlopt: optimize equal bounds fail\n");
        return 1;
    }

    if (test_optimize_contradictory_conjuncts() != 0) {
        printf("test_sqlopt: optimize contradictory conjuncts fail\n");
        return 1;
    }

    if (test_optimize_not_compare() != 0) {
        printf("test_sqlopt: optimize not compare fail\n");
        return 1;
    }

    if (test_optimize_same_column_compare() != 0) {
        printf("test_sqlopt: optimize same column compare fail\n");
        return 1;
    }

    if (test_optimize_where_source_masks() != 0) {
        printf("test_sqlopt: optimize source masks fail\n");
        return 1;
    }

    if (test_optimize_where_bound_slots() != 0) {
        printf("test_sqlopt: optimize bound slots fail\n");
        return 1;
    }

    if (test_optimize_preserves_having() != 0) {
        printf("test_sqlopt: optimize preserve having fail\n");
        return 1;
    }

    if (test_optimize_where_runtime_without_binding() != 0) {
        printf("test_sqlopt: optimize runtime binding fail\n");
        return 1;
    }

    cleanup_optimizer_fixture();
    printf("test_sqlopt: ok\n");
    return 0;
}
