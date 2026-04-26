/*
 * Interactive SQL shell entry point.
 * Reads SQL statements line by line, executes them against a DBF-backed
 * catalog rooted at the path given on the command line, and uses
 * platform.c for all terminal I/O.
 *
 * MIT License (see: LICENSE)
 * Copyright (C) 2026 tomaz stih
 */

#include "dbf.h"
#include "sql.h"
#include "platform.h"

#include <string.h>

#if defined(__SDCC)
extern int mkdir(const char *path);
#else
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

#define catalog_name_length   16
#define catalog_path_length   48
#define catalog_slot_length    2
#define catalog_record_length 66
#define catalog_max_databases 15
#define path_buffer_size     128
#define sql_buffer_size      256
#define table_record_size   4096

#define CTRL_C  3
#define BS      8
#define DEL     127

/* ------------------------------------------------------------------ */
/* Output helpers                                                       */
/* ------------------------------------------------------------------ */

static void write_nl(void)
{
    write_char('\r');
    write_char('\n');
}

static void write_str(const char *s)
{
    while (*s)
        write_char(*s++);
}

static void uint_to_str(unsigned short n, char *buf)
{
    unsigned short i = 0;
    unsigned short j = 0;
    char tmp;
    if (!n) { buf[0] = '0'; buf[1] = '\0'; return; }
    while (n) { buf[i++] = (char)('0' + n % 10); n = (unsigned short)(n / 10); }
    buf[i] = '\0';
    while (j < i / 2) {
        tmp = buf[j]; buf[j] = buf[i - 1 - j]; buf[i - 1 - j] = tmp;
        j++;
    }
}

static void write_uint(unsigned short n)
{
    char buf[6];
    uint_to_str(n, buf);
    write_str(buf);
}

/* ------------------------------------------------------------------ */
/* DBF catalog field helpers                                            */
/* ------------------------------------------------------------------ */

static void fill_catalog_fields(dbf_field *fields)
{
    strcpy(fields[0].name, "name");
    fields[0].type = 'C'; fields[0].length = catalog_name_length; fields[0].decimals = 0;
    strcpy(fields[1].name, "path");
    fields[1].type = 'C'; fields[1].length = catalog_path_length; fields[1].decimals = 0;
    strcpy(fields[2].name, "slot");
    fields[2].type = 'N'; fields[2].length = catalog_slot_length; fields[2].decimals = 0;
}

static void set_field(char *target, unsigned short length, const char *value)
{
    unsigned short vlen = (unsigned short)strlen(value);
    if (vlen > length) vlen = length;
    memset(target, ' ', length);
    memcpy(target, value, vlen);
}

static void get_field(char *target, unsigned short target_size,
    const char *source, unsigned short length)
{
    unsigned short size = length;
    while (size > 0 && source[size - 1] == ' ') size--;
    if (size + 1 > target_size) size = (unsigned short)(target_size - 1);
    memcpy(target, source, size);
    target[size] = '\0';
}

static void set_slot_field(char *target, unsigned short slot)
{
    target[0] = slot >= 10 ? (char)('0' + slot / 10) : ' ';
    target[1] = (char)('0' + slot % 10);
}

static unsigned short get_slot_field(const char *src)
{
    unsigned short n = 0;
    if (src[0] >= '0' && src[0] <= '9') n = (unsigned short)(src[0] - '0');
    if (src[1] >= '0' && src[1] <= '9') n = (unsigned short)(n * 10 + (src[1] - '0'));
    return n;
}

/* ------------------------------------------------------------------ */
/* Path and directory helpers                                           */
/* ------------------------------------------------------------------ */

static int join_path(char *target, const char *left, const char *right)
{
    unsigned short llen = (unsigned short)strlen(left);
    unsigned short rlen = (unsigned short)strlen(right);
    if (llen + 1 + rlen + 1 > path_buffer_size) return -1;
    strcpy(target, left);
    if (llen > 0 && target[llen - 1] != '/') { target[llen] = '/'; target[llen + 1] = '\0'; }
    strcat(target, right);
    return 0;
}

