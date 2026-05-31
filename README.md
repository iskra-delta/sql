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

SELECT * | col... | COUNT(*) | COUNT(n) FROM table|view|(SELECT...)
  [JOIN table ON col = col]
  [WHERE expr];
INSERT INTO table VALUES (val [, val ...]);
UPDATE table SET col = val [, ...] [WHERE expr];
DELETE FROM table [WHERE expr];
```

Column types: `CHAR(n)`, `CHARACTER(n)`, `NUMERIC(n[,d])`, `DATE`,
`LOGICAL`.

WHERE supports `AND`, `OR`, `IN (...)`, nested parentheses, and the
comparison operators `=`, `<>`, `!=`, `<`, `<=`, `>`, `>=`.

The JOIN `ON` condition is merged into the WHERE tree at parse time,
so a single N-source evaluator handles both join equality and filter
predicates with no special-case code in the executor.

Identifiers and keywords are case-insensitive. Every statement ends
with `;`.

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

The shell runs a three-phase pipeline for every statement:

```
SQL text
  → sql_run()       parse + view expansion  → sqlexec_program
  → sqlopt_run()    catalog-driven rewrites  → sqlexec_program
  → sqlexec_run()   tree execution           → output
```

All three phases share one `sql_context` struct which holds the
storage root, active database name, the execution tree, and the I/O
callback. On CP/M this struct will sit at a fixed address in the
resident kernel; each phase binary can be loaded separately and called
via a single `module_run(sql_context *)` entry point.

Subqueries and views are materialised to a temp table (`_tmp.dbf` in
the current database directory) before the outer query runs. The temp
file is removed after the outer query closes.

### Optimizer

`lib/sqlopt` performs one catalog-driven pass:

- reads `sys/ndx.dbf` for registered single-field indexes
- rewrites `filter(col = val → table_scan)` to `index_scan_eq`
- rewrites `filter(col op val → table_scan)` to `index_scan_range`
  for `<`, `<=`, `>`, `>=`
- leaves the `filter` node in place as a correctness guard

The executor acts on index-scan nodes: equality probes do a B-tree
lookup, range scans walk the B-tree within the specified bounds.

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

Slots run from `1` to `15`. `sys/db.dbf` maps names to paths.
`sys/ndx.dbf` is used by mutation statements to rebuild indexes and by
the optimizer to discover available indexes. `sys/vw.dbf` stores
user-defined view SQL text; built-in `sys_*` views are hardcoded in
the executor.

## Project Layout

| Path | Contents |
|---|---|
| `src/` | shell entry point and platform I/O |
| `include/` | public headers |
| `lib/dbf/` | DBF storage library |
| `lib/ndx/` | dBase III B-tree index library |
| `lib/shared/` | catalog, table, WHERE helpers shared by all phases |
| `lib/sql/` | SQL parser and execution-tree builder |
| `lib/sqlopt/` | catalog-driven query optimizer |
| `lib/sqlexec/` | execution-tree executor |
| `docs/` | design and reference documentation |
| `tests/` | automated tests |
| `build/` | compiler outputs |
| `bin/` | executables |

## Documentation

| File | Contents |
|---|---|
| `INTRO.md` | shell usage guide with examples |
| `docs/SQL.md` | SQL grammar, execution tree, statement semantics |
| `docs/DBF.md` | DBF wire format and API reference |
| `docs/NDX.md` | NDX wire format and API reference |
| `docs/MODULES.md` | module architecture for CP/M overlay loading |
| `docs/REFACTORING.md` | refactoring record and rationale |

## Limits

| Resource | Limit |
|---|---|
| Databases per root | 15 |
| Columns per table | 16 |
| Identifier length | 16 characters |
| Value text length | 32 characters |
| Plan nodes per statement | 20 |
| Pooled names per plan | 24 |
| WHERE nodes per statement | 16 |
| Maximum record buffer | 4096 bytes |

## Shell Keys

| Key | Effect |
|---|---|
| Printable character | echo and append |
| Backspace / DEL | erase last character |
| Enter | execute current line |
| Ctrl+C | exit |
