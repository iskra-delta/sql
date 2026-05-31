/*
 * Provides small automated tests for the dBase III NDX library.
 * The tests build fresh indexes from DBF tables and also open one
 * sample fixture so the page-reader code is checked against a
 * pre-existing NDX tree.
 *
 * MIT License (see: LICENSE)
 * Copyright (C) 2026 tomaz stih
 */

#include "dbf.h"
#include "ndx.h"

#include <stdio.h>
#include <string.h>

#define ndx_test_record_length 22
#define ndx_decimal_record_length 6

typedef struct walk_capture {
    unsigned long records[64];
    unsigned short count;
} walk_capture;

typedef struct ndx_fixture_case {
    const char *path;
    const char *expression;
    unsigned long root_page;
    unsigned long total_pages;
    unsigned short key_length;
    unsigned short key_type;
    unsigned short expected_count;
    unsigned long first_record;
    unsigned long last_record;
} ndx_fixture_case;

static walk_capture *walk_target;

/*
 * Stores visited record numbers into a small test capture buffer.
 */
static int capture_records(unsigned long record_number)
{
    if (walk_target == NULL) {
        return 1;
    }
    if (walk_target->count >= 64) {
        return 1;
    }

    walk_target->records[walk_target->count++] = record_number;
    return 0;
}

/*
 * Fills the DBF fields used by the NDX build tests.
 */
static void fill_fields(dbf_field *fields)
{
    strcpy(fields[0].name, "name");
    fields[0].type = 'C';
    fields[0].length = 8;
    fields[0].decimals = 0;

    strcpy(fields[1].name, "city");
    fields[1].type = 'C';
    fields[1].length = 3;
    fields[1].decimals = 0;

    strcpy(fields[2].name, "id");
    fields[2].type = 'N';
    fields[2].length = 3;
    fields[2].decimals = 0;

    strcpy(fields[3].name, "born");
    fields[3].type = 'D';
    fields[3].length = 8;
    fields[3].decimals = 0;
}

/*
 * Writes one small DBF fixture table used by the index build tests.
 */
static int build_table(dbf_file *table)
{
    dbf_field fields[4];
    char record[ndx_test_record_length];
    const char *path;

    fill_fields(fields);
    path = "../bin/ndx_build.dbf";

    if (dbf_create(table, path, fields, 4) != 0) {
        return 1;
    }

    memcpy(record, "alice   ATL01020240102", ndx_test_record_length);
    if (dbf_append(table, record) != 0) {
        dbf_close(table);
        return 1;
    }

    memcpy(record, "carol   BOS00220231231", ndx_test_record_length);
    if (dbf_append(table, record) != 0) {
        dbf_close(table);
        return 1;
    }

    memcpy(record, "bob     ATL00520240101", ndx_test_record_length);
    if (dbf_append(table, record) != 0) {
        dbf_close(table);
        return 1;
    }

    memcpy(record, "dave    ATL00520240101", ndx_test_record_length);
    if (dbf_append(table, record) != 0) {
        dbf_close(table);
        return 1;
    }

    memcpy(record, "eric    DEN01120230101", ndx_test_record_length);
    if (dbf_append(table, record) != 0) {
        dbf_close(table);
        return 1;
    }

    if (dbf_delete(table, 4) != 0) {
        dbf_close(table);
        return 1;
    }

    return 0;
}

/*
 * Writes one small DBF table with fractional numeric text values.
 */
static int build_decimal_table(dbf_file *table)
{
    dbf_field field;
    char record[ndx_decimal_record_length];
    const char *path;

    strcpy(field.name, "value");
    field.type = 'N';
    field.length = ndx_decimal_record_length;
    field.decimals = 2;
    path = "../bin/ndx_decimal.dbf";

    if (dbf_create(table, path, &field, 1) != 0) {
        return 1;
    }

    memcpy(record, "  1.20", ndx_decimal_record_length);
    if (dbf_append(table, record) != 0) {
        dbf_close(table);
        return 1;
    }

    memcpy(record, "  1.25", ndx_decimal_record_length);
    if (dbf_append(table, record) != 0) {
        dbf_close(table);
        return 1;
    }

    memcpy(record, "  1.10", ndx_decimal_record_length);
    if (dbf_append(table, record) != 0) {
        dbf_close(table);
        return 1;
    }

    return 0;
}

/*
 * Compares a captured walk order against a fixed expected list.
 */
