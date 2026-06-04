/*
 * Built-in system view generators. Each gen_sys_* function builds a
 * temp DBF that the outer SELECT query reads as a normal table.
 * Separated from execute_subquery.c so it can be loaded as a distinct
 * CP/M module (MOD_SYSV.BIN) that is only paged in for sys_* queries.
 *
 * MIT License (see: LICENSE)
 * Copyright (C) 2026 tomaz stih
 */

#include "exec_impl.h"
#include "metacache.h"

#include <stdlib.h>
#include <string.h>

#if defined(__SDCC)
extern int unlink(const char *path);
#else
#include <unistd.h>
#include <dirent.h>
#endif


/* ------------------------------------------------------------------ */
/* Built-in view generators                                             */
/* ------------------------------------------------------------------ */

static int gen_sys_databases(const sqlexec_env *env,
    const char *temp_path)
{
    dbf_field fields[3];
    dbf_file cat;
    dbf_file tmp;
    char catalog_path[path_buffer_size];
    char *record;
    char name[catalog_name_length + 1];
    char path[catalog_path_length + 1];
    char tmp_record[catalog_name_length + catalog_path_length
        + catalog_slot_length];
    unsigned long i;
    int state;

    fill_catalog_fields(fields);
    if (ensure_catalog(env->root, catalog_path) != 0) {
        return -1;
    }
    if (dbf_open(&cat, catalog_path) != 0) {
        return -1;
    }
    if (dbf_create(&tmp, temp_path, fields, 3) != 0) {
        dbf_close(&cat);
        return -1;
    }
    record = (char *)malloc(catalog_record_length);
    if (!record) {
        dbf_close(&cat);
        dbf_close(&tmp);
        return -1;
    }
    for (i = 0; i < cat.record_count; i++) {
        state = dbf_read(&cat, i, record);
        if (state < 0) {
            free(record); dbf_close(&cat); dbf_close(&tmp); return -1;
        }
        if (state == 1) continue;
        get_field(name, sizeof(name), record, catalog_name_length);
        get_field(path, sizeof(path), record + catalog_name_length,
            catalog_path_length);
        set_field(tmp_record, catalog_name_length, name);
        set_field(tmp_record + catalog_name_length,
            catalog_path_length, path);
        tmp_record[catalog_name_length + catalog_path_length] =
            record[catalog_name_length + catalog_path_length];
        tmp_record[catalog_name_length + catalog_path_length + 1] =
            record[catalog_name_length + catalog_path_length + 1];
        dbf_append(&tmp, tmp_record);
    }
    free(record);
    dbf_close(&cat);
    return dbf_close(&tmp);
}

#define sys_fields_table_len  16
#define sys_fields_name_len   16
#define sys_fields_type_len    1
#define sys_fields_len_len     3
#define sys_fields_dec_len     2
#define sys_fields_pos_len     2
#define sys_fields_record_length (sys_fields_table_len \
    + sys_fields_name_len + sys_fields_type_len \
    + sys_fields_len_len + sys_fields_dec_len + sys_fields_pos_len)

static void fill_sys_fields_schema(dbf_field *fields)
{
    strcpy(fields[0].name, "table_name");
    fields[0].type = 'C'; fields[0].length = sys_fields_table_len;
    fields[0].decimals = 0;
    strcpy(fields[1].name, "name");
    fields[1].type = 'C'; fields[1].length = sys_fields_name_len;
    fields[1].decimals = 0;
    strcpy(fields[2].name, "type");
    fields[2].type = 'C'; fields[2].length = sys_fields_type_len;
    fields[2].decimals = 0;
    strcpy(fields[3].name, "length");
    fields[3].type = 'N'; fields[3].length = sys_fields_len_len;
    fields[3].decimals = 0;
    strcpy(fields[4].name, "decimals");
    fields[4].type = 'N'; fields[4].length = sys_fields_dec_len;
    fields[4].decimals = 0;
    strcpy(fields[5].name, "position");
    fields[5].type = 'N'; fields[5].length = sys_fields_pos_len;
    fields[5].decimals = 0;
}

