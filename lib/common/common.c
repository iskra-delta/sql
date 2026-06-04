/*
 * Implements low-level utility functions used across parser,
 * optimizer, catalog, and executor modules.
 * All functions here are pure helpers with no dependency on SQL IR,
 * catalog layout, or executor runtime state.
 *
 * MIT License (see: LICENSE)
 * Copyright (C) 2026 tomaz stih
 */

#include "common.h"

#include <string.h>

void copy_name(char *target, const char *source)
{
    unsigned short index;

    for (index = 0; index + 1 < sql_name_size; index++) {
        target[index] = source[index];
        if (source[index] == '\0') {
            return;
        }
    }
    target[sql_name_size - 1] = '\0';
}

void get_field(char *target, unsigned short target_size,
    const char *source, unsigned short length)
{
    unsigned short size;

    size = length;
    while (size > 0 && source[size - 1] == ' ') {
        size--;
    }
    if (size + 1 > target_size) {
        size = (unsigned short)(target_size - 1);
    }
    memcpy(target, source, size);
    target[size] = '\0';
}

void set_field(char *target, unsigned short length, const char *value)
{
    unsigned short vlen;

    vlen = (unsigned short)strlen(value);
    if (vlen > length) {
        vlen = length;
    }
    memset(target, ' ', length);
    memcpy(target, value, vlen);
}

unsigned short get_slot_field(const char *src)
{
    unsigned short n;

    n = 0;
    if (src[0] >= '0' && src[0] <= '9') {
        n = (unsigned short)(src[0] - '0');
    }
    if (src[1] >= '0' && src[1] <= '9') {
        n = (unsigned short)(n * 10 + (src[1] - '0'));
    }
    return n;
}

void set_slot_field(char *target, unsigned short slot)
{
    target[0] = slot >= 10 ? (char)('0' + slot / 10) : ' ';
    target[1] = (char)('0' + slot % 10);
}

int join_path(char *target, const char *left, const char *right)
{
    unsigned short llen;
    unsigned short rlen;

    llen = (unsigned short)strlen(left);
    rlen = (unsigned short)strlen(right);
    if (llen + 1 + rlen + 1 > path_buffer_size) {
        return -1;
    }
    strcpy(target, left);
    if (llen > 0 && target[llen - 1] != '/') {
        target[llen] = '/';
        target[llen + 1] = '\0';
    }
    strcat(target, right);
    return 0;
}

void uint_to_str(unsigned short n, char *buf)
{
    unsigned short i;
    unsigned short j;
    char tmp;

    i = 0;
    j = 0;
    if (!n) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }
    while (n) {
        buf[i++] = (char)('0' + n % 10);
        n = (unsigned short)(n / 10);
    }
    buf[i] = '\0';
    while (j < i / 2) {
        tmp = buf[j];
        buf[j] = buf[i - 1 - j];
        buf[i - 1 - j] = tmp;
        j++;
    }
}

void trim_field_value(char *target, unsigned short target_size,
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

long parse_integer_text(const char *text, int *ok)
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

int compare_longs(long left, long right, sql_compare_operator op)
{
    switch (op) {
    case sql_compare_equal:         return left == right;
    case sql_compare_not_equal:     return left != right;
    case sql_compare_less:          return left <  right;
    case sql_compare_less_equal:    return left <= right;
    case sql_compare_greater:       return left >  right;
    case sql_compare_greater_equal: return left >= right;
    default:                        return 0;
    }
}

int compare_strings(const char *left, const char *right,
    sql_compare_operator op)
{
    int order;

    order = strcmp(left, right);
    switch (op) {
    case sql_compare_equal:         return order == 0;
    case sql_compare_not_equal:     return order != 0;
    case sql_compare_less:          return order <  0;
    case sql_compare_less_equal:    return order <= 0;
    case sql_compare_greater:       return order >  0;
    case sql_compare_greater_equal: return order >= 0;
    default:                        return 0;
    }
}

int field_is_null(const char *source, unsigned short length)
{
    unsigned short index;

    for (index = 0; index < length; index++) {
        if (source[index] != ' ') {
            return 0;
        }
    }
    return 1;
}

int find_field_index(const dbf_field *fields, unsigned short field_count,
    const char *name)
{
    unsigned short index;

    for (index = 0; index < field_count; index++) {
        if (strcmp(fields[index].name, name) == 0) {
            return (int)index;
        }
    }
    return -1;
}

int build_field_offsets(const dbf_field *fields, unsigned short field_count,
    unsigned short *offsets)
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

int store_value_in_field(char *target, const dbf_field *field,
    const sql_value *value)
{
    unsigned short value_size;
    const char *text;
    unsigned short index;

    for (index = 0; index < field->length; index++) {
        target[index] = ' ';
    }
    if (value->type == sql_value_null) {
        return 0;
    }
    text = value->text;
    value_size = (unsigned short)strlen(text);
    if (value_size > field->length) {
        return -1;
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

void clear_record(char *record, unsigned short length)
{
    memset(record, ' ', length);
}

void copy_subquery(char *dest, const char *src)
{
    strncpy(dest, src, sql_subquery_size - 1);
    dest[sql_subquery_size - 1] = '\0';
}

unsigned long ndx_to_dbf_index(unsigned long ndx_record)
{
    return ndx_record > 0 ? ndx_record - 1 : 0;
}

unsigned short crc16(const char *data, unsigned short length)
{
    unsigned short crc = 0xFFFFu;
    unsigned short i;
    unsigned char  j;

    for (i = 0; i < length; i++) {
        crc ^= (unsigned short)((unsigned char)data[i]) << 8;
        for (j = 0; j < 8; j++) {
            if (crc & 0x8000u)
                crc = (unsigned short)((crc << 1) ^ 0x1021u);
            else
                crc = (unsigned short)(crc << 1);
        }
    }
    return crc;
}

/* ------------------------------------------------------------------ */
/* Intrusive singly-linked list                                         */
/* ------------------------------------------------------------------ */

int list_match_eq(list_item *item, void *arg)
{
    return item == (list_item *)arg;
}

list_item *list_find(list_item *first, list_item **prev_out,
    list_match_fn match, void *arg)
{
    *prev_out = NULL;
    while (first && !match(first, arg)) {
        *prev_out = first;
        first = first->next;
    }
    return first;
}

list_item *list_insert(list_item **first, list_item *el)
{
    el->next = *first;
    *first = el;
    return el;
}

list_item *list_append(list_item **first, list_item *el)
{
    list_item *cur;

    el->next = NULL;
    if (!*first) {
        *first = el;
        return el;
    }
    cur = *first;
    while (cur->next)
        cur = cur->next;
    cur->next = el;
    return el;
}

list_item *list_remove(list_item **first, list_item *el)
{
    list_item *prev;

    if (!list_find(*first, &prev, list_match_eq, el))
        return NULL;
    if (prev)
        prev->next = el->next;
    else
        *first = el->next;
    return el;
}

list_item *list_remove_first(list_item **first)
{
    list_item *el = *first;

    if (el)
        *first = el->next;
    return el;
}
