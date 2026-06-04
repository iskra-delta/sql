# NDX Reference

`libndx` builds and reads dBase III style `.ndx` B-tree index files.
Like `libdbf`, it targets the Z80/CP/M memory budget: no heap, raw
file-descriptor I/O, and explicit little-endian encoding.

The SQL shell uses this library for `CREATE INDEX`, for keeping
registered indexes current after `INSERT`, `UPDATE`, and `DELETE`, and
for index-driven `SELECT`, `UPDATE`, and `DELETE` execution.

## File Format

An NDX file is divided into fixed 512-byte pages.

- Page 0 is the file header.
- Pages 1..n are B-tree pages.
- Byte offset of page *p* is always `p * 512`.

### Header page

```
Offset  Size  Content
------  ----  -------
0       4     Root page number (little-endian uint32)
4       4     Total pages / next free page (little-endian uint32)
8       4     Reserved
12      2     Key length in bytes
14      2     Maximum keys per page
16      2     Key type (0 = character, 1 = numeric / date)
18      2     Key record size (child + record pointers + key, padded to 4)
20      2     Reserved
22      2     Unique flag (0 = off, non-zero = on)
24      488   ASCIIZ key expression text
```

### B-tree page

```
Offset  Size  Content
------  ----  -------
0       2     Entry count
2       2     Unused / padding
4       ...   Key records (count x key_record_size)
              + one trailing 4-byte rightmost child page pointer
```

Each key record:

```
Offset  Size  Content
------  ----  -------
0       4     Left child page number (0 in a leaf page)
4       4     DBF record number, 1-based (0 in a branch page)
8       n     Key bytes, padded to 4-byte boundary
```

### Key types

- **Character**: fixed-width bytes, space-padded to `key_length`.
- **Numeric / date**: 8-byte dBase III binary64 encoding.

Numeric keys are packed from DBF decimal text with a software codec
independent of host floating-point width — same binary64 format from
both GCC and SDCC. Date keys convert `YYYYMMDD` text to Julian day
numbers before packing.

### B-tree navigation

A separator key represents the maximum key in its left child:
- If `search_key <= separator_key`, follow the separator's left child.
- If no separator matches, follow the final rightmost child.
- In a leaf page, a matching entry is the actual DBF record.

**Important:** DBF record numbers in the index are **1-based**. All
DBF I/O functions (`dbf_read`, `dbf_write`, `dbf_delete`) take
**0-based** indices. Subtract 1 when using NDX-sourced record numbers.

---

## Builder Strategy

`ndx_create` builds in three stages:

1. Read every live DBF record and extract key bytes.
2. Sort keys ascending; drop duplicates when `UNIQUE` is set.
3. Write leaf pages first, then build upper levels until one root
   page remains.

The result is a standard dBase III page layout.

---

## API

```c
int ndx_create(ndx_file *file, const char *path,
    dbf_file *table,
    const dbf_field *fields, unsigned short field_count,
    const unsigned short *key_fields, unsigned short key_field_count,
    unsigned char unique);
```
Build one `.ndx` from an open DBF table. Supports one `C`, `N`, or `D`
field, or multiple `C` fields concatenated in DBF byte order.

```c
int ndx_open(ndx_file *file, const char *path);
int ndx_close(ndx_file *file);
```
Open and close an existing NDX file.

```c
int ndx_find_text(ndx_file *file, const char *key,
    unsigned long *record_number);
int ndx_find_number(ndx_file *file, const char *value_text,
    unsigned long *record_number);
int ndx_find_date(ndx_file *file, const char *yyyymmdd,
    unsigned long *record_number);
```
Find the first exact match. Returns 0 found, 1 not found, -1 error.
`record_number` is 1-based.

```c
typedef int (*ndx_visit_fn)(unsigned long record_number);
int ndx_walk(ndx_file *file, ndx_visit_fn visit);
```
Visit every leaf in ascending key order. Returning non-zero stops the
walk. Returns 0 full walk, 1 stopped, -1 error.

```c
typedef int (*ndx_scan_fn)(unsigned long record_number,
    const unsigned char *key, unsigned short key_length, void *ctx);
int ndx_scan(ndx_file *file, ndx_scan_fn visit, void *ctx);
```
Like `ndx_walk` but passes raw key bytes and a caller context pointer.
Used by the executor for equality probes and range walks where the
callback must inspect the key to know when to stop.

```c
int ndx_encode_text_key(unsigned char *key_out,
    unsigned short key_length, const char *text);
int ndx_encode_number_key(unsigned char *key_out,
    const char *value_text);
```
Build a raw NDX key from a SQL value string. `key_out` must be at
least `ndx_max_key_length` (100) bytes.

```c
int ndx_compare_key(const ndx_file *file,
    const unsigned char *left, const unsigned char *right);
```
Compare two raw key arrays using the file's key type.
Returns negative, zero, or positive.

---

## SQL Integration

Index metadata lives in `sys/ndx.dbf`:

| Field | Content |
|---|---|
| `db_name` | database name |
| `name` | index name |
| `table_name` | table the index belongs to |
| `key_fields` | comma-separated field names |
| `unique` | `Y` or `N` |

**`CREATE [UNIQUE] INDEX`** — validates, builds `.ndx`, appends catalog
row.

**`INSERT`, `UPDATE`, `DELETE`** — rebuild every registered index for
the affected table.

**`DROP TABLE`** — removes `.ndx` files and catalog rows.

**Optimizer** — reads `sys/ndx.dbf` and annotates `table_scan` with
index equality or range access for single-field top-level conjuncts.
When that access fully enforces a top-level predicate, the optimizer
prunes it from the residual `WHERE` tree.

**Executor** — drives `ndx_scan` from the `table_scan` access mode:
- equality access stops when key passes the target
- range access skips below the lower bound and stops past the upper
  bound

---

## References

- https://pic.hallikainen.org/techref/language/dbase/ndxs.htm
