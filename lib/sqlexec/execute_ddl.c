/*
 * Executes DDL statements: CREATE/DROP DATABASE/TABLE, USE, SHOW
 * DATABASES, and CREATE INDEX. All catalog mutation lives here; none
 * of this code touches row data or WHERE evaluation.
 *
 * MIT License (see: LICENSE)
 * Copyright (C) 2026 tomaz stih
 */

#include "exec_impl.h"
#include "../shared/metacache.h"

#include <string.h>
#include <stdlib.h>

#if defined(__SDCC)
extern int unlink(const char *path);
#else
#include <unistd.h>
#endif

/* Rebuild the schema cache for the active database. */
static void refresh_schema(sqlexec_env *env)
{
    if (!env->current_db || !env->current_db[0]) {
        return;
    }
    meta_cache_free(env->schema);
    env->schema = meta_cache_load(env->root, env->current_db);
}

/* ------------------------------------------------------------------ */
/* Output helpers (local to this module)                               */
/* ------------------------------------------------------------------ */

static void ddl_write_char(const sqlexec_env *env, char c)
{
    if (env->io && env->io->write_char) {
        env->io->write_char(c);
    }
}

static void ddl_write_nl(const sqlexec_env *env)
{
    ddl_write_char(env, '\r');
    ddl_write_char(env, '\n');
}

static void ddl_write_str(const sqlexec_env *env, const char *s)
{
    while (*s) {
        ddl_write_char(env, *s++);
    }
}

/* ------------------------------------------------------------------ */
/* Index key helpers                                                    */
/* ------------------------------------------------------------------ */