static void write_sys_fields_row(dbf_file *tmp, const char *table_name,
    const dbf_field *f, unsigned char position)
{
    char rec[sys_fields_record_length];
    char num[8];
    unsigned short off;

    off = 0;
    set_field(rec + off, sys_fields_table_len, table_name); off += sys_fields_table_len;
    set_field(rec + off, sys_fields_name_len, f->name);     off += sys_fields_name_len;
    rec[off] = f->type;                                     off += sys_fields_type_len;
    uint_to_str(f->length, num);
    set_field(rec + off, sys_fields_len_len, num);          off += sys_fields_len_len;
    uint_to_str(f->decimals, num);
    set_field(rec + off, sys_fields_dec_len, num);          off += sys_fields_dec_len;
    uint_to_str(position, num);
    set_field(rec + off, sys_fields_pos_len, num);
    dbf_append(tmp, rec);
}

static int gen_sys_fields_for_table(const sqlexec_env *env,
    dbf_file *tmp, const char *table_name)
{
    dbf_file src;
    dbf_field *src_fields;
    unsigned short offsets[sql_max_columns];
    unsigned short i;

    src_fields = (dbf_field *)malloc(sql_max_columns * sizeof(dbf_field));
    if (!src_fields)
        return -1;
    if (open_table_file(env->root, env->current_db, table_name,
        &src, src_fields, offsets) != 0) {
        free(src_fields);
        return -1;
    }
    for (i = 0; i < src.field_count; i++) {
        write_sys_fields_row(tmp, table_name, &src_fields[i],
            (unsigned char)(i + 1));
    }
    free(src_fields);
    return dbf_close(&src);
}

static int gen_sys_tables(const sqlexec_env *env, const char *temp_path)
{
    dbf_field fields[1];
    dbf_file tmp;
    char rec[16];
    meta_table *t;

    strcpy(fields[0].name, "name");
    fields[0].type = 'C'; fields[0].length = 16; fields[0].decimals = 0;

    if (!env->current_db || !env->current_db[0]) {
        return -1;
    }
    if (dbf_create(&tmp, temp_path, fields, 1) != 0) {
        return -1;
    }

    if (env->schema) {
        /* Cache path: iterate in-memory table list, no file I/O. */
        for (t = env->schema->tables; t; t = t->next) {
            set_field(rec, 16, t->name);
            dbf_append(&tmp, rec);
        }
        return dbf_close(&tmp);
    }

#if !defined(__SDCC)
    /* No cache: fall back to directory scan. */
    {
        char db_path[path_buffer_size];
        char entry_path[path_buffer_size];
        DIR *dir;
        struct dirent *ent;
        unsigned short nlen;

        if (find_database_path(env->root, env->current_db, db_path) != 0) {
            dbf_close(&tmp);
            return -1;
        }
        dir = opendir(db_path);
        if (!dir) {
            dbf_close(&tmp);
            return -1;
        }
        while ((ent = readdir(dir)) != NULL) {
            nlen = (unsigned short)strlen(ent->d_name);
            if (nlen < 5 || ent->d_name[0] == '_') continue;
            if (ent->d_name[nlen - 4] != '.'
                || (ent->d_name[nlen - 3] | 0x20) != 'd'
                || (ent->d_name[nlen - 2] | 0x20) != 'b'
                || (ent->d_name[nlen - 1] | 0x20) != 'f') continue;
            if (join_path(entry_path, db_path, ent->d_name) != 0) continue;
            set_field(rec, 16, "");
            memcpy(rec, ent->d_name, nlen - 4 < 16 ? nlen - 4 : 16);
            dbf_append(&tmp, rec);
        }
        closedir(dir);
    }
#endif

    return dbf_close(&tmp);
}

