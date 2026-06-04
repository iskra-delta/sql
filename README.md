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
```

Column types: `CHAR(n)`, `CHARACTER(n)`, `NUMERIC(n[,d])`, `DATE`,
`LOGICAL`.

Supported SELECT functions:
- scalar: `TRIM(col)`
- aggregate: `COUNT(*)`, `COUNT(col)`, `MIN(col)`, `MAX(col)`,
  `SUM(col)`, `AVG(col)`

The `NULL` literal is accepted in `INSERT`, `UPDATE`, and predicate
comparisons.

WHERE supports `AND`, `OR`, `NOT`, `IN (...)`, `IN (SELECT ...)`,
`BETWEEN`, `LIKE`, `IS NULL`, `IS NOT NULL`, `EXISTS (SELECT ...)`,
quantified comparisons with `ANY` / `ALL`, nested parentheses, the
comparison operators `=`, `<>`, `!=`, `<`, `<=`, `>`, `>=`, and
qualified column-to-column comparisons such as `p.city = c.code` in
multi-source queries.

SELECT supports up to 4 total row sources across comma-separated table
lists and/or 3 INNER JOIN clauses.
Grouped SELECT and `SELECT DISTINCT` each keep up to 8 in-memory result
rows. `HAVING` reuses the comparison syntax from `WHERE`, but
references projected output names or aliases rather than base-table
qualifiers. `AVG` currently uses integer arithmetic.
Predicate subqueries are limited to uncorrelated single-column SELECTs.
They are materialised before the outer scan, with up to 8 predicate
subqueries and up to 32 cached rows per predicate subquery.

`NULL` currently maps to a blank DBF field. That is enough for
three-valued predicate logic, but blank character data and `NULL` are
not yet distinguished as separate stored values.

Qualified multi-source `WHERE` comparisons and JOIN `ON` conditions are
both evaluated through one shared N-source WHERE path, so the executor
does not need separate old-style-vs-modern join logic.

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

Simple base-table views and simple inline subqueries over one base
table may flatten during lowering when it is safe to do so, including
plain projected column lists with aliases. More complex FROM
subqueries and views are materialised to a temp table (`_tmp.dbf` in
the current database directory) before the outer query runs. The temp
file is removed after the outer query closes. Predicate subqueries
(`EXISTS`, `IN (SELECT ...)`, `op ANY (...)`, `op ALL (...)`) are
collected into bounded in-memory caches before the outer scan begins.

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
| `lib/common/` | phase-neutral utility helpers |
| `lib/catalog/` | catalog and table-access infrastructure |
| `lib/sql/` | SQL parser and execution-tree builder |
| `lib/sqlopt/` | catalog-driven query optimizer |
| `lib/sqlexec/` | execution-tree executor and runtime-only support |
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
| `docs/OPTIMIZATION.md` | optimization close-out note and optional follow-up ideas |
| `docs/STANDARD86.md` | implementation prompt and gap analysis for SQL-86 |
| `docs/REFACTORING.md` | refactoring record and rationale |

## Limits

| Resource | Limit |
|---|---|
| Databases per root | 15 |
| Columns per table | 16 |
| Identifier length | 16 characters |
| Value text length | 33 characters |
| Plan nodes per statement | 20 |
| Pooled names per plan | 64 |
| Row sources per SELECT | 4 |
| JOIN clauses per SELECT | 3 |
| WHERE nodes per statement | 48 |
| WHERE values per statement | 32 |
| HAVING nodes per statement | 48 |
| HAVING values per statement | 32 |
| Distinct groups or DISTINCT rows per SELECT | 8 |
| Maximum record buffer | 4096 bytes |

## Shell Keys

| Key | Effect |
|---|---|
| Printable character | echo and append |
| Backspace / DEL | erase last character |
| Enter | execute current line |
| Ctrl+C | exit |
