# libsql

`libsql` is the parser layer for the project. Its public job is to turn
one SQL string into one shared `sqlexec_program` execution tree.

Public header:

- `include/sql.h`

Implementation:

- `lib/sql/sql.c`
- `lib/sql/program.c`

## Purpose

`libsql` is the first phase of the runtime pipeline:

1. parse text into `sqlexec_program`
2. let `lib/sqlopt` rewrite that tree
3. let `lib/sqlexec` execute the result

This means the parser no longer hands later phases a syntax-only
statement structure.

## Public Interface

- `int sql_parse(const char *text, sqlexec_program *program, const char *root, const char *current_db);`
- `int sql_parse_statement(const char *text, sql_statement *statement);`
- `int sql_parse_select_body(const char *text, sql_statement *statement);`
- `int sql_parse_select_program(const char *text, sqlexec_program *program, const char *root, const char *current_db);`

`sql_parse()` is the normal public entry point. It accepts `root` and
`current_db` for view name resolution; pass `NULL, NULL` when catalog
access is not needed (e.g. in tests).

It:

- clears and fills one `sqlexec_program`
- expands `sys_*` table names to built-in view subqueries
- looks up user view names in `sys/vw.dbf` and validates their SQL
- returns `0` on success
- returns `-1` on parse, view validation, or lowering failure

`sql_parse_select_body()` is the low-level SELECT-body parser for
subqueries and stored view text. It expects `SELECT ...` without a
trailing semicolon.

`sql_parse_select_program()` lowers one SELECT body directly into a
`sqlexec_program`. The executor uses it for complex stored views,
materialized FROM subqueries, and predicate subqueries so those paths
do not need to wrap inner SQL in a fake top-level statement first.

`sql_parse_statement()` remains available as a lower-level helper for:

- parser internals
- syntax-focused tests
- debugging parse behavior without involving the plan tree

## Internal Syntax Structure

The parser still uses `sql_statement` internally because it is a small,
syntax-oriented scratch structure.

Important payload areas:

- `type`
- `name`
- `table_name`
- `detail.select` for `SELECT`-only scratch
- `detail.variant.create_table` for `CREATE TABLE`
- `detail.variant.create_index` for `CREATE INDEX`
- `detail.variant.mutate` for `INSERT` and `UPDATE`
- `where`
- `having`

For `INSERT`, plain `VALUES (...)` input now reuses `assignments` with
empty column names instead of carrying a second separate value array.
The `detail` union keeps those statement-specific payloads overlapped so
the parser does not reserve all of them at once.

But that structure is not the cross-phase contract anymore.

## Shared Output

The real hand-off to the rest of the system is `sqlexec_program`.

`lib/sql/program.c` lowers parsed statements into canonical execution
trees such as:

- `create_database`
- `create_table`
- `create_index`
- `project(table_scan)`
- `write_current(...)`

Keeping the parser responsible for those shapes means:

- optimizer logic can stay tree-based
- executor logic can stay parser-independent
- future separately loaded phases can share one compact representation

## Supported Statements

- `CREATE DATABASE name;`
- `SHOW DATABASES;`
- `USE name;`
- `DROP DATABASE name;`
- `DROP TABLE name;`
- `CREATE TABLE name (...);`
- `CREATE [UNIQUE] INDEX name ON table (column[, column ...]);`
- `SELECT * FROM table;`
- `SELECT COUNT(*) FROM table [WHERE ...];`
- `SELECT [DISTINCT] column[, column ...] FROM table [WHERE ...];`
- `SELECT TRIM(column) FROM table [WHERE ...];`
- `SELECT group_col, COUNT(*), COUNT(column), MIN(column), MAX(column),
  SUM(column), AVG(column) FROM table
  [WHERE ...] GROUP BY group_col [HAVING alias op value];`
- `SELECT table.column [AS alias] FROM table [AS alias] [WHERE ...];`
- `SELECT ... FROM left [AS a], right [AS b] WHERE a.col = b.col
  [AND ...];`
