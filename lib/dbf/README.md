# libdbf

`libdbf` is the small DBF storage library used by the project. It
targets dBase III style files with fixed-length records and keeps the
API low-level on purpose so it remains small and portable.

Public header:

- `include/dbf.h`

Implementation:

- `lib/dbf/dbf.c`

## Public API

The library exports these functions:

- `dbf_create()`
- `dbf_open()`
- `dbf_close()`
- `dbf_append()`
- `dbf_read()`
- `dbf_write()`
- `dbf_delete()`

The central data structures are:

- `dbf_field`
  Describes one field in a DBF table
- `dbf_file`
  Holds one open file handle and the loaded header values

Records are passed as raw fixed-length byte arrays. The library does
not decode field values into C types for you.

## How It Works

- `dbf_create()` writes a dBase III header and field descriptors
- `dbf_open()` reads the basic header values into `dbf_file`
- `dbf_append()` adds one live record to the end of the file
- `dbf_read()` reads one record by zero-based index
- `dbf_write()` replaces one existing record in place
- `dbf_delete()` marks one record as deleted

The library does not currently provide:

- memo file support
- field-name lookup
- typed field decoding
- record packing

## Example: Create A DBF File

```c
#include "dbf.h"

dbf_field fields[2];
dbf_file file;

strcpy(fields[0].name, "name");
fields[0].type = 'C';
fields[0].length = 8;
fields[0].decimals = 0;

strcpy(fields[1].name, "id");
fields[1].type = 'N';
fields[1].length = 3;
fields[1].decimals = 0;

if (dbf_create(&file, "people.dbf", fields, 2) != 0) {
    return 1;
}

dbf_close(&file);
```

## Example: Read All Records

```c
#include "dbf.h"

dbf_file file;
char record[64];
unsigned long index;
int state;

if (dbf_open(&file, "people.dbf") != 0) {
    return 1;
}

for (index = 0; index < file.record_count; index++) {
    state = dbf_read(&file, index, record);
    if (state < 0) {
        dbf_close(&file);
        return 1;
    }

    if (state == 1) {
        continue;
    }

    /*
     * record now contains file.record_length bytes.
     * For a NAME(8) + ID(3) layout:
     * bytes 0..7 are the name
     * bytes 8..10 are the id
     */
}

dbf_close(&file);
```

## Example: Append One Record

```c
#include "dbf.h"

dbf_file file;
char record[11];

if (dbf_open(&file, "people.dbf") != 0) {
    return 1;
}

memcpy(record, "alice   001", 11);

if (dbf_append(&file, record) != 0) {
    dbf_close(&file);
    return 1;
}

dbf_close(&file);
```

`dbf_append()` always adds the new record at the end of the file.

## Example: Update One Record

```c
#include "dbf.h"

dbf_file file;
char record[11];

if (dbf_open(&file, "people.dbf") != 0) {
    return 1;
}

memcpy(record, "carol   003", 11);

if (dbf_write(&file, 1, record) != 0) {
    dbf_close(&file);
    return 1;
}

dbf_close(&file);
```

`dbf_write()` overwrites the record in place.

## Example: Delete One Record

```c
#include "dbf.h"

dbf_file file;

if (dbf_open(&file, "people.dbf") != 0) {
    return 1;
}

if (dbf_delete(&file, 3) != 0) {
    dbf_close(&file);
    return 1;
}

dbf_close(&file);
```

This marks the record as deleted by writing `'*'` into the DBF delete
flag byte. It does not pack the file.

## Notes

- The buffer for `dbf_append()`, `dbf_read()`, and `dbf_write()` must
  match `file.record_length`.
- Deleted records remain in the file until a later pack operation.
- The library does not handle memo files.
- Fields use classic DBF type codes such as `C`, `N`, `D`, and `L`.
