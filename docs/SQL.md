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
select          ::= SELECT [ ALL | DISTINCT ] select_list
                    FROM from_item [ table_ref_tail ... ]
                    [where_clause] [group_by_clause]
                    [having_clause] ;
insert          ::= INSERT INTO <name>
                    [ ( column_name [, column_name ...] ) ]
                    VALUES ( value [, value ...] ) ;
update          ::= UPDATE <name> SET assignment [, assignment ...]
                    [where_clause] ;
delete          ::= DELETE FROM <name> [where_clause] ;

select_list     ::= * | COUNT(*) | select_item [, select_item ...]
select_item     ::= column_ref [ [ AS ] column_name ]
                  | TRIM ( column_ref ) [ [ AS ] column_name ]
                  | COUNT ( * | column_ref ) [ [ AS ] column_name ]
                  | MIN ( column_ref ) [ [ AS ] column_name ]
                  | MAX ( column_ref ) [ [ AS ] column_name ]
                  | SUM ( column_ref ) [ [ AS ] column_name ]
                  | AVG ( column_ref ) [ [ AS ] column_name ]
from_item       ::= <name> [ [ AS ] alias ]
                  | ( select_body ) [ [ AS ] alias ]
table_ref_tail  ::= , from_item
                  | join_clause
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
group_by_clause ::= GROUP BY column_ref [, column_ref ...]
having_clause   ::= HAVING where_expr
where_expr      ::= where_term [ OR where_term ... ]
where_term      ::= where_not [ AND where_not ... ]
where_not       ::= [ NOT ] where_primary
where_primary   ::= EXISTS ( select_body )
                  | column_ref op value
                  | column_ref IN ( value [, value ...] )
                  | column_ref IN ( select_body )
                  | column_ref BETWEEN value AND value
                  | column_ref LIKE 'pattern'
                  | column_ref IS NULL
                  | column_ref IS NOT NULL
                  | column_ref op quantifier ( select_body )
                  | ( where_expr )
assignment      ::= <column> = value
op              ::= = | <> | != | < | <= | > | >=
quantifier      ::= ANY | ALL
value           ::= 'string' | number | NULL | identifier
select_body     ::= SELECT [ ALL | DISTINCT ] select_list
                    FROM from_item [ table_ref_tail ... ]
                    [where_clause] [group_by_clause]
                    [having_clause]
```

Identifiers and keywords are case-insensitive. Every top-level
statement must end with `;`. The `select_body` inside `CREATE VIEW`
and inside a subquery `(...)` does not carry its own semicolon.

Current non-goals:
- no outer joins
- no unbounded join plans
- no correlated subqueries
- no `ORDER BY`
- no arbitrary scalar expressions beyond built-in function calls

Implementation notes:
- Predicate subqueries are currently limited to uncorrelated
  single-column `SELECT` bodies.
- Predicate subqueries are materialised before the outer scan begins.
- `NULL` is currently stored as a blank DBF field, so blank character
  data and `NULL` are not yet distinct stored values.

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

Simple base-table views and simple inline subqueries over one base
table may flatten during lowering when safe, including plain projected
column lists with aliases. In the remaining cases, the program builder
redirects the table name to `_tmp` and stores the inner SQL text in
`subquery_text`. The executor then materialises `_tmp.dbf` before the
outer query and removes it afterward.

---

## Execution Tree

The parser builds a `sqlexec_program` — a fixed-size node arena with
child/sibling links and no heap allocation.

Key sizes: 3 nodes, 64 pooled names, 241-byte subquery text.

### Opcode groups

**Schema / catalog:**
`create_database`, `use_database`, `drop_database`,
`create_table`, `drop_table`, `create_index`,
`create_view`, `drop_view`, `show_views`

**Query:**
`table_scan`, `join_scan`, `project`

**Mutation:**
`append_record`, `write_current`, `delete_current`

### Tree examples

`SELECT name FROM people WHERE age = 18;`
```
project name where age = 18
  table_scan
```

`SELECT * FROM active_people WHERE age > 30;` (view or subquery)
```
project * where age > 30
  table_scan
```

`SELECT p.name, c.label FROM people p JOIN cities c ON p.city = c.code WHERE c.region = 'EU';`
```
project p.name, c.label where (c.region = 'EU') AND (p.city = c.code)
  join_scan people as p join cities as c
```

`SELECT city, MAX(age) AS max_age FROM people GROUP BY city HAVING max_age > 18;`
```text
project city, MAX(age) as max_age group_by city having max_age > 18
  table_scan
