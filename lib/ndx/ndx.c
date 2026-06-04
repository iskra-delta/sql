/*
 * Implements a tiny dBase III compatible NDX builder and reader.
 * The code builds 512-byte-page B-trees from DBF tables, supports
 * character, numeric, and date keys, and keeps the API low-level so
 * later SQL integration can stay explicit.
 *
 * MIT License (see: LICENSE)
 * Copyright (C) 2026 tomaz stih
 */

#include "ndx.h"
#include "binary64.h"

#if defined(__SDCC)
#define O_CREAT 0x0100
#define O_TRUNC 0x0200
#define O_RDWR 0x0002
#define SEEK_SET 0
#define SEEK_END 2
extern int open(const char *path, int flags, ...);
extern int close(int fd);
extern int read(int fd, void *buffer, unsigned int size);
extern int write(int fd, const void *buffer, unsigned int size);
extern long lseek(int fd, long offset, int whence);
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include <stdlib.h>
#include <string.h>

#define ndx_page_size 512
#define ndx_expression_size 488
#define ndx_numeric_key_size 8
#define ndx_max_key_parts 32
#define ndx_walk_depth 32

typedef struct ndx_key_part {
    unsigned short offset;
    unsigned short length;
    char type;
} ndx_key_part;

typedef struct ndx_key_plan {
    unsigned short key_length;
    unsigned short key_type;
    unsigned short part_count;
    ndx_key_part parts[ndx_max_key_parts];
    char expression[ndx_max_expression_length + 1];
} ndx_key_plan;

typedef struct ndx_build_entry {
    unsigned long record_number;
    unsigned char key[ndx_max_key_length];
} ndx_build_entry;

typedef struct ndx_page_ref {
    unsigned long page_number;
    unsigned char max_key[ndx_max_key_length];
} ndx_page_ref;

typedef struct ndx_walk_frame {
    unsigned long page_number;
    unsigned long index;
} ndx_walk_frame;

/*
 * Clears the file handle so failed calls leave no stale values.
 */
static void ndx_reset(ndx_file *file)
{
    unsigned short index;

    file->fd = -1;
    file->root_page = 0;
    file->total_pages = 0;
    file->key_length = 0;
    file->max_keys_per_page = 0;
    file->key_type = 0;
    file->key_record_size = 0;
    file->unique = 0;
    for (index = 0; index <= ndx_max_expression_length; index++) {
        file->expression[index] = '\0';
    }
}

/*
 * Writes the full buffer even if the host splits the request.
 */
static int write_exact(int fd, const unsigned char *buffer,
    unsigned short size)
{
    unsigned short done;
    int wrote;

    done = 0;
    while (done < size) {
        wrote = (int)write(fd, buffer + done, size - done);
        if (wrote <= 0) {
            return -1;
        }
        done = (unsigned short)(done + wrote);
    }

    return 0;
}

/*
 * Reads the full buffer even if the host splits the request.
 */
static int read_exact(int fd, unsigned char *buffer, unsigned short size)
{
    unsigned short done;
    int got;

    done = 0;
    while (done < size) {
        got = (int)read(fd, buffer + done, size - done);
        if (got <= 0) {
            return -1;
        }
        done = (unsigned short)(done + got);
    }

    return 0;
}

/*
 * Stores an unsigned short in little-endian order.
 */
static void store_u16(unsigned char *buffer, unsigned short value)
{
    buffer[0] = (unsigned char)(value & 0xff);
    buffer[1] = (unsigned char)((value >> 8) & 0xff);
}

/*
 * Stores an unsigned long in little-endian order.
 */
static void store_u32(unsigned char *buffer, unsigned long value)
{
    buffer[0] = (unsigned char)(value & 0xff);
    buffer[1] = (unsigned char)((value >> 8) & 0xff);
    buffer[2] = (unsigned char)((value >> 16) & 0xff);
    buffer[3] = (unsigned char)((value >> 24) & 0xff);
}

/*
 * Loads an unsigned short from little-endian order.
 */
static unsigned short load_u16(const unsigned char *buffer)
{
    return (unsigned short)buffer[0]
        | (unsigned short)((unsigned short)buffer[1] << 8);
}

