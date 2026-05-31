/*
 * Declares a schema metadata cache populated at USE time.
 * Tables are held in a linked list; each table's fields are a
 * malloc'd flat array. The cache lives in sql_context and survives
 * across queries within the same session. It is freed and rebuilt
 * when CREATE TABLE or DROP TABLE changes the schema.
 *
 * MIT License (see: LICENSE)
 * Copyright (C) 2026 tomaz stih
 */

#ifndef metacache_h
#define metacache_h

#include "shared.h"

typedef struct meta_table {
    char             name[sql_name_size];
    dbf_field       *fields;        /* malloc'd, field_count entries */
    unsigned short  *offsets;       /* malloc'd, field_count entries */
    unsigned char    field_count;
    struct meta_table *next;
} meta_table;

typedef struct meta_cache {
    char        db_name[sql_name_size];
    meta_table *tables;
    unsigned char table_count;
} meta_cache;

/*
 * Allocates and populates a meta_cache for the named database by
 * scanning its directory and reading DBF file headers.
 * Returns a heap-allocated cache on success and NULL on failure.
 */
meta_cache *meta_cache_load(const char *root, const char *db_name);

/*
 * Frees the cache and all its node allocations.
 * Safe to call with NULL.
 */
void meta_cache_free(meta_cache *cache);

/*
 * Looks up a table by name in the cache.
 * Returns the meta_table node or NULL when not found.
 */
meta_table *meta_cache_find_table(const meta_cache *cache,
    const char *table_name);

#endif
