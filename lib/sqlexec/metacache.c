/*
 * Implements the sqlexec schema metadata cache.
 * This stays executor-local because it is runtime state tied to USE,
 * DDL refreshes, and system-view execution.
 *
 * MIT License (see: LICENSE)
 * Copyright (C) 2026 tomaz stih
 */

#include "metacache.h"
#include "../catalog/catalog.h"

#include <stdlib.h>
#include <string.h>

#if !defined(__SDCC)
#include <dirent.h>
#endif

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

static meta_table *alloc_table_node(const char *name,
    const dbf_field *fields, unsigned char field_count)
{
    meta_table *node;
    unsigned short i;

    node = (meta_table *)malloc(sizeof(meta_table));
    if (!node) {
        return NULL;
    }
    copy_name(node->name, name);
    node->field_count = field_count;
    node->next = NULL;
    node->fields = NULL;
    node->offsets = NULL;

    if (field_count == 0) {
        return node;
    }

    node->fields = (dbf_field *)malloc(
        (unsigned short)(field_count * sizeof(dbf_field)));
    if (!node->fields) {
        free(node);
        return NULL;
    }
    node->offsets = (unsigned short *)malloc(
        (unsigned short)(field_count * sizeof(unsigned short)));
    if (!node->offsets) {
        free(node->fields);
        free(node);
        return NULL;
    }

    for (i = 0; i < field_count; i++) {
        node->fields[i] = fields[i];
    }
    build_field_offsets(node->fields, field_count, node->offsets);
    return node;
}

static void free_table_node(meta_table *node)
{
    if (!node) {
        return;
    }
    free(node->offsets);
    free(node->fields);
    free(node);
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

meta_cache *meta_cache_load(const char *root, const char *db_name)
{
    meta_cache *cache;
    char db_path[path_buffer_size];
    meta_table *tail;

#if !defined(__SDCC)
    dbf_field *fields;
    unsigned short field_count;
    DIR *dir;
    struct dirent *ent;
    char table_name[sql_name_size];
    unsigned short nlen;
    meta_table *node;
#endif

    if (find_database_path(root, db_name, db_path) != 0) {
        return NULL;
    }

    cache = (meta_cache *)malloc(sizeof(meta_cache));
    if (!cache) {
        return NULL;
    }
    copy_name(cache->db_name, db_name);
    cache->tables = NULL;
    cache->table_count = 0;
    tail = NULL;

#if !defined(__SDCC)
    fields = (dbf_field *)malloc(sql_max_columns * sizeof(dbf_field));
    if (!fields) {
        free(cache);
        return NULL;
    }
    dir = opendir(db_path);
    if (!dir) {
        free(fields);
        free(cache);
        return NULL;
    }

    while ((ent = readdir(dir)) != NULL) {
        nlen = (unsigned short)strlen(ent->d_name);
        if (nlen < 5 || ent->d_name[0] == '_') {
            continue;
        }
        if (ent->d_name[nlen - 4] != '.'
            || (ent->d_name[nlen - 3] | 0x20) != 'd'
            || (ent->d_name[nlen - 2] | 0x20) != 'b'
            || (ent->d_name[nlen - 1] | 0x20) != 'f') {
            continue;
        }

        memset(table_name, 0, sizeof(table_name));
        memcpy(table_name, ent->d_name,
            nlen - 4 < sql_name_size - 1 ? nlen - 4 : sql_name_size - 1);

        field_count = 0;
        if (open_table_fields(root, db_name, table_name,
            fields, &field_count) != 0) {
            continue;
        }

        node = alloc_table_node(table_name, fields,
            (unsigned char)field_count);
        if (!node) {
            closedir(dir);
            free(fields);
            meta_cache_free(cache);
            return NULL;
        }

        if (tail) {
            tail->next = node;
        } else {
            cache->tables = node;
        }
        tail = node;
        cache->table_count++;
    }

    closedir(dir);
    free(fields);
#endif

    return cache;
}

void meta_cache_free(meta_cache *cache)
{
    meta_table *node;
    meta_table *next;

    if (!cache) {
        return;
    }
    node = cache->tables;
    while (node) {
        next = node->next;
        free_table_node(node);
        node = next;
    }
    free(cache);
}

meta_table *meta_cache_find_table(const meta_cache *cache,
    const char *table_name)
{
    meta_table *node;

    if (!cache) {
        return NULL;
    }
    node = cache->tables;
    while (node) {
        if (strcmp(node->name, table_name) == 0) {
            return node;
        }
        node = node->next;
    }
    return NULL;
}
