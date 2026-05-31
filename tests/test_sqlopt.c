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

static void set_slot_field(char *target, unsigned short slot)
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
    set_slot_field(record + catalog_name_length + catalog_path_length, 1);
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

static int test_optimize_select_eq(void)
{
    sqlexec_program program;
    const sqlexec_node *scan;
    sqlexec_ref emit;
    sqlexec_ref filter;
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

    emit = child_at(&program, program.root, 1);
    filter = child_at(&program, child_at(&program, emit, 0), 0);
    scan_ref = child_at(&program, filter, 0);
    scan = sqlexec_get_const(&program, scan_ref);
    if (!scan || scan->opcode != sqlexec_index_scan_eq
        || strcmp(scan->data.index_probe.index_name, "people_age") != 0
        || strcmp(scan->data.index_probe.value.text, "18") != 0) {
        return 1;
    }

    if (sqlexec_dump(&program, dump, sizeof(dump)) != 0
        || strstr(dump, "index_scan_eq people_age = 18") == NULL) {
        return 1;
    }

    return 0;
}

static int test_optimize_select_range(void)
{
    sqlexec_program program;
    const sqlexec_node *scan;
    sqlexec_ref emit;
    sqlexec_ref filter;
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

    emit = child_at(&program, program.root, 1);
    filter = child_at(&program, child_at(&program, emit, 0), 0);
    scan_ref = child_at(&program, filter, 0);
    scan = sqlexec_get_const(&program, scan_ref);
    if (!scan || scan->opcode != sqlexec_index_scan_range
        || strcmp(scan->data.index_range.index_name, "people_age") != 0
        || scan->data.index_range.lower_operator
            != sql_compare_greater_equal
        || strcmp(scan->data.index_range.lower_value.text, "18") != 0
        || scan->data.index_range.upper_operator != sql_compare_invalid) {
        return 1;
    }

    return 0;
}

static int test_optimize_skips_composite_index(void)
{
    sqlexec_program program;
    sqlexec_ref emit;
    sqlexec_ref filter;
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

    emit = child_at(&program, program.root, 1);
    filter = child_at(&program, child_at(&program, emit, 0), 0);
    scan_ref = child_at(&program, filter, 0);
    return program.nodes[scan_ref].opcode == sqlexec_table_scan ? 0 : 1;
}

static int test_optimize_skips_complex_where(void)
{
    sqlexec_program program;
    sqlexec_ref emit;
    sqlexec_ref filter;
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

    emit = child_at(&program, program.root, 1);
    filter = child_at(&program, child_at(&program, emit, 0), 0);
    scan_ref = child_at(&program, filter, 0);
    return program.nodes[scan_ref].opcode == sqlexec_table_scan ? 0 : 1;
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

    if (test_optimize_skips_composite_index() != 0) {
        printf("test_sqlopt: optimize composite skip fail\n");
        return 1;
    }

    if (test_optimize_skips_complex_where() != 0) {
        printf("test_sqlopt: optimize complex skip fail\n");
        return 1;
    }

    cleanup_optimizer_fixture();
    printf("test_sqlopt: ok\n");
    return 0;
}
