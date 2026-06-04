# Transaction Implementation Notes

Describes what was built, where it lives, and every design decision
made during the implementation of BEGIN / COMMIT / ROLLBACK.

---

## 1. Strategy: In-Memory Log

Every DML operation inside an open transaction appends one `txn_entry`
node to a heap-allocated singly-linked list.  Each node carries the
full new-record payload.  **No files are written until COMMIT.**

This makes ROLLBACK free: just walk the list, `free` every node, and
clear the active flag.  Read overlay (making pending changes visible to
SELECT) is a linear scan of the in-memory list — no disk I/O, no file
opens.

---

## 2. Data Structures (`lib/tran/tran.h`, `include/sqlctx.h`)

### txn_entry — one DML operation

```c
typedef struct txn_entry {
    txn_op          op;             /* insert / update / delete */
    char            table[17];      /* target table name */
    unsigned long   record_no;      /* 0-based DBF index (0 for INSERT) */
    unsigned short  old_crc;        /* CRC-16 of original record (U/D only) */
    char           *new_record;     /* malloc'd full record (I/U); NULL for D */
    unsigned short  record_length;
    struct txn_entry *next;
} txn_entry;
```

Memory per entry ≈ `sizeof(txn_entry)` (~26 bytes on Z80) plus one
`malloc(record_length)` for INSERT and UPDATE.

### txn_stmt — one SQL text for replay

```c
typedef struct txn_stmt {
    char            *text;          /* strdup'd original SQL */
    struct txn_stmt *next;
} txn_stmt;
```

### sql_txn — embedded in sql_context

```c
typedef struct sql_txn {
    unsigned char     active;
    struct txn_entry *entries;
    struct txn_stmt  *stmts;
} sql_txn;
```

Adds 5 bytes to `sql_context`.  Zero cost when no transaction is open.

---

## 3. Write Side (`lib/tran/tran.c`)

Called from `execute_mutate.c` **instead of** the raw DBF functions
when `env->txn->active` is set.

### txn_insert

Allocates a `txn_entry` with `op = txn_op_insert`, copies `record` into
`entry->new_record`, and appends to the list.  The real table file is
not touched.

### txn_update

Computes `old_crc = crc16(old_record, record_length)` for the
precondition check.  Allocates a `txn_entry` with `op = txn_op_update`,
copies `new_record`, and appends.

### txn_delete

Computes `old_crc`.  Allocates a `txn_entry` with `op = txn_op_delete`
(`new_record = NULL`).  No record data is stored — the delete operation
only needs the position and precondition snapshot.

All three functions have simple signatures (no `fields`/`offsets`
parameters needed):

```c
int txn_insert(env, table_name, record, record_length);
int txn_update(env, table_name, record_no, old_record, new_record, record_length);
int txn_delete(env, table_name, record_no, old_record, record_length);
```

---

## 4. Read Side (`lib/tran/tran.c`)

### txn_overlay_record

Called after every `dbf_read` in the scan loops of `execute_select.c`
and `execute_mutate.c`.

```
for each entry in txn->entries:
    skip if entry.table ≠ table_name
    skip INSERT entries (they don't shadow physical rows)
    if record_no matches:
        DELETE → return 1 (caller skips this row)
        UPDATE → memcpy entry.new_record → record; return 0
return 0 (use physical record as-is)
```

Cost: O(pending_entries) with no I/O.  For typical small transactions
(< 32 pending rows) this is a handful of comparisons per physical row.

### txn_scan_inserts

Called after the physical scan loop for single-source non-grouped
SELECTs.

```
for each entry in txn->entries:
    skip if op ≠ INSERT or table ≠ table_name
    call cb(virtual_recno, entry.new_record, entry.record_length, ctx)
```

The callback in `execute_select.c` applies the same WHERE filter and
projection as the physical scan rows.

---

## 5. Commit Protocol

### Phase 1 — Precondition check (no writes)

Walk `txn->entries`.  For each UPDATE or DELETE:
1. Open real table, `dbf_read(entry.record_no, buf)`, close.
2. `crc16(buf, file.record_length)` must equal `entry.old_crc`.
3. Any mismatch → return -1 immediately, no file touched.
   The transaction stays open; the caller should ROLLBACK and retry.

INSERT entries have no precondition.

### Phase 2 — Application

For each distinct table touched, open the real file and replay every
entry for that table in log order:

| op | action |
|----|--------|
| DELETE | `dbf_delete(file, record_no)` |
| UPDATE | `dbf_write(file, record_no, entry.new_record)` |
| INSERT | `dbf_append(file, entry.new_record)` |

Rebuild indexes after all entries for each table are applied.

### Phase 3 — Cleanup

`free` every `txn_entry` (and its `new_record`), every `txn_stmt`, set
`active = 0`.  No file I/O.

### ROLLBACK

Identical to Phase 3 only.  Extremely cheap.

---

## 6. Statement Recording

After each successful DML dispatch inside an active transaction,
`execute.c` calls `txn_record_stmt(env->txn, env->source_text)`.
This appends a `txn_stmt` node containing a `strdup`'d copy of the
original SQL.

The `stmts` list is available for conflict replay: on COMMIT failure
the caller can ROLLBACK, re-issue the same SQL statements, and COMMIT
again against the current data state.  Automatic replay is not yet
implemented in `exec_commit` but the data is in place.

---

## 7. Changes to Existing Files

| File | Change |
|------|--------|
| `include/sqlexec.h` | Added `sqlexec_begin`, `sqlexec_commit`, `sqlexec_rollback` opcodes |
| `include/sql.h` | Added `sql_statement_begin/commit/rollback` types |
| `include/sqlctx.h` | Embedded `sql_txn txn` + forward decls for `txn_entry`/`txn_stmt` |
| `lib/common/common.h/.c` | Added `crc16(data, length)` |
| `lib/sqlexec/exec_impl.h` | Added `txn`, `source_text`, `ctx` to `sqlexec_env`; includes `sqlctx.h` |
| `lib/sqlexec/execute.c` | Routes begin/commit/rollback to `exec_txn`; populates new env fields; records successful DML |
| `lib/sqlexec/execute_ddl.c` | Returns -1 when `txn->active` |
| `lib/sqlexec/execute_mutate.c` | INSERT/UPDATE/DELETE dispatch to txn_* when active; UPDATE saves original record before patching; skips `dbf_close`/rebuild when in shadow mode |
| `lib/sqlexec/execute_select.c` | All three scan loops call `txn_overlay_record`; single-source plain scan calls `txn_scan_inserts` after physical rows |
| `lib/sql/sql.c` | `BEGIN`/`COMMIT`/`ROLLBACK` keywords + `parse_transaction` |
| `lib/sql/program.c` | Three new `lower_named_root` cases |

---

## 8. Limitations

| Limitation | Reason |
|------------|--------|
| No nested transactions | `exec_begin` returns -1 if already active |
| No DDL inside a transaction | Schema change would invalidate pending records |
| Index scans bypass read overlay | Callbacks in `execute_scan.c` don't call `txn_overlay_record`; full-scan paths are correct |
| No automatic conflict replay | `stmts` list is ready; the replay loop is not yet wired |
| Memory grows with pending DML | Each INSERT/UPDATE holds a full record copy; acceptable for the CP/M target with small tables |
