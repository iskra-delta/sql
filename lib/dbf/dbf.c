/*
 * Implements a tiny DBF reader and writer for fixed-layout tables.
 * The code sticks to the dBase III header format, avoids parsing
 * field values, and moves records with direct descriptor I/O.
 *
 * MIT License (see: LICENSE)
 * Copyright (C) 2026 tomaz stih
 */

#include "dbf.h"

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

#define dbf_header_size 32
#define dbf_field_size 32
#define dbf_header_end 0x0d
#define dbf_file_end 0x1a
#define dbf_version 0x03

/*
 * Clears the file handle so failed calls leave no stale values.
 */
static void dbf_reset(dbf_file *file)
{
    file->fd = -1;
    file->field_count = 0;
    file->header_length = 0;
    file->record_length = 0;
    file->record_count = 0;
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
 * Stores an unsigned short into the little-endian DBF header.
 */
static void store_u16(unsigned char *buffer, unsigned short value)
{
    buffer[0] = (unsigned char)(value & 0xff);
    buffer[1] = (unsigned char)((value >> 8) & 0xff);
}

/*
 * Stores an unsigned long into the little-endian DBF header.
 */
static void store_u32(unsigned char *buffer, unsigned long value)
{
    buffer[0] = (unsigned char)(value & 0xff);
    buffer[1] = (unsigned char)((value >> 8) & 0xff);
    buffer[2] = (unsigned char)((value >> 16) & 0xff);
    buffer[3] = (unsigned char)((value >> 24) & 0xff);
}

/*
 * Loads an unsigned short from the little-endian DBF header.
 */
static unsigned short load_u16(const unsigned char *buffer)
{
    return (unsigned short)buffer[0]
        | (unsigned short)((unsigned short)buffer[1] << 8);
}

/*
 * Loads an unsigned long from the little-endian DBF header.
 */
static unsigned long load_u32(const unsigned char *buffer)
{
    return (unsigned long)buffer[0]
        | ((unsigned long)buffer[1] << 8)
        | ((unsigned long)buffer[2] << 16)
        | ((unsigned long)buffer[3] << 24);
}

/*
 * Checks whether the basic DBF header values are safe to use.
 */
static int header_is_valid(unsigned long file_size,
    unsigned short header_length, unsigned short file_record_length)
{
    if (header_length < 33) {
        return 0;
    }

    if (file_record_length == 0) {
        return 0;
    }

    if (header_length > file_size) {
        return 0;
    }

    return 1;
}

/*
 * Calculates the payload size of one record from the field list.
 */
static unsigned short record_length_for(const dbf_field *fields,
    unsigned short field_count)
{
    unsigned short index;
    unsigned short length;

    length = 0;
    for (index = 0; index < field_count; index++) {
        length = (unsigned short)(length + fields[index].length);
    }

    return length;
}

/*
 * Copies the field name into the fixed 11-byte descriptor slot.
 */
static void copy_name(unsigned char *target, const char *name)
{
    unsigned short index;

    for (index = 0; index < 11; index++) {
        if (name[index] == '\0') {
            break;
        }
        target[index] = (unsigned char)name[index];
    }
}

/*
 * Rewrites the live record count in the DBF header.
 */
static int sync_record_count(const dbf_file *file)
{
    unsigned char count[4];

    if (lseek(file->fd, 4L, SEEK_SET) < 0) {
        return -1;
    }

    store_u32(count, file->record_count);
    return write_exact(file->fd, count, 4);
}

/*
 * Seeks to the flag byte of one record by zero-based index.
 */
static int seek_record(const dbf_file *file, unsigned long record_index)
{
    unsigned long offset;

    if (file->fd < 0 || record_index >= file->record_count) {
        return -1;
    }

    offset = (unsigned long)file->header_length
        + (record_index * (unsigned long)(file->record_length + 1));
    if (lseek(file->fd, (long)offset, SEEK_SET) < 0) {
        return -1;
    }

    return 0;
}

int dbf_create(dbf_file *file, const char *path,
    const dbf_field *fields, unsigned short field_count)
{
    unsigned char header[dbf_header_size];
    unsigned char field[dbf_field_size];
    unsigned char mark;
    unsigned short index;
    unsigned short field_index;
    unsigned short payload_length;
    unsigned short file_record_length;
    unsigned short header_length;
    int fd;
    int flags;

    dbf_reset(file);
    payload_length = record_length_for(fields, field_count);
    file_record_length = (unsigned short)(payload_length + 1);
    header_length = (unsigned short)(dbf_header_size
        + (field_count * dbf_field_size) + 1);

    flags = O_CREAT | O_TRUNC | O_RDWR;
    fd = open(path, flags, 0644);
    if (fd < 0) {
        return -1;
    }

    for (index = 0; index < dbf_header_size; index++) {
        header[index] = 0;
    }

    header[0] = dbf_version;
    store_u32(header + 4, 0);
    store_u16(header + 8, header_length);
    store_u16(header + 10, file_record_length);

    if (write_exact(fd, header, dbf_header_size) != 0) {
        close(fd);
        return -1;
    }

    for (index = 0; index < field_count; index++) {
        for (field_index = 0; field_index < dbf_field_size;
            field_index++) {
            field[field_index] = 0;
        }

        copy_name(field, fields[index].name);
        field[11] = (unsigned char)fields[index].type;
        field[16] = fields[index].length;
        field[17] = fields[index].decimals;

        if (write_exact(fd, field, dbf_field_size) != 0) {
            close(fd);
            return -1;
        }
    }

    mark = dbf_header_end;
    if (write_exact(fd, &mark, 1) != 0) {
        close(fd);
        return -1;
    }

    mark = dbf_file_end;
    if (write_exact(fd, &mark, 1) != 0) {
        close(fd);
        return -1;
    }

    file->fd = fd;
    file->field_count = field_count;
    file->header_length = header_length;
    file->record_length = payload_length;
    file->record_count = 0;
    return 0;
}

int dbf_open(dbf_file *file, const char *path)
{
    unsigned char header[dbf_header_size];
    unsigned short file_record_length;
    unsigned short header_length;
    long file_size;
    int fd;

    dbf_reset(file);
    fd = open(path, O_RDWR);
    if (fd < 0) {
        return -1;
    }

    if (read_exact(fd, header, dbf_header_size) != 0) {
        close(fd);
        return -1;
    }

    file_record_length = load_u16(header + 10);
    header_length = load_u16(header + 8);
    file_size = lseek(fd, 0L, SEEK_END);
    if (file_size < 0) {
        close(fd);
        return -1;
    }

    if (!header_is_valid((unsigned long)file_size, header_length,
        file_record_length)) {
        close(fd);
        return -1;
    }

    file->fd = fd;
    file->field_count = (unsigned short)
        ((header_length - 33) / dbf_field_size);
    file->header_length = header_length;
    file->record_length = (unsigned short)(file_record_length - 1);
    file->record_count = load_u32(header + 4);
    return 0;
}

int dbf_close(dbf_file *file)
{
    int result;

    if (file->fd < 0) {
        return 0;
    }

    result = close(file->fd);
    dbf_reset(file);
    if (result != 0) {
        return -1;
    }

    return 0;
}

int dbf_append(dbf_file *file, const char *data)
{
    unsigned char live;
    unsigned char eof_mark;
    long end;

    if (file->fd < 0) {
        return -1;
    }

    end = lseek(file->fd, 0L, SEEK_END);
    if (end < 0) {
        return -1;
    }

    if (end > (long)file->header_length) {
        if (lseek(file->fd, end - 1L, SEEK_SET) < 0) {
            return -1;
        }
    }

    live = ' ';
    if (write_exact(file->fd, &live, 1) != 0) {
        return -1;
    }

    if (write_exact(file->fd, (const unsigned char *)data,
        file->record_length) != 0) {
        return -1;
    }

    eof_mark = dbf_file_end;
    if (write_exact(file->fd, &eof_mark, 1) != 0) {
        return -1;
    }

    file->record_count++;
    return sync_record_count(file);
}

int dbf_read(dbf_file *file, unsigned long record_index, char *data)
{
    unsigned char state;

    if (seek_record(file, record_index) != 0) {
        return -1;
    }

    if (read_exact(file->fd, &state, 1) != 0) {
        return -1;
    }

    if (read_exact(file->fd, (unsigned char *)data,
        file->record_length) != 0) {
        return -1;
    }

    if (state == '*') {
        return 1;
    }

    return 0;
}

int dbf_write(dbf_file *file, unsigned long record_index,
    const char *data)
{
    unsigned char state;

    if (seek_record(file, record_index) != 0) {
        return -1;
    }

    if (read_exact(file->fd, &state, 1) != 0) {
        return -1;
    }

    if (state == '*') {
        state = ' ';
    }

    if (seek_record(file, record_index) != 0) {
        return -1;
    }

    if (write_exact(file->fd, &state, 1) != 0) {
        return -1;
    }

    if (write_exact(file->fd, (const unsigned char *)data,
        file->record_length) != 0) {
        return -1;
    }

    return 0;
}

int dbf_delete(dbf_file *file, unsigned long record_index)
{
    unsigned char deleted;

    if (seek_record(file, record_index) != 0) {
        return -1;
    }

    deleted = '*';
    return write_exact(file->fd, &deleted, 1);
}
