# libndx

`libndx` is the small dBase III compatible index library used by the
project. It builds standalone `.ndx` files from DBF tables and keeps
the API low-level so the SQL shell can use it without dragging in a
large dependency surface. Hosted SQL currently uses `libndx` for
`CREATE INDEX`, full index rebuilds after row changes, and optimizer
catalog-driven index discovery.

Public header:

- `include/ndx.h`

Implementation:

- `lib/ndx/ndx.c`

## Public API

The library exports these functions:

- `ndx_create()`
- `ndx_open()`
- `ndx_close()`
- `ndx_find_text()`
- `ndx_find_number()`
- `ndx_find_date()`
- `ndx_walk()`

`ndx_find_number()` now accepts decimal text rather than host numeric
types, so the public API no longer depends on `double`.

The central data structure is:

- `ndx_file`
  Holds one open file handle and the loaded NDX header values

`ndx_create()` builds a full B-tree index from an existing DBF table.
The current library supports:

- one `C`, `N`, or `D` DBF field
- many DBF fields when every indexed field is `C`
- optional `UNIQUE` behavior that keeps only one record per key

## How It Works

- `ndx_create()` reads the DBF table, extracts index keys, sorts them,
  and writes a dBase III style 512-byte-page B-tree
- `ndx_open()` loads the NDX header and validates the page geometry
- `ndx_find_text()`, `ndx_find_number()`, and `ndx_find_date()` do
  exact-match lookups through the B-tree
- `ndx_walk()` traverses all leaf entries in ascending key order and
  reports DBF record numbers
- the automated tests also open and walk the bundled real-world NDX
  samples under `tests/data/db/`

The library does not currently provide:

- SQL planner integration for `SELECT`
- in-place incremental insert, update, or delete maintenance
- arbitrary dBase expression parsing
- descending indexes

The SQL layer handles index registration in `<root>/sys/ndx.dbf` and
currently chooses the simplest maintenance strategy: rebuild every
registered index for a table after `INSERT`, `UPDATE`, or `DELETE`.

## Example: Build One Character Index

```c
#include "dbf.h"
#include "ndx.h"

dbf_field fields[2];
dbf_file table;
ndx_file index;
unsigned short key_fields[2];

if (dbf_open(&table, "people.dbf") != 0) {
    return 1;
}

if (dbf_read_fields(&table, fields, 2) != 0) {
    dbf_close(&table);
    return 1;
}

key_fields[0] = 0; /* name */

if (ndx_create(&index, "people_name.ndx", &table, fields, 2,
    key_fields, 1, 0) != 0) {
    dbf_close(&table);
    return 1;
}

ndx_close(&index);
dbf_close(&table);
```

## Example: Build One Composite Character Index

```c
#include "dbf.h"
#include "ndx.h"

unsigned short key_fields[2];

key_fields[0] = 1; /* city */
key_fields[1] = 0; /* name */

if (ndx_create(&index, "people_city_name.ndx", &table, fields, 2,
    key_fields, 2, 0) != 0) {
    return 1;
}
```

Composite indexes concatenate the raw DBF field bytes in field order.

## Example: Exact Lookup

```c
#include "ndx.h"

ndx_file index;
unsigned long record_number;

if (ndx_open(&index, "people_name.ndx") != 0) {
    return 1;
}

if (ndx_find_text(&index, "alice", &record_number) != 0) {
    ndx_close(&index);
    return 1;
}

ndx_close(&index);
```

## Example: Exact Numeric Lookup

```c
#include "ndx.h"

ndx_file index;
unsigned long record_number;

if (ndx_open(&index, "people_age.ndx") != 0) {
    return 1;
}

if (ndx_find_number(&index, "18", &record_number) != 0) {
    ndx_close(&index);
    return 1;
}

ndx_close(&index);
```

## Notes

- Character keys are stored as fixed-width ASCII bytes padded with
  spaces on the right.
- Numeric and date keys are stored in the dBase III 8-byte IEEE
  binary64 representation used by NDX files.
- The builder skips deleted DBF records.
- `ndx_walk()` visits records in index order, which is useful for
  verification and future SQL integration.
- Numeric keys are packed from DBF decimal text with a small software
  codec, and date keys are packed from DBF `YYYYMMDD` text through
  exact Julian day conversion. The library does not depend on host
  floating-point width for those paths.