/*
 * Loads an unsigned long from little-endian order.
 */
static unsigned long load_u32(const unsigned char *buffer)
{
    return (unsigned long)buffer[0]
        | ((unsigned long)buffer[1] << 8)
        | ((unsigned long)buffer[2] << 16)
        | ((unsigned long)buffer[3] << 24);
}

/*
 * Rounds a value up to the next 4-byte boundary.
 */
static unsigned long align4(unsigned long value)
{
    return (value + 3UL) & ~3UL;
}

/*
 * Zeroes one byte buffer.
 */
static void clear_bytes(unsigned char *buffer, unsigned short size)
{
    unsigned short index;

    for (index = 0; index < size; index++) {
        buffer[index] = 0;
    }
}

/*
 * Converts one ASCII character to lowercase without locale rules.
 */
static char ascii_lower(char value)
{
    if (value >= 'A' && value <= 'Z') {
        return (char)(value + ('a' - 'A'));
    }

    return value;
}

/*
 * Builds one lowercase field-name expression joined with '+'.
 */
static int build_expression(char *expression, const dbf_field *fields,
    const unsigned short *key_fields, unsigned short key_field_count)
{
    unsigned short part_index;
    unsigned short name_index;
    unsigned short out;

    out = 0;
    for (part_index = 0; part_index < key_field_count; part_index++) {
        if (part_index > 0) {
            if (out >= ndx_max_expression_length) {
                return -1;
            }
            expression[out++] = '+';
        }

        for (name_index = 0; name_index < 11; name_index++) {
            if (fields[key_fields[part_index]].name[name_index] == '\0') {
                break;
            }
            if (out >= ndx_max_expression_length) {
                return -1;
            }
            expression[out++] = ascii_lower(
                fields[key_fields[part_index]].name[name_index]);
        }
    }

    expression[out] = '\0';
    return 0;
}

/*
 * Returns the DBF record offset of one field inside the payload bytes.
 */
static unsigned short field_offset_for(const dbf_field *fields,
    unsigned short field_count, unsigned short field_index)
{
    unsigned short index;
    unsigned short offset;

    offset = 0;
    for (index = 0; index < field_count; index++) {
        if (index == field_index) {
            return offset;
        }
        offset = (unsigned short)(offset + fields[index].length);
    }

    return 0;
}

/*
 * Validates indexed fields and resolves offsets for key extraction.
 */
static int resolve_key_plan(const dbf_field *fields,
    unsigned short field_count, const unsigned short *key_fields,
    unsigned short key_field_count, ndx_key_plan *plan)
{
    unsigned short index;
    unsigned short key_length;
    unsigned short field_index;
    char field_type;

    if (key_field_count == 0 || key_field_count > ndx_max_key_parts) {
        return -1;
    }

    clear_bytes((unsigned char *)plan, (unsigned short)sizeof(ndx_key_plan));
    plan->part_count = key_field_count;

    if (build_expression(plan->expression, fields, key_fields,
        key_field_count) != 0) {
        return -1;
    }

    if (key_field_count == 1) {
        field_index = key_fields[0];
        if (field_index >= field_count) {
            return -1;
        }

        field_type = fields[field_index].type;
        if (field_type == 'C') {
            key_length = fields[field_index].length;
            if (key_length == 0 || key_length > ndx_max_key_length) {
                return -1;
            }

            plan->key_type = ndx_key_type_character;
            plan->key_length = key_length;
            plan->parts[0].offset = field_offset_for(fields, field_count,
                field_index);
            plan->parts[0].length = fields[field_index].length;
            plan->parts[0].type = field_type;
            return 0;
        }

        if (field_type == 'N' || field_type == 'D') {
            plan->key_type = ndx_key_type_numeric;
            plan->key_length = ndx_numeric_key_size;
            plan->parts[0].offset = field_offset_for(fields, field_count,
                field_index);
            plan->parts[0].length = fields[field_index].length;
            plan->parts[0].type = field_type;
            return 0;
        }

        return -1;
    }

    key_length = 0;
    for (index = 0; index < key_field_count; index++) {
        field_index = key_fields[index];
        if (field_index >= field_count) {
            return -1;
        }
        if (fields[field_index].type != 'C') {
            return -1;
        }
        if ((unsigned short)(key_length + fields[field_index].length)
            > ndx_max_key_length) {
            return -1;
        }

        plan->parts[index].offset = field_offset_for(fields, field_count,
            field_index);
        plan->parts[index].length = fields[field_index].length;
        plan->parts[index].type = fields[field_index].type;
        key_length = (unsigned short)(key_length
            + fields[field_index].length);
    }

    plan->key_type = ndx_key_type_character;
    plan->key_length = key_length;
    return 0;
}

