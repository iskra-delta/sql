/*
 * Implements catalog and table-file access helpers.
 * All catalog helpers that were previously duplicated or only in
 * execute.c now live here so the optimizer and executor modules can
 * share them without each carrying a private copy.
 *
 * MIT License (see: LICENSE)
 * Copyright (C) 2026 tomaz stih
 */

#include "catalog.h"

#include <string.h>

#if defined(__SDCC)
extern int mkdir(const char *path);
extern int unlink(const char *path);
#else
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

/* ------------------------------------------------------------------ */
/* Directory helpers                                                    */
/* ------------------------------------------------------------------ */

int ensure_directory(const char *path)
{
#if defined(__SDCC)
    return mkdir(path);
#else
    struct stat st;

    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 0 : -1;
    }
    return (mkdir(path, 0775) != 0 && errno != EEXIST) ? -1 : 0;
#endif
}

/* ------------------------------------------------------------------ */
/* Catalog field layout helpers                                         */
/* ------------------------------------------------------------------ */

void fill_catalog_fields(dbf_field *fields)
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

void fill_index_catalog_fields(dbf_field *fields)
{
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
}

void fill_index_catalog_record(char *record, const char *db_name,
    const char *index_name, const char *table_name,
    const char *field_list, unsigned char unique)
{
    set_field(record, index_catalog_db_length, db_name);
    set_field(record + index_catalog_db_length,
        index_catalog_name_length, index_name);
    set_field(record + index_catalog_db_length + index_catalog_name_length,
        index_catalog_table_length, table_name);
    set_field(record + index_catalog_db_length + index_catalog_name_length
        + index_catalog_table_length,
        index_catalog_fields_length, field_list);
    set_field(record + index_catalog_db_length + index_catalog_name_length
        + index_catalog_table_length + index_catalog_fields_length,
        index_catalog_unique_length, unique ? "Y" : "N");
}

/* ------------------------------------------------------------------ */
/* Catalog bootstrap                                                    */
/* ------------------------------------------------------------------ */

