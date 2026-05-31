# DBF Reference

`libdbf` is a minimal dBase III reader/writer. It targets the Z80/CP/M
memory budget: no heap, raw file-descriptor I/O, and explicit
little-endian encoding.

## File format

```
Offset  Size  Content
------  ----  -------
0       1     Version byte (0x03 = dBase III)
1       3     Last-update date (unused on write, not validated on read)
4       4     Record count (little-endian uint32)
8       2     Header length in bytes (little-endian uint16)
10      2     Record length including the deletion flag (little-endian uint16)
12      20    Reserved / unused
32      32*n  Field descriptors (one per column)
32+32*n 1     Header terminator (0x0D)
```

Each 32-byte field descriptor:

```
Offset  Size  Content
------  ----  -------
0       11    Field name, null-padded
11      1     Field type ('C', 'N', 'D', 'L')
12      4     Reserved
16      1     Field length in bytes
17      1     Decimal count
18      14    Reserved
```

Each record on disk:

```
1 byte  Deletion flag: ' ' (0x20) = live, '*' (0x2A) = deleted
n bytes Field data (no separators; fields are fixed-width and concatenated)
```

The file ends with an EOF marker byte (0x1A) after the last record.

## API

```c
int dbf_create(dbf_file *file, const char *path,
    const dbf_field *fields, unsigned short field_count);
```
Creates a new DBF file, writes the header and field descriptors, and
leaves the file open for subsequent reads and appends.

```c
int dbf_open(dbf_file *file, const char *path);
```
Opens an existing DBF file and reads the header. Field descriptors are
not loaded; call `dbf_read_fields` separately.

```c
int dbf_read_fields(dbf_file *file, dbf_field *fields,
    unsigned short field_count);
```
Reads all field descriptors from an open file into a caller-supplied
array. The array must hold at least `file->field_count` entries.

```c
int dbf_close(dbf_file *file);
```
Closes the file descriptor and resets the handle to a safe state.

```c
int dbf_append(dbf_file *file, const char *data);
```
Appends one live record. `data` must be exactly `file->record_length`
bytes. The header record count is updated on disk after each append.

```c
int dbf_read(dbf_file *file, unsigned long record_index, char *data);
```
Reads one record by zero-based index. Returns `0` for a live record,
`1` for a deleted record, `-1` on I/O failure. The caller buffer must
hold at least `file->record_length` bytes.

```c
int dbf_write(dbf_file *file, unsigned long record_index,
    const char *data);
```
Replaces a record in place. If the record was previously deleted, the
deletion flag is cleared (the record becomes live again).

```c
int dbf_delete(dbf_file *file, unsigned long record_index);
```
Marks a record deleted by writing `*` over the deletion flag byte.
The field data is left intact. Space is never reclaimed by this library.

All `record_index` parameters are **zero-based**. NDX files store
**1-based** record numbers; callers must subtract 1 before passing to
these functions.

## Design constraints

- **No dynamic allocation.** All buffers are stack-local. The library
  is safe to use in environments with no heap.
- **No stdio.** All I/O uses raw file descriptors (`open`, `read`,
  `write`, `lseek`, `close`). On CP/M these are declared as externs;
  on Linux they come from `<fcntl.h>` and `<unistd.h>`.
- **Explicit little-endian codec.** `store_u16`/`load_u16` and
  `store_u32`/`load_u32` hand-pack multi-byte header fields. No struct
  casts against the wire format.
- **Partial-I/O safety.** `write_exact` and `read_exact` retry until
  the full buffer is transferred or an error is returned.
- **Soft deletes only.** `dbf_delete` sets the flag byte. No compaction
  or space reclaim is provided; that is left to external tooling.
- **Supported field types.** The library stores and retrieves field
  bytes without interpreting them. Type validation is the caller's job.
  The shell uses `C`, `N`, `D`, and `L`.
