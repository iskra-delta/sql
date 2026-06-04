/*
 * Exercises SQL shell index execution through the real hosted binary.
 * The test drives CREATE INDEX plus INSERT, UPDATE, DELETE, and
 * DROP TABLE, then inspects DBF and NDX files on disk to verify that
 * the global ndx.dbf catalog and table indexes stay in sync.
 *
 * MIT License (see: LICENSE)
 * Copyright (C) 2026 tomaz stih
 */

#include "dbf.h"
#include "ndx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define test_root_path "../bin/sql_index_root"
#define test_sql_path "../bin/sql"
#define test_commands_path "../bin/sql_index_commands.sql"
#define test_output_path "../bin/sql_index_output.txt"
#define index_catalog_path "../bin/sql_index_root/sys/ndx.dbf"
#define people_table_path "../bin/sql_index_root/1/people.dbf"
#define cities_table_path "../bin/sql_index_root/1/cities.dbf"
#define regions_table_path "../bin/sql_index_root/1/regions.dbf"
#define name_index_path "../bin/sql_index_root/1/name_idx.ndx"
#define city_name_index_path "../bin/sql_index_root/1/city_name.ndx"

#define index_catalog_db_length     16
#define index_catalog_name_length   16
#define index_catalog_table_length  16
#define index_catalog_fields_length ((16 * 11) + 15)
#define index_catalog_unique_length  1
#define index_catalog_record_length (index_catalog_db_length \
    + index_catalog_name_length + index_catalog_table_length \
    + index_catalog_fields_length + index_catalog_unique_length)

typedef struct record_capture {
    unsigned short count;
    unsigned long records[8];
} record_capture;

static record_capture capture;

static void trim_field(char *target, unsigned short target_size,
    const char *source, unsigned short length)
{
    unsigned short start;
    unsigned short end;
    unsigned short size;

    start = 0;
    end = length;
    while (start < length && source[start] == ' ') {
        start++;
    }
    while (end > start && source[end - 1] == ' ') {
        end--;
    }

    size = (unsigned short)(end - start);
    if (size + 1 > target_size) {
        size = (unsigned short)(target_size - 1);
    }

    memcpy(target, source + start, size);
    target[size] = '\0';
}

