/*
 * Provides the tiny SQL shell entry point for hosted development.
 * The program parses one SQL statement, manages the database
 * catalog in a DBF file, and creates numbered database folders.
 *
 * MIT License (see: LICENSE)
 * Copyright (C) 2026 tomaz stih
 */

#include "dbf.h"
#include "sql.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__SDCC)
extern int mkdir(const char *path);
#else
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

#define catalog_name_length 16
#define catalog_path_length 48
#define catalog_slot_length 2
#define catalog_record_length 66
#define catalog_max_databases 15
#define path_buffer_size 128
#define sql_buffer_size 256

/*
 * Fills the DBF schema used by the system catalog file.
 */
static void fill_catalog_fields(dbf_field *fields)
{
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
}

/*
 * Copies one string into a fixed DBF character field with spaces.
 */
static void set_field(char *target, unsigned short length,
    const char *value)
{
    unsigned short index;

    for (index = 0; index < length; index++) {
        target[index] = ' ';
    }

    for (index = 0; index < length && value[index] != '\0'; index++) {
        target[index] = value[index];
    }
}

/*
 * Trims one fixed DBF field into a C string.
 */
static void get_field(char *target, unsigned short target_size,
    const char *source, unsigned short length)
{
    unsigned short size;

    size = length;
    while (size > 0 && source[size - 1] == ' ') {
        size--;
    }

    if (size + 1 > target_size) {
        size = (unsigned short)(target_size - 1);
    }

    memcpy(target, source, size);
    target[size] = '\0';
}

/*
 * Writes one numeric slot as a two-byte right-aligned DBF field.
 */
static void set_slot_field(char *target, unsigned short slot)
{
    target[0] = ' ';
    target[1] = '0';
    if (slot >= 10) {
        target[0] = (char)('0' + (slot / 10));
        target[1] = (char)('0' + (slot % 10));
    } else {
        target[1] = (char)('0' + slot);
    }
}

/*
 * Parses a two-byte DBF numeric slot field into an integer.
 */
static unsigned short get_slot_field(const char *source)
{
    unsigned short slot;

    slot = 0;
    if (source[0] >= '0' && source[0] <= '9') {
        slot = (unsigned short)(source[0] - '0');
    }

    if (source[1] >= '0' && source[1] <= '9') {
        slot = (unsigned short)((slot * 10) + (source[1] - '0'));
    }

    return slot;
}

/*
 * Joins a base path and one child name into a fixed buffer.
 */
static int join_path(char *target, const char *left, const char *right)
{
    size_t left_size;
    size_t right_size;

    left_size = strlen(left);
    right_size = strlen(right);
    if (left_size + 1 + right_size + 1 > path_buffer_size) {
        return -1;
    }

    strcpy(target, left);
    if (left_size > 0 && target[left_size - 1] != '/') {
        target[left_size] = '/';
        target[left_size + 1] = '\0';
    }

    strcat(target, right);
    return 0;
}

/*
 * Creates one directory if it does not already exist.
 */
static int ensure_directory(const char *path)
{
#if defined(__SDCC)
    return mkdir(path);
#else
    struct stat status;

    if (stat(path, &status) == 0) {
        if (S_ISDIR(status.st_mode)) {
            return 0;
        }
        return -1;
    }

    if (mkdir(path, 0775) != 0 && errno != EEXIST) {
        return -1;
    }

    return 0;
#endif
}

/*
 * Creates the root and sys directories for the catalog.
 */
static int ensure_root_layout(const char *root, char *sys_path)
{
    if (ensure_directory(root) != 0) {
        return -1;
    }

    if (join_path(sys_path, root, "sys") != 0) {
        return -1;
    }

    if (ensure_directory(sys_path) != 0) {
        return -1;
    }

    return 0;
}

/*
 * Creates the catalog DBF file when it does not already exist.
 */
static int ensure_catalog(const char *root, char *catalog_path)
{
    dbf_field fields[3];
    dbf_file file;
    char sys_path[path_buffer_size];

    if (ensure_root_layout(root, sys_path) != 0) {
        return -1;
    }

    if (join_path(catalog_path, sys_path, "db.dbf") != 0) {
        return -1;
    }

    if (dbf_open(&file, catalog_path) == 0) {
        return dbf_close(&file);
    }

    fill_catalog_fields(fields);
    if (dbf_create(&file, catalog_path, fields, 3) != 0) {
        return -1;
    }

    return dbf_close(&file);
}

/*
 * Creates one raw catalog record from database data.
 */
static void build_catalog_record(char *record, const char *name,
    const char *path, unsigned short slot)
{
    set_field(record, catalog_name_length, name);
    set_field(record + catalog_name_length, catalog_path_length, path);
    set_slot_field(record + catalog_name_length + catalog_path_length,
        slot);
}

/*
 * Finds the next free slot and checks for duplicate names.
 */