/*
 * Builds one NDX key from one DBF payload record.
 */
static int build_key(unsigned char *key, const ndx_key_plan *plan,
    const char *record)
{
    unsigned short index;
    unsigned short out;

    clear_bytes(key, ndx_max_key_length);

    if (plan->key_type == ndx_key_type_numeric) {
        if (plan->parts[0].type == 'N') {
            if (ndx_binary64_from_numeric_text(key,
                record + plan->parts[0].offset,
                plan->parts[0].length) != 0) {
                return -1;
            }
        } else if (plan->parts[0].type == 'D') {
            if (ndx_binary64_from_date_text(key,
                record + plan->parts[0].offset,
                plan->parts[0].length) != 0) {
                return -1;
            }
        } else {
            return -1;
        }
        return 0;
    }

    out = 0;
    for (index = 0; index < plan->part_count; index++) {
        memcpy(key + out, record + plan->parts[index].offset,
            plan->parts[index].length);
        out = (unsigned short)(out + plan->parts[index].length);
    }

    return 0;
}

/*
 * Compares two character keys in DBF byte order.
 */
static int compare_text_keys(const unsigned char *left,
    const unsigned char *right, unsigned short key_length)
{
    unsigned short index;

    for (index = 0; index < key_length; index++) {
        if (left[index] < right[index]) {
            return -1;
        }
        if (left[index] > right[index]) {
            return 1;
        }
    }

    return 0;
}

/*
 * Compares two keys according to the active NDX key type.
 */
static int compare_keys(unsigned short key_type, unsigned short key_length,
    const unsigned char *left, const unsigned char *right)
{
    if (key_type == ndx_key_type_character) {
        return compare_text_keys(left, right, key_length);
    }

    return ndx_binary64_compare(left, right);
}

/*
 * Compares two build entries with record number as the stable tie-break.
 */
static int compare_entries(const ndx_file *file,
    const ndx_build_entry *left, const ndx_build_entry *right)
{
    int result;

    result = compare_keys(file->key_type, file->key_length,
        left->key, right->key);
    if (result != 0) {
        return result;
    }

    if (left->record_number < right->record_number) {
        return -1;
    }
    if (left->record_number > right->record_number) {
        return 1;
    }

    return 0;
}

/*
 * Sorts build entries in-place with an iterative shell sort.
 */
static void sort_entries(const ndx_file *file, ndx_build_entry *entries,
    unsigned long entry_count)
{
    unsigned long gap;
    unsigned long index;
    unsigned long probe;
    ndx_build_entry temp;

    gap = entry_count / 2UL;
    while (gap > 0UL) {
        for (index = gap; index < entry_count; index++) {
            temp = entries[index];
            probe = index;
            while (probe >= gap
                && compare_entries(file, &entries[probe - gap], &temp) > 0) {
                entries[probe] = entries[probe - gap];
                probe = probe - gap;
            }
            entries[probe] = temp;
        }
        gap = gap / 2UL;
    }
}

/*
 * Removes duplicate keys in-place when UNIQUE mode is active.
 */
static unsigned long compact_unique_entries(const ndx_file *file,
    ndx_build_entry *entries, unsigned long entry_count)
{
    unsigned long read_index;
    unsigned long write_index;

    if (entry_count == 0UL) {
        return 0UL;
    }

    write_index = 1UL;
    for (read_index = 1UL; read_index < entry_count; read_index++) {
        if (compare_keys(file->key_type, file->key_length,
            entries[write_index - 1UL].key,
            entries[read_index].key) != 0) {
            entries[write_index++] = entries[read_index];
        }
    }

    return write_index;
}

