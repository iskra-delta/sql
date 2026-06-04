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
- searches top-level conjunctive `WHERE` terms for simple literal
  comparisons
- checks whether the comparison value matches the field type
- annotates `table_scan` leaves with indexed equality or range access
  when the choice is safe
- prunes top-level predicates already enforced by that access and
  compacts the remaining `WHERE` tree

Current rewrites:

- equality predicates become `table_scan` with equality access
- `<`, `<=`, `>`, `>=` predicates become `table_scan` with range access

It does not currently rewrite join plans.

## What It Does Not Do Yet

- composite-index selection
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
- The executor now consumes those access annotations directly, so these
  rewrites improve runtime as well as plan shape.
