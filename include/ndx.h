/*
 * Declares a tiny dBase III NDX builder and reader for B-tree indexes.
 * The code builds standalone .ndx files from DBF tables, supports one
 * character, numeric, or date field, and also supports composite
 * character keys formed by concatenating multiple DBF fields.
 *
 * MIT License (see: LICENSE)
 * Copyright (C) 2026 tomaz stih
 */

#ifndef ndx_h
#define ndx_h

#include "dbf.h"

#define ndx_max_key_length 100
#define ndx_max_expression_length 488

typedef enum ndx_key_type {
    ndx_key_type_character = 0,
    ndx_key_type_numeric = 1
} ndx_key_type;

typedef struct ndx_file {
    int fd;
    unsigned long root_page;
    unsigned long total_pages;
    unsigned short key_length;
    unsigned short max_keys_per_page;
    unsigned short key_type;
    unsigned long key_record_size;
    unsigned char unique;
    char expression[ndx_max_expression_length + 1];
} ndx_file;

typedef int (*ndx_visit_fn)(unsigned long record_number);

/*
 * Extended scan callback that also receives the raw key bytes and a
 * caller-supplied context pointer. Used by ndx_scan to support equality
 * probes and range walks without global state.
 * Returning non-zero stops the walk.
 */
typedef int (*ndx_scan_fn)(unsigned long record_number,
    const unsigned char *key, unsigned short key_length, void *ctx);

/*
 * Creates one dBase III compatible NDX file from an open DBF table.
 * A single field may be character, numeric, or date. Multiple fields
 * must all be character fields and are concatenated in DBF byte order.
 * Deleted DBF records are skipped. Returns zero on success and -1 on
 * failure.
 */
int ndx_create(ndx_file *file, const char *path, dbf_file *table,
    const dbf_field *fields, unsigned short field_count,
    const unsigned short *key_fields, unsigned short key_field_count,
    unsigned char unique);

/*
 * Opens an existing NDX file and loads its header metadata.
 * The function validates the page geometry but does not walk the tree.
 * Returns zero on success and -1 on failure.
 */
int ndx_open(ndx_file *file, const char *path);

/*
 * Closes an open NDX file descriptor.
 * Returns zero on success and -1 on failure.
 */
int ndx_close(ndx_file *file);

/*
 * Finds the first exact match in a character index.
 * Short keys are padded with spaces to the declared key length.
 * Returns zero on match, one when no match exists, and -1 on failure.
 */
int ndx_find_text(ndx_file *file, const char *key,
    unsigned long *record_number);

/*
 * Finds the first exact match in a numeric index.
 * The search key is parsed from DBF-style decimal text and encoded as
 * the exact dBase III IEEE binary64 key form.
 * Returns zero on match, one when no match exists, and -1 on failure.
 */
int ndx_find_number(ndx_file *file, const char *value_text,
    unsigned long *record_number);

/*
 * Finds the first exact match in a date index.
 * The date must be supplied as YYYYMMDD from the DBF field bytes.
 * Returns zero on match, one when no match exists, and -1 on failure.
 */
int ndx_find_date(ndx_file *file, const char *yyyymmdd,
    unsigned long *record_number);

/*
 * Walks all index entries in ascending key order and calls back once
 * per leaf entry with the DBF record number. Returning non-zero from
 * the callback stops the walk and makes ndx_walk return one.
 * Returns zero on full walk, one when stopped by callback, and -1 on failure.
 */
int ndx_walk(ndx_file *file, ndx_visit_fn visit);

/*
 * Like ndx_walk but passes the raw key bytes and a context pointer to
 * each callback invocation. Use this for equality probes and range
 * scans where the scan needs to inspect or stop on the key value.
 * Returns zero on full walk, one when stopped by callback, and -1 on failure.
 */
int ndx_scan(ndx_file *file, ndx_scan_fn visit, void *ctx);

/*
 * Encodes a null-terminated string into a space-padded character key.
 * key_out must be at least ndx_max_key_length bytes.
 * Returns zero on success and non-zero when text is longer than key_length.
 */
int ndx_encode_text_key(unsigned char *key_out, unsigned short key_length,
    const char *text);

/*
 * Encodes a decimal number string into an NDX binary64 numeric key.
 * key_out must be at least ndx_max_key_length bytes.
 * Returns zero on success and -1 on failure.
 */
int ndx_encode_number_key(unsigned char *key_out, const char *value_text);

/*
 * Compares two raw key byte arrays using the index key type.
 * Returns negative, zero, or positive like strcmp.
 */
int ndx_compare_key(const ndx_file *file, const unsigned char *left,
    const unsigned char *right);

#endif
