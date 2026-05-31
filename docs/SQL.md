# SQL Reference

The SQL layer is intentionally small. It is designed to be easy to
debug under GCC and to split into separately loadable phases on CP/M.

## Grammar

```text
statement ::= create_database
            | show_databases
            | use
            | drop_database
            | create_table
            | drop_table
            | create_index
            | create_view
            | drop_view
            | show_views
            | select
            | insert
            | update
            | delete

create_database ::= CREATE DATABASE <name> ;
show_databases  ::= SHOW DATABASES ;
use             ::= USE <name> ;
drop_database   ::= DROP DATABASE <name> ;
drop_table      ::= DROP TABLE <name> ;
create_table    ::= CREATE TABLE <name> ( column_def [, column_def ...] ) ;
create_index    ::= CREATE [ UNIQUE ] INDEX <name> ON <name>
                    ( column_name [, column_name ...] ) ;
create_view     ::= CREATE VIEW <name> AS select_body ;
drop_view       ::= DROP VIEW <name> ;
show_views      ::= SHOW VIEWS ;
select          ::= SELECT select_list FROM from_item [join_clause]
                    [where_clause] ;
insert          ::= INSERT INTO <name> VALUES ( value [, value ...] ) ;
update          ::= UPDATE <name> SET assignment [, assignment ...]
                    [where_clause] ;
delete          ::= DELETE FROM <name> [where_clause] ;

select_list     ::= * | COUNT(*) | select_item [, select_item ...]
select_item     ::= column_ref [ [ AS ] column_name ]
from_item       ::= <name> [ [ AS ] alias ]
                  | ( select_body ) [ [ AS ] alias ]
join_clause     ::= JOIN from_item ON qualified_column = qualified_column
column_ref      ::= column_name | qualifier . column_name
qualified_column ::= qualifier . column_name
column_def      ::= <name> column_type
column_type     ::= CHAR ( n )
                  | CHARACTER ( n )
                  | NUMERIC ( n [, d] )
                  | DATE
                  | LOGICAL
where_clause    ::= WHERE where_expr
where_expr      ::= where_term [ OR where_term ... ]
where_term      ::= where_primary [ AND where_primary ... ]
where_primary   ::= column_ref op value
                  | column_ref IN ( value [, value ...] )
                  | ( where_expr )
assignment      ::= <column> = value
op              ::= = | <> | != | < | <= | > | >=
value           ::= 'string' | number | identifier
select_body     ::= SELECT select_list FROM from_item [join_clause]
                    [where_clause]
```

Identifiers and keywords are case-insensitive. Every top-level
statement must end with `;`. The `select_body` inside `CREATE VIEW`
and inside a subquery `(...)` does not carry its own semicolon.

Current non-goals:
- no outer joins
- no multi-join plans
- no correlated subqueries
- no `ORDER BY` or `GROUP BY`
- no aggregates beyond `COUNT(*)`

---

## Pipeline

Every statement passes through three phases:

```
SQL text
  → sql_run()       parse + view expansion  → sqlexec_program
  → sqlopt_run()    catalog-driven rewrites  → sqlexec_program (rewritten)
  → sqlexec_run()   tree execution           → terminal output
```

All three phases share one `sql_context` struct (`include/sqlctx.h`).
On CP/M this struct sits at a fixed address; each phase can be loaded
as an independent overlay binary with a single `module_run(ctx)`
entry point.

### View expansion

During `sql_run`, the FROM table name is checked:
- Names beginning with `sys_` → built-in view (hardcoded in executor)
- Other names → looked up in `sys/vw.dbf`

In both cases the program builder inserts `run_subquery` and
`delete_temp` nodes around the outer table scan, and the table name
is redirected to `_tmp`.

---

## Execution Tree

The parser builds a `sqlexec_program` — a fixed-size node arena with
child/sibling links and no heap allocation.

Key sizes: 20 nodes, 24 pooled names, 241-byte subquery text.

### Opcode groups

**Schema / catalog:**
`create_database`, `show_databases`, `use_database`, `drop_database`,
`create_table`, `drop_table`, `build_index`, `register_index`,
`unregister_table_indexes`, `unregister_database_indexes`,
`create_view`, `drop_view`, `show_views`

**Query:**
`open_table`, `close_table`, `table_scan`, `join_scan`,
`index_scan_eq`, `index_scan_range`, `filter`, `project`,
`count_rows`, `emit_rows`, `emit_count`

**Mutation:**
`make_record`, `append_record`, `apply_assignments`,
`write_current`, `delete_current`, `count_affected`,
`rebuild_table_indexes`

**Subquery / view:**
`run_subquery`, `delete_temp`

### Tree examples

`SELECT name FROM people WHERE age = 18;`
```
sequence
  open_table people
  emit_rows
    project name
      filter age = 18
        table_scan
  close_table
```

`SELECT * FROM active_people WHERE age > 30;` (view or subquery)
```
sequence
  run_subquery _tmp
  open_table _tmp
  emit_rows
    project *
      filter age > 30
        table_scan
  close_table _tmp
  delete_temp _tmp
```

`SELECT p.name, c.city FROM people p JOIN cities c ON p.city = c.code;`
```
sequence
  open_table people
  emit_rows
    project p.name, c.city
      join_scan people as p join cities as c on p.city = c.code
  close_table
```