static int walk_matches(const walk_capture *capture,
    const unsigned long *records, unsigned short count)
{
    unsigned short index;

    if (capture->count != count) {
        return 0;
    }

    for (index = 0; index < count; index++) {
        if (capture->records[index] != records[index]) {
            return 0;
        }
    }

    return 1;
}

/*
 * Opens one real NDX fixture and verifies its metadata and walk order.
 * Returns zero on success and one on failure.
 */
static int check_fixture(const ndx_fixture_case *fixture)
{
    ndx_file index;
    walk_capture capture;

    capture.count = 0;
    walk_target = &capture;

    if (ndx_open(&index, fixture->path) != 0) {
        return 1;
    }

    if (index.root_page != fixture->root_page
        || index.total_pages != fixture->total_pages
        || index.key_length != fixture->key_length
        || index.key_type != fixture->key_type
        || strcmp(index.expression, fixture->expression) != 0) {
        ndx_close(&index);
        return 1;
    }

    if (ndx_walk(&index, capture_records) != 0) {
        ndx_close(&index);
        return 1;
    }

    if (capture.count != fixture->expected_count) {
        ndx_close(&index);
        return 1;
    }

    if (fixture->expected_count > 0) {
        if (capture.records[0] != fixture->first_record
            || capture.records[capture.count - 1]
                != fixture->last_record) {
            ndx_close(&index);
            return 1;
        }
    }

    if (ndx_close(&index) != 0) {
        return 1;
    }

    return 0;
}

/*
 * Exercises a single character-field index build and lookup.
 * Returns zero on success and one on failure.
 */
static int test_build_name_index(void)
{
    static const unsigned long expected[4] = { 1, 3, 2, 4 };
    dbf_field fields[4];
    dbf_file table;
    ndx_file index;
    walk_capture capture;
    unsigned short key_fields[1];
    unsigned long record_number;

    fill_fields(fields);
    capture.count = 0;
    walk_target = &capture;
    key_fields[0] = 0;

    if (build_table(&table) != 0) {
        return 1;
    }

    if (ndx_create(&index, "../bin/name.ndx", &table, fields, 4,
        key_fields, 1, 0) != 0) {
        dbf_close(&table);
        return 1;
    }

    if (strcmp(index.expression, "name") != 0
        || index.key_type != ndx_key_type_character
        || index.key_length != 8) {
        ndx_close(&index);
        dbf_close(&table);
        return 1;
    }

    if (ndx_find_text(&index, "alice", &record_number) != 0
        || record_number != 1UL) {
        ndx_close(&index);
        dbf_close(&table);
        return 1;
    }

    if (ndx_find_text(&index, "zoe", &record_number) != 1) {
        ndx_close(&index);
        dbf_close(&table);
        return 1;
    }

    if (ndx_walk(&index, capture_records) != 0
        || !walk_matches(&capture, expected, 4)) {
        ndx_close(&index);
        dbf_close(&table);
        return 1;
    }

    if (ndx_close(&index) != 0 || dbf_close(&table) != 0) {
        return 1;
    }

    return 0;
}

/*
 * Exercises a numeric-field index build and exact lookup.
 * Returns zero on success and one on failure.
 */
static int test_build_id_index(void)
{
    static const unsigned long expected[4] = { 2, 3, 4, 1 };
    dbf_field fields[4];
    dbf_file table;
    ndx_file index;
    walk_capture capture;
    unsigned short key_fields[1];
    unsigned long record_number;

    fill_fields(fields);
    capture.count = 0;
    walk_target = &capture;
    key_fields[0] = 2;

    if (dbf_open(&table, "../bin/ndx_build.dbf") != 0) {
        return 1;
    }

    if (ndx_create(&index, "../bin/id.ndx", &table, fields, 4,
        key_fields, 1, 0) != 0) {
        dbf_close(&table);
        return 1;
    }

    if (strcmp(index.expression, "id") != 0
        || index.key_type != ndx_key_type_numeric
        || index.key_length != 8) {
        ndx_close(&index);
        dbf_close(&table);
        return 1;
    }

    if (ndx_find_number(&index, "2", &record_number) != 0
        || record_number != 2UL) {
        ndx_close(&index);
        dbf_close(&table);
        return 1;
    }

    if (ndx_find_number(&index, "5", &record_number) != 0
        || record_number != 3UL) {
        ndx_close(&index);
        dbf_close(&table);
        return 1;
    }

    if (ndx_walk(&index, capture_records) != 0
        || !walk_matches(&capture, expected, 4)) {
        ndx_close(&index);
        dbf_close(&table);
        return 1;
    }

    if (ndx_close(&index) != 0 || dbf_close(&table) != 0) {
        return 1;
    }

    return 0;
}

