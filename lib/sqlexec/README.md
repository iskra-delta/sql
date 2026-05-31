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
- shared pools for names, columns, values, assignments, and `WHERE`
  expression payloads

Current hard limits:

- 32 nodes per statement
- 16 pooled columns
- 16 pooled values
- 16 pooled assignments
- 16 `WHERE` nodes
- 16 `WHERE` values
- 32 pooled names

## Current Opcode Set

Schema and catalog:

- `sequence`
- `create_database`
- `show_databases`
- `use_database`
- `drop_database`
- `create_table`
- `drop_table`
- `build_index`
- `register_index`
- `unregister_table_indexes`
- `unregister_database_indexes`

Query:

- `open_table`
- `close_table`
- `table_scan`
- `join_scan`
- `index_scan_eq`
- `index_scan_range`
- `filter`
- `project`
- `count_rows`
- `emit_rows`
- `emit_count`

Mutation:

- `make_record`
- `append_record`
- `apply_assignments`
- `write_current`
- `delete_current`
- `count_affected`
- `rebuild_table_indexes`

## Canonical Plan Shapes

The current parser emits a small set of canonical trees. The hosted
executor recognizes those shapes directly.

Example `SELECT name FROM people WHERE age = 18;`

```text
sequence
  open_table people
  emit_rows
    project name
      filter age = 18
        table_scan
  close_table
```

Example `SELECT p.name, c.title FROM people AS p JOIN cities c
ON p.city = c.code;`

```text
sequence
  open_table people
  emit_rows
    project p.name, c.title
      join_scan people as p join cities as c on p.city = c.code
  close_table
```

Example `UPDATE people SET age = 19 WHERE age = 18;`

```text
sequence
  open_table people
  count_affected
    write_current
      apply_assignments assignments=1
        filter age = 18
          table_scan
  rebuild_table_indexes people
  close_table
```

Example `CREATE INDEX people_name ON people (name);`

```text
sequence
  build_index people_name on people (name)
  register_index people_name on people (name)
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
- `sqlexec_add_values()`
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

- optimizer-produced `index_scan_eq` and `index_scan_range` nodes are
  accepted structurally
- `filter` nodes evaluate full boolean `WHERE` trees, including
  `AND`, `OR`, and `IN (...)`
- `join_scan` currently runs as one nested-loop inner join
- actual row access still happens through full table scans today
- the `filter` node remains the correctness guard

So the executor already consumes optimized trees, but indexed row
retrieval is still future work.

## Output Boundary

`sqlexec_execute()` writes user-visible output through `sqlexec_io`:

```c
typedef struct sqlexec_io {
    void (*write_char)(char c);
} sqlexec_io;
```

This keeps shell I/O outside the executor and keeps `main` small.

## Notes

- `sequence` nodes model ordered side effects such as open, work,
  rebuild, and close.
- `COUNT(*)` lowers as `emit_count(count_rows(...))`.
- Mutation paths rebuild registered indexes after table changes because
  incremental NDX maintenance is not implemented yet.