`INSERT INTO people VALUES ('alice', 30, T);`
```
sequence
  open_table people
  count_affected
    append_record
      make_record values=3
  rebuild_table_indexes people
  close_table
```

`CREATE UNIQUE INDEX age_idx ON people (age);`
```
sequence
  build_index age_idx on people (age) unique
  register_index age_idx on people (age) unique
```

---

## Optimizer

`lib/sqlopt` performs one conservative, catalog-driven pass.

**What it does:**
- reads `sys/ndx.dbf` for registered indexes in the active database
- matches single-field indexes only
- considers only filters with a single simple comparison
- checks field-type compatibility with the comparison value

**Current rewrites:**

| Before | After |
|---|---|
| `filter(col = val → table_scan)` | `filter(col = val → index_scan_eq)` |
| `filter(col < val → table_scan)` | `filter(col < val → index_scan_range)` |
| same for `<=`, `>`, `>=` | — |

The `filter` node stays in place after rewrite as a correctness guard.

**Not yet:**
- composite-index selection
- multi-predicate reasoning
- `IN (...)` optimization
- join optimization
- cost-based index choice

---

## Executor

`lib/sqlexec` executes the optimized tree against DBF and NDX storage.

**Index scanning:**
- `index_scan_eq` — B-tree probe; walks with key comparison for
  non-unique indexes
- `index_scan_range` — B-tree walk stopping when past the upper bound

**Subquery materialisation:**
- `run_subquery` parses and executes the inner SQL (or calls a
  built-in generator), writing projected rows into `_tmp.dbf`
- `delete_temp` removes `_tmp.dbf` after the outer query closes it

**Index maintenance:**
Every registered index for the affected table is rebuilt after every
successful `INSERT`, `UPDATE`, or `DELETE`.

---

## Statement Semantics

### `CREATE DATABASE <name>`
Allocates the first free slot (1–15), creates the slot directory, and
appends a row to `sys/db.dbf`. Makes the new database current.

### `SHOW DATABASES`
Reads and prints all live rows from `sys/db.dbf`.

### `USE <name>`
Validates the name against `sys/db.dbf` and switches the active
context.

### `DROP DATABASE <name>`
Soft-deletes the `sys/db.dbf` row and removes all `sys/ndx.dbf` and
`sys/vw.dbf` rows for that database. The slot directory is not
deleted.

### `CREATE TABLE <name> (...)`
Creates `<table>.dbf` in the active database directory.

| SQL type | DBF type | Storage |
|---|---|---|
| `CHAR(n)` / `CHARACTER(n)` | `C` | left-aligned, space-padded |
| `NUMERIC(n[,d])` | `N` | right-aligned text |
| `DATE` | `D` | `YYYYMMDD` text (8 bytes) |
| `LOGICAL` | `L` | one byte (`T` or `F`) |

### `DROP TABLE <name>`
Removes `<table>.dbf`, registered `.ndx` files, and `sys/ndx.dbf`
rows for that table.

### `CREATE [UNIQUE] INDEX <name> ON <table> (...)`
Builds the `.ndx` file and appends a row to `sys/ndx.dbf`.

Supported key combinations:
- one `CHAR`, `NUMERIC`, or `DATE` field
- multiple `CHAR` fields (concatenated in DBF byte order)

### `CREATE VIEW <name> AS SELECT ...`
Stores the SELECT text in `sys/vw.dbf`. On each use the text is
re-parsed, optimized, and executed to a temp table transparently.

### `DROP VIEW <name>`
Soft-deletes the matching `sys/vw.dbf` row.

### `SHOW VIEWS`
Lists all live user-defined views for the current database.

### `SELECT`
Scans live rows, applies the optional WHERE expression, and projects
columns. Deleted rows are skipped. FROM may reference a table, a
user-defined view, a built-in `sys_*` view, or an inline subquery.
One optional `JOIN` is supported.

### `INSERT INTO <name> VALUES (...)`
Appends one record and rebuilds all registered indexes.

### `UPDATE <name> SET ... [WHERE ...]`
Rewrites matching rows in place, rebuilds indexes on any change.

### `DELETE FROM <name> [WHERE ...]`
Soft-deletes matching rows, rebuilds indexes on any deletion.

---

## Built-in System Views

Names beginning with `sys_` are reserved and cannot be used for user
tables or views.

| View | Schema | Source |
|---|---|---|
| `sys_databases` | name, path, slot | `sys/db.dbf` |
| `sys_tables` | name | directory scan of active database |
| `sys_fields` | table_name, name, type, length, decimals, position | DBF headers |
| `sys_indexes` | name, table_name, key_fields, unique | `sys/ndx.dbf` |
| `sys_views` | name, type | `sys/vw.dbf` |

`sys_tables` and `sys_fields` require an active database.

---

## Storage and Catalogs

```text
<root>/
  sys/
    db.dbf     (name C16, path C48, slot N2)
    ndx.dbf    (db_name C16, name C16, table_name C16,
                key_fields C191, unique C1)
    vw.dbf     (db_name C16, name C16, type C1, statement C240)
    _tmp.dbf   transient — present only during subquery execution
  1/
    table.dbf
    index.ndx
  2/
  ...
```

---

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
| WHERE values per statement | 16 |
| Subquery nesting | 1 level |
| Maximum record buffer | 4096 bytes |
| View SQL text length | 240 characters |
