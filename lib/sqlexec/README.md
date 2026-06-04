# libsqlexec

`libsqlexec` is the shared execution-tree and executor layer for the
project.

Public headers:

- `include/sqlexec.h`
- `include/sqltypes.h`

Implementation:

- `lib/sqlexec/sqlexec.c`
- `lib/sqlexec/execute.c`

## Purpose

`libsqlexec` has two jobs:

1. define the bounded tree format shared by parser, optimizer, and
   executor
2. execute the final optimized tree in the hosted build

That makes it the central cross-phase boundary for the future modular
server design.

## Tree Representation

Each statement lives in one `sqlexec_program`.

Important properties:

- fixed-size arena
- no heap allocation
- `first_child` for nested input
- `next_sibling` for ordered siblings
- shared pools for names and `WHERE` expression payloads
- one statement-specific storage union for create-table columns,
  mutation assignments, or subquery/view SQL text

Current hard limits:

- 2 nodes per statement
- 16 pooled columns
- 16 pooled assignments
- 48 `WHERE` nodes
- 32 `WHERE` values
- 48 `HAVING` nodes
- 32 `HAVING` values
- 64 pooled names
- 8 predicate subqueries per statement
- 32 cached rows per predicate subquery
- 8 in-memory groups per grouped `SELECT`

## Current Opcode Set

Schema and catalog:

- `sequence`
- `create_database`
- `use_database`
- `drop_database`
- `create_table`
- `drop_table`
- `create_index`

Query:

- `table_scan`
- `join_scan`
- `project`

Mutation:

- `append_record`
- `write_current`
- `delete_current`

Materialised FROM subqueries and views are not separate opcodes.
Simple base-table views and simple inline one-table subqueries,
including plain projected column lists with aliases, may flatten away
before execution. The remaining cases are driven by
`program->table_name` plus `program_subquery_text(program)`, with executor
helpers in `execute_subquery.c`.

## Canonical Plan Shapes

The current parser emits a small set of canonical trees. The hosted
executor recognizes those shapes directly.

Example `SELECT name FROM people WHERE age = 18;`

```text
project name where age = 18
  table_scan
```

Example `SELECT p.name, c.title FROM people AS p JOIN cities c
ON p.city = c.code;`

```text
project p.name, c.title
  join_scan people as p join cities as c on p.city = c.code
```

Example `SELECT p.name, c.title FROM people AS p, cities c
WHERE p.city = c.code;`

```text
project p.name, c.title where p.city = c.code
  join_scan people as p join cities as c
```

Example `SELECT city, MAX(age) AS max_age FROM people GROUP BY city
HAVING max_age > 18;`

```text
project city, MAX(age) as max_age group_by city having max_age > 18
  table_scan
```

Example `UPDATE people SET age = 19 WHERE age = 18;`

```text
write_current assignments=1
  table_scan
```

Example `CREATE INDEX people_name ON people (name);`

```text
create_index people_name on people (name)
```

## Public API

Tree helpers:

- `sqlexec_reset()`
- `sqlexec_make_root()`
- `sqlexec_append_child()`
- `sqlexec_insert_sibling_after()`
- `sqlexec_remove()`
- `sqlexec_replace()`
- `sqlexec_get()`
- `sqlexec_get_const()`

Payload pool helpers:

- `sqlexec_add_names()`
- `sqlexec_add_columns()`
- `sqlexec_add_assignments()`

Validation and inspection:

- `sqlexec_validate()`
- `sqlexec_dump()`
- `sqlexec_opcode_name()`

Execution:

- `sqlexec_execute()`
- `sqlexec_io`

## Hosted Executor Behavior

The hosted executor owns:

- database catalog updates
- table creation and removal
- index creation and registration
- registered-index cleanup on drop
- row reads and writes
- index rebuilds after insert, update, and delete

Current execution caveat:

- project roots carry full boolean `WHERE` trees, including `AND`,
  `OR`, `NOT`, `IN (...)`, `IN (SELECT ...)`, `BETWEEN`, `LIKE`,
  `IS NULL`, `IS NOT NULL`, `EXISTS`, and quantified `ANY` / `ALL`
  comparisons with three-valued logic
- `join_scan` currently runs as one nested-loop inner join for both
  comma-style table lists and `JOIN ... ON ...` queries
- grouped `SELECT` and `SELECT DISTINCT` keep bounded in-memory result
  tables and apply `HAVING` after aggregation
- predicate subqueries are currently limited to uncorrelated
  single-column SELECTs and are materialised into bounded caches before
  the outer scan begins
- optimizer-selected access on `table_scan` drives real NDX lookups for
  simple SELECT, COUNT, DISTINCT, grouped, UPDATE, and DELETE paths
- top-level predicates already enforced by index access are pruned from
  the residual `WHERE` tree before execution

## Output Boundary

`sqlexec_execute()` writes user-visible output through `sqlexec_io`:

```c
typedef struct sqlexec_io {
    void (*write_char)(char c);
} sqlexec_io;
```

This keeps shell I/O outside the executor and keeps `main` small.

## Notes

- `sequence` remains available as a generic tree helper, but parser-
  generated SELECT and mutation plans now store their target table in
  `program->table_name` and start directly at the operation root.
- `COUNT(*)` lowers as a count-only scan at the root, with `WHERE`
  stored on the shared program predicate tree.
- grouped aggregates such as `COUNT(col)`, `MIN(col)`, `MAX(col)`,
  `SUM(col)`, and `AVG(col)` stay inside the normal
  `project(...)` shape; the project metadata switches the executor into
  grouped mode.
- Mutation paths rebuild registered indexes after table changes because
  incremental NDX maintenance is not implemented yet.