static int ensure_directory(const char *path)
{
#if defined(__SDCC)
    return mkdir(path);
#else
    struct stat st;
    if (stat(path, &st) == 0) return S_ISDIR(st.st_mode) ? 0 : -1;
    return (mkdir(path, 0775) != 0 && errno != EEXIST) ? -1 : 0;
#endif
}

static int ensure_catalog(const char *root, char *catalog_path)
{
    dbf_field fields[3];
    dbf_file file;
    char sys_path[path_buffer_size];

    if (ensure_directory(root) != 0) return -1;
    if (join_path(sys_path, root, "sys") != 0) return -1;
    if (ensure_directory(sys_path) != 0) return -1;
    if (join_path(catalog_path, sys_path, "db.dbf") != 0) return -1;
    if (dbf_open(&file, catalog_path) == 0) return dbf_close(&file);
    fill_catalog_fields(fields);
    if (dbf_create(&file, catalog_path, fields, 3) != 0) return -1;
    return dbf_close(&file);
}

/* ------------------------------------------------------------------ */
/* Catalog record operations                                            */
/* ------------------------------------------------------------------ */

static int find_catalog_slot(dbf_file *file, const char *name,
    unsigned short *slot_out)
{
    unsigned char used[catalog_max_databases + 1];
    unsigned long index;
    char record[catalog_record_length];
    char existing_name[catalog_name_length + 1];
    int state;
    unsigned short slot;

    for (slot = 0; slot <= catalog_max_databases; slot++) used[slot] = 0;
    for (index = 0; index < file->record_count; index++) {
        state = dbf_read(file, index, record);
        if (state < 0) return -1;
        if (state == 1) continue;
        get_field(existing_name, sizeof(existing_name), record, catalog_name_length);
        if (strcmp(existing_name, name) == 0) return -1;
        slot = get_slot_field(record + catalog_name_length + catalog_path_length);
        if (slot <= catalog_max_databases) used[slot] = 1;
    }
    for (slot = 1; slot <= catalog_max_databases; slot++) {
        if (!used[slot]) { *slot_out = slot; return 0; }
    }
    return -1;
}

static int find_database_path(const char *root, const char *name, char *db_path)
{
    dbf_file file;
    char catalog_path[path_buffer_size];
    char record[catalog_record_length];
    char existing_name[catalog_name_length + 1];
    int state;
    unsigned long index;

    if (ensure_catalog(root, catalog_path) != 0) return -1;
    if (dbf_open(&file, catalog_path) != 0) return -1;
    for (index = 0; index < file.record_count; index++) {
        state = dbf_read(&file, index, record);
        if (state < 0) { dbf_close(&file); return -1; }
        if (state == 1) continue;
        get_field(existing_name, sizeof(existing_name), record, catalog_name_length);
        if (strcmp(existing_name, name) == 0) {
            get_field(db_path, path_buffer_size,
                record + catalog_name_length, catalog_path_length);
            return dbf_close(&file);
        }
    }
    dbf_close(&file);
    return -1;
}

/* ------------------------------------------------------------------ */
/* Table helpers                                                        */
/* ------------------------------------------------------------------ */

static void clear_record(char *record, unsigned short length)
{
    memset(record, ' ', length);
}

