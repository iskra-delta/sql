# sql

An interactive SQL shell for creating and querying small databases.
Runs on Linux and on Z80-based CP/M machines such as the Iskra Delta
Partner. Databases are stored as dBase III `.dbf` files; secondary
indexes use dBase III `.ndx` files.

## Build

```sh
make          # debug build with ASAN/UBSAN
make release  # optimised build
make test     # run all automated tests
```

The hosted GCC build uses `-std=c11 -Wall -Wextra -pedantic
-g -fsanitize=address,undefined`.

## Running

```sh
./bin/sql <root>
```

`<root>` is the directory where all databases are stored. It is created
on first run.

Prompt:
- `> ` — no active database
- `dbname> ` — after `CREATE DATABASE` or `USE`

Exit with **Ctrl+C**.

Under SDCC/CP/M the default root is `db`.

## Supported SQL

```text
CREATE DATABASE name;
SHOW DATABASES;
USE name;
DROP DATABASE name;

CREATE TABLE name (col type [, col type ...]);
DROP TABLE name;

CREATE [UNIQUE] INDEX name ON table (col [, col ...]);

CREATE VIEW name AS SELECT ...;
DROP VIEW name;
SHOW VIEWS;

SELECT [ALL|DISTINCT] select_list FROM from_item
  [{, from_item} | {JOIN table ON col = col}]...
  [WHERE expr]
  [GROUP BY col [, col ...]]
  [HAVING expr];
INSERT INTO table [(col [, col ...])] VALUES (val [, val ...]);
UPDATE table SET col = val [, ...] [WHERE expr];
DELETE FROM table [WHERE expr];

BEGIN;
COMMIT;
ROLLBACK;
```

Column types: `CHAR(n)`, `CHARACTER(n)`, `NUMERIC(n[,d])`, `DATE`,
`LOGICAL`.

Supported SELECT functions:
- scalar: `TRIM(col)`
- aggregate: `COUNT(*)`, `COUNT(col)`, `MIN(col)`, `MAX(col)`,
  `SUM(col)`, `AVG(col)` (integer arithmetic)

The `NULL` literal is accepted in `INSERT`, `UPDATE`, and predicate
comparisons.

WHERE and HAVING support `AND`, `OR`, `NOT`, `IN (...)`,
`IN (SELECT ...)`, `BETWEEN`, `LIKE`, `IS NULL`, `IS NOT NULL`,
`EXISTS (SELECT ...)`, quantified comparisons with `ANY` / `ALL`,
nested parentheses, the comparison operators `=`, `<>`, `!=`, `<`,
`<=`, `>`, `>=`, and qualified column-to-column comparisons such as
`p.city = c.code` in multi-source queries.

SELECT supports up to 4 total row sources across comma-separated table
lists and/or 3 INNER JOIN clauses.  Grouped SELECT and `SELECT DISTINCT`
each keep up to 8 in-memory result rows.  HAVING reuses the WHERE
syntax but references projected output names or aliases.

Predicate subqueries (`IN (SELECT ...)`, `EXISTS`, `ANY`, `ALL`) are
materialised before the outer scan runs; up to 2 per statement, up to
32 cached rows each.

`NULL` maps to a blank DBF field — blank character data and `NULL` are
not yet distinguished as stored values.

Identifiers and keywords are case-insensitive. Every statement ends
with `;`.

## Transactions

Statements between `BEGIN` and `COMMIT` execute atomically.  All DML
is held in an in-memory log until COMMIT; ROLLBACK discards it
instantly.  COMMIT verifies CRC-16 preconditions on every updated and
deleted row before touching any file.

## Built-in System Views

| View | Contents |
|---|---|
| `sys_databases` | all registered databases |
| `sys_tables` | tables in the current database |
| `sys_fields` | field descriptors for each table |
| `sys_indexes` | indexes registered for the current database |
| `sys_views` | user-defined views in the current database |

Example:
```sql
SELECT * FROM sys_tables;
SELECT name, type, length FROM sys_fields WHERE table_name = 'people';
```

## Architecture

Three-phase pipeline per statement:

```
SQL text
  → sql_run()       parse + view expansion  → sqlexec_program
  → sqlopt_run()    catalog-driven rewrites  → sqlexec_program
  → sqlexec_run()   tree execution           → output
```

All three phases share one `sql_context` struct which holds the storage
root, active database name, the execution-tree IR, the I/O callback,
and the transaction state.  On CP/M this struct sits at a fixed address
in the resident kernel; each phase binary is loaded separately and
called via a single `module_run(sql_context *)` entry point.

See `docs/ARCHITECTURE.md` for the full layout.

### Optimizer

A single catalog-driven pass rewrites the execution tree:

- reads `sys/ndx.dbf` for registered single-field indexes
- annotates `table_scan` with equality or range index access when a
  suitable index exists
- prunes FROM the WHERE tree any predicates the index already enforces

### Transactions

An in-memory linked list accumulates INSERT/UPDATE/DELETE operations.
COMMIT replays the list to the real tables after a CRC-16 precondition
check; ROLLBACK frees the list with no disk I/O.  See
`docs/TRAN-IMPL.md`.

## Storage Layout

```text
<root>/
  sys/
    db.dbf      database catalog  (name, path, slot)
    ndx.dbf     index catalog     (db, name, table, key_fields, unique)
    vw.dbf      view catalog      (db, name, type, statement)
  1/             first database slot
    table.dbf
    index.ndx
  2/
  ...
```

Slots run from `1` to `15`. `sys/db.dbf` maps names to slot paths.

## Project Layout

| Path | Contents |
|---|---|
| `src/` | shell entry point and platform I/O |
| `include/` | public headers |
| `lib/dbf/` | dBase III file reader/writer |
| `lib/ndx/` | dBase III B-tree index library |
| `lib/common/` | phase-neutral utility helpers |
| `lib/catalog/` | catalog and table-access infrastructure |
| `lib/sql/` | SQL parser and execution-tree builder |
| `lib/sqlopt/` | catalog-driven query optimizer |
| `lib/sqlexec/` | execution-tree executor and runtime support |
| `lib/tran/` | in-memory transaction log (BEGIN/COMMIT/ROLLBACK) |
| `docs/` | design and reference documentation |
| `tests/` | automated tests |
| `build/` | compiler outputs |
| `bin/` | executables |

## Documentation

| File | Contents |
|---|---|
| `INTRO.md` | shell usage guide with examples |
| `docs/ARCHITECTURE.md` | system design, pipeline, storage, modules |
| `docs/SQL.md` | SQL grammar, optimizer, executor, statement semantics |
| `docs/DBF.md` | DBF wire format and API reference |
| `docs/NDX.md` | NDX B-tree format and API reference |
| `docs/TRAN-IMPL.md` | transaction implementation notes |

## Limits

| Resource | Limit |
|---|---|
| Databases per root | 15 |
| Columns per table | 16 |
| Identifier length | 16 characters |
| Value text length | 33 characters |
| Plan nodes per statement | 2 |
| Pooled names per plan | 24 |
| Row sources per SELECT | 4 |
| JOIN clauses per SELECT | 3 |
| WHERE / HAVING nodes per statement | 24 |
| WHERE / HAVING values per statement | 16 |
| Predicate subqueries per statement | 2 |
| Distinct / grouped result rows per SELECT | 8 |
| Subquery nesting depth | 1 |
| Record buffers | heap-allocated (actual record length) |

## Shell Keys

| Key | Effect |
|---|---|
| Printable character | echo and append |
| Backspace / DEL | erase last character |
| Enter | execute current line |
| Ctrl+C | exit |
