# libdbf

`libdbf` is a very small DBF library aimed at low-memory systems.
It currently supports fixed-length dBase III style records and keeps
the API small on purpose.

Public header:

- `include/dbf.h`

Implementation:

- `lib/dbf/dbf.c`

## API

- `dbf_create()`
- `dbf_open()`
- `dbf_close()`
- `dbf_append()`
- `dbf_read()`
- `dbf_write()`
- `dbf_delete()`

Records are passed as raw fixed-length byte arrays. The library does
not parse field values for you.

## 1. Create a DBF file with fields

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

## 2. Read all records from a DBF file

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

## 3. Delete a record from a DBF file

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

## 4. Insert data into a DBF file

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

## 5. Change a record in a DBF file

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

## Notes

- The buffer for `dbf_append()`, `dbf_read()`, and `dbf_write()` must
  match `file.record_length`.
- Deleted records remain in the file until a later pack operation.
- The library does not handle memo files.
