/*
 * Implements subquery execution and built-in view generators.
 *
 * run_subquery materialises an inner SELECT (or a built-in view) into
 * a temporary DBF at <root>/sys/_tmp.dbf, then returns so the outer
 * query can open that file as an ordinary table.
 *
 * delete_temp removes the temporary file after the outer query closes
 * it.
 *
 * Built-in views (sys_ prefix) generate their rows procedurally from
 * catalog files and DBF headers rather than from stored SQL text.
 *
 * MIT License (see: LICENSE)
 * Copyright (C) 2026 tomaz stih
 */

#include "exec_impl.h"
#include "sqlctx.h"
#include "sql.h"
#include "sqlopt.h"
#include "../shared/catalog.h"

#include <string.h>

#if defined(__SDCC)
extern int unlink(const char *path);
#else
#include <unistd.h>
#include <dirent.h>
#endif

#define temp_table_name "_tmp"

/* ------------------------------------------------------------------ */
/* Temp table path                                                      */
/* ------------------------------------------------------------------ */

/*
 * The temp table lives in the current database directory so that
 * open_table_file(root, current_db, "_tmp", ...) finds it naturally.
 * Falls back to <root>/sys/ when no database is selected.
 */
static int temp_table_path(const char *root, const char *db_name,
    char *path_out)
{
    char db_path[path_buffer_size];

    if (db_name && db_name[0]) {
        if (find_database_path(root, db_name, db_path) != 0) {
            return -1;
        }
    } else {
        if (join_path(db_path, root, "sys") != 0) {
            return -1;
        }
    }
    return join_path(path_out, db_path, "_tmp.dbf");
}

/* ------------------------------------------------------------------ */
/* Write-to-temp: schema determination                                  */
/* ------------------------------------------------------------------ */

/*
 * Determines the output schema of an inner query by opening the source
 * table and resolving the project columns. Writes the schema into
 * fields_out/count_out.
 */
static int determine_inner_schema(const char *root, const char *db,
    const char *table_name, unsigned char select_all,
    unsigned char select_count, const char (*names)[sql_name_size],
    dbf_field *fields_out, unsigned short *count_out)
{
    dbf_file file;
    dbf_field src_fields[sql_max_columns];
    unsigned short offsets[sql_max_columns];
    unsigned short i;
    int fi;

    if (open_table_file(root, db, table_name, &file, src_fields,
        offsets) != 0) {
        return -1;
    }

    if (select_all || select_count == 0) {
        for (i = 0; i < file.field_count; i++) {
            fields_out[i] = src_fields[i];
        }
        *count_out = file.field_count;
    } else {
        *count_out = 0;
        for (i = 0; i < select_count; i++) {
            fi = find_field_index(src_fields, file.field_count, names[i]);
            if (fi < 0) {
                dbf_close(&file);
                return -1;
            }
            fields_out[*count_out] = src_fields[fi];
            (*count_out)++;
        }
    }

    return dbf_close(&file);
}

/* ------------------------------------------------------------------ */
/* Append projected row to temp DBF                                     */
/* ------------------------------------------------------------------ */

/*
 * Builds one DBF record from the projected columns and appends it.
 * env->write_to_temp must be 1 and env->temp_out must be open.
 */
void append_projected_to_temp(const sqlexec_env *env,
    const sqlexec_project_def *project,
    const row_source *source)
{
    char record[table_record_size];
    const row_source *src;
    char value[sql_value_size];
    unsigned short i;
    unsigned short out_count;
    int fi;
    unsigned short offset;

    out_count = project->select_all
        ? source->field_count : project->names.count;

    clear_record(record, env->temp_field_count > 0
        ? (unsigned short)(env->temp_offsets[env->temp_field_count - 1]
            + env->temp_fields[env->temp_field_count - 1].length)
        : 0);

    offset = 0;
    for (i = 0; i < out_count; i++) {
        if (project->select_all) {
            src = source;
            fi = (int)i;
        } else {
            if (resolve_field_ref(source, NULL,
                env->program->names[project->qualifiers.first + i],
                env->program->names[project->names.first + i],
                &src, &fi) != 0) {
                return;
            }
        }
        trim_field_value(value, sizeof(value),
            src->record + src->offsets[fi],
            src->fields[fi].length);
        set_field(record + offset, env->temp_fields[i].length, value);
        offset = (unsigned short)(offset
            + env->temp_fields[i].length);
    }

    dbf_append(env->temp_out, record);
}

/* ------------------------------------------------------------------ */
/* Execute inner SQL to temp DBF                                        */
/* ------------------------------------------------------------------ */

/*
 * Parses and executes an inner SELECT, redirecting output to a temp DBF
 * instead of the terminal. The temp DBF is created at temp_path with
 * a schema matching the inner SELECT's project columns.
 */