static int gen_sys_fields_all(const sqlexec_env *env,
    const char *temp_path)
{
    dbf_field schema[6];
    dbf_file tmp;
    meta_table *t;
    unsigned char i;

    fill_sys_fields_schema(schema);
    if (!env->current_db || !env->current_db[0]) {
        return -1;
    }
    if (dbf_create(&tmp, temp_path, schema, 6) != 0) {
        return -1;
    }

    if (env->schema) {
        /* Cache path: iterate in-memory fields, no file I/O. */
        for (t = env->schema->tables; t; t = t->next) {
            for (i = 0; i < t->field_count; i++) {
                write_sys_fields_row(&tmp, t->name,
                    &t->fields[i], (unsigned char)(i + 1));
            }
        }
        return dbf_close(&tmp);
    }

#if !defined(__SDCC)
    /* No cache: fall back to opening each DBF file. */
    {
        char db_path[path_buffer_size];
        char table_name[17];
        DIR *dir;
        struct dirent *ent;
        unsigned short nlen;

        if (find_database_path(env->root, env->current_db, db_path) != 0) {
            dbf_close(&tmp);
            return -1;
        }
        dir = opendir(db_path);
        if (!dir) {
            dbf_close(&tmp);
            return -1;
        }
        while ((ent = readdir(dir)) != NULL) {
            nlen = (unsigned short)strlen(ent->d_name);
            if (nlen < 5 || ent->d_name[0] == '_') continue;
            if (ent->d_name[nlen - 4] != '.'
                || (ent->d_name[nlen - 3] | 0x20) != 'd'
                || (ent->d_name[nlen - 2] | 0x20) != 'b'
                || (ent->d_name[nlen - 1] | 0x20) != 'f') continue;
            memset(table_name, 0, sizeof(table_name));
            memcpy(table_name, ent->d_name,
                nlen - 4 < 16 ? nlen - 4 : 16);
            gen_sys_fields_for_table(env, &tmp, table_name);
        }
        closedir(dir);
    }
#endif

    return dbf_close(&tmp);
}

static int gen_sys_indexes(const sqlexec_env *env, const char *temp_path)
{
    dbf_field fields[4];
    dbf_file cat;
    dbf_file tmp;
    char catalog_path[path_buffer_size];
    char *record;
    char rec_db[index_catalog_db_length + 1];
    char rec_name[index_catalog_name_length + 1];
    char rec_table[index_catalog_table_length + 1];
    char rec_fields[64];
    char rec_unique[2];
    char tmp_record[16 + 16 + 64 + 1];
    unsigned long i;
    int state;

    strcpy(fields[0].name, "name");
    fields[0].type = 'C'; fields[0].length = 16; fields[0].decimals = 0;
    strcpy(fields[1].name, "table_name");
    fields[1].type = 'C'; fields[1].length = 16; fields[1].decimals = 0;
    strcpy(fields[2].name, "key_fields");
    fields[2].type = 'C'; fields[2].length = 64; fields[2].decimals = 0;
    strcpy(fields[3].name, "unique");
    fields[3].type = 'C'; fields[3].length = 1; fields[3].decimals = 0;

    if (ensure_index_catalog(env->root, catalog_path) != 0) {
        return -1;
    }
    if (dbf_open(&cat, catalog_path) != 0) {
        return -1;
    }
    if (dbf_create(&tmp, temp_path, fields, 4) != 0) {
        dbf_close(&cat);
        return -1;
    }
    record = (char *)malloc(index_catalog_record_length);
    if (!record) {
        dbf_close(&cat);
        dbf_close(&tmp);
        return -1;
    }
    for (i = 0; i < cat.record_count; i++) {
        state = dbf_read(&cat, i, record);
        if (state < 0) {
            free(record); dbf_close(&cat); dbf_close(&tmp); return -1;
        }
        if (state == 1) continue;
        get_field(rec_db, sizeof(rec_db), record, index_catalog_db_length);
        if (env->current_db[0]
            && strcmp(rec_db, env->current_db) != 0) continue;
        get_field(rec_name, sizeof(rec_name),
            record + index_catalog_db_length, index_catalog_name_length);
        get_field(rec_table, sizeof(rec_table),
            record + index_catalog_db_length + index_catalog_name_length,
            index_catalog_table_length);
        get_field(rec_fields, sizeof(rec_fields),
            record + index_catalog_db_length + index_catalog_name_length
                + index_catalog_table_length,
            index_catalog_fields_length < 64 ? index_catalog_fields_length : 63);
        rec_unique[0] = record[index_catalog_db_length
            + index_catalog_name_length + index_catalog_table_length
            + index_catalog_fields_length];
        rec_unique[1] = '\0';
        set_field(tmp_record, 16, rec_name);
        set_field(tmp_record + 16, 16, rec_table);
        set_field(tmp_record + 32, 64, rec_fields);
        tmp_record[96] = rec_unique[0];
        dbf_append(&tmp, tmp_record);
    }
    free(record);
    dbf_close(&cat);
    return dbf_close(&tmp);
}

