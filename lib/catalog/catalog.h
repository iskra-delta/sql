/*
 * Declares catalog and table-file access helpers shared across the
 * optimizer and executor modules.
 *
 * MIT License (see: LICENSE)
 * Copyright (C) 2026 tomaz stih
 */

#ifndef catalog_h
#define catalog_h

#include "../common/common.h"
#include "ndx.h"

/* ------------------------------------------------------------------ */
/* Catalog record layout constants                                      */
/* ------------------------------------------------------------------ */

#define catalog_name_length    16
#define catalog_path_length    48
#define catalog_slot_length     2
#define catalog_record_length  66
#define catalog_max_databases  15

#define index_catalog_db_length     16
#define index_catalog_name_length   16
#define index_catalog_table_length  16
#define index_catalog_fields_length ((sql_max_columns * 11) \
    + (sql_max_columns - 1))
#define index_catalog_unique_length  1
#define index_catalog_record_length (index_catalog_db_length \
    + index_catalog_name_length + index_catalog_table_length \
    + index_catalog_fields_length + index_catalog_unique_length)

#define view_catalog_db_length    16
#define view_catalog_name_length  16
#define view_catalog_type_length   1
#define view_catalog_stmt_length 240
#define view_catalog_record_length (view_catalog_db_length \
    + view_catalog_name_length + view_catalog_type_length \
    + view_catalog_stmt_length)

/* ------------------------------------------------------------------ */
/* Directory and catalog bootstrap                                      */
/* ------------------------------------------------------------------ */

/*
 * Creates path as a directory if it does not already exist.
 * Returns zero on success and -1 on failure.
 */
int ensure_directory(const char *path);

/*
 * Ensures the database catalog file exists at <root>/sys/db.dbf,
 * creating it if necessary, and writes the path into catalog_path.
 * Returns zero on success and -1 on failure.
 */
int ensure_catalog(const char *root, char *catalog_path);

/*
 * Ensures the index catalog file exists at <root>/sys/ndx.dbf,
 * creating it if necessary, and writes the path into catalog_path.
 * Returns zero on success and -1 on failure.
 */
int ensure_index_catalog(const char *root, char *catalog_path);

/* ------------------------------------------------------------------ */
/* Database catalog operations                                          */
/* ------------------------------------------------------------------ */

/*
 * Scans the open catalog file for the first unused slot number.
 * Returns -1 if name already exists or all slots are taken.
 */
int find_catalog_slot(dbf_file *file, const char *name,
    unsigned short *slot_out);

/*
 * Looks up db_name in the catalog and copies its path into db_path.
 * Returns zero on success and -1 when the name is not found.
 */
int find_database_path(const char *root, const char *name, char *db_path);

/*
 * Fills a three-field array describing the database catalog layout.
 */
void fill_catalog_fields(dbf_field *fields);

/* ------------------------------------------------------------------ */
/* Index catalog operations                                             */
/* ------------------------------------------------------------------ */

/*
 * Fills a five-field array describing the index catalog layout.
 */
void fill_index_catalog_fields(dbf_field *fields);

/*
 * Writes one index catalog record into a caller-supplied buffer.
 */
void fill_index_catalog_record(char *record, const char *db_name,
    const char *index_name, const char *table_name,
    const char *field_list, unsigned char unique);

/*
 * Returns 1 when (db_name, index_name) already exists in the open
 * catalog file, 0 when absent, and -1 on read error.
 */
int index_catalog_has_name(dbf_file *file, const char *db_name,
    const char *index_name);

/*
 * Registers one index in the index catalog.
 * Returns 0 on success, 1 if already present, -1 on failure.
 */
int append_index_catalog_entry(const char *root, const char *db_name,
    const char *index_name, const char *table_name,
    const char *field_list, unsigned char unique);

/*
 * Marks catalog entries as deleted and unlinks .ndx files for every
 * index belonging to db_name. Passing NULL for table_name removes all
 * indexes for the database; otherwise only indexes for that table.
 * Returns zero on success and -1 on failure.
 */
int remove_registered_indexes(const char *root, const char *db_name,
    const char *table_name);

/*
 * Rebuilds every NDX file registered for (db_name, table_name).
 * Returns zero on success and -1 on failure.
 */
int rebuild_table_indexes(const char *root, const char *db_name,
    const char *table_name);

/* ------------------------------------------------------------------ */
/* Table file access                                                    */
/* ------------------------------------------------------------------ */

/*
 * Builds the full .ndx path for index_name in db_name into index_path.
 * Returns zero on success and -1 on failure.
 */
int build_index_path(const char *root, const char *db_name,
    const char *index_name, char *index_path);

/*
 * Opens table_name.dbf in db_name, reads fields and offsets,
 * and leaves the file open. The caller must call dbf_close.
 * Returns zero on success and -1 on failure.
 */
int open_table_file(const char *root, const char *db_name,
    const char *table_name, dbf_file *file, dbf_field *fields,
    unsigned short *offsets);

/*
 * Opens table_name.dbf, reads its field descriptors into fields,
 * writes the count into field_count, and closes the file.
 * Returns zero on success and -1 on failure.
 */
int open_table_fields(const char *root, const char *db_name,
    const char *table_name, dbf_field *fields,
    unsigned short *field_count);

/*
 * Serialises key_fields into a comma-separated field name string.
 * Returns zero on success and -1 on overflow.
 */
int build_index_field_list(const dbf_field *fields,
    const unsigned short *key_fields, unsigned short key_count,
    char *target, unsigned short target_size);

/*
 * Parses a comma-separated field name string into key_fields indices.
 * Returns zero on success and -1 on error.
 */
int parse_index_field_list(const char *text, const dbf_field *fields,
    unsigned short field_count, unsigned short *key_fields,
    unsigned short *key_count);

/*
 * Builds one NDX file from the index catalog record for (db_name,
 * index_name, table_name, field_list, unique). Called by
 * rebuild_table_indexes.
 * Returns zero on success and -1 on failure.
 */
int build_index_from_catalog_entry(const char *root, const char *db_name,
    const char *index_name, const char *table_name,
    const char *field_list, unsigned char unique);

/*
 * Ensures the view catalog file exists at <root>/sys/vw.dbf.
 * Returns zero on success and -1 on failure.
 */
int ensure_view_catalog(const char *root, char *catalog_path);

/*
 * Looks up a view by (db_name, name) in vw.dbf.
 * Writes the type character ('S' or 'U') and SQL text into the
 * caller-supplied buffers. type_out must hold at least 2 bytes;
 * stmt_out must hold at least view_catalog_stmt_length + 1 bytes.
 * Returns zero when found and -1 when not found or on error.
 */
int find_view(const char *root, const char *db_name,
    const char *name, char *type_out, char *stmt_out);

/*
 * Writes one view record into vw.dbf. Fails if the name already
 * exists for the given db. type is 'S' or 'U'. stmt may be empty
 * for built-in views.
 * Returns zero on success and -1 on failure.
 */
int register_view(const char *root, const char *db_name,
    const char *name, char type, const char *stmt);

/*
 * Marks the matching view record as deleted in vw.dbf.
 * Returns zero on success and -1 when not found or on error.
 */
int unregister_view(const char *root, const char *db_name,
    const char *name);

#endif