int ensure_catalog(const char *root, char *catalog_path)
{
    dbf_field fields[3];
    dbf_file file;
    char sys_path[path_buffer_size];

    if (ensure_directory(root) != 0) {
        return -1;
    }
    if (join_path(sys_path, root, "sys") != 0) {
        return -1;
    }
    if (ensure_directory(sys_path) != 0) {
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

int ensure_index_catalog(const char *root, char *catalog_path)
{
    dbf_field fields[5];
    dbf_file file;
    char sys_path[path_buffer_size];

    if (ensure_directory(root) != 0) {
        return -1;
    }
    if (join_path(sys_path, root, "sys") != 0) {
        return -1;
    }
    if (ensure_directory(sys_path) != 0) {
        return -1;
    }
    if (join_path(catalog_path, sys_path, "ndx.dbf") != 0) {
        return -1;
    }
    if (dbf_open(&file, catalog_path) == 0) {
        return dbf_close(&file);
    }
    fill_index_catalog_fields(fields);
    if (dbf_create(&file, catalog_path, fields, 5) != 0) {
        return -1;
    }
    return dbf_close(&file);
}

/* ------------------------------------------------------------------ */
/* Database catalog operations                                          */
/* ------------------------------------------------------------------ */

int find_catalog_slot(dbf_file *file, const char *name,
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

int find_database_path(const char *root, const char *name, char *db_path)
{
    dbf_file file;
    char catalog_path[path_buffer_size];
    char record[catalog_record_length];
    char existing_name[catalog_name_length + 1];
    int state;
    unsigned long index;

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
        get_field(existing_name, sizeof(existing_name), record,
            catalog_name_length);
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
/* Index catalog operations                                             */
/* ------------------------------------------------------------------ */

int index_catalog_has_name(dbf_file *file, const char *db_name,
    const char *index_name)
{
    char record[index_catalog_record_length];
    char record_db[index_catalog_db_length + 1];
    char record_name[index_catalog_name_length + 1];
    unsigned long index;
    int state;

    for (index = 0; index < file->record_count; index++) {
        state = dbf_read(file, index, record);
        if (state < 0) {
            return -1;
        }
        if (state == 1) {
            continue;
        }
        get_field(record_db, sizeof(record_db), record,
            index_catalog_db_length);
        get_field(record_name, sizeof(record_name),
            record + index_catalog_db_length, index_catalog_name_length);
        if (strcmp(record_db, db_name) == 0
            && strcmp(record_name, index_name) == 0) {
            return 1;
        }
    }
    return 0;
}

int append_index_catalog_entry(const char *root, const char *db_name,
    const char *index_name, const char *table_name,
    const char *field_list, unsigned char unique)
{
    dbf_file file;
    char catalog_path[path_buffer_size];
    char record[index_catalog_record_length];
    int present;

    if (ensure_index_catalog(root, catalog_path) != 0) {
        return -1;
    }
    if (dbf_open(&file, catalog_path) != 0) {
        return -1;
    }
    present = index_catalog_has_name(&file, db_name, index_name);
    if (present != 0) {
        dbf_close(&file);
        return present < 0 ? -1 : 1;
    }
    fill_index_catalog_record(record, db_name, index_name, table_name,
        field_list, unique);
    if (dbf_append(&file, record) != 0) {
        dbf_close(&file);
        return -1;
    }
    return dbf_close(&file);
}

int remove_registered_indexes(const char *root, const char *db_name,
    const char *table_name)
{
    dbf_file file;
    char catalog_path[path_buffer_size];
    char record[index_catalog_record_length];
    char record_db[index_catalog_db_length + 1];
    char record_name[index_catalog_name_length + 1];
    char record_table[index_catalog_table_length + 1];
    char index_path[path_buffer_size];
    unsigned long index;
    int state;

    if (ensure_index_catalog(root, catalog_path) != 0) {
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
        get_field(record_db, sizeof(record_db), record,
            index_catalog_db_length);
        get_field(record_name, sizeof(record_name),
            record + index_catalog_db_length, index_catalog_name_length);
        get_field(record_table, sizeof(record_table),
            record + index_catalog_db_length + index_catalog_name_length,
            index_catalog_table_length);
        if (strcmp(record_db, db_name) != 0) {
            continue;
        }
        if (table_name && strcmp(record_table, table_name) != 0) {
            continue;
        }
        if (build_index_path(root, db_name, record_name,
            index_path) != 0) {
            dbf_close(&file);
            return -1;
        }
        if (unlink(index_path) != 0) {
            dbf_close(&file);
            return -1;
        }
        if (dbf_delete(&file, index) != 0) {
            dbf_close(&file);
            return -1;
        }
    }
    return dbf_close(&file);
}

/* ------------------------------------------------------------------ */
/* Table file access                                                    */
/* ------------------------------------------------------------------ */

int build_index_path(const char *root, const char *db_name,
    const char *index_name, char *index_path)
{
    char db_path[path_buffer_size];

    if (!db_name || !db_name[0]) {
        return -1;
    }
    if (find_database_path(root, db_name, db_path) != 0) {
        return -1;
    }
    if (join_path(index_path, db_path, index_name) != 0) {
        return -1;
    }
    if ((unsigned short)(strlen(index_path) + 5) > path_buffer_size) {
        return -1;
    }
    strcat(index_path, ".ndx");
    return 0;
}

int open_table_file(const char *root, const char *db_name,
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

int open_table_fields(const char *root, const char *db_name,
    const char *table_name, dbf_field *fields,
    unsigned short *field_count)
{
    dbf_file file;
    char db_path[path_buffer_size];
    char table_path[path_buffer_size];

    if (find_database_path(root, db_name, db_path) != 0) {
        return -1;
    }
    if (join_path(table_path, db_path, table_name) != 0) {
        return -1;
    }
    if ((unsigned short)(strlen(table_path) + 5u) > path_buffer_size) {
        return -1;
    }
    strcat(table_path, ".dbf");
    if (dbf_open(&file, table_path) != 0) {
        return -1;
    }
    if (file.field_count > sql_max_columns) {
        dbf_close(&file);
        return -1;
    }
    if (dbf_read_fields(&file, fields, file.field_count) != 0) {
        dbf_close(&file);
        return -1;
    }
    *field_count = file.field_count;
    return dbf_close(&file);
}

int build_index_field_list(const dbf_field *fields,
    const unsigned short *key_fields, unsigned short key_count,
    char *target, unsigned short target_size)
{
    unsigned short index;
    unsigned short out;
    unsigned short length;

    if (target_size == 0) {
        return -1;
    }
    out = 0;
    for (index = 0; index < key_count; index++) {
        if (key_fields[index] >= sql_max_columns) {
            return -1;
        }
        if (index > 0) {
            if (out + 1 >= target_size) {
                return -1;
            }
            target[out++] = ',';
        }
        length = (unsigned short)strlen(fields[key_fields[index]].name);
        if (out + length + 1 > target_size) {
            return -1;
        }
        memcpy(target + out, fields[key_fields[index]].name, length);
        out = (unsigned short)(out + length);
    }
    target[out] = '\0';
    return 0;
}

int parse_index_field_list(const char *text, const dbf_field *fields,
    unsigned short field_count, unsigned short *key_fields,
    unsigned short *key_count)
{
    char field_name[12];
    unsigned short in;
    unsigned short out;
    int field_index;

    *key_count = 0;
    if (!text || text[0] == '\0') {
        return -1;
    }
    in = 0;
    while (1) {
        out = 0;
        while (text[in] != '\0' && text[in] != ',') {
            if ((unsigned short)(out + 1) >= sizeof(field_name)) {
                return -1;
            }
            field_name[out++] = text[in++];
        }
        if (out == 0) {
            return -1;
        }
        field_name[out] = '\0';
        field_index = find_field_index(fields, field_count, field_name);
        if (field_index < 0 || *key_count >= sql_max_columns) {
            return -1;
        }
        key_fields[*key_count] = (unsigned short)field_index;
        (*key_count)++;
        if (text[in] == '\0') {
            return 0;
        }
        in++;
    }
}

int rebuild_table_indexes(const char *root, const char *db_name,
    const char *table_name)
{
    dbf_file file;
    char catalog_path[path_buffer_size];
    char record[index_catalog_record_length];
    char record_db[index_catalog_db_length + 1];
    char record_name[index_catalog_name_length + 1];
    char record_table[index_catalog_table_length + 1];
    char field_list[index_catalog_fields_length + 1];
    char unique_text[index_catalog_unique_length + 1];
    unsigned long index;
    int state;

    if (ensure_index_catalog(root, catalog_path) != 0) {
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
        get_field(record_db, sizeof(record_db), record,
            index_catalog_db_length);
        get_field(record_name, sizeof(record_name),
            record + index_catalog_db_length, index_catalog_name_length);
        get_field(record_table, sizeof(record_table),
            record + index_catalog_db_length + index_catalog_name_length,
            index_catalog_table_length);
        if (strcmp(record_db, db_name) != 0
            || strcmp(record_table, table_name) != 0) {
            continue;
        }
        get_field(field_list, sizeof(field_list),
            record + index_catalog_db_length + index_catalog_name_length
                + index_catalog_table_length,
            index_catalog_fields_length);
        get_field(unique_text, sizeof(unique_text),
            record + index_catalog_db_length + index_catalog_name_length
                + index_catalog_table_length + index_catalog_fields_length,
            index_catalog_unique_length);
        if (build_index_from_catalog_entry(root, db_name, record_name,
            record_table, field_list,
            unique_text[0] == 'Y') != 0) {
            dbf_close(&file);
            return -1;
        }
    }
    return dbf_close(&file);
}

int build_index_from_catalog_entry(const char *root, const char *db_name,
    const char *index_name, const char *table_name,
    const char *field_list, unsigned char unique)
{
    dbf_file table;
    dbf_field fields[sql_max_columns];
    unsigned short offsets[sql_max_columns];
    unsigned short key_fields[sql_max_columns];
    unsigned short key_count;
    ndx_file index;
    char index_path[path_buffer_size];

    if (open_table_file(root, db_name, table_name, &table, fields,
        offsets) != 0) {
        return -1;
    }
    if (parse_index_field_list(field_list, fields, table.field_count,
        key_fields, &key_count) != 0) {
        dbf_close(&table);
        return -1;
    }
    if (build_index_path(root, db_name, index_name, index_path) != 0) {
        dbf_close(&table);
        return -1;
    }
    if (ndx_create(&index, index_path, &table, fields, table.field_count,
        key_fields, key_count, unique) != 0) {
        dbf_close(&table);
        return -1;
    }
    if (ndx_close(&index) != 0) {
        dbf_close(&table);
        return -1;
    }
    return dbf_close(&table);
}

/* ------------------------------------------------------------------ */
/* View catalog                                                         */
/* ------------------------------------------------------------------ */

static void fill_view_catalog_fields(dbf_field *fields)
{
    strcpy(fields[0].name, "db_name");
    fields[0].type = 'C';
    fields[0].length = view_catalog_db_length;
    fields[0].decimals = 0;

    strcpy(fields[1].name, "name");
    fields[1].type = 'C';
    fields[1].length = view_catalog_name_length;
    fields[1].decimals = 0;

    strcpy(fields[2].name, "type");
    fields[2].type = 'C';
    fields[2].length = view_catalog_type_length;
    fields[2].decimals = 0;

    strcpy(fields[3].name, "statement");
    fields[3].type = 'C';
    fields[3].length = (unsigned char)view_catalog_stmt_length;
    fields[3].decimals = 0;
}

int ensure_view_catalog(const char *root, char *catalog_path)
{
    dbf_field fields[4];
    dbf_file file;
    char sys_path[path_buffer_size];

    if (ensure_directory(root) != 0) {
        return -1;
    }
    if (join_path(sys_path, root, "sys") != 0) {
        return -1;
    }
    if (ensure_directory(sys_path) != 0) {
        return -1;
    }
    if (join_path(catalog_path, sys_path, "vw.dbf") != 0) {
        return -1;
    }
    if (dbf_open(&file, catalog_path) == 0) {
        return dbf_close(&file);
    }
    fill_view_catalog_fields(fields);
    if (dbf_create(&file, catalog_path, fields, 4) != 0) {
        return -1;
    }
    return dbf_close(&file);
}

int find_view(const char *root, const char *db_name,
    const char *name, char *type_out, char *stmt_out)
{
    dbf_file file;
    char catalog_path[path_buffer_size];
    char record[view_catalog_record_length];
    char record_db[view_catalog_db_length + 1];
    char record_name[view_catalog_name_length + 1];
    int state;
    unsigned long index;

    if (ensure_view_catalog(root, catalog_path) != 0) {
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
        get_field(record_db, sizeof(record_db), record,
            view_catalog_db_length);
        get_field(record_name, sizeof(record_name),
            record + view_catalog_db_length,
            view_catalog_name_length);
        if (strcmp(record_db, db_name) == 0
            && strcmp(record_name, name) == 0) {
            if (type_out) {
                type_out[0] = record[view_catalog_db_length
                    + view_catalog_name_length];
                type_out[1] = '\0';
            }
            get_field(stmt_out,
                view_catalog_stmt_length + 1,
                record + view_catalog_db_length
                    + view_catalog_name_length
                    + view_catalog_type_length,
                view_catalog_stmt_length);
            return dbf_close(&file);
        }
    }
    dbf_close(&file);
    return -1;
}

int register_view(const char *root, const char *db_name,
    const char *name, char type, const char *stmt)
{
    dbf_file file;
    char catalog_path[path_buffer_size];
    char record[view_catalog_record_length];
    char type_buf[2];
    char stmt_buf[view_catalog_stmt_length + 1];

    if (find_view(root, db_name, name, type_buf, stmt_buf) == 0) {
        return -1;
    }
    if (ensure_view_catalog(root, catalog_path) != 0) {
        return -1;
    }
    if (dbf_open(&file, catalog_path) != 0) {
        return -1;
    }
    set_field(record, view_catalog_db_length, db_name);
    set_field(record + view_catalog_db_length,
        view_catalog_name_length, name);
    record[view_catalog_db_length + view_catalog_name_length] = type;
    set_field(record + view_catalog_db_length
        + view_catalog_name_length + view_catalog_type_length,
        view_catalog_stmt_length, stmt ? stmt : "");
    if (dbf_append(&file, record) != 0) {
        dbf_close(&file);
        return -1;
    }
    return dbf_close(&file);
}

int unregister_view(const char *root, const char *db_name,
    const char *name)
{
    dbf_file file;
    char catalog_path[path_buffer_size];
    char record[view_catalog_record_length];
    char record_db[view_catalog_db_length + 1];
    char record_name[view_catalog_name_length + 1];
    int state;
    unsigned long index;

    if (ensure_view_catalog(root, catalog_path) != 0) {
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
        get_field(record_db, sizeof(record_db), record,
            view_catalog_db_length);
        get_field(record_name, sizeof(record_name),
            record + view_catalog_db_length,
            view_catalog_name_length);
        if (strcmp(record_db, db_name) == 0
            && strcmp(record_name, name) == 0) {
            if (dbf_delete(&file, index) != 0) {
                dbf_close(&file);
                return -1;
            }
            return dbf_close(&file);
        }
    }
    dbf_close(&file);
    return -1;
}