static int gen_sys_views(const sqlexec_env *env, const char *temp_path)
{
    dbf_field fields[2];
    dbf_file cat;
    dbf_file tmp;
    char catalog_path[path_buffer_size];
    char *record;
    char rec_db[view_catalog_db_length + 1];
    char rec_name[view_catalog_name_length + 1];
    char tmp_record[16 + 1];
    unsigned long i;
    int state;

    strcpy(fields[0].name, "name");
    fields[0].type = 'C'; fields[0].length = 16; fields[0].decimals = 0;
    strcpy(fields[1].name, "type");
    fields[1].type = 'C'; fields[1].length = 1; fields[1].decimals = 0;

    if (ensure_view_catalog(env->root, catalog_path) != 0) {
        return -1;
    }
    if (dbf_open(&cat, catalog_path) != 0) {
        return -1;
    }
    if (dbf_create(&tmp, temp_path, fields, 2) != 0) {
        dbf_close(&cat);
        return -1;
    }
    record = (char *)malloc(view_catalog_record_length);
    if (!record) {
        dbf_close(&cat);
        dbf_close(&tmp);
        return -1;
    }
    for (i = 0; i < cat.record_count; i++) {
        state = dbf_read(&cat, i, record);
        if (state < 0) {
            free(record); dbf_close(&cat); dbf_close(&tmp); return -1;
        }
        if (state == 1) continue;
        get_field(rec_db, sizeof(rec_db), record, view_catalog_db_length);
        if (env->current_db[0]
            && strcmp(rec_db, env->current_db) != 0) continue;
        get_field(rec_name, sizeof(rec_name),
            record + view_catalog_db_length, view_catalog_name_length);
        set_field(tmp_record, 16, rec_name);
        tmp_record[16] = record[view_catalog_db_length
            + view_catalog_name_length];
        dbf_append(&tmp, tmp_record);
    }
    free(record);
    dbf_close(&cat);
    return dbf_close(&tmp);
}

/* ------------------------------------------------------------------ */
/* Built-in view dispatch                                               */
/* ------------------------------------------------------------------ */

int dispatch_builtin_view(const sqlexec_env *env,
    const char *view_name, const char *temp_path)
{
    if (strcmp(view_name, "sys_databases") == 0) {
        return gen_sys_databases(env, temp_path);
    }
    if (strcmp(view_name, "sys_tables") == 0) {
        return gen_sys_tables(env, temp_path);
    }
    if (strcmp(view_name, "sys_fields") == 0) {
        return gen_sys_fields_all(env, temp_path);
    }
    if (strcmp(view_name, "sys_indexes") == 0) {
        return gen_sys_indexes(env, temp_path);
    }
    if (strcmp(view_name, "sys_views") == 0) {
        return gen_sys_views(env, temp_path);
    }
    return -1;
}