- `SELECT ... FROM left [AS a] JOIN right [AS b] ON a.col = b.col
  [WHERE ...];`
- `SELECT ... FROM table WHERE col IS [NOT] NULL;`
- `SELECT ... FROM table WHERE EXISTS (SELECT ...);`
- `SELECT ... FROM table WHERE col IN (SELECT ...);`
- `SELECT ... FROM table WHERE col op ANY (SELECT ...)
  [OR col op ALL (SELECT ...)];`
- `INSERT INTO table VALUES (value[, value ...]);`
- `INSERT INTO table (column[, column ...]) VALUES (value[, value ...]);`
- `UPDATE table SET column = value[, column = value ...] [WHERE ...];`
- `DELETE FROM table [WHERE ...];`

## Supported Types And Predicates

Column types:

- `CHAR(n)`
- `CHARACTER(n)`
- `NUMERIC(n[,d])`
- `DATE`
- `LOGICAL`

`WHERE` support:

- one expression tree per statement
- `AND`
- `OR`
- `NOT`
- `IN (...)`
- `IN (SELECT ...)`
- `BETWEEN`
- `LIKE`
- `IS NULL`
- `IS NOT NULL`
- `EXISTS (SELECT ...)`
- quantified comparisons with `ANY` / `ALL`
- parentheses for grouping
- operators: `=`, `!=`, `<>`, `<`, `<=`, `>`, `>=`
- values: quoted strings, numbers, `NULL`, or identifiers, including
  qualified column references in multi-source WHERE comparisons
- column references may be qualified with the current table name or its
  alias in `SELECT` statements, including additional row sources in
  comma-table and join queries
- `HAVING` reuses the same boolean grammar but references projected
  output names or aliases

Not supported:

- outer joins
- more than three joined tables
- correlated subqueries

## How It Works

`libsql` is intentionally small and hand-written.

It:

- skips ASCII whitespace
- matches keywords case-insensitively
- reads identifiers and literals into fixed-size buffers
- recognizes `NULL` as a literal value token
- reads `table.column` references for `SELECT`, `WHERE`, and value
  positions used by column-to-column comparisons
- parses bounded comma-separated table lists and `JOIN ... ON
  left.col = right.col` chains for `SELECT`
- parses bounded `GROUP BY` and `HAVING` clauses for grouped `SELECT`
- captures bounded uncorrelated predicate subquery text for later
  execution by `lib/sqlexec`
- uses recursive-descent parsing for each statement form
- lowers successful parses into bounded tree nodes

There is no heap allocation in the parser path.

## Examples

### Syntax-only parse

```c
#include "sql.h"

sql_statement statement;

if (sql_parse_statement("CREATE UNIQUE INDEX people_name ON people (city, name);",
    &statement) != 0) {
    return 1;
}

if (statement.type == sql_statement_create_index) {
    /* statement.name == "people_name" */
    /* statement.table_name == "people" */
    /* index keys live in statement.variant.create_index */
}
```

### Parse directly into the shared execution tree

```c
#include "sql.h"
#include "sqlexec.h"

sqlexec_program program;

if (sql_parse("SELECT name FROM people WHERE age >= 18;", &program) != 0) {
    return 1;
}

/* program now holds the shared execution tree */
```

### Parse SELECT With Aliases

```c
#include "sql.h"

sql_statement statement;

if (sql_parse_statement(
    "SELECT p.name AS person FROM people AS p WHERE p.name = 'alice';",
    &statement) != 0) {
    return 1;
}

/* statement.detail.select.from_alias == "p" */
/* statement.detail.select.select_items[0].column.name == "name" */
/* statement.detail.select.select_items[0].alias == "person" */
/* statement.where.root references one compare node for name = 'alice' */
```

## Notes

- `libsql` depends only on the shared plan contract, not on optimizer
  or executor internals.
- The canonical tree shapes emitted here are what the current executor
  pattern-matches later.