static void get_catalog_field(char *target, unsigned short target_size,
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

static int build_offsets(const dbf_field *fields, unsigned short field_count,
    unsigned short *offsets)
{
    unsigned short index;
    unsigned short offset;

    offset = 0;
    for (index = 0; index < field_count; index++) {
        offsets[index] = offset;
        offset = (unsigned short)(offset + fields[index].length);
    }

    return 0;
}

static int file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static void cleanup_files(void)
{
    unlink(test_output_path);
    unlink(test_commands_path);
    unlink(name_index_path);
    unlink(city_name_index_path);
    unlink(people_table_path);
    unlink(cities_table_path);
    unlink(regions_table_path);
    unlink(index_catalog_path);
    unlink("../bin/sql_index_root/sys/db.dbf");
    unlink("../bin/sql_index_root/sys/vw.dbf");
    rmdir("../bin/sql_index_root/sys");
    rmdir("../bin/sql_index_root/1");
    rmdir(test_root_path);
}

static int write_commands(const char *text)
{
    FILE *file;

    file = fopen(test_commands_path, "wb");
    if (!file) {
        return 1;
    }

    if (fputs(text, file) == EOF) {
        fclose(file);
        return 1;
    }

    if (fclose(file) != 0) {
        return 1;
    }

    return 0;
}

static int output_has_error(void)
{
    FILE *file;
    char line[256];

    file = fopen(test_output_path, "rb");
    if (!file) {
        return 1;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        if (strstr(line, "error") != NULL
            || strstr(line, "parse error") != NULL) {
            fclose(file);
            return 1;
        }
    }

    fclose(file);
    return 0;
}

static int output_contains(const char *needle)
{
    FILE *file;
    char line[256];

    file = fopen(test_output_path, "rb");
    if (!file) {
        return 0;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        if (strstr(line, needle) != NULL) {
            fclose(file);
            return 1;
        }
    }

    fclose(file);
    return 0;
}

static int run_shell_script(const char *commands)
{
    char command[512];

    if (write_commands(commands) != 0) {
        return 1;
    }

    snprintf(command, sizeof(command), "%s %s < %s > %s", test_sql_path,
        test_root_path, test_commands_path, test_output_path);
    if (system(command) != 0) {
        return 1;
    }

    return output_has_error();
}

static int capture_records(unsigned long record_number)
{
    if (capture.count >= sizeof(capture.records)
        / sizeof(capture.records[0])) {
        return 1;
    }

    capture.records[capture.count++] = record_number;
    return 0;
}

static int check_catalog_entry(const char *db_name, const char *index_name,
    const char *table_name, const char *field_list, const char *unique_text)
{
    dbf_file file;
    char record[index_catalog_record_length];
    char record_db[index_catalog_db_length + 1];
    char record_name[index_catalog_name_length + 1];
    char record_table[index_catalog_table_length + 1];
    char record_fields[index_catalog_fields_length + 1];
    char record_unique[index_catalog_unique_length + 1];
    unsigned long index;
    int state;

    if (dbf_open(&file, index_catalog_path) != 0) {
        return 1;
    }

    for (index = 0; index < file.record_count; index++) {
        state = dbf_read(&file, index, record);
        if (state < 0) {
            dbf_close(&file);
            return 1;
        }
        if (state == 1) {
            continue;
        }

        get_catalog_field(record_db, sizeof(record_db), record,
            index_catalog_db_length);
        get_catalog_field(record_name, sizeof(record_name),
            record + index_catalog_db_length, index_catalog_name_length);
        get_catalog_field(record_table, sizeof(record_table),
            record + index_catalog_db_length + index_catalog_name_length,
            index_catalog_table_length);
        get_catalog_field(record_fields, sizeof(record_fields),
            record + index_catalog_db_length + index_catalog_name_length
                + index_catalog_table_length,
            index_catalog_fields_length);
        get_catalog_field(record_unique, sizeof(record_unique),
            record + index_catalog_db_length + index_catalog_name_length
                + index_catalog_table_length + index_catalog_fields_length,
            index_catalog_unique_length);
        if (strcmp(record_db, db_name) == 0
            && strcmp(record_name, index_name) == 0
            && strcmp(record_table, table_name) == 0
            && strcmp(record_fields, field_list) == 0
            && strcmp(record_unique, unique_text) == 0) {
            dbf_close(&file);
            return 0;
        }
    }

    dbf_close(&file);
    return 1;
}

static int count_live_catalog_entries(void)
{
    dbf_file file;
    char record[index_catalog_record_length];
    unsigned long index;
    int state;
    int count;

    if (dbf_open(&file, index_catalog_path) != 0) {
        return -1;
    }

    count = 0;
    for (index = 0; index < file.record_count; index++) {
        state = dbf_read(&file, index, record);
        if (state < 0) {
            dbf_close(&file);
            return -1;
        }
        if (state == 0) {
            count++;
        }
    }

    if (dbf_close(&file) != 0) {
        return -1;
    }

    return count;
}

static int check_people_table(void)
{
    dbf_file file;
    dbf_field fields[3];
    unsigned short offsets[3];
    char record[32];
    char name[16];
    char city[16];
    char age[16];

    if (dbf_open(&file, people_table_path) != 0) {
        return 1;
    }
    if (dbf_read_fields(&file, fields, 3) != 0) {
        dbf_close(&file);
        return 1;
    }
    if (build_offsets(fields, file.field_count, offsets) != 0) {
        dbf_close(&file);
        return 1;
    }

    if (file.record_count != 3) {
        dbf_close(&file);
        return 1;
    }

    if (dbf_read(&file, 0, record) != 0) {
        dbf_close(&file);
        return 1;
    }
    trim_field(name, sizeof(name), record + offsets[0], fields[0].length);
    trim_field(city, sizeof(city), record + offsets[1], fields[1].length);
    trim_field(age, sizeof(age), record + offsets[2], fields[2].length);
    if (strcmp(name, "zoe") != 0 || strcmp(city, "LON") != 0
        || strcmp(age, "18") != 0) {
        dbf_close(&file);
        return 1;
    }

    if (dbf_read(&file, 1, record) != 1) {
        dbf_close(&file);
        return 1;
    }

    if (dbf_read(&file, 2, record) != 0) {
        dbf_close(&file);
        return 1;
    }
    trim_field(name, sizeof(name), record + offsets[0], fields[0].length);
    trim_field(city, sizeof(city), record + offsets[1], fields[1].length);
    trim_field(age, sizeof(age), record + offsets[2], fields[2].length);
    if (strcmp(name, "amy") != 0 || strcmp(city, "NYC") != 0
        || strcmp(age, "21") != 0) {
        dbf_close(&file);
        return 1;
    }

    return dbf_close(&file) != 0;
}

static int check_name_index(void)
{
    ndx_file file;
    unsigned long record_number;

    capture.count = 0;
    if (ndx_open(&file, name_index_path) != 0) {
        return 1;
    }

    if (ndx_find_text(&file, "amy", &record_number) != 0
        || record_number != 3UL) {
        ndx_close(&file);
        return 1;
    }
    if (ndx_find_text(&file, "zoe", &record_number) != 0
        || record_number != 1UL) {
        ndx_close(&file);
        return 1;
    }
    if (ndx_find_text(&file, "bob", &record_number) != 1) {
        ndx_close(&file);
        return 1;
    }
    if (ndx_walk(&file, capture_records) != 0) {
        ndx_close(&file);
        return 1;
    }
    if (capture.count != 2 || capture.records[0] != 3UL
        || capture.records[1] != 1UL) {
        ndx_close(&file);
        return 1;
    }

    return ndx_close(&file) != 0;
}

static int check_city_name_index(void)
{
    ndx_file file;
    unsigned long record_number;

    capture.count = 0;
    if (ndx_open(&file, city_name_index_path) != 0) {
        return 1;
    }

    if (ndx_find_text(&file, "LONzoe", &record_number) != 0
        || record_number != 1UL) {
        ndx_close(&file);
        return 1;
    }
    if (ndx_find_text(&file, "NYCamy", &record_number) != 0
        || record_number != 3UL) {
        ndx_close(&file);
        return 1;
    }
    if (ndx_walk(&file, capture_records) != 0) {
        ndx_close(&file);
        return 1;
    }
    if (capture.count != 2 || capture.records[0] != 1UL
        || capture.records[1] != 3UL) {
        ndx_close(&file);
        return 1;
    }

    return ndx_close(&file) != 0;
}

static int test_create_and_maintain_indexes(void)
{
    const char *commands =
        "CREATE DATABASE demo;\n"
        "CREATE TABLE people (name CHAR(8), city CHAR(3), age NUMERIC(3));\n"
        "INSERT INTO people VALUES ('alice', 'LON', 18);\n"
        "INSERT INTO people VALUES ('bob', 'ATL', 19);\n"
        "CREATE INDEX name_idx ON people (name);\n"
        "CREATE INDEX city_name ON people (city, name);\n"
        "UPDATE people SET name = 'zoe' WHERE name = 'alice';\n"
        "DELETE FROM people WHERE name = 'bob';\n"
        "INSERT INTO people VALUES ('amy', 'NYC', 21);\n";

    if (run_shell_script(commands) != 0) {
        return 1;
    }

    if (!file_exists(index_catalog_path) || !file_exists(people_table_path)
        || !file_exists(name_index_path)
        || !file_exists(city_name_index_path)) {
        return 1;
    }

    if (check_catalog_entry("demo", "name_idx", "people", "name", "N") != 0) {
        return 1;
    }
    if (check_catalog_entry("demo", "city_name", "people",
        "city,name", "N") != 0) {
        return 1;
    }
    if (count_live_catalog_entries() != 2) {
        return 1;
    }

    if (check_people_table() != 0) {
        return 1;
    }
    if (check_name_index() != 0) {
        return 1;
    }
    if (check_city_name_index() != 0) {
        return 1;
    }

    return 0;
}

static int test_drop_table_cleans_indexes(void)
{
    const char *commands =
        "USE demo;\n"
        "DROP TABLE people;\n";

    if (run_shell_script(commands) != 0) {
        return 1;
    }

    if (file_exists(people_table_path) || file_exists(name_index_path)
        || file_exists(city_name_index_path)) {
        return 1;
    }

    if (count_live_catalog_entries() != 0) {
        return 1;
    }

    return 0;
}

static int test_select_aliases(void)
{
    const char *commands =
        "USE demo;\n"
        "SELECT p.name AS person, p.age years FROM people AS p "
        "WHERE p.age >= 18;\n";

    if (run_shell_script(commands) != 0) {
        return 1;
    }

    if (!output_contains("zoe | 18")
        || !output_contains("amy | 21")
        || !output_contains("2 rows")) {
        return 1;
    }

    return 0;
}

static int test_select_logic_in(void)
{
    const char *commands =
        "USE demo;\n"
        "SELECT p.name FROM people AS p WHERE p.city = 'LON' OR p.age IN "
        "(21, 19);\n"
        "SELECT name FROM people WHERE city = 'LON' AND age IN (18, 21);\n";

    if (run_shell_script(commands) != 0) {
        return 1;
    }

    if (!output_contains("zoe")
        || !output_contains("amy")
        || !output_contains("2 rows")
        || !output_contains("1 row")) {
        return 1;
    }

    return 0;
}

static int test_select_same_source_column_compare(void)
{
    const char *commands =
        "USE demo;\n"
        "SELECT name FROM people WHERE age = age;\n";

    if (run_shell_script(commands) != 0) {
        return 1;
    }

    if (!output_contains("zoe")
        || !output_contains("amy")
        || !output_contains("2 rows")) {
        return 1;
    }

    return 0;
}

static int test_select_join(void)
{
    const char *commands =
        "USE demo;\n"
        "CREATE TABLE cities (code CHAR(3), title CHAR(16), region CHAR(2));\n"
        "INSERT INTO cities VALUES ('LON', 'London', 'EU');\n"
        "INSERT INTO cities VALUES ('NYC', 'New York', 'US');\n"
        "SELECT p.name, c.title FROM people AS p JOIN cities c "
        "ON p.city = c.code WHERE c.region = 'EU';\n";

    if (run_shell_script(commands) != 0) {
        return 1;
    }

    if (!file_exists(cities_table_path)
        || !output_contains("zoe | London")
        || output_contains("amy | New York")
        || !output_contains("1 row")) {
        return 1;
    }

    return 0;
}

static int test_select_table_list(void)
{
    const char *commands =
        "USE demo;\n"
        "SELECT p.name, c.title FROM people AS p, cities c "
        "WHERE p.city = c.code AND c.region = 'EU';\n";

    if (run_shell_script(commands) != 0) {
        return 1;
    }

    if (!output_contains("zoe | London")
        || output_contains("amy | New York")
        || !output_contains("1 row")) {
        return 1;
    }

    return 0;
}

static int test_select_multi_join_and_long_where(void)
{
    const char *commands =
        "USE demo;\n"
        "CREATE TABLE regions (code CHAR(2), name CHAR(16), zone CHAR(1));\n"
        "INSERT INTO regions VALUES ('EU', 'Europe', 'W');\n"
        "INSERT INTO regions VALUES ('US', 'America', 'E');\n"
        "SELECT p.name, c.title, r.name FROM people AS p "
        "JOIN cities c ON p.city = c.code "
        "JOIN regions r ON c.region = r.code "
        "WHERE p.age >= 18 AND c.region = 'EU' AND r.zone = 'W' "
        "AND p.name = 'zoe';\n";

    if (run_shell_script(commands) != 0) {
        return 1;
    }

    if (!file_exists(regions_table_path)
        || !output_contains("zoe | London | Europe")
        || output_contains("amy | New York | America")
        || !output_contains("1 row")) {
        return 1;
    }

    return 0;
}

static int test_select_group_and_trim(void)
{
    const char *commands =
        "USE demo;\n"
        "INSERT INTO people VALUES ('ian', 'LON', 24);\n"
        "SELECT TRIM(name) AS clean FROM people WHERE city = 'LON' "
        "AND age = 24;\n"
        "SELECT c.region, MAX(p.age) AS max_age, AVG(p.age) AS avg_age "
        "FROM people AS p JOIN cities c ON p.city = c.code "
        "GROUP BY c.region HAVING max_age > 21;\n";

    if (run_shell_script(commands) != 0) {
        return 1;
    }

    if (!output_contains("ian")
        || !output_contains("EU | 24 | 21")
        || output_contains("US | 21 | 21")
        || !output_contains("1 row")) {
        return 1;
    }

    return 0;
}

static int test_insert_column_list(void)
{
    const char *commands =
        "USE demo;\n"
        "INSERT INTO cities (title, code, region) "
        "VALUES ('Paris', 'PAR', 'EU');\n"
        "SELECT title FROM cities WHERE code = 'PAR';\n";

    if (run_shell_script(commands) != 0) {
        return 1;
    }

    if (!output_contains("Paris")
        || !output_contains("1 row")) {
        return 1;
    }

    return 0;
}

static int test_null_predicates(void)
{
    const char *commands =
        "USE demo;\n"
        "UPDATE people SET city = NULL WHERE name = 'zoe';\n"
        "SELECT name FROM people WHERE city IS NULL;\n"
        "SELECT name FROM people WHERE city IS NOT NULL;\n"
        "SELECT name FROM people WHERE city = NULL;\n"
        "SELECT name FROM people WHERE NOT city = NULL;\n"
        "SELECT name FROM people WHERE city = NULL OR name = 'amy';\n"
        "SELECT name FROM people WHERE city = NULL AND name = 'amy';\n";

    if (run_shell_script(commands) != 0) {
        return 1;
    }

    if (!output_contains("zoe")
        || !output_contains("amy")
        || !output_contains("ian")
        || !output_contains("2 rows")
        || !output_contains("1 row")
        || !output_contains("0 rows")) {
        return 1;
    }

    return 0;
}

static int test_subquery_predicates(void)
{
    const char *commands =
        "USE demo;\n"
        "SELECT name FROM people WHERE EXISTS "
        "(SELECT code FROM cities WHERE region = 'EU');\n"
        "SELECT name FROM people WHERE city = ANY "
        "(SELECT code FROM cities WHERE region = 'US');\n"
        "SELECT name FROM people WHERE city IN "
        "(SELECT code FROM cities WHERE region = 'EU');\n"
        "SELECT name FROM people WHERE age >= ALL "
        "(SELECT age FROM people WHERE city IS NOT NULL);\n"
        "SELECT name FROM people WHERE age > ALL "
        "(SELECT age FROM people WHERE city = 'ZZZ');\n"
        "SELECT name FROM people WHERE age = ANY "
        "(SELECT age FROM people WHERE city = 'ZZZ');\n";

    if (run_shell_script(commands) != 0) {
        return 1;
    }

    if (!output_contains("zoe")
        || !output_contains("amy")
        || !output_contains("ian")
        || !output_contains("3 rows")
        || !output_contains("1 row")
        || !output_contains("0 rows")) {
        return 1;
    }

    return 0;
}

static int test_exists_multi_column_subquery(void)
{
    const char *commands =
        "USE demo;\n"
        "SELECT name FROM people WHERE EXISTS "
        "(SELECT code, region FROM cities WHERE region = 'EU');\n";

    if (run_shell_script(commands) != 0) {
        return 1;
    }

    if (!output_contains("zoe")
        || !output_contains("amy")
        || !output_contains("ian")
        || !output_contains("3 rows")) {
        return 1;
    }

    return 0;
}

static int test_select_distinct_aggregates_and_predicates(void)
{
    const char *commands =
        "USE demo;\n"
        "SELECT DISTINCT city FROM people WHERE age BETWEEN 18 AND 24;\n"
        "SELECT city, COUNT(*) AS total, MIN(age) AS min_age, "
        "SUM(age) AS sum_age FROM people "
        "WHERE NOT city LIKE 'N%' AND age BETWEEN 18 AND 24 "
        "GROUP BY city HAVING total > 1;\n";

    if (run_shell_script(commands) != 0) {
        return 1;
    }

    if (!output_contains("LON")
        || !output_contains("NYC")
        || !output_contains("2 rows")
        || !output_contains("LON | 2 | 18 | 42")
        || output_contains("NYC | 1 | 21 | 21")
        || !output_contains("1 row")) {
        return 1;
    }

    return 0;
}

static int test_simple_view_flattening(void)
{
    const char *commands =
        "USE demo;\n"
        "CREATE VIEW adult_people AS "
        "SELECT * FROM people p WHERE p.age >= 18;\n"
        "SELECT name FROM adult_people WHERE city = 'LON';\n"
        "SELECT a.name, c.title FROM adult_people a "
        "JOIN cities c ON a.city = c.code WHERE c.region = 'EU';\n"
        "UPDATE adult_people SET city = 'LON' WHERE name = 'zoe';\n"
        "SELECT city FROM people WHERE name = 'zoe';\n";

    if (run_shell_script(commands) != 0) {
        return 1;
    }

    if (!output_contains("zoe")
        || !output_contains("ian")
        || !output_contains("zoe | London")
        || !output_contains("ian | London")
        || !output_contains("1 updated")
        || !output_contains("LON")
        || output_contains("amy | New York")) {
        return 1;
    }

    return 0;
}

static int test_projected_view_flattening(void)
{
    const char *commands =
        "USE demo;\n"
        "CREATE VIEW adult_names AS "
        "SELECT name AS person, city AS home FROM people p "
        "WHERE p.age >= 18;\n"
        "SELECT person FROM adult_names WHERE home = 'LON';\n"
        "SELECT a.person, c.title FROM adult_names a "
        "JOIN cities c ON a.home = c.code WHERE c.region = 'EU';\n";

    if (run_shell_script(commands) != 0) {
        return 1;
    }

    if (!output_contains("zoe")
        || !output_contains("ian")
        || !output_contains("zoe | London")
        || !output_contains("ian | London")
        || !output_contains("2 rows")
        || output_contains("amy | New York")) {
        return 1;
    }

    return 0;
}

static int test_nested_view_materialization(void)
{
    const char *commands =
        "USE demo;\n"
        "CREATE VIEW adult_names_2 AS "
        "SELECT name AS person, city AS home FROM people p "
        "WHERE p.age >= 18;\n"
        "CREATE VIEW eu_adult_names AS "
        "SELECT a.person, c.title FROM adult_names_2 a "
        "JOIN cities c ON a.home = c.code WHERE c.region = 'EU';\n"
        "SELECT person, title FROM eu_adult_names;\n";

    if (run_shell_script(commands) != 0) {
        return 1;
    }

    if (!output_contains("zoe | London")
        || !output_contains("ian | London")
        || !output_contains("2 rows")
        || output_contains("amy | New York")) {
        return 1;
    }

    return 0;
}

int main(void)
{
    cleanup_files();

    if (test_create_and_maintain_indexes() != 0) {
        printf("test_sql_exec: index maintenance fail\n");
        cleanup_files();
        return 1;
    }

    if (test_select_aliases() != 0) {
        printf("test_sql_exec: select alias fail\n");
        cleanup_files();
        return 1;
    }

    if (test_select_logic_in() != 0) {
        printf("test_sql_exec: select logic/in fail\n");
        cleanup_files();
        return 1;
    }

    if (test_select_same_source_column_compare() != 0) {
        printf("test_sql_exec: select same-source column compare fail\n");
        cleanup_files();
        return 1;
    }

    if (test_select_join() != 0) {
        printf("test_sql_exec: select join fail\n");
        cleanup_files();
        return 1;
    }

    if (test_select_table_list() != 0) {
        printf("test_sql_exec: select table list fail\n");
        cleanup_files();
        return 1;
    }

    if (test_select_multi_join_and_long_where() != 0) {
        printf("test_sql_exec: select multi join fail\n");
        cleanup_files();
        return 1;
    }

    if (test_select_group_and_trim() != 0) {
        printf("test_sql_exec: select group fail\n");
        cleanup_files();
        return 1;
    }

    if (test_insert_column_list() != 0) {
        printf("test_sql_exec: insert column list fail\n");
        cleanup_files();
        return 1;
    }

    if (test_select_distinct_aggregates_and_predicates() != 0) {
        printf("test_sql_exec: select distinct fail\n");
        cleanup_files();
        return 1;
    }

    if (test_simple_view_flattening() != 0) {
        printf("test_sql_exec: simple view flatten fail\n");
        cleanup_files();
        return 1;
    }

    if (test_projected_view_flattening() != 0) {
        printf("test_sql_exec: projected view flatten fail\n");
        cleanup_files();
        return 1;
    }

    if (test_nested_view_materialization() != 0) {
        printf("test_sql_exec: nested view materialization fail\n");
        cleanup_files();
        return 1;
    }

    if (test_null_predicates() != 0) {
        printf("test_sql_exec: null predicates fail\n");
        cleanup_files();
        return 1;
    }

    if (test_subquery_predicates() != 0) {
        printf("test_sql_exec: subquery predicates fail\n");
        cleanup_files();
        return 1;
    }

    if (test_exists_multi_column_subquery() != 0) {
        printf("test_sql_exec: exists multi-column fail\n");
        cleanup_files();
        return 1;
    }

    if (test_drop_table_cleans_indexes() != 0) {
        printf("test_sql_exec: drop table cleanup fail\n");
        cleanup_files();
        return 1;
    }

    cleanup_files();
    printf("test_sql_exec: ok\n");
    return 0;
}