```

`SELECT DISTINCT city FROM people WHERE NOT city LIKE 'N%' AND age BETWEEN 18 AND 24;`
```text
project DISTINCT city where (NOT city LIKE 'N%' AND (age >= 18 AND age <= 24))
  table_scan
```

The JOIN ON condition is merged into the WHERE tree at parse time. The
right-hand column of the ON equality (`c.code`) is stored as a
structured column operand. The WHERE evaluator resolves both column
sides against all active row sources using the N-source
`where_matches_n` path — no separate join-key check in the executor.

Old SQL-86-style table lists use that same path. A query such as
`FROM people p, cities c WHERE p.city = c.code` lowers to the same
`join_scan` plus shared N-source WHERE model; only the equality comes
from the user-written `WHERE` clause instead of a `JOIN ... ON ...`
rewrite.

`INSERT INTO people VALUES ('alice', 30, T);`
```
append_record values=3
```

`INSERT INTO people (age, name) VALUES (30, 'alice');`
```
append_record values=2
```

`CREATE UNIQUE INDEX age_idx ON people (age);`
```
create_index age_idx on people (age) unique
```

---

## Optimizer

`lib/sqlopt` performs one conservative, catalog-driven pass.

**What it does:**
- reads `sys/ndx.dbf` for registered indexes in the active database
- matches single-field indexes only
- searches top-level conjunctive `WHERE` terms for simple literal
  comparisons
- checks field-type compatibility with the comparison value
- records source-local access on `table_scan` as full scan, equality,
  or range
- prunes top-level predicates already enforced by the chosen index
  access and compacts the remaining `WHERE` tree

**Current rewrites:**

| Before | After |
|---|---|
| `project ... where age = 18` / `table_scan` | `project ...` / `table_scan people_age = 18` |
| `project ... where age >= 18` / `table_scan` | `project ...` / `table_scan people_age >= 18` |
| `project ... where age >= 18 AND city = 'LON'` / `table_scan` | `project ... where city = 'LON'` / `table_scan people_age >= 18` |

**Not yet:**
- composite-index selection
- `IN (...)` optimization
- join optimization
- cost-based index choice

---

## Executor

`lib/sqlexec` executes the optimized tree against DBF and NDX storage.

**Index scanning:**
- equality access on `table_scan` — B-tree probe; walks with key
  comparison for non-unique indexes
- range access on `table_scan` — B-tree walk stopping when past the
  upper bound

**Subquery materialisation:**
- when `subquery_text` is present on a query root, the executor parses
  and executes the inner SQL (or calls a built-in generator), writing
  projected rows into `_tmp.dbf`
- the executor removes `_tmp.dbf` after the outer query finishes
- simple base-table views and simple inline one-table subqueries,
  including plain projected column lists with aliases, may flatten
  earlier and therefore skip `_tmp.dbf` entirely

**Join execution:**
For join queries, the ON condition lives in the WHERE tree as a compare
node. The executor opens both tables and evaluates the unified WHERE
tree (ON condition + regular WHERE) against both row sources using the
N-source `where_matches_n` evaluator. No separate join-key check.

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
Up to 4 total row sources are supported across comma-separated table
lists and/or 3 INNER JOIN clauses.
`GROUP BY` and `SELECT DISTINCT` each keep up to 8 in-memory result
rows per statement. `HAVING` uses the same boolean operators as
`WHERE`, but references projected output names or aliases instead of
base-table qualifiers. `COUNT(*)`, `COUNT(col)`, `MIN`, `MAX`, `SUM`,
and `AVG` are supported; `SUM` and `AVG` currently require
integer-compatible numeric fields and `AVG` returns an integer average.

### `INSERT INTO <name> VALUES (...)`
Appends one record and rebuilds all registered indexes.

### `INSERT INTO <name> (<col ...>) VALUES (...)`
Appends one record, maps each input value to the named target column,
leaves unspecified fields blank, and rebuilds all registered indexes.

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
| Value text length | 33 characters |
| Plan nodes per statement | 20 |
| Pooled names per plan | 64 |
| Row sources per SELECT | 4 |
| JOIN clauses per SELECT | 3 |
| WHERE nodes per statement | 48 |
| WHERE values per statement | 32 |
| HAVING nodes per statement | 48 |
| HAVING values per statement | 32 |
| Distinct groups per grouped SELECT | 8 |
| Subquery nesting | 1 level |
| Maximum record buffer | 4096 bytes |
| View SQL text length | 240 characters |

Practical WHERE capacity depends on shape. A flat `AND` chain of simple
comparisons uses one compare node plus one connector node per extra
term, so the current 48-node budget fits up to 24 simple predicates in
single-table queries. JOIN `ON` conditions and `IN (...)` lists consume
the same fixed pools.
