/*
 * Declares low-level utility functions and constants used across the
 * parser, optimizer, catalog, and executor modules.
 * The code here is intentionally phase-neutral infrastructure with no
 * knowledge of SQL IR or executor runtime state.
 *
 * MIT License (see: LICENSE)
 * Copyright (C) 2026 tomaz stih
 */

#ifndef common_h
#define common_h

#include "dbf.h"
#include "sqltypes.h"

/* ------------------------------------------------------------------ */
/* Common buffer size constants                                         */
/* ------------------------------------------------------------------ */

#define path_buffer_size    128
#define table_record_size  4096

/* ------------------------------------------------------------------ */
/* String utilities                                                     */
/* ------------------------------------------------------------------ */

/*
 * Copies one name into a fixed sql_name_size buffer, always
 * null-terminating the result.
 */
void copy_name(char *target, const char *source);

/*
 * Reads one space-padded DBF field value into a null-terminated string.
 */
void get_field(char *target, unsigned short target_size,
    const char *source, unsigned short length);

/*
 * Writes a null-terminated string into a space-padded DBF field.
 */
void set_field(char *target, unsigned short length, const char *value);

/*
 * Reads a two-byte numeric slot field (right-aligned, space-padded).
 */
unsigned short get_slot_field(const char *src);

/*
 * Writes a slot number into a two-byte numeric field.
 */
void set_slot_field(char *target, unsigned short slot);

/*
 * Joins two path segments with a '/' separator into target.
 * Returns zero on success and -1 when the result would overflow.
 */
int join_path(char *target, const char *left, const char *right);

/*
 * Converts an unsigned short to a decimal string in buf.
 */
void uint_to_str(unsigned short n, char *buf);

/*
 * Trims leading and trailing spaces from a fixed-length field value.
 */
void trim_field_value(char *target, unsigned short target_size,
    const char *source, unsigned short length);

/*
 * Parses a signed decimal integer from text.
 * Sets *ok to 1 on success and 0 on failure.
 */
long parse_integer_text(const char *text, int *ok);

/*
 * Compares two long values using the given SQL comparison operator.
 */
int compare_longs(long left, long right, sql_compare_operator op);

/*
 * Compares two strings using the given SQL comparison operator.
 */
int compare_strings(const char *left, const char *right,
    sql_compare_operator op);

/*
 * Returns 1 when a fixed-width DBF field stores a NULL-like blank
 * value, 0 otherwise.
 */
int field_is_null(const char *source, unsigned short length);

/* ------------------------------------------------------------------ */
/* DBF field utilities                                                  */
/* ------------------------------------------------------------------ */

/*
 * Returns the zero-based index of the field named name, or -1.
 */
int find_field_index(const dbf_field *fields, unsigned short field_count,
    const char *name);

/*
 * Computes byte offsets for each field in the record payload.
 * Returns zero on success and -1 if the total exceeds table_record_size.
 */
int build_field_offsets(const dbf_field *fields, unsigned short field_count,
    unsigned short *offsets);

/*
 * Stores a SQL value into one fixed-width DBF field, padding as required
 * by the field type. Returns zero on success and -1 on overflow.
 */
int store_value_in_field(char *target, const dbf_field *field,
    const sql_value *value);

/*
 * Fills a record buffer with space characters.
 */
void clear_record(char *record, unsigned short length);

/*
 * Copies a subquery text string safely into a sql_subquery_size buffer.
 */
void copy_subquery(char *dest, const char *src);

/*
 * Converts a 1-based NDX record number to a 0-based DBF record index.
 * NDX files store record numbers starting at 1; dbf_read/write use 0-based.
 */
unsigned long ndx_to_dbf_index(unsigned long ndx_record);

/*
 * Computes CRC-16/CCITT of length bytes starting at data.
 * Used by the transaction layer to snapshot record content for
 * precondition checking at COMMIT time.
 */
unsigned short crc16(const char *data, unsigned short length);

/* ------------------------------------------------------------------ */
/* Intrusive singly-linked list                                         */
/* ------------------------------------------------------------------ */

/*
 * Embed list_item as the FIRST member of any struct that participates
 * in a list, then cast freely between list_item * and the struct *.
 */
typedef struct list_item {
    struct list_item *next;
} list_item;

typedef int (*list_match_fn)(list_item *item, void *arg);

/*
 * Default match: returns 1 when item == arg (pointer identity).
 */
int list_match_eq(list_item *item, void *arg);

/*
 * Returns the first item for which match returns non-zero.
 * Writes the predecessor into *prev_out (NULL when item is the head).
 * Returns NULL when no item matches.
 */
list_item *list_find(list_item *first, list_item **prev_out,
    list_match_fn match, void *arg);

/*
 * Inserts el at the head of the list.  Returns el.
 */
list_item *list_insert(list_item **first, list_item *el);

/*
 * Appends el to the tail of the list.  Returns el.
 */
list_item *list_append(list_item **first, list_item *el);

/*
 * Removes el from the list.  Returns el, or NULL when not found.
 */
list_item *list_remove(list_item **first, list_item *el);

/*
 * Removes and returns the head element, or NULL when the list is empty.
 */
list_item *list_remove_first(list_item **first);

#endif
