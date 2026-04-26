/*
 * Provides tiny automated tests for the DBF reader and writer.
 * The tests avoid external frameworks so the same code style can
 * later move toward SDCC and CP/M with minimal adaptation.
 *
 * MIT License (see: LICENSE)
 * Copyright (C) 2026 tomaz stih
 */

#include "dbf.h"

#include <stdio.h>
#include <string.h>

#define dbf_fixture_count 11
#define dbf_record_buffer_size 4096

/*
 * Compares two fixed-length buffers byte by byte.
 * Returns one when both match and zero when they differ.
 */
static int buffer_matches(const char *left, const char *right,
    unsigned short size)
{
    unsigned short index;

    for (index = 0; index < size; index++) {
        if (left[index] != right[index]) {
            return 0;
        }
    }

    return 1;
}

/*
 * Fills the field definitions used by the DBF smoke tests.
 */
static void fill_fields(dbf_field *fields)
{
    strcpy(fields[0].name, "name");
    fields[0].type = 'C';
    fields[0].length = 8;
    fields[0].decimals = 0;

    strcpy(fields[1].name, "id");
    fields[1].type = 'N';
    fields[1].length = 3;
    fields[1].decimals = 0;
}

/*
 * Exercises create, append, reopen, and read in one pass.
 * Returns zero on success and one on failure.
 */
static int test_create_append_read(void)
{
    dbf_field fields[2];
    dbf_file file;
    char written[11];
    char read_back[11];
    int state;
    const char *path;

    fill_fields(fields);
    path = "../bin/test.dbf";

    if (dbf_create(&file, path, fields, 2) != 0) {
        return 1;
    }

    memcpy(written, "alice   001", 11);
    if (dbf_append(&file, written) != 0) {
        dbf_close(&file);
        return 1;
    }

    if (dbf_close(&file) != 0) {
        return 1;
    }

    if (dbf_open(&file, path) != 0) {
        return 1;
    }

    state = dbf_read(&file, 0, read_back);
    if (dbf_close(&file) != 0) {
        return 1;
    }

    if (state != 0) {
        return 1;
    }

    if (!buffer_matches(written, read_back, 11)) {
        return 1;
    }

    return 0;
}

/*
 * Exercises record update and record deletion on one DBF file.
 * Returns zero on success and one on failure.
 */
static int test_write_delete(void)
{
    dbf_field fields[2];
    dbf_file file;
    char first[11];
    char second[11];
    char read_back[11];
    int state;
    const char *path;

    fill_fields(fields);
    path = "../bin/edit.dbf";

    if (dbf_create(&file, path, fields, 2) != 0) {
        return 1;
    }

    memcpy(first, "alice   001", 11);
    memcpy(second, "bob     002", 11);

    if (dbf_append(&file, first) != 0) {
        dbf_close(&file);
        return 1;
    }

    if (dbf_append(&file, second) != 0) {
        dbf_close(&file);
        return 1;
    }

    memcpy(second, "carol   003", 11);
    if (dbf_write(&file, 1, second) != 0) {
        dbf_close(&file);
        return 1;
    }

    if (dbf_delete(&file, 0) != 0) {
        dbf_close(&file);
        return 1;
    }

    state = dbf_read(&file, 0, read_back);
    if (state != 1) {
        dbf_close(&file);
        return 1;
    }

    state = dbf_read(&file, 1, read_back);
    if (state != 0) {
        dbf_close(&file);
        return 1;
    }

    if (!buffer_matches(second, read_back, 11)) {
        dbf_close(&file);
        return 1;
    }

    if (dbf_close(&file) != 0) {
        return 1;
    }

    return 0;
}

/*
 * Verifies that DBF field descriptors can be read back after create.
 * Returns zero on success and one on failure.
 */
static int test_read_fields(void)
{
    dbf_field written[2];
    dbf_field read_back[2];
    dbf_file file;
    const char *path;

    fill_fields(written);
    path = "../bin/fields.dbf";

    if (dbf_create(&file, path, written, 2) != 0) {
        return 1;
    }

    if (dbf_read_fields(&file, read_back, 2) != 0) {
        dbf_close(&file);
        return 1;
    }

    if (dbf_close(&file) != 0) {
        return 1;
    }

    if (strcmp(read_back[0].name, "name") != 0
        || read_back[0].type != 'C'
        || read_back[0].length != 8) {
        return 1;
    }

    if (strcmp(read_back[1].name, "id") != 0
        || read_back[1].type != 'N'
        || read_back[1].length != 3) {
        return 1;
    }

    return 0;
}

/*
 * Opens every DBF fixture and reads sample records when supported.
 * Old dBase II input is expected to be rejected cleanly.
 * Returns zero on success and one on failure.
 */
static int test_fixture_reads(void)
{
    static const char *paths[dbf_fixture_count] = {
        "../tests/data/cp1251.dbf",
        "../tests/data/dbase_02.dbf",
        "../tests/data/dbase_03.dbf",
        "../tests/data/dbase_03_cyrillic.dbf",
        "../tests/data/dbase_30.dbf",
        "../tests/data/dbase_31.dbf",
        "../tests/data/dbase_83.dbf",
        "../tests/data/dbase_83_missing_memo.dbf",
        "../tests/data/dbase_8b.dbf",
        "../tests/data/dbase_8c.dbf",
        "../tests/data/polygon.dbf"
    };
    unsigned short index;
    dbf_file file;
    char record[dbf_record_buffer_size];
    int state;

    for (index = 0; index < dbf_fixture_count; index++) {
        if (strcmp(paths[index], "../tests/data/dbase_02.dbf") == 0) {
            if (dbf_open(&file, paths[index]) == 0) {
                printf("unexpected open success: %s\n", paths[index]);
                dbf_close(&file);
                return 1;
            }
            continue;
        }

        if (dbf_open(&file, paths[index]) != 0) {
            printf("open failed: %s\n", paths[index]);
            return 1;
        }

        if (file.record_length > dbf_record_buffer_size) {
            printf("record too large: %s\n", paths[index]);
            dbf_close(&file);
            return 1;
        }

        if (file.record_count > 0) {
            state = dbf_read(&file, 0, record);
            if (state < 0) {
                printf("first read failed: %s\n", paths[index]);
                dbf_close(&file);
                return 1;
            }

            state = dbf_read(&file, file.record_count - 1, record);
            if (state < 0) {
                printf("last read failed: %s\n", paths[index]);
                dbf_close(&file);
                return 1;
            }
        }

        if (dbf_close(&file) != 0) {
            printf("close failed: %s\n", paths[index]);
            return 1;
        }
    }

    return 0;
}

int main(void)
{
    if (test_create_append_read() != 0) {
        printf("test_dbf: fail\n");
        return 1;
    }

    if (test_write_delete() != 0) {
        printf("test_dbf: write delete fail\n");
        return 1;
    }

    if (test_read_fields() != 0) {
        printf("test_dbf: read fields fail\n");
        return 1;
    }

    if (test_fixture_reads() != 0) {
        printf("test_dbf: fixture read fail\n");
        return 1;
    }

    printf("test_dbf: ok\n");
    return 0;
}