static int find_catalog_slot(dbf_file *file, const char *name,
    unsigned short *slot_out)
{
    unsigned char used[catalog_max_databases + 1];
    unsigned long index;
    char record[catalog_record_length];
    char existing_name[catalog_name_length + 1];
    int state;
    unsigned short slot;

    for (slot = 0; slot <= catalog_max_databases; slot++) {
        used[slot] = 0;
    }

    for (index = 0; index < file->record_count; index++) {
        state = dbf_read(file, index, record);
        if (state < 0) {
            return -1;
        }

        if (state == 1) {
            continue;
        }

        get_field(existing_name, sizeof(existing_name), record,
            catalog_name_length);
        if (strcmp(existing_name, name) == 0) {
            return -1;
        }

        slot = get_slot_field(record + catalog_name_length
            + catalog_path_length);
        if (slot <= catalog_max_databases) {
            used[slot] = 1;
        }
    }

    for (slot = 1; slot <= catalog_max_databases; slot++) {
        if (!used[slot]) {
            *slot_out = slot;
            return 0;
        }
    }

    return -1;
}

/*
 * Runs CREATE DATABASE against the DBF-backed system catalog.
 */
static int execute_create_database(const char *root,
    const sql_statement *statement)
{
    dbf_file file;
    char catalog_path[path_buffer_size];
    char db_path[path_buffer_size];
    char slot_name[6];
    char record[catalog_record_length];
    unsigned short slot;

    if (ensure_catalog(root, catalog_path) != 0) {
        return -1;
    }

    if (dbf_open(&file, catalog_path) != 0) {
        return -1;
    }

    if (find_catalog_slot(&file, statement->name, &slot) != 0) {
        dbf_close(&file);
        return -1;
    }

    sprintf(slot_name, "%u", slot);
    if (join_path(db_path, root, slot_name) != 0) {
        dbf_close(&file);
        return -1;
    }

    if (ensure_directory(db_path) != 0) {
        dbf_close(&file);
        return -1;
    }

    build_catalog_record(record, statement->name, db_path, slot);
    if (dbf_append(&file, record) != 0) {
        dbf_close(&file);
        return -1;
    }

    if (dbf_close(&file) != 0) {
        return -1;
    }

    printf("created database %s at %s\n", statement->name, db_path);
    return 0;
}

/*
 * Runs SHOW DATABASES against the DBF-backed system catalog.
 */
static int execute_show_databases(const char *root)
{
    dbf_file file;
    char catalog_path[path_buffer_size];
    char record[catalog_record_length];
    char name[catalog_name_length + 1];
    char path[catalog_path_length + 1];
    unsigned long index;
    int state;

    if (ensure_catalog(root, catalog_path) != 0) {
        return -1;
    }

    if (dbf_open(&file, catalog_path) != 0) {
        return -1;
    }

    for (index = 0; index < file.record_count; index++) {
        state = dbf_read(&file, index, record);
        if (state < 0) {
            dbf_close(&file);
            return -1;
        }

        if (state == 1) {
            continue;
        }

        get_field(name, sizeof(name), record, catalog_name_length);
        get_field(path, sizeof(path), record + catalog_name_length,
            catalog_path_length);
        printf("%2u %s %s\n",
            get_slot_field(record + catalog_name_length
                + catalog_path_length),
            path, name);
    }

    return dbf_close(&file);
}

/*
 * Builds one SQL string from the remaining command-line arguments.
 */
static int build_sql(char *sql_text, int argc, char *argv[], int start)
{
    int index;

    sql_text[0] = '\0';
    for (index = start; index < argc; index++) {
        if (strlen(sql_text) + strlen(argv[index]) + 2
            > sql_buffer_size) {
            return -1;
        }

        if (sql_text[0] != '\0') {
            strcat(sql_text, " ");
        }
        strcat(sql_text, argv[index]);
    }

    return 0;
}

/*
 * Parses the minimal command line and returns the SQL start index.
 */
static int parse_options(int argc, char *argv[], const char **root_out)
{
    int index;

    *root_out = "db";
    index = 1;
    while (index < argc) {
        if (strcmp(argv[index], "--root") == 0) {
            index++;
            if (index >= argc) {
                return -1;
            }
            *root_out = argv[index];
            index++;
            continue;
        }

        break;
    }

    return index;
}

static int run_program(int argc, char *argv[])
{
    const char *root;
    const char *program_name;
    sql_statement statement;
    char sql_text[sql_buffer_size];
    int sql_index;

    program_name = "sql_demo";
    if (argc > 0 && argv != (char **)0 && argv[0] != (char *)0) {
        program_name = argv[0];
    }

    sql_index = parse_options(argc, argv, &root);
    if (sql_index < 0 || sql_index >= argc) {
        printf("usage: %s [--root path] \"SQL;\"\n", program_name);
        return 1;
    }

    if (build_sql(sql_text, argc, argv, sql_index) != 0) {
        return 1;
    }

    if (sql_parse(sql_text, &statement) != 0) {
        printf("sql parse error\n");
        return 1;
    }

    if (statement.type == sql_statement_create_database) {
        return execute_create_database(root, &statement);
    }

    if (statement.type == sql_statement_show_databases) {
        return execute_show_databases(root);
    }

    return 1;
}

#if defined(__SDCC)
int main(void)
{
    return run_program(0, (char **)0);
}
#else
int main(int argc, char *argv[])
{
    return run_program(argc, argv);
}
#endif