static int resolve_index_key_fields(const dbf_field *fields,
    unsigned short field_count, const sqlexec_program *program,
    sqlexec_span key_names, unsigned short *key_fields)
{
    unsigned char index;
    int field_index;

    for (index = 0; index < key_names.count; index++) {
        field_index = find_field_index(fields, field_count,
            program->names[key_names.first + index]);
        if (field_index < 0) {
            return -1;
        }
        key_fields[index] = (unsigned short)field_index;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* DDL executors                                                        */
/* ------------------------------------------------------------------ */

static int execute_create_database(sqlexec_env *env, const char *name)
{
    dbf_file file;
    char catalog_path[path_buffer_size];
    char db_path[path_buffer_size];
    char slot_name[6];
    char record[catalog_record_length];
    unsigned short slot;

    if (ensure_catalog(env->root, catalog_path) != 0) {
        return -1;
    }
    if (dbf_open(&file, catalog_path) != 0) {
        return -1;
    }
    if (find_catalog_slot(&file, name, &slot) != 0) {
        dbf_close(&file);
        return -1;
    }
    uint_to_str(slot, slot_name);
    if (join_path(db_path, env->root, slot_name) != 0) {
        dbf_close(&file);
        return -1;
    }
    if (ensure_directory(db_path) != 0) {
        dbf_close(&file);
        return -1;
    }
    set_field(record, catalog_name_length, name);
    set_field(record + catalog_name_length, catalog_path_length, db_path);
    set_slot_field(record + catalog_name_length + catalog_path_length, slot);
    if (dbf_append(&file, record) != 0) {
        dbf_close(&file);
        return -1;
    }
    if (dbf_close(&file) != 0) {
        return -1;
    }
    strcpy(env->current_db, name);
    refresh_schema(env);
    ddl_write_str(env, "created ");
    ddl_write_str(env, name);
    ddl_write_nl(env);
    return 0;
}

static int execute_use(sqlexec_env *env, const char *name)
{
    char db_path[path_buffer_size];

    if (find_database_path(env->root, name, db_path) != 0) {
        return -1;
    }
    strcpy(env->current_db, name);
    refresh_schema(env);
    ddl_write_str(env, "using ");
    ddl_write_str(env, name);
    ddl_write_nl(env);
    return 0;
}

static int execute_drop_database(sqlexec_env *env, const char *name)
{
    dbf_file file;
    char catalog_path[path_buffer_size];
    char record[catalog_record_length];
    char existing_name[catalog_name_length + 1];
    unsigned long index;
    int state;

    if (ensure_catalog(env->root, catalog_path) != 0) {
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
            if (remove_registered_indexes(env->root, name, NULL) != 0) {
                dbf_close(&file);
                return -1;
            }
            if (dbf_delete(&file, index) != 0) {
                dbf_close(&file);
                return -1;
            }
            if (dbf_close(&file) != 0) {
                return -1;
            }
            if (env->current_db[0]
                && strcmp(env->current_db, name) == 0) {
                env->current_db[0] = '\0';
            }
            ddl_write_str(env, "dropped ");
            ddl_write_str(env, name);
            ddl_write_nl(env);
            return 0;
        }
    }
    dbf_close(&file);
    return -1;
}

static int execute_drop_table(sqlexec_env *env, const char *table_name)
{
    char db_path[path_buffer_size];
    char table_path[path_buffer_size];

    if (!env->current_db || !env->current_db[0]) {
        return -1;
    }
    if (find_database_path(env->root, env->current_db, db_path) != 0) {
        return -1;
    }
    if (remove_registered_indexes(env->root, env->current_db,
        table_name) != 0) {
        return -1;
    }
    if (join_path(table_path, db_path, table_name) != 0) {
        return -1;
    }
    if ((unsigned short)(strlen(table_path) + 5) > path_buffer_size) {
        return -1;
    }
    strcat(table_path, ".dbf");
    if (unlink(table_path) != 0) {
        return -1;
    }
    refresh_schema(env);
    ddl_write_str(env, "dropped ");
    ddl_write_str(env, table_name);
    ddl_write_nl(env);
    return 0;
}

static int execute_create_table(sqlexec_env *env,
    const sqlexec_table_def *table_def)
{
    dbf_file file;
    dbf_field fields[sql_max_columns];
    char db_path[path_buffer_size];
    char table_path[path_buffer_size];
    unsigned short index;

    if (!env->current_db || !env->current_db[0]) {
        return -1;
    }
    if (find_database_path(env->root, env->current_db, db_path) != 0) {
        return -1;
    }
    if (join_path(table_path, db_path, table_def->name) != 0) {
        return -1;
    }
    if ((unsigned short)(strlen(table_path) + 5) > path_buffer_size) {
        return -1;
    }
    strcat(table_path, ".dbf");
    if (dbf_open(&file, table_path) == 0) {
        dbf_close(&file);
        return -1;
    }
    for (index = 0; index < table_def->columns.count; index++) {
        strcpy(fields[index].name,
            env->program->columns[
                table_def->columns.first + index].name);
        fields[index].type =
            env->program->columns[
                table_def->columns.first + index].dbf_type;
        fields[index].length =
            env->program->columns[
                table_def->columns.first + index].length;
        fields[index].decimals =
            env->program->columns[
                table_def->columns.first + index].decimals;
    }
    if (dbf_create(&file, table_path, fields,
        table_def->columns.count) != 0) {
        return -1;
    }
    if (dbf_close(&file) != 0) {
        return -1;
    }
    refresh_schema(env);
    ddl_write_str(env, "created ");
    ddl_write_str(env, table_def->name);
    ddl_write_nl(env);
    return 0;
}

static int execute_create_index(sqlexec_env *env,
    const sqlexec_index_def *index_def)
{
    dbf_file table;
    dbf_file catalog;
    dbf_field fields[sql_max_columns];
    unsigned short offsets[sql_max_columns];
    unsigned short key_fields[sql_max_columns];
    ndx_file index;
    char catalog_path[path_buffer_size];
    char index_path[path_buffer_size];
    char field_list[index_catalog_fields_length + 1];
    int present;

    if (open_table_file(env->root, env->current_db,
        index_def->table_name, &table, fields, offsets) != 0) {
        return -1;
    }
    if (resolve_index_key_fields(fields, table.field_count,
        env->program, index_def->key_names, key_fields) != 0) {
        dbf_close(&table);
        return -1;
    }
    if (build_index_field_list(fields, key_fields,
        index_def->key_names.count, field_list,
        sizeof(field_list)) != 0) {
        dbf_close(&table);
        return -1;
    }
    if (ensure_index_catalog(env->root, catalog_path) != 0) {
        dbf_close(&table);
        return -1;
    }
    if (dbf_open(&catalog, catalog_path) != 0) {
        dbf_close(&table);
        return -1;
    }
    present = index_catalog_has_name(&catalog, env->current_db,
        index_def->index_name);
    if (dbf_close(&catalog) != 0) {
        dbf_close(&table);
        return -1;
    }
    if (present != 0) {
        dbf_close(&table);
        return -1;
    }
    if (build_index_path(env->root, env->current_db,
        index_def->index_name, index_path) != 0) {
        dbf_close(&table);
        return -1;
    }
    if (ndx_create(&index, index_path, &table, fields, table.field_count,
        key_fields, index_def->key_names.count,
        index_def->unique) != 0) {
        dbf_close(&table);
        return -1;
    }
    if (ndx_close(&index) != 0) {
        dbf_close(&table);
        return -1;
    }
    if (dbf_close(&table) != 0) {
        return -1;
    }
    if (append_index_catalog_entry(env->root, env->current_db,
        index_def->index_name, index_def->table_name, field_list,
        index_def->unique) != 0) {
        unlink(index_path);
        return -1;
    }
    ddl_write_str(env, "created index ");
    ddl_write_str(env, index_def->index_name);
    ddl_write_nl(env);
    return 0;
}

/* ------------------------------------------------------------------ */
/* View executors                                                       */
/* ------------------------------------------------------------------ */

static int execute_create_view(sqlexec_env *env,
    const char *name)
{
    if (!env->current_db || !env->current_db[0]) {
        return -1;
    }
    if (register_view(env->root, env->current_db, name, 'U',
        env->program->subquery_text) != 0) {
        return -1;
    }
    ddl_write_str(env, "created view ");
    ddl_write_str(env, name);
    ddl_write_nl(env);
    return 0;
}

static int execute_drop_view(sqlexec_env *env, const char *name)
{
    if (!env->current_db || !env->current_db[0]) {
        return -1;
    }
    if (unregister_view(env->root, env->current_db, name) != 0) {
        return -1;
    }
    ddl_write_str(env, "dropped view ");
    ddl_write_str(env, name);
    ddl_write_nl(env);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Module entry point                                                   */
/* ------------------------------------------------------------------ */

int exec_ddl(sqlexec_env *env)
{
    const sqlexec_node *root_node;
    const sqlexec_node *first_node;
    const sqlexec_node *second_node;
    sqlexec_ref first_ref;
    sqlexec_ref second_ref;

    root_node = sqlexec_get_const(env->program, env->program->root);
    if (!root_node) {
        return -1;
    }

    switch (root_node->opcode) {
    case sqlexec_create_database:
        return execute_create_database(env, root_node->data.named.name);
    case sqlexec_use_database:
        return execute_use(env, root_node->data.named.name);
    case sqlexec_create_table:
        return execute_create_table(env, &root_node->data.table);
    case sqlexec_create_view:
        return execute_create_view(env, root_node->data.named.name);
    case sqlexec_drop_view:
        return execute_drop_view(env, root_node->data.named.name);
    case sqlexec_sequence:
        break;
    default:
        return -1;
    }

    /* Sequence root: first child determines the DDL operation. */
    first_ref = env->program->nodes[env->program->root].first_child;
    if (first_ref == sqlexec_nil) {
        return -1;
    }
    first_node = sqlexec_get_const(env->program, first_ref);
    if (!first_node) {
        return -1;
    }
    second_ref = first_node->next_sibling;
    second_node = second_ref != sqlexec_nil
        ? sqlexec_get_const(env->program, second_ref) : NULL;

    switch (first_node->opcode) {
    case sqlexec_build_index:
        if (!second_node
            || second_node->opcode != sqlexec_register_index) {
            return -1;
        }
        return execute_create_index(env, &first_node->data.index);
    case sqlexec_unregister_table_indexes:
        if (!second_node
            || second_node->opcode != sqlexec_drop_table) {
            return -1;
        }
        return execute_drop_table(env,
            second_node->data.named.name);
    case sqlexec_unregister_database_indexes:
        if (!second_node
            || second_node->opcode != sqlexec_drop_database) {
            return -1;
        }
        return execute_drop_database(env,
            second_node->data.named.name);
    default:
        return -1;
    }
}
