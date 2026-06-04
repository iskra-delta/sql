# Architecture

A tiny SQL shell for Z80 / CP/M machines with 128 KB banked memory,
using dBase III DBF files for storage and NDX B-tree files for indexes.
The same codebase runs on Linux under GCC for development and testing.

---

## 1. Big Picture

```
User types SQL
     │
     ▼
 ┌──────────┐     sql_context     ┌───────────┐     sql_context     ┌───────────┐
 │ sql_run  │ ──────────────────► │sqlopt_run │ ──────────────────► │sqlexec_run│
 │ (parse)  │                     │ (optimize)│                     │ (execute) │
 └──────────┘                     └───────────┘                     └───────────┘
      │                                │                                  │
  sql_statement                sqlexec_program                      DBF / NDX
  (stack, parse time)          (rewritten in place)                 storage
```

All three phases share one `sql_context` struct (`include/sqlctx.h`).
The context is allocated once for the life of the session.  On CP/M
each phase is compiled to a separate `.COM` overlay; the dispatcher
loads, calls, and unloads one overlay at a time.

---

## 2. Source Tree

```
include/               Public headers (used by more than one library)
  sqltypes.h           Shared value/schema types, compile-time limits
  sqlexec.h            IR node arena API + opcodes
  sql.h                Parser public API (sql_statement type, sql_parse)
  sqlopt.h             Optimizer public API (sqlopt_run)
  sqlctx.h             sql_context + phase entry points + sql_txn
  tran.h               Transaction log types (txn_entry, txn_stmt)
  ndx.h                NDX index API

lib/
  dbf/                 dBase III file reader/writer (no heap)
  ndx/                 dBase III B-tree index library (no heap)
  common/              Phase-neutral utilities (strings, fields, CRC-16)
  catalog/             Catalog and table-file access helpers
  sql/
    sql.c              Recursive-descent parser + view expansion
    program.c          IR builder (lowers sql_statement → sqlexec_program)
  sqlopt/
    optimize.c         Catalog-driven access-path rewriter
  sqlexec/
    sqlexec.c          Node arena management (alloc / free / validate)
    execute.c          Top-level dispatcher (routes by root opcode)
    execute_ddl.c      DDL executors (CREATE/DROP/USE/SHOW)
    execute_select.c   SELECT executor (scan, filter, project, aggregate)
    execute_mutate.c   INSERT / UPDATE / DELETE executors
    execute_scan.c     Shared NDX scan callbacks (SELECT + mutate)
    execute_subquery.c FROM-subquery and built-in view materialisation
    execute_sysviews.c sys_* view generators
    project.c          Column binding + temp-table output helpers
    where.c            WHERE evaluation + row-source field resolution
    metacache.c        In-session schema cache (USE → load, DDL → reload)
  tran/
    tran.c             Transaction log + BEGIN/COMMIT/ROLLBACK
    tran.h             Transaction types and API

src/
  main.c               Interactive shell loop + phase orchestration
  platform.c           Terminal I/O (raw termios on Linux, BDOS on CP/M)

tests/                 GCC-hosted unit and integration tests
docs/                  Design documents
```

---

## 3. Communication Contract

```c
typedef struct sql_context {
    const char       *root;          /* storage root path */
    char              current_db[17];/* active database name */
    const char       *text;          /* SQL input, set before parse */
    sqlexec_program   program;       /* IR: written by parse, rewritten by opt */
    sqlexec_io        io;            /* write_char output callback */
    int               result;        /* last phase result */
    struct meta_cache *schema;       /* NULL until USE; rebuilt on DDL */
    sql_txn           txn;           /* transaction state */
} sql_context;
```

`sql_txn` is three fields (active flag + two NULL list pointers) and
adds five bytes to the context.  No memory is consumed until `BEGIN`
opens a transaction.

---

## 4. The Three Pipeline Phases

### 4.1 Parse — `sql_run`

**Source:** `lib/sql/sql.c` + `lib/sql/program.c`

A hand-written recursive-descent parser reads `ctx->text` into a
stack-allocated `sql_statement` struct, then `program.c` lowers it
into the `sqlexec_program` arena in `ctx->program`.

Lowering includes view expansion:
- `sys_*` names → set `subquery_text` for executor materialisation
- Named views in `sys/vw.dbf` → flatten or set `subquery_text`
- Inline `FROM (SELECT ...)` subqueries → flatten or set `subquery_text`

### 4.2 Optimize — `sqlopt_run`

**Source:** `lib/sqlopt/optimize.c`

A single catalog-driven pass rewrites `ctx->program` in place:

1. Looks up `sys/ndx.dbf` for single-field indexes on the target table.
2. Matches top-level AND conjuncts with simple literal comparisons.
3. Annotates `table_scan` with `sqlexec_scan_index_eq` or
   `sqlexec_scan_index_range` when a matching index exists.
4. Packs direct field-slot bindings into `where_left_slots[]` and
   `where_value_slots[]` for fast WHERE evaluation without re-resolving
   column names at scan time.
5. Prunes the residual WHERE tree of predicates already enforced by the
   index.

### 4.3 Execute — `sqlexec_run`

**Source:** `lib/sqlexec/execute.c` (dispatcher) and the six executor
files listed in §2.

`execute.c` reads the root opcode from `ctx->program` and routes to:

| Root opcode family | Executor |
|---|---|
| `sqlexec_begin/commit/rollback` | `lib/tran/tran.c` |
| DDL opcodes | `execute_ddl.c` |
| `sqlexec_project`, `table_scan`, `join_scan` | `execute_select.c` |
| `sqlexec_append_record` | `execute_mutate.c` |
| `sqlexec_write_current` | `execute_mutate.c` |
| `sqlexec_delete_current` | `execute_mutate.c` |