/*
 * Seeks to one logical page number inside the NDX file.
 */
static int seek_page(int fd, unsigned long page_number)
{
    unsigned long offset;

    offset = page_number * (unsigned long)ndx_page_size;
    if (lseek(fd, (long)offset, SEEK_SET) < 0) {
        return -1;
    }

    return 0;
}

/*
 * Writes one full 512-byte page to the requested page slot.
 */
static int write_page(int fd, unsigned long page_number,
    const unsigned char *page)
{
    if (seek_page(fd, page_number) != 0) {
        return -1;
    }
    return write_exact(fd, page, ndx_page_size);
}

/*
 * Reads one full 512-byte page from the requested page slot.
 */
static int read_page(int fd, unsigned long page_number,
    unsigned char *page)
{
    if (seek_page(fd, page_number) != 0) {
        return -1;
    }
    return read_exact(fd, page, ndx_page_size);
}

/*
 * Returns the maximum keys that fit in one 512-byte NDX page.
 */
static unsigned short max_keys_for(unsigned short key_length)
{
    unsigned long group_length;

    group_length = align4(8UL + (unsigned long)key_length);
    return (unsigned short)((ndx_page_size - 8UL) / group_length);
}

/*
 * Returns how many pages are needed when a level is evenly distributed.
 */
static unsigned long page_count_for(unsigned long item_count,
    unsigned long max_items_per_page)
{
    if (item_count == 0UL) {
        return 1UL;
    }
    return (item_count + max_items_per_page - 1UL) / max_items_per_page;
}

/*
 * Returns the evenly distributed item count of one output page.
 */
static unsigned long items_for_page(unsigned long item_count,
    unsigned long page_count, unsigned long page_index)
{
    unsigned long base;
    unsigned long extra;

    base = item_count / page_count;
    extra = item_count % page_count;
    if (page_index < extra) {
        return base + 1UL;
    }
    return base;
}

/*
 * Copies one fixed-width key into a page-ref maximum key slot.
 */
static void copy_max_key(unsigned char *target, const unsigned char *source,
    unsigned short key_length)
{
    memcpy(target, source, key_length);
}

/*
 * Writes the header page from the live ndx_file values.
 */
static int write_header_page(const ndx_file *file)
{
    unsigned char header[ndx_page_size];
    unsigned short index;

    clear_bytes(header, ndx_page_size);
    store_u32(header + 0, file->root_page);
    store_u32(header + 4, file->total_pages);
    store_u16(header + 12, file->key_length);
    store_u16(header + 14, file->max_keys_per_page);
    store_u16(header + 16, file->key_type);
    store_u16(header + 18, (unsigned short)file->key_record_size);
    store_u16(header + 20, 0U);
    store_u16(header + 22, (unsigned short)file->unique);

    for (index = 0; index < ndx_expression_size; index++) {
        if (file->expression[index] == '\0') {
            break;
        }
        header[24 + index] = (unsigned char)file->expression[index];
    }

    return write_page(file->fd, 0, header);
}

/*
 * Builds leaf pages from sorted key entries.
 */
