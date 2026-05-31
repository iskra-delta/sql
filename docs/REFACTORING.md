# Refactoring Record

This document records the refactoring work done to make the codebase
fit the Z80/CP/M memory model and split cleanly into loadable phases.

---

## Summary of Work Done

### Priority 1 — Extract shared helpers ✅

Created `lib/shared/` with three compilation units:

| File | Contents |
|---|---|
| `shared.c` | `copy_name`, `get_field`, `set_field`, `join_path`, `uint_to_str`, `trim_field_value`, `parse_integer_text`, `compare_longs`, `compare_strings`, `find_field_index`, `build_field_offsets`, `store_value_in_field`, `clear_record` |
| `catalog.c` | `ensure_catalog`, `find_database_path`, `open_table_file`, `rebuild_table_indexes`, view catalog operations |
| `where.c` | `row_source` struct, `where_matches`, `resolve_field_ref`, `field_matches_value` |

These were previously duplicated independently in `optimize.c`,
`execute.c`, and `program.c`. Each module binary now calls the kernel-
resident copies instead of carrying its own.

### Priority 2 — Slim the IR ✅

| Constant | Before | After | Saving |
|---|---|---|---|
| `sqlexec_max_nodes` | 32 | 20 | ~1.1 KB per program |
| `sqlexec_max_names` | 64 | 24 | ~680 bytes per program |

No current statement type requires more than 12 nodes or 20 names.
Total IR size reduced from ~7 KB to ~5 KB.

### Priority 3 — Gate validate/dump ✅

`sqlexec_validate` and `sqlexec_dump` are compiled only when
`SQLEXEC_DEBUG` is defined. Debug builds (and all automated tests)
define this flag. Production builds skip ~500 bytes of validation code
and three unnecessary traversals per statement.

### Priority 4 — Split execute.c ✅

`execute.c` (2000+ lines) split into:

| File | Responsibility |
|---|---|
| `execute_ddl.c` | CREATE/DROP DATABASE/TABLE/INDEX/VIEW, USE, SHOW |
| `execute_select.c` | SELECT, COUNT, JOIN, WHERE, project |
| `execute_mutate.c` | INSERT, UPDATE, DELETE |
| `execute_scan.c` | `ndx_scan` callbacks for index-driven scans |
| `execute_subquery.c` | `run_subquery`, `delete_temp`, built-in view generators |
| `execute.c` | thin dispatch (~150 lines) |

All shared helpers moved to `lib/shared/`, so each executor file
carries no private copies.

### Priority 5 — Introduce `sql_context` ✅

`include/sqlctx.h` defines:

```c
typedef struct sql_context {
    const char      *root;
    char             current_db[sql_name_size];
    const char      *text;
    sqlexec_program  program;
    sqlexec_io       io;
    int              result;
} sql_context;
```

The three pipeline entry points each take only this struct:

```c
int sql_run(sql_context *ctx);
int sqlopt_run(sql_context *ctx);
int sqlexec_run(sql_context *ctx);
```

`src/main.c` now allocates one `sql_context` for the shell lifetime
and sets `ctx.text = buf` before each statement. The separate
`sqlexec_program`, `sqlexec_io`, and `current_db` locals are gone.

Each entry point doubles as a `module_run(sql_context *)` binary entry
for the future CP/M overlay loader.

### Priority 6 — Index payload side pool (not done)

The optional step of moving `sqlexec_index_range`'s two `sql_value`
members out of the payload union (to save ~30 bytes per node) was not
implemented. The slimmed node count of 20 fits the 8 KB module slot
comfortably without this step.

---

## Additional Work

### Index scan implementation

The optimizer's `index_scan_eq` and `index_scan_range` rewrites were
wired to actual execution:

- `ndx_scan` added to the NDX API: extended walk that passes key bytes
  and a context pointer to the callback
- `ndx_encode_text_key`, `ndx_encode_number_key`, `ndx_compare_key`
  added for building and comparing raw NDX keys
- `exec_eq_scan_callback` and `exec_range_scan_callback` in
  `execute_scan.c` read qualifying DBF records via the NDX

Equality scans on unique indexes do a B-tree probe O(log n).
Range scans walk only the qualifying key range.
The WHERE filter node stays as a correctness guard in both cases.

### Views and nested SQL

`lib/shared/catalog.c` gains `sys/vw.dbf` operations.

Parser additions:
- `CREATE VIEW name AS SELECT ...`
- `DROP VIEW name`
- `SHOW VIEWS`
- `FROM (SELECT ...) [AS alias]` inline subqueries

`sql_parse` accepts `root` and `current_db` for view name resolution.
View names are expanded to `from_is_subquery = 1` before the tree is
built. The `run_subquery` and `delete_temp` opcodes materialise the
inner query to `_tmp.dbf` and clean it up afterward.

Built-in system views (`sys_tables`, `sys_fields`, `sys_indexes`,
`sys_databases`, `sys_views`) are hardcoded in `execute_subquery.c`.

---

## Measured Improvements

| Metric | Before | After |
|---|---|---|
| `execute.c` lines | 2177 | ~150 (dispatch only) |
| `sqlexec_program` size | ~7 KB | ~5 KB |
| Duplicate helper functions | 3 copies each | 1 in lib/shared |
| `sqlexec_validate` in production | always compiled | gated by `SQLEXEC_DEBUG` |
| Index scan | always table scan | B-tree probe/walk |
| View support | none | user views + 5 built-in |
| Subquery support | none | one level, FROM clause |