/*
 * Exercises exact fractional numeric packing from decimal text.
 * Returns zero on success and one on failure.
 */
static int test_build_fractional_index(void)
{
    static const unsigned long expected[3] = { 3, 1, 2 };
    dbf_field field;
    dbf_file table;
    ndx_file index;
    walk_capture capture;
    unsigned short key_fields[1];
    unsigned long record_number;

    strcpy(field.name, "value");
    field.type = 'N';
    field.length = ndx_decimal_record_length;
    field.decimals = 2;
    capture.count = 0;
    walk_target = &capture;
    key_fields[0] = 0;

    if (build_decimal_table(&table) != 0) {
        return 1;
    }

    if (ndx_create(&index, "../bin/decimal.ndx", &table, &field, 1,
        key_fields, 1, 0) != 0) {
        dbf_close(&table);
        return 1;
    }

    if (ndx_find_number(&index, "1.2", &record_number) != 0
        || record_number != 1UL) {
        ndx_close(&index);
        dbf_close(&table);
        return 1;
    }

    if (ndx_find_number(&index, "1.25", &record_number) != 0
        || record_number != 2UL) {
        ndx_close(&index);
        dbf_close(&table);
        return 1;
    }

    if (ndx_walk(&index, capture_records) != 0
        || !walk_matches(&capture, expected, 3)) {
        ndx_close(&index);
        dbf_close(&table);
        return 1;
    }

    if (ndx_close(&index) != 0 || dbf_close(&table) != 0) {
        return 1;
    }

    return 0;
}

/*
 * Exercises a date-field index build and exact lookup.
 * Returns zero on success and one on failure.
 */
static int test_build_date_index(void)
{
    static const unsigned long expected[4] = { 2, 3, 4, 1 };
    dbf_field fields[4];
    dbf_file table;
    ndx_file index;
    walk_capture capture;
    unsigned short key_fields[1];
    unsigned long record_number;

    fill_fields(fields);
    capture.count = 0;
    walk_target = &capture;
    key_fields[0] = 3;

    if (dbf_open(&table, "../bin/ndx_build.dbf") != 0) {
        return 1;
    }

    if (ndx_create(&index, "../bin/born.ndx", &table, fields, 4,
        key_fields, 1, 0) != 0) {
        dbf_close(&table);
        return 1;
    }

    if (strcmp(index.expression, "born") != 0
        || index.key_type != ndx_key_type_numeric
        || index.key_length != 8) {
        ndx_close(&index);
        dbf_close(&table);
        return 1;
    }

    if (ndx_find_date(&index, "20240101", &record_number) != 0
        || record_number != 3UL) {
        ndx_close(&index);
        dbf_close(&table);
        return 1;
    }

    if (ndx_walk(&index, capture_records) != 0
        || !walk_matches(&capture, expected, 4)) {
        ndx_close(&index);
        dbf_close(&table);
        return 1;
    }

    if (ndx_close(&index) != 0 || dbf_close(&table) != 0) {
        return 1;
    }

    return 0;
}

/*
 * Exercises a composite character index and a UNIQUE character index.
 * Returns zero on success and one on failure.
 */
static int test_build_composite_indexes(void)
{
    static const unsigned long expected_composite[4] = { 1, 3, 4, 2 };
    static const unsigned long expected_unique[2] = { 1, 2 };
    dbf_field fields[4];
    dbf_file table;
    ndx_file index;
    walk_capture capture;
    unsigned short composite_fields[2];
    unsigned short unique_fields[1];
    unsigned long record_number;

    fill_fields(fields);
    composite_fields[0] = 1;
    composite_fields[1] = 0;
    unique_fields[0] = 1;
    walk_target = &capture;

    if (dbf_open(&table, "../bin/ndx_build.dbf") != 0) {
        return 1;
    }

    capture.count = 0;
    if (ndx_create(&index, "../bin/city_name.ndx", &table, fields, 4,
        composite_fields, 2, 0) != 0) {
        dbf_close(&table);
        return 1;
    }

    if (strcmp(index.expression, "city+name") != 0
        || index.key_type != ndx_key_type_character
        || index.key_length != 11) {
        ndx_close(&index);
        dbf_close(&table);
        return 1;
    }

    if (ndx_find_text(&index, "ATLbob", &record_number) != 0
        || record_number != 3UL) {
        ndx_close(&index);
        dbf_close(&table);
        return 1;
    }

    if (ndx_walk(&index, capture_records) != 0
        || !walk_matches(&capture, expected_composite, 4)) {
        ndx_close(&index);
        dbf_close(&table);
        return 1;
    }

    if (ndx_close(&index) != 0) {
        dbf_close(&table);
        return 1;
    }

    capture.count = 0;
    if (ndx_create(&index, "../bin/city_unique.ndx", &table, fields, 4,
        unique_fields, 1, 1) != 0) {
        dbf_close(&table);
        return 1;
    }

    if (ndx_find_text(&index, "ATL", &record_number) != 0
        || record_number != 1UL) {
        ndx_close(&index);
        dbf_close(&table);
        return 1;
    }

    if (ndx_walk(&index, capture_records) != 0
        || !walk_matches(&capture, expected_unique, 2)) {
        ndx_close(&index);
        dbf_close(&table);
        return 1;
    }

    if (ndx_close(&index) != 0 || dbf_close(&table) != 0) {
        return 1;
    }

    return 0;
}

