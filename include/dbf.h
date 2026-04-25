/*
 * Declares a tiny DBF reader and writer for fixed-layout tables.
 * The code targets dBase III style files with no memo support and
 * uses file-descriptor I/O so records move in single block reads.
 *
 * MIT License (see: LICENSE)
 * Copyright (C) 2026 tomaz stih
 */

#ifndef dbf_h
#define dbf_h

typedef struct dbf_field {
    char name[12];
    char type;
    unsigned char length;
    unsigned char decimals;
} dbf_field;

typedef struct dbf_file {
    int fd;
    unsigned short field_count;
    unsigned short header_length;
    unsigned short record_length;
    unsigned long record_count;
} dbf_file;

/*
 * Creates a new DBF file with the supplied field layout.
 * Only dBase III style fixed-length fields are supported.
 * Returns zero on success and -1 on failure.
 */
int dbf_create(dbf_file *file, const char *path,
    const dbf_field *fields, unsigned short field_count);

/*
 * Opens an existing DBF file and loads its basic header values.
 * The function does not parse field descriptors beyond the count.
 * Returns zero on success and -1 on failure.
 */
int dbf_open(dbf_file *file, const char *path);

/*
 * Closes an open DBF file descriptor.
 * Returns zero on success and -1 on failure.
 */
int dbf_close(dbf_file *file);

/*
 * Appends one active record to the end of the DBF file.
 * The data buffer must contain exactly record_length bytes.
 * Returns zero on success and -1 on failure.
 */
int dbf_append(dbf_file *file, const char *data);

/*
 * Reads one record by zero-based index into the caller buffer.
 * The buffer must hold exactly record_length bytes.
 * Returns zero for a live record, one for a deleted record,
 * and -1 on failure.
 */
int dbf_read(dbf_file *file, unsigned long record_index, char *data);

/*
 * Replaces one record by zero-based index with caller data.
 * The data buffer must contain exactly record_length bytes.
 * Returns zero on success and -1 on failure.
 */
int dbf_write(dbf_file *file, unsigned long record_index,
    const char *data);

/*
 * Marks one record as deleted by zero-based index.
 * The payload bytes remain in place and only the DBF flag changes.
 * Returns zero on success and -1 on failure.
 */
int dbf_delete(dbf_file *file, unsigned long record_index);

#endif