static int exec_sql_to_temp(const char *root, const char *db,
    const char *sql_text, const char *temp_path)
{
    sql_context inner;
    char sql_with_semi[sql_subquery_size + 2];
    size_t slen;
    unsigned short len;
    dbf_field fields[sql_max_columns];
    unsigned short field_count;

    /* Prepare the inner SQL text with a trailing semicolon. */
    slen = strlen(sql_text);
    len = (unsigned short)slen;
    if (len == 0 || slen + 2 > sizeof(sql_with_semi)) {
        return -1;
    }
    memcpy(sql_with_semi, sql_text, len);
    sql_with_semi[len] = ';';
    sql_with_semi[len + 1] = '\0';

    /* Parse the inner query without view expansion (one level only). */
    inner.root = root;
    inner.current_db[0] = '\0';
    if (db) {
        copy_name(inner.current_db, db);
    }
    inner.text = sql_with_semi;
    inner.io.write_char = NULL;
    inner.result = 0;

    if (sql_parse(inner.text, &inner.program, NULL, NULL) != 0) {
        return -1;
    }
    if (sqlopt_run(&inner) != 0) {
        return -1;
    }

    /*
     * Determine the temp table schema from the inner program's
     * open_table and project nodes.
     */
    {
        const sqlexec_node *node;
        sqlexec_ref child;
        const char *src_table = NULL;
        unsigned char sel_all = 1;
        unsigned char sel_count = 0;
        const char (*col_names)[sql_name_size] = NULL;

        /* Find open_table node to get source table name. */
        if (inner.program.root != sqlexec_nil) {
            node = sqlexec_get_const(&inner.program, inner.program.root);
            if (node && node->opcode == sqlexec_open_table) {
                src_table = node->data.named.name;
            } else if (node && node->opcode == sqlexec_sequence) {
                child = node->first_child;
                while (child != sqlexec_nil) {
                    node = sqlexec_get_const(&inner.program, child);
                    if (node && node->opcode == sqlexec_open_table) {
                        src_table = node->data.named.name;
                        break;
                    }
                    child = node ? node->next_sibling : sqlexec_nil;
                }
                /* Find project node for column list. */
                child = sqlexec_get_const(&inner.program,
                    inner.program.root)->first_child;
                while (child != sqlexec_nil) {
                    node = sqlexec_get_const(&inner.program, child);
                    if (node && node->opcode == sqlexec_project) {
                        sel_all = node->data.project.select_all;
                        sel_count = node->data.project.names.count;
                        if (!sel_all && sel_count > 0) {
                            col_names = (const char (*)[sql_name_size])
                                &inner.program.names[
                                    node->data.project.names.first];
                        }
                        break;
                    }
                    if (node && node->opcode == sqlexec_emit_rows) {
                        sqlexec_ref pr = node->first_child;
                        node = sqlexec_get_const(&inner.program, pr);
                        if (node && node->opcode == sqlexec_project) {
                            sel_all = node->data.project.select_all;
                            sel_count = node->data.project.names.count;
                            if (!sel_all && sel_count > 0) {
                                col_names =
                                    (const char (*)[sql_name_size])
                                    &inner.program.names[
                                        node->data.project.names.first];
                            }
                        }
                        break;
                    }
                    child = node ? node->next_sibling : sqlexec_nil;
                }
            }
        }

        if (!src_table) {
            return -1;
        }

        if (determine_inner_schema(root, db, src_table, sel_all,
            sel_count, col_names, fields, &field_count) != 0) {
            return -1;
        }
    }

    /* Create the temp DBF. */
    {
        sqlexec_env env2;
        dbf_file temp_dbf;
        unsigned short temp_offsets[sql_max_columns];
        int ret;

        if (dbf_create(&temp_dbf, temp_path, fields, field_count) != 0) {
            return -1;
        }
        build_field_offsets(fields, field_count, temp_offsets);

        /* Execute inner query with output redirected to temp_dbf. */
        env2.root = root;
        env2.program = &inner.program;
        env2.current_db = inner.current_db;
        env2.io = &inner.io;
        env2.write_to_temp = 1;
        env2.temp_out = &temp_dbf;
        env2.temp_fields = fields;
        env2.temp_offsets = temp_offsets;
        env2.temp_field_count = field_count;

        ret = sqlexec_execute_env(&env2);
        dbf_close(&temp_dbf);
        return ret;
    }
}

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
    char record[catalog_record_length];
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
    for (i = 0; i < cat.record_count; i++) {
        state = dbf_read(&cat, i, record);
        if (state < 0) { dbf_close(&cat); dbf_close(&tmp); return -1; }
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
    dbf_field src_fields[sql_max_columns];
    unsigned short offsets[sql_max_columns];
    unsigned short i;

    if (open_table_file(env->root, env->current_db, table_name,
        &src, src_fields, offsets) != 0) {
        return -1;
    }
    for (i = 0; i < src.field_count; i++) {
        write_sys_fields_row(tmp, table_name, &src_fields[i],
            (unsigned char)(i + 1));
    }
    return dbf_close(&src);
}