/*
 * Opens one existing sample NDX and verifies its sorted walk order.
 * Returns zero on success and one on failure.
 */
static int test_fixture_open_walk(void)
{
    static const ndx_fixture_case fixtures[] = {
        {
            "../tests/data/db/aval_flt.ndx",
            "adep_city+ades_city+dtoc(adate) ",
            1UL, 2UL, 14, ndx_key_type_character, 13, 2UL, 11UL
        },
        {
            "../tests/data/db/chkno.ndx",
            "Chkno ",
            1UL, 2UL, 8, ndx_key_type_numeric, 0, 0UL, 0UL
        },
        {
            "../tests/data/db/cnames.ndx",
            "LASTNAME + FIRSTNAME ",
            6UL, 7UL, 40, ndx_key_type_character, 49, 15UL, 8UL
        },
        {
            "../tests/data/db/customer.ndx",
            "CDES_CITY+CNAME ",
            1UL, 2UL, 28, ndx_key_type_character, 5, 1UL, 2UL
        },
        {
            "../tests/data/db/flt_no.ndx",
            "aflt_no ",
            1UL, 2UL, 3, ndx_key_type_character, 13, 6UL, 2UL
        },
        {
            "../tests/data/db/location.ndx",
            "STATE ",
            3UL, 4UL, 2, ndx_key_type_character, 49, 3UL, 40UL
        },
        {
            "../tests/data/db/names.ndx",
            "LASTNAME + FIRSTNAME ",
            6UL, 7UL, 40, ndx_key_type_character, 49, 15UL, 8UL
        },
        {
            "../tests/data/db/tnames.ndx",
            "LASTNAME + FIRSTNAME ",
            6UL, 7UL, 40, ndx_key_type_character, 49, 15UL, 8UL
        },
        {
            "../tests/data/db/tourdate.ndx",
            "DEPARTURE ",
            1UL, 2UL, 8, ndx_key_type_numeric, 30, 1UL, 26UL
        },
        {
            "../tests/data/db/trips.ndx",
            "TRAVELCODE + LASTNAME ",
            5UL, 6UL, 24, ndx_key_type_character, 49, 23UL, 11UL
        },
        {
            "../tests/data/db/zipcode.ndx",
            "ZIPCODE ",
            3UL, 4UL, 5, ndx_key_type_character, 49, 27UL, 18UL
        }
    };
    unsigned short index;

    for (index = 0; index < sizeof(fixtures) / sizeof(fixtures[0]);
        index++) {
        if (check_fixture(&fixtures[index]) != 0) {
            return 1;
        }
    }

    return 0;
}

int main(void)
{
    if (test_build_name_index() != 0) {
        printf("test_ndx: name build fail\n");
        return 1;
    }

    if (test_build_id_index() != 0) {
        printf("test_ndx: id build fail\n");
        return 1;
    }

    if (test_build_fractional_index() != 0) {
        printf("test_ndx: decimal build fail\n");
        return 1;
    }

    if (test_build_date_index() != 0) {
        printf("test_ndx: date build fail\n");
        return 1;
    }

    if (test_build_composite_indexes() != 0) {
        printf("test_ndx: composite build fail\n");
        return 1;
    }

    if (test_fixture_open_walk() != 0) {
        printf("test_ndx: fixture walk fail\n");
        return 1;
    }

    printf("test_ndx: ok\n");
    return 0;
}