---

## 5. Execution Tree (IR)

`sqlexec_program` is a fixed-size node arena — no heap, no pointers
into external memory.  Every plan fits in at most **two nodes**:

```
plan root
  └── one child (for SELECT and DML that carry a scan)
```

DDL statements are single root nodes.  SELECT and DML have a root
describing the action plus a child `table_scan` or `join_scan`
describing the data source.

The arena also holds:
- `names[]` — pool of up to 24 identifier strings shared across nodes
- `where_nodes[]` / `where_values[]` — WHERE and HAVING tree storage
- `predicate_subqueries[]` — text of IN / EXISTS subqueries
- `storage` union — columns (for CREATE TABLE), assignments (for
  INSERT/UPDATE), or subquery text (for FROM subqueries)

---

## 6. Storage Layout

```
<root>/
  sys/
    db.dbf       database catalog  (name C16, path C48, slot N2)
    ndx.dbf      index catalog     (db C16, name C16, table C16,
                                    fields C191, unique C1)
    vw.dbf       view catalog      (db C16, name C16, type C1,
                                    statement C240)
    _tmp.dbf     transient — present only during subquery execution
  1/             slot for first database
    table.dbf    user table
    index.ndx    user index
  2/ …
```

Each database occupies a numbered slot directory (1–15).  The path to
its slot is stored in `sys/db.dbf`.  Indexes are rebuilt from scratch
after every INSERT, UPDATE, or DELETE.

---

## 7. Transaction Model

Transactions are fully in-memory.  No files are written to disk until
`COMMIT`.

```
BEGIN
  │
  ├── INSERT/UPDATE/DELETE
  │     malloc txn_entry { op, table, record_no, old_crc, new_record }
  │     append to txn_entry linked list
  │     (real table untouched)
  │
  ├── SELECT
  │     physical scan + linked-list overlay:
  │       txn_op_delete  → skip row
  │       txn_op_update  → replace row with new_record
  │       txn_op_insert  → append as virtual row after physical scan
  │
COMMIT
  │  Phase 1: for each UPDATE/DELETE entry, re-read real record,
  │           compare CRC-16 against entry->old_crc.
  │           Mismatch → return -1 (caller should ROLLBACK and retry).
  │  Phase 2: for each unique table, open real file,
  │           replay entries in log order (delete/write/append).
  │           Rebuild indexes.
  │  Phase 3: free linked lists, set active = 0.
  │
ROLLBACK
     free linked lists, set active = 0.
     (no disk I/O needed)
```

See `docs/TRAN-IMPL.md` for implementation details.

---

## 8. Schema Cache

The `metacache` module maintains an in-session linked list of table
names and field descriptors for the active database.  It is populated
on `USE`, refreshed after `CREATE TABLE` or `DROP TABLE`, and freed on
`USE` to a different database.

During system-view generation (`sys_tables`, `sys_fields`) the cache
avoids directory scans on every query.  The cache is never consulted
for transaction shadow logic — transactions see the live table schema
directly.

---

## 9. Library Dependencies

```
          sqltypes.h
              │
         ┌────┴────────────────────┐
        dbf.h                    ndx.h
         │                        │
         └──────────┬─────────────┘
                 common.h
                    │
                catalog.h
              ┌─────┴──────────┐
          sql.h             sqlexec.h
           │                    │
       sqlopt.h             sqlctx.h ◄── tran.h
                                │
                           exec_impl.h
```

Libraries compile in dependency order.  `tran.c` links against
`catalog`, `common`, `dbf`, and (for Phase 2 replay) the same
`sql_run` / `sqlopt_run` / `sqlexec_run` entry points as the shell.

---

## 10. CP/M Overlay Layout

On Z80 with 128 KB banked memory each library becomes a separately
loadable `.COM` binary.  The dispatcher in `main.c` loads one module at
a time into a fixed bank, calls its `module_run(sql_context *)` entry
point, and unloads it.

Estimated Z80 code sizes (from SDCC `.lst` files):

| Module | ~Bytes |
|---|---:|
| `lib/dbf` | 3 034 |
| `lib/ndx` | 17 700 |
| `lib/common` | 2 510 |
| `lib/catalog` | 5 673 |
| `lib/sql` | 26 900 |
| `lib/sqlopt` | 16 431 |
| `lib/sqlexec/sqlexec.c` | 5 169 |
| `lib/sqlexec/execute.c` | 1 233 |
| `lib/sqlexec/execute_select.c` | 13 413 |
| `lib/sqlexec/execute_mutate.c` | 5 791 |
| `lib/sqlexec/execute_ddl.c` | 4 428 |
| `lib/sqlexec/execute_subquery.c` | 5 136 |
| `lib/sqlexec/execute_sysviews.c` | 3 770 |
| `lib/sqlexec/execute_scan.c` | 3 135 |
| `lib/sqlexec/project.c` | 6 740 |
| `lib/sqlexec/where.c` | 7 233 |
| `lib/sqlexec/metacache.c` | 575 |
| `lib/tran` | ~3 500 |

Splitting into loadable modules of ≤ 20 KB is documented in
`docs/TRAN-IMPL.md` (transaction overlay) — the same pattern applies
to the parser and executor groups.  The key shared libraries (`dbf`,
`ndx`, `common`, `catalog`, `sqlexec.c`) belong in the resident kernel.

---

## 11. Build

```sh
make debug    # GCC with -g -fsanitize=address,undefined
make release  # GCC with -O2
make test     # build and run all test suites
make sdcc_check  # SDCC Z80 compile check (no link)
```

SDCC is used only after the GCC build and all tests pass.  The
`sdcc_check` target compiles each library to `.rel` files to catch
any SDCC-specific issues without attempting a full link.