static int build_leaf_pages(ndx_file *file, const ndx_build_entry *entries,
    unsigned long entry_count, ndx_page_ref **refs_out,
    unsigned long *ref_count_out)
{
    unsigned char page[ndx_page_size];
    ndx_page_ref *refs;
    unsigned long page_count;
    unsigned long page_index;
    unsigned long count;
    unsigned long entry_index;
    unsigned long entry_offset;
    unsigned long start;

    page_count = page_count_for(entry_count,
        (unsigned long)file->max_keys_per_page);
    refs = (ndx_page_ref *)malloc((size_t)(page_count * sizeof(ndx_page_ref)));
    if (refs == NULL) {
        return -1;
    }

    start = 0UL;
    file->total_pages = 1UL;
    for (page_index = 0UL; page_index < page_count; page_index++) {
        clear_bytes(page, ndx_page_size);
        count = items_for_page(entry_count, page_count, page_index);
        store_u32(page + 0, count);

        for (entry_index = 0UL; entry_index < count; entry_index++) {
            entry_offset = 4UL + (entry_index * file->key_record_size);
            store_u32(page + entry_offset, 0UL);
            store_u32(page + entry_offset + 4UL,
                entries[start + entry_index].record_number);
            memcpy(page + entry_offset + 8UL,
                entries[start + entry_index].key, file->key_length);
        }

        store_u32(page + 4UL + (count * file->key_record_size), 0UL);
        file->total_pages++;
        refs[page_index].page_number = file->total_pages - 1UL;
        if (count > 0UL) {
            copy_max_key(refs[page_index].max_key,
                entries[start + count - 1UL].key, file->key_length);
        } else {
            clear_bytes(refs[page_index].max_key, ndx_max_key_length);
        }

        if (write_page(file->fd, refs[page_index].page_number, page) != 0) {
            free(refs);
            return -1;
        }

        start = start + count;
    }

    *refs_out = refs;
    *ref_count_out = page_count;
    return 0;
}

/*
 * Builds one upper branch level from an array of child pages.
 */
static int build_branch_level(ndx_file *file, const ndx_page_ref *children,
    unsigned long child_count, ndx_page_ref **refs_out,
    unsigned long *ref_count_out)
{
    unsigned char page[ndx_page_size];
    ndx_page_ref *refs;
    unsigned long page_count;
    unsigned long page_index;
    unsigned long children_in_page;
    unsigned long child_index;
    unsigned long entry_index;
    unsigned long entry_offset;
    unsigned long start;
    unsigned long count;

    page_count = page_count_for(child_count,
        (unsigned long)file->max_keys_per_page + 1UL);
    refs = (ndx_page_ref *)malloc((size_t)(page_count * sizeof(ndx_page_ref)));
    if (refs == NULL) {
        return -1;
    }

    start = 0UL;
    for (page_index = 0UL; page_index < page_count; page_index++) {
        clear_bytes(page, ndx_page_size);
        children_in_page = items_for_page(child_count, page_count,
            page_index);
        count = children_in_page - 1UL;
        store_u32(page + 0, count);

        for (entry_index = 0UL; entry_index < count; entry_index++) {
            child_index = start + entry_index;
            entry_offset = 4UL + (entry_index * file->key_record_size);
            store_u32(page + entry_offset,
                children[child_index].page_number);
            store_u32(page + entry_offset + 4UL, 0UL);
            memcpy(page + entry_offset + 8UL,
                children[child_index].max_key, file->key_length);
        }

        store_u32(page + 4UL + (count * file->key_record_size),
            children[start + children_in_page - 1UL].page_number);
        file->total_pages++;
        refs[page_index].page_number = file->total_pages - 1UL;
        copy_max_key(refs[page_index].max_key,
            children[start + children_in_page - 1UL].max_key,
            file->key_length);

        if (write_page(file->fd, refs[page_index].page_number, page) != 0) {
            free(refs);
            return -1;
        }

        start = start + children_in_page;
    }

    *refs_out = refs;
    *ref_count_out = page_count;
    return 0;
}

/*
 * Returns one when the loaded header values are safe to use.
 */
static int header_is_valid(const ndx_file *file, unsigned long file_size)
{
    unsigned short expected_max;

    if (file->root_page == 0UL || file->total_pages < 2UL) {
        return 0;
    }
    if (file->root_page >= file->total_pages) {
        return 0;
    }
    if (file->key_length == 0
        || file->key_length > ndx_max_key_length) {
        return 0;
    }
    if (file->key_record_size < align4(8UL + file->key_length)) {
        return 0;
    }
    if ((file->key_record_size & 3UL) != 0UL) {
        return 0;
    }
    expected_max = max_keys_for(file->key_length);
    if (file->max_keys_per_page == 0
        || file->max_keys_per_page > expected_max) {
        return 0;
    }
    if (file->key_type != ndx_key_type_character
        && file->key_type != ndx_key_type_numeric) {
        return 0;
    }
    if (file_size < file->total_pages * (unsigned long)ndx_page_size) {
        return 0;
    }
    if ((file_size % (unsigned long)ndx_page_size) != 0UL) {
        return 0;
    }

    return 1;
}

