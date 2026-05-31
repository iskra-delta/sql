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

`sql_parse()` is the normal public entry point. It accepts `root` and
`current_db` for view name resolution; pass `NULL, NULL` when catalog
access is not needed (e.g. in tests).

It:

- clears and fills one `sqlexec_program`
- expands `sys_*` table names to built-in view subqueries
- looks up user view names in `sys/vw.dbf` and validates their SQL
- returns `0` on success
- returns `-1` on parse, view validation, or lowering failure

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
- `columns`
- `create_index_unique`
- `key_names`
- `select_all`
- `select_count_star`
- `select_items`
- `from_alias`
- `values`
- `assignments`
- `where`

But that structure is not the cross-phase contract anymore.

## Shared Output

The real hand-off to the rest of the system is `sqlexec_program`.

`lib/sql/program.c` lowers parsed statements into canonical execution
trees such as:

- `create_database`
- `create_table`
- `sequence(build_index, register_index)`
- `sequence(open_table, emit_rows(project(filter(table_scan))), close_table)`
- `sequence(open_table, count_affected(write_current(...)),
  rebuild_table_indexes, close_table)`

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
- `SELECT column[, column ...] FROM table [WHERE ...];`
- `SELECT table.column [AS alias] FROM table [AS alias] [WHERE ...];`
- `SELECT ... FROM left [AS a] JOIN right [AS b] ON a.col = b.col
  [WHERE ...];`
- `INSERT INTO table VALUES (value[, value ...]);`
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
- `IN (...)`
- parentheses for grouping
- operators: `=`, `!=`, `<>`, `<`, `<=`, `>`, `>=`
- values: quoted strings, numbers, or identifiers
- column references may be qualified with the current table name or its
  alias in `SELECT` statements, including the joined table in one
  join query

Not supported:

- outer joins
- more than one joined table
- subqueries

## How It Works

`libsql` is intentionally small and hand-written.

It:

- skips ASCII whitespace
- matches keywords case-insensitively
- reads identifiers and literals into fixed-size buffers
- reads `table.column` references for `SELECT` and `WHERE`
- parses one `JOIN ... ON left.col = right.col` clause for `SELECT`
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
    /* statement.key_count == 2 */
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

/* statement.from_alias == "p" */
/* statement.select_items[0].column.name == "name" */
/* statement.select_items[0].alias == "person" */
/* statement.where.root references one compare node for name = 'alice' */
```

## Notes

- `libsql` depends only on the shared plan contract, not on optimizer
  or executor internals.
- The canonical tree shapes emitted here are what the current executor
  pattern-matches later.