static void trim_field_value(char *target, unsigned short target_size,
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

static long parse_integer_text(const char *text, int *ok)
{
    long value;
    int sign;

    *ok = 0;
    while (*text == ' ') {
        text++;
    }

    sign = 1;
    if (*text == '-') {
        sign = -1;
        text++;
    } else if (*text == '+') {
        text++;
    }

    if (*text < '0' || *text > '9') {
        return 0;
    }

    value = 0;
    while (*text >= '0' && *text <= '9') {
        value = (value * 10L) + (long)(*text - '0');
        text++;
    }

    while (*text == ' ') {
        text++;
    }

    if (*text != '\0') {
        return 0;
    }

    *ok = 1;
    return value * (long)sign;
}

static int compare_longs(long left, long right,
    sql_compare_operator operator)
{
    switch (operator) {
    case sql_compare_equal:
        return left == right;
    case sql_compare_not_equal:
        return left != right;
    case sql_compare_less:
        return left < right;
    case sql_compare_less_equal:
        return left <= right;
    case sql_compare_greater:
        return left > right;
    case sql_compare_greater_equal:
        return left >= right;
    default:
        return 0;
    }
}

static int compare_strings(const char *left, const char *right,
    sql_compare_operator operator)
{
    int order;

    order = strcmp(left, right);
    switch (operator) {
    case sql_compare_equal:
        return order == 0;
    case sql_compare_not_equal:
        return order != 0;
    case sql_compare_less:
        return order < 0;
    case sql_compare_less_equal:
        return order <= 0;
    case sql_compare_greater:
        return order > 0;
    case sql_compare_greater_equal:
        return order >= 0;
    default:
        return 0;
    }
}

static int build_field_offsets(const dbf_field *fields,
    unsigned short field_count, unsigned short *offsets)
{
    unsigned short index;
    unsigned short offset;

    offset = 0;
    for (index = 0; index < field_count; index++) {
        offsets[index] = offset;
        offset = (unsigned short)(offset + fields[index].length);
        if (offset > table_record_size) {
            return -1;
        }
    }

    return 0;
}

static int find_field_index(const dbf_field *fields,
    unsigned short field_count, const char *name)
{
    unsigned short index;

    for (index = 0; index < field_count; index++) {
        if (strcmp(fields[index].name, name) == 0) {
            return (int)index;
        }
    }

    return -1;
}

static int open_table_file(const char *root, const char *db_name,
    const char *table_name, dbf_file *file, dbf_field *fields,
    unsigned short *offsets)
{
    char db_path[path_buffer_size];
    char table_path[path_buffer_size];

    if (!db_name || !db_name[0]) {
        return -1;
    }

    if (find_database_path(root, db_name, db_path) != 0) {
        return -1;
    }

    if (join_path(table_path, db_path, table_name) != 0) {
        return -1;
    }

    if ((unsigned short)(strlen(table_path) + 5) > path_buffer_size) {
        return -1;
    }
    strcat(table_path, ".dbf");

    if (dbf_open(file, table_path) != 0) {
        return -1;
    }

    if (file->field_count > sql_max_columns
        || file->record_length > table_record_size) {
        dbf_close(file);
        return -1;
    }

    if (dbf_read_fields(file, fields, sql_max_columns) != 0) {
        dbf_close(file);
        return -1;
    }

    if (build_field_offsets(fields, file->field_count, offsets) != 0) {
        dbf_close(file);
        return -1;
    }

    return 0;
}

static int where_matches(const sql_where *where, const dbf_field *fields,
    const unsigned short *offsets, const char *record,
    unsigned short field_count)
{
    char left[sql_value_size];
    int field_index;
    int ok_left;
    int ok_right;
    long left_number;
    long right_number;

    if (!where->active) {
        return 1;
    }

    field_index = find_field_index(fields, field_count, where->column_name);
    if (field_index < 0) {
        return 0;
    }

    trim_field_value(left, sizeof(left), record + offsets[field_index],
        fields[field_index].length);

    if (fields[field_index].type == 'N'
        && where->value.type == sql_value_number) {
        left_number = parse_integer_text(left, &ok_left);
        right_number = parse_integer_text(where->value.text, &ok_right);
        if (!ok_left || !ok_right) {
            return 0;
        }
        return compare_longs(left_number, right_number, where->operator);
    }

    return compare_strings(left, where->value.text, where->operator);
}

static int store_value_in_field(char *target, const dbf_field *field,
    const sql_value *value)
{
    unsigned short index;
    unsigned short value_size;
    const char *text;

    text = value->text;
    value_size = (unsigned short)strlen(text);
    if (value_size > field->length) {
        return -1;
    }

    for (index = 0; index < field->length; index++) {
        target[index] = ' ';
    }

    if (field->type == 'N') {
        memcpy(target + (field->length - value_size), text, value_size);
        return 0;
    }

    if (field->type == 'L') {
        if (value_size == 0) {
            return -1;
        }
        target[0] = text[0];
        return 0;
    }

    memcpy(target, text, value_size);
    return 0;
}

static void write_selected_row(const sql_statement *stmt,
    const dbf_field *fields, const unsigned short *offsets,
    const char *record, unsigned short field_count)
{
    char value[sql_value_size];
    unsigned short index;
    unsigned short output_count;
    int field_index;

    output_count = stmt->select_all ? field_count : stmt->select_count;
    for (index = 0; index < output_count; index++) {
        if (stmt->select_all) {
            field_index = (int)index;
        } else {
            field_index = find_field_index(fields, field_count,
                stmt->select_names[index]);
            if (field_index < 0) {
                write_str("error");
                write_nl();
                return;
            }
        }

        trim_field_value(value, sizeof(value), record + offsets[field_index],
            fields[field_index].length);
        if (index > 0) {
            write_str(" | ");
        }
        write_str(value);
    }

    write_nl();
}

/* ------------------------------------------------------------------ */
/* SQL executors                                                        */
/* ------------------------------------------------------------------ */

static int execute_create_database(const char *root, const sql_statement *stmt,
    char *current_db)
{
    dbf_file file;
    char catalog_path[path_buffer_size];
    char db_path[path_buffer_size];
    char slot_name[4];
    char record[catalog_record_length];
    unsigned short slot;

    if (ensure_catalog(root, catalog_path) != 0) return -1;
    if (dbf_open(&file, catalog_path) != 0) return -1;
    if (find_catalog_slot(&file, stmt->name, &slot) != 0) {
        dbf_close(&file); return -1;
    }
    uint_to_str(slot, slot_name);
    if (join_path(db_path, root, slot_name) != 0) { dbf_close(&file); return -1; }
    if (ensure_directory(db_path) != 0) { dbf_close(&file); return -1; }

    set_field(record, catalog_name_length, stmt->name);
    set_field(record + catalog_name_length, catalog_path_length, db_path);
    set_slot_field(record + catalog_name_length + catalog_path_length, slot);
    if (dbf_append(&file, record) != 0) { dbf_close(&file); return -1; }
    if (dbf_close(&file) != 0) return -1;

    strcpy(current_db, stmt->name);
    write_str("created "); write_str(stmt->name); write_nl();
    return 0;
}

static int execute_show_databases(const char *root)
{
    dbf_file file;
    char catalog_path[path_buffer_size];
    char record[catalog_record_length];
    char name[catalog_name_length + 1];
    char path[catalog_path_length + 1];
    unsigned long index;
    int state;
    unsigned short slot;

    if (ensure_catalog(root, catalog_path) != 0) return -1;
    if (dbf_open(&file, catalog_path) != 0) return -1;
    for (index = 0; index < file.record_count; index++) {
        state = dbf_read(&file, index, record);
        if (state < 0) { dbf_close(&file); return -1; }
        if (state == 1) continue;
        get_field(name, sizeof(name), record, catalog_name_length);
        get_field(path, sizeof(path), record + catalog_name_length, catalog_path_length);
        slot = get_slot_field(record + catalog_name_length + catalog_path_length);
        if (slot < 10) write_char(' ');
        write_uint(slot);
        write_char(' '); write_str(path);
        write_char(' '); write_str(name);
        write_nl();
    }
    return dbf_close(&file);
}

static int execute_create_table(const char *root, const char *db_name,
    const sql_statement *stmt)
{
    dbf_file file;
    dbf_field fields[sql_max_columns];
    char db_path[path_buffer_size];
    char table_path[path_buffer_size];
    unsigned short i;

    if (!db_name || !db_name[0]) return -1;
    if (find_database_path(root, db_name, db_path) != 0) return -1;
    if (join_path(table_path, db_path, stmt->name) != 0) return -1;
    if ((unsigned short)(strlen(table_path) + 5) > path_buffer_size) return -1;
    strcat(table_path, ".dbf");
    if (dbf_open(&file, table_path) == 0) { dbf_close(&file); return -1; }
    for (i = 0; i < stmt->column_count; i++) {
        strcpy(fields[i].name, stmt->columns[i].name);
        fields[i].type     = stmt->columns[i].dbf_type;
        fields[i].length   = stmt->columns[i].length;
        fields[i].decimals = stmt->columns[i].decimals;
    }
    if (dbf_create(&file, table_path, fields, stmt->column_count) != 0) return -1;
    if (dbf_close(&file) != 0) return -1;
    write_str("created "); write_str(stmt->name); write_nl();
    return 0;
}

static int execute_insert(const char *root, const char *db_name,
    const sql_statement *stmt)
{
    dbf_file file;
    dbf_field fields[sql_max_columns];
    unsigned short offsets[sql_max_columns];
    char record[table_record_size];
    unsigned short index;

    if (open_table_file(root, db_name, stmt->name, &file, fields,
        offsets) != 0) {
        return -1;
    }

    if (stmt->value_count != file.field_count) {
        dbf_close(&file);
        return -1;
    }

    clear_record(record, file.record_length);
    for (index = 0; index < file.field_count; index++) {
        if (store_value_in_field(record + offsets[index], &fields[index],
            &stmt->values[index]) != 0) {
            dbf_close(&file);
            return -1;
        }
    }

    if (dbf_append(&file, record) != 0) {
        dbf_close(&file);
        return -1;
    }

    if (dbf_close(&file) != 0) {
        return -1;
    }

    write_str("inserted 1");
    write_nl();
    return 0;
}

static int execute_select(const char *root, const char *db_name,
    const sql_statement *stmt)
{
    dbf_file file;
    dbf_field fields[sql_max_columns];
    unsigned short offsets[sql_max_columns];
    char record[table_record_size];
    unsigned long index;
    unsigned short row_count;
    unsigned short output_count;
    unsigned short select_index;
    int state;
    int field_index;

    if (open_table_file(root, db_name, stmt->name, &file, fields,
        offsets) != 0) {
        return -1;
    }

    output_count = stmt->select_all ? file.field_count : stmt->select_count;
    for (select_index = 0; select_index < output_count; select_index++) {
        if (stmt->select_all) {
            continue;
        }

        field_index = find_field_index(fields, file.field_count,
            stmt->select_names[select_index]);
        if (field_index < 0) {
            dbf_close(&file);
            return -1;
        }
    }

    if (stmt->where.active
        && find_field_index(fields, file.field_count,
            stmt->where.column_name) < 0) {
        dbf_close(&file);
        return -1;
    }

    row_count = 0;
    for (index = 0; index < file.record_count; index++) {
        state = dbf_read(&file, index, record);
        if (state < 0) {
            dbf_close(&file);
            return -1;
        }
        if (state == 1) {
            continue;
        }
        if (!where_matches(&stmt->where, fields, offsets, record,
            file.field_count)) {
            continue;
        }

        write_selected_row(stmt, fields, offsets, record, file.field_count);
        row_count++;
    }

    if (dbf_close(&file) != 0) {
        return -1;
    }

    write_uint(row_count);
    write_str(" row");
    if (row_count != 1) {
        write_char('s');
    }
    write_nl();
    return 0;
}

static int execute_update(const char *root, const char *db_name,
    const sql_statement *stmt)
{
    dbf_file file;
    dbf_field fields[sql_max_columns];
    unsigned short offsets[sql_max_columns];
    char record[table_record_size];
    unsigned long index;
    unsigned short changed_count;
    unsigned short assignment_index;
    int state;
    int field_index;

    if (open_table_file(root, db_name, stmt->name, &file, fields,
        offsets) != 0) {
        return -1;
    }

    for (assignment_index = 0; assignment_index < stmt->assignment_count;
        assignment_index++) {
        field_index = find_field_index(fields, file.field_count,
            stmt->assignments[assignment_index].column_name);
        if (field_index < 0) {
            dbf_close(&file);
            return -1;
        }
    }

    if (stmt->where.active
        && find_field_index(fields, file.field_count,
            stmt->where.column_name) < 0) {
        dbf_close(&file);
        return -1;
    }

    changed_count = 0;
    for (index = 0; index < file.record_count; index++) {
        state = dbf_read(&file, index, record);
        if (state < 0) {
            dbf_close(&file);
            return -1;
        }
        if (state == 1) {
            continue;
        }
        if (!where_matches(&stmt->where, fields, offsets, record,
            file.field_count)) {
            continue;
        }

        for (assignment_index = 0; assignment_index < stmt->assignment_count;
            assignment_index++) {
            field_index = find_field_index(fields, file.field_count,
                stmt->assignments[assignment_index].column_name);
            if (store_value_in_field(record + offsets[field_index],
                &fields[field_index],
                &stmt->assignments[assignment_index].value) != 0) {
                dbf_close(&file);
                return -1;
            }
        }

        if (dbf_write(&file, index, record) != 0) {
            dbf_close(&file);
            return -1;
        }
        changed_count++;
    }

    if (dbf_close(&file) != 0) {
        return -1;
    }

    write_uint(changed_count);
    write_str(" updated");
    write_nl();
    return 0;
}

static int execute_delete(const char *root, const char *db_name,
    const sql_statement *stmt)
{
    dbf_file file;
    dbf_field fields[sql_max_columns];
    unsigned short offsets[sql_max_columns];
    char record[table_record_size];
    unsigned long index;
    unsigned short deleted_count;
    int state;

    if (open_table_file(root, db_name, stmt->name, &file, fields,
        offsets) != 0) {
        return -1;
    }

    if (stmt->where.active
        && find_field_index(fields, file.field_count,
            stmt->where.column_name) < 0) {
        dbf_close(&file);
        return -1;
    }

    deleted_count = 0;
    for (index = 0; index < file.record_count; index++) {
        state = dbf_read(&file, index, record);
        if (state < 0) {
            dbf_close(&file);
            return -1;
        }
        if (state == 1) {
            continue;
        }
        if (!where_matches(&stmt->where, fields, offsets, record,
            file.field_count)) {
            continue;
        }

        if (dbf_delete(&file, index) != 0) {
            dbf_close(&file);
            return -1;
        }
        deleted_count++;
    }

    if (dbf_close(&file) != 0) {
        return -1;
    }

    write_uint(deleted_count);
    write_str(" deleted");
    write_nl();
    return 0;
}

/* ------------------------------------------------------------------ */
/* Interactive shell                                                    */
/* ------------------------------------------------------------------ */

static void shell(const char *root)
{
    char buf[sql_buffer_size];
    char current_db[sql_name_size];
    sql_statement stmt;
    unsigned short len;
    int c;

    current_db[0] = '\0';

    while (1) {
        if (current_db[0]) { write_str(current_db); write_char('>'); }
        else { write_char('>'); }
        write_char(' ');
        len = 0;

        while (1) {
            c = read_char();
            if (c < 0) {
                write_nl();
                return;
            }
            if (c == CTRL_C) { write_str("^C"); write_nl(); return; }
            if (c == '\r' || c == '\n') { write_nl(); break; }
            if ((c == BS || c == DEL) && len > 0) {
                len--;
                write_char(BS); write_char(' '); write_char(BS);
                continue;
            }
            if (c >= 32 && len + 1 < sql_buffer_size) {
                buf[len++] = (char)c;
                write_char((char)c);
            }
        }

        buf[len] = '\0';
        if (!len) continue;

        if (sql_parse(buf, &stmt) != 0) {
            write_str("parse error"); write_nl();
            continue;
        }

        switch (stmt.type) {
        case sql_statement_create_database:
            if (execute_create_database(root, &stmt, current_db) != 0)
                write_str("error"), write_nl();
            break;
        case sql_statement_show_databases:
            if (execute_show_databases(root) != 0)
                write_str("error"), write_nl();
            break;
        case sql_statement_create_table:
            if (execute_create_table(root, current_db, &stmt) != 0)
                write_str("error"), write_nl();
            break;
        case sql_statement_select:
            if (execute_select(root, current_db, &stmt) != 0)
                write_str("error"), write_nl();
            break;
        case sql_statement_insert:
            if (execute_insert(root, current_db, &stmt) != 0)
                write_str("error"), write_nl();
            break;
        case sql_statement_update:
            if (execute_update(root, current_db, &stmt) != 0)
                write_str("error"), write_nl();
            break;
        case sql_statement_delete:
            if (execute_delete(root, current_db, &stmt) != 0)
                write_str("error"), write_nl();
            break;
        default:
            write_str("not implemented"); write_nl();
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Entry point                                                          */
/* ------------------------------------------------------------------ */

#if defined(__SDCC)
int main(void)
{
    platform_init();
    shell("db");
    platform_exit();
    return 0;
}
#else
int main(int argc, char *argv[])
{
    platform_init();
    if (argc < 2) {
        write_str("usage: sql <root>"); write_nl();
        platform_exit();
        return 1;
    }
    shell(argv[1]);
    platform_exit();
    return 0;
}
#endif
