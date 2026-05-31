# libsqlopt

`libsqlopt` is the optimizer layer for the project. It rewrites shared
`sqlexec_program` trees before execution.

Public header:

- `include/sqlopt.h`

Implementation:

- `lib/sqlopt/optimize.c`

## Purpose

The runtime pipeline is:

1. parse text into `sqlexec_program`
2. optimize `sqlexec_program`
3. execute the final tree with `sqlexec_execute()`

`libsqlopt` owns step 2 only.

That separation matters because the project wants `main` to orchestrate
phases rather than mix planning with execution.

## Current Pass

The current optimizer is intentionally conservative and catalog-driven.

It:

- resolves the active database path through `<root>/sys/db.dbf`
- reads registered indexes from `<root>/sys/ndx.dbf`
- inspects the current table schema through DBF field descriptors
- matches only single-field indexes
- only considers filters whose `WHERE` tree is one simple comparison
- checks whether the comparison value matches the field type
- rewrites `table_scan` leaves under `filter` nodes to indexed probe
  nodes when the rewrite is safe

Current rewrites:

- equality predicates become `index_scan_eq`
- `<`, `<=`, `>`, `>=` predicates become `index_scan_range`

The optimizer deliberately keeps the `filter` node in place.
It does not currently rewrite join plans.

## What It Does Not Do Yet

- composite-index selection
- redundant filter removal
- multi-predicate planning
- `IN (...)` planning
- selectivity or cost estimation
- sibling reordering
- join optimization

## Public API

- `sqlopt_optimize()`

## Notes

- `libsqlopt` depends on the shared plan contract and uses
  `libsqlexec` helpers to rewrite nodes safely.
- The optimizer already reads the same on-disk catalogs the SQL shell
  uses for runtime maintenance, so planning decisions and DDL metadata
  stay aligned.
- The executor still performs full table scans today, so these rewrites
  currently improve the architecture and plan shape more than runtime
  speed.