/*
 * Reads and validates one page plus its entry count.
 */
static int load_page(const ndx_file *file, unsigned long page_number,
    unsigned char *page, unsigned long *count)
{
    unsigned long end_offset;

    if (page_number == 0UL || page_number >= file->total_pages) {
        return -1;
    }
    if (read_page(file->fd, page_number, page) != 0) {
        return -1;
    }
    *count = (unsigned long)load_u16(page + 0);
    if (*count > file->max_keys_per_page) {
        return -1;
    }
    end_offset = 4UL + (*count * file->key_record_size) + 4UL;
    if (end_offset > ndx_page_size) {
        return -1;
    }
    return 0;
}

/*
 * Returns one when the page is a leaf page.
 */
static int page_is_leaf(const ndx_file *file, const unsigned char *page,
    unsigned long count)
{
    unsigned long index;
    unsigned long entry_offset;

    for (index = 0UL; index < count; index++) {
        entry_offset = 4UL + (index * file->key_record_size);
        if (load_u32(page + entry_offset) != 0UL) {
            return 0;
        }
    }

    return load_u32(page + 4UL + (count * file->key_record_size)) == 0UL;
}

/*
 * Finds one exact key match using a caller-built raw key buffer.
 */
static int find_raw_key(ndx_file *file, const unsigned char *search_key,
    unsigned long *record_number)
{
    unsigned char page[ndx_page_size];
    unsigned long page_number;
    unsigned long count;
    unsigned long index;
    unsigned long entry_offset;
    int result;

    page_number = file->root_page;
    while (1) {
        if (load_page(file, page_number, page, &count) != 0) {
            return -1;
        }

        for (index = 0UL; index < count; index++) {
            entry_offset = 4UL + (index * file->key_record_size);
            result = compare_keys(file->key_type, file->key_length,
                search_key, page + entry_offset + 8UL);
            if (result <= 0) {
                if (load_u32(page + entry_offset) != 0UL) {
                    page_number = load_u32(page + entry_offset);
                    goto next_page;
                }
                if (result == 0) {
                    *record_number = load_u32(page + entry_offset + 4UL);
                    return 0;
                }
                return 1;
            }
        }

        page_number = load_u32(page + 4UL + (count * file->key_record_size));
        if (page_number == 0UL) {
            return 1;
        }

next_page:
        continue;
    }
}

/*
 * Builds a padded character search key from user text.
 */
static int build_text_search_key(unsigned char *key,
    unsigned short key_length, const char *text)
{
    unsigned short index;

    clear_bytes(key, ndx_max_key_length);
    for (index = 0; index < key_length; index++) {
        key[index] = ' ';
    }

    index = 0;
    while (text[index] != '\0') {
        if (index >= key_length) {
            return 1;
        }
        key[index] = (unsigned char)text[index];
        index++;
    }

    return 0;
}

/*
 * Visits one page subtree in ascending order using an explicit stack.
 * Passes the raw key bytes and caller context to each leaf entry.
 */
static int walk_tree_ex(ndx_file *file, ndx_scan_fn visit, void *ctx)
{
    unsigned char page[ndx_page_size];
    ndx_walk_frame stack[ndx_walk_depth];
    ndx_scan_entry entry;
    unsigned long count;
    unsigned long entry_offset;
    unsigned long child_page;
    int leaf;
    int depth;

    depth = 0;
    stack[0].page_number = file->root_page;
    stack[0].index = 0UL;

    while (depth >= 0) {
        if (load_page(file, stack[depth].page_number, page, &count) != 0) {
            return -1;
        }

        leaf = page_is_leaf(file, page, count);
        if (leaf) {
            if (stack[depth].index >= count) {
                depth--;
                continue;
            }

            entry_offset = 4UL + (stack[depth].index * file->key_record_size);
            stack[depth].index++;
            entry.key = page + entry_offset + 8UL;
            entry.key_length = file->key_length;
            entry.ctx = ctx;
            if (visit(load_u32(page + entry_offset + 4UL), &entry) != 0) {
                return 1;
            }
            continue;
        }

        if (stack[depth].index > count) {
            depth--;
            continue;
        }

        if (stack[depth].index < count) {
            entry_offset = 4UL
                + (stack[depth].index * file->key_record_size);
            child_page = load_u32(page + entry_offset);
        } else {
            child_page = load_u32(page + 4UL
                + (count * file->key_record_size));
        }

        stack[depth].index++;
        if (child_page == 0UL) {
            return -1;
        }
        if (depth + 1 >= ndx_walk_depth) {
            return -1;
        }

        depth++;
        stack[depth].page_number = child_page;
        stack[depth].index = 0UL;
    }

    return 0;
}