#if !defined(__SDCC)
static int gen_sys_tables(const sqlexec_env *env, const char *temp_path)
{
    dbf_field fields[1];
    dbf_file tmp;
    char db_path[path_buffer_size];
    char entry_path[path_buffer_size];
    char rec[16];
    DIR *dir;
    struct dirent *ent;
    unsigned short nlen;

    strcpy(fields[0].name, "name");
    fields[0].type = 'C'; fields[0].length = 16; fields[0].decimals = 0;

    if (!env->current_db || !env->current_db[0]) {
        return -1;
    }
    if (find_database_path(env->root, env->current_db, db_path) != 0) {
        return -1;
    }
    if (dbf_create(&tmp, temp_path, fields, 1) != 0) {
        return -1;
    }

    dir = opendir(db_path);
    if (!dir) {
        dbf_close(&tmp);
        return -1;
    }
    while ((ent = readdir(dir)) != NULL) {
        nlen = (unsigned short)strlen(ent->d_name);
        /* Only .dbf files not starting with '_' */
        if (nlen < 5) continue;
        if (ent->d_name[0] == '_') continue;
        if (ent->d_name[nlen - 4] != '.'
            || (ent->d_name[nlen - 3] | 0x20) != 'd'
            || (ent->d_name[nlen - 2] | 0x20) != 'b'
            || (ent->d_name[nlen - 1] | 0x20) != 'f') continue;
        /* Verify the file exists and is accessible */
        if (join_path(entry_path, db_path, ent->d_name) != 0) continue;
        /* Strip extension for the table name */
        set_field(rec, 16, "");
        memcpy(rec, ent->d_name, nlen - 4 < 16 ? nlen - 4 : 16);
        dbf_append(&tmp, rec);
    }
    closedir(dir);
    return dbf_close(&tmp);
}

static int gen_sys_fields_all(const sqlexec_env *env,
    const char *temp_path)
{
    dbf_field schema[6];
    dbf_file tmp;
    char db_path[path_buffer_size];
    char table_name[17];
    DIR *dir;
    struct dirent *ent;
    unsigned short nlen;

    fill_sys_fields_schema(schema);
    if (!env->current_db || !env->current_db[0]) {
        return -1;
    }
    if (find_database_path(env->root, env->current_db, db_path) != 0) {
        return -1;
    }
    if (dbf_create(&tmp, temp_path, schema, 6) != 0) {
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
    return dbf_close(&tmp);
}
#else
/* CP/M: directory scan not yet implemented */
static int gen_sys_tables(const sqlexec_env *env, const char *temp_path)
{
    dbf_field fields[1];
    dbf_file tmp;
    (void)env;
    strcpy(fields[0].name, "name");
    fields[0].type = 'C'; fields[0].length = 16; fields[0].decimals = 0;
    if (dbf_create(&tmp, temp_path, fields, 1) != 0) return -1;
    return dbf_close(&tmp);
}
static int gen_sys_fields_all(const sqlexec_env *env, const char *temp_path)
{
    dbf_field schema[6];
    dbf_file tmp;
    (void)env;
    fill_sys_fields_schema(schema);
    if (dbf_create(&tmp, temp_path, schema, 6) != 0) return -1;
    return dbf_close(&tmp);
}
#endif

static int gen_sys_indexes(const sqlexec_env *env, const char *temp_path)
{
    dbf_field fields[4];
    dbf_file cat;
    dbf_file tmp;
    char catalog_path[path_buffer_size];
    char record[index_catalog_record_length];
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
    for (i = 0; i < cat.record_count; i++) {
        state = dbf_read(&cat, i, record);
        if (state < 0) { dbf_close(&cat); dbf_close(&tmp); return -1; }
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
    dbf_close(&cat);
    return dbf_close(&tmp);
}

static int gen_sys_views(const sqlexec_env *env, const char *temp_path)
{
    dbf_field fields[2];
    dbf_file cat;
    dbf_file tmp;
    char catalog_path[path_buffer_size];
    char record[view_catalog_record_length];
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
    for (i = 0; i < cat.record_count; i++) {
        state = dbf_read(&cat, i, record);
        if (state < 0) { dbf_close(&cat); dbf_close(&tmp); return -1; }
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
    dbf_close(&cat);
    return dbf_close(&tmp);
}

/* ------------------------------------------------------------------ */
/* Built-in view dispatch                                               */
/* ------------------------------------------------------------------ */

static int dispatch_builtin_view(const sqlexec_env *env,
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

/* ------------------------------------------------------------------ */
/* Public entry points                                                  */
/* ------------------------------------------------------------------ */

int exec_run_subquery(sqlexec_env *env)
{
    char temp_path[path_buffer_size];
    const char *subquery = env->program->subquery_text;

    if (temp_table_path(env->root, env->current_db, temp_path) != 0) {
        return -1;
    }
    /* Remove any leftover temp file from a previous failed query. */
    unlink(temp_path);

    /* Built-in views start with "sys_" prefix. */
    if (subquery[0] == 's' && subquery[1] == 'y'
        && subquery[2] == 's' && subquery[3] == '_') {
        return dispatch_builtin_view(env, subquery, temp_path);
    }

    /* User SQL text — execute inner query to temp DBF. */
    return exec_sql_to_temp(env->root, env->current_db,
        subquery, temp_path);
}

int exec_delete_temp(sqlexec_env *env)
{
    char temp_path[path_buffer_size];

    if (temp_table_path(env->root, env->current_db, temp_path) != 0) {
        return 0;
    }
    unlink(temp_path);
    return 0;
}