typedef struct walk_adapter_ctx {
    ndx_visit_fn visit;
} walk_adapter_ctx;

static int walk_adapter(unsigned long record_number,
    const ndx_scan_entry *entry)
{
    return ((walk_adapter_ctx *)entry->ctx)->visit(record_number);
}

/*
 * Visits one page subtree in ascending order (record number only).
 */
static int walk_tree(ndx_file *file, ndx_visit_fn visit)
{
    walk_adapter_ctx ctx;
    ctx.visit = visit;
    return walk_tree_ex(file, walk_adapter, &ctx);
}

int ndx_create(ndx_file *file, const char *path, dbf_file *table,
    const dbf_field *fields, unsigned short field_count,
    const unsigned short *key_fields, unsigned short key_field_count,
    unsigned char unique)
{
    ndx_key_plan plan;
    ndx_build_entry *entries;
    ndx_page_ref *current_refs;
    ndx_page_ref *next_refs;
    char *record;
    unsigned long record_index;
    unsigned long entry_count;
    unsigned long current_count;
    unsigned long next_count;
    int fd;
    int state;
    int flags;

    ndx_reset(file);
    if (table == NULL || fields == NULL || key_fields == NULL) {
        return -1;
    }
    if (table->fd < 0 || field_count < table->field_count) {
        return -1;
    }
    if (resolve_key_plan(fields, field_count, key_fields, key_field_count,
        &plan) != 0) {
        return -1;
    }

    file->key_length = plan.key_length;
    file->key_type = plan.key_type;
    file->max_keys_per_page = max_keys_for(file->key_length);
    file->key_record_size = align4(8UL + file->key_length);
    file->unique = unique ? 1 : 0;
    memcpy(file->expression, plan.expression,
        ndx_max_expression_length + 1U);

    flags = O_CREAT | O_TRUNC | O_RDWR;
    fd = open(path, flags, 0644);
    if (fd < 0) {
        ndx_reset(file);
        return -1;
    }

    file->fd = fd;
    record = (char *)malloc((size_t)table->record_length);
    if (record == NULL && table->record_length > 0) {
        ndx_close(file);
        return -1;
    }

    entries = NULL;
    if (table->record_count > 0UL) {
        entries = (ndx_build_entry *)malloc((size_t)(table->record_count
            * sizeof(ndx_build_entry)));
        if (entries == NULL) {
            free(record);
            ndx_close(file);
            return -1;
        }
    }

    entry_count = 0UL;
    for (record_index = 0UL; record_index < table->record_count;
        record_index++) {
        state = dbf_read(table, record_index, record);
        if (state < 0) {
            free(entries);
            free(record);
            ndx_close(file);
            return -1;
        }
        if (state == 1) {
            continue;
        }

        entries[entry_count].record_number = record_index + 1UL;
        if (build_key(entries[entry_count].key, &plan, record) != 0) {
            free(entries);
            free(record);
            ndx_close(file);
            return -1;
        }
        entry_count++;
    }

    free(record);
    sort_entries(file, entries, entry_count);
    if (file->unique) {
        entry_count = compact_unique_entries(file, entries, entry_count);
    }

    if (build_leaf_pages(file, entries, entry_count, &current_refs,
        &current_count) != 0) {
        free(entries);
        ndx_close(file);
        return -1;
    }

    while (current_count > 1UL) {
        if (build_branch_level(file, current_refs, current_count, &next_refs,
            &next_count) != 0) {
            free(current_refs);
            free(entries);
            ndx_close(file);
            return -1;
        }
        free(current_refs);
        current_refs = next_refs;
        current_count = next_count;
    }

    file->root_page = current_refs[0].page_number;
    if (write_header_page(file) != 0) {
        free(current_refs);
        free(entries);
        ndx_close(file);
        return -1;
    }

    free(current_refs);
    free(entries);
    return 0;
}

int ndx_open(ndx_file *file, const char *path)
{
    unsigned char header[ndx_page_size];
    unsigned short index;
    long file_size;
    int fd;

    ndx_reset(file);
    fd = open(path, O_RDWR);
    if (fd < 0) {
        return -1;
    }

    if (read_exact(fd, header, ndx_page_size) != 0) {
        close(fd);
        return -1;
    }

    file->fd = fd;
    file->root_page = load_u32(header + 0);
    file->total_pages = load_u32(header + 4);
    file->key_length = load_u16(header + 12);
    file->max_keys_per_page = load_u16(header + 14);
    file->key_type = load_u16(header + 16);
    file->key_record_size = (unsigned long)load_u16(header + 18);
    file->unique = (unsigned char)(load_u16(header + 22) != 0U);

    for (index = 0; index < ndx_expression_size; index++) {
        file->expression[index] = (char)header[24 + index];
        if (header[24 + index] == '\0') {
            break;
        }
    }
    file->expression[ndx_max_expression_length] = '\0';

    file_size = lseek(fd, 0L, SEEK_END);
    if (file_size < 0) {
        ndx_close(file);
        return -1;
    }
    if (!header_is_valid(file, (unsigned long)file_size)) {
        ndx_close(file);
        return -1;
    }

    return 0;
}

int ndx_close(ndx_file *file)
{
    int result;

    if (file->fd < 0) {
        return 0;
    }

    result = close(file->fd);
    ndx_reset(file);
    if (result != 0) {
        return -1;
    }

    return 0;
}

int ndx_find_text(ndx_file *file, const char *key,
    unsigned long *record_number)
{
    unsigned char search_key[ndx_max_key_length];
    int result;

    if (file->fd < 0 || file->key_type != ndx_key_type_character) {
        return -1;
    }

    result = build_text_search_key(search_key, file->key_length, key);
    if (result != 0) {
        return result;
    }

    return find_raw_key(file, search_key, record_number);
}

int ndx_find_number(ndx_file *file, const char *value_text,
    unsigned long *record_number)
{
    unsigned char search_key[ndx_max_key_length];

    if (file->fd < 0 || file->key_type != ndx_key_type_numeric) {
        return -1;
    }
    if (ndx_binary64_from_numeric_text(search_key, value_text,
        (unsigned short)strlen(value_text)) != 0) {
        return -1;
    }

    return find_raw_key(file, search_key, record_number);
}

int ndx_find_date(ndx_file *file, const char *yyyymmdd,
    unsigned long *record_number)
{
    unsigned char search_key[ndx_max_key_length];

    if (file->fd < 0 || file->key_type != ndx_key_type_numeric) {
        return -1;
    }
    if (ndx_binary64_from_date_text(search_key, yyyymmdd, 8U) != 0) {
        return -1;
    }

    return find_raw_key(file, search_key, record_number);
}

int ndx_walk(ndx_file *file, ndx_visit_fn visit)
{
    if (file->fd < 0 || visit == NULL) {
        return -1;
    }

    return walk_tree(file, visit);
}

int ndx_scan(ndx_file *file, ndx_scan_fn visit, void *ctx)
{
    if (file->fd < 0 || visit == NULL) {
        return -1;
    }

    return walk_tree_ex(file, visit, ctx);
}

int ndx_encode_text_key(unsigned char *key_out, unsigned short key_length,
    const char *text)
{
    return build_text_search_key(key_out, key_length, text);
}

int ndx_encode_number_key(unsigned char *key_out, const char *value_text)
{
    return ndx_binary64_from_numeric_text(key_out, value_text,
        (unsigned short)strlen(value_text));
}

int ndx_compare_key(const ndx_file *file, const unsigned char *left,
    const unsigned char *right)
{
    return compare_keys(file->key_type, file->key_length, left, right);
}
