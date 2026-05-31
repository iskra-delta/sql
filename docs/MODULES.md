# Module Architecture

This document describes both the current hosted architecture and the
target CP/M overlay architecture that motivated the code structure.
The important rule: the current code already respects the future phase
boundaries, even before the CP/M loader exists.

---

## Current Architecture

### Communication contract

All three pipeline phases share one `sql_context` struct defined in
`include/sqlctx.h`:

```c
typedef struct sql_context {
    const char      *root;            /* storage root path              */
    char             current_db[17];  /* active db; DDL may change this */
    const char      *text;            /* SQL input, set before parse    */
    sqlexec_program  program;         /* IR written by parse, opt reads */
    sqlexec_io       io;              /* write_char output callback     */
    int              result;
} sql_context;
```

The three entry points each take this struct and nothing else:

```c
int sql_run(sql_context *ctx);      /* parse + view expansion */
int sqlopt_run(sql_context *ctx);   /* catalog-driven rewrites */
int sqlexec_run(sql_context *ctx);  /* tree execution */
```

This is the `module_run(sql_context *)` convention used by the future
CP/M overlay loader.

### Source layout

```
src/
  main.c        shell loop + orchestrator (calls sql_run, sqlopt_run, sqlexec_run)
  platform.c    terminal I/O abstraction (raw termios / BDOS)

include/
  sqltypes.h    shared value and schema types
  sqlexec.h     execution-tree format and arena API
  sqlopt.h      optimizer API
  sql.h         parser API
  sqlctx.h      sql_context and module entry points

lib/
  dbf/          dBase III DBF reader/writer (kernel)
  ndx/          dBase III NDX B-tree library (kernel)
  shared/       helpers shared by all phases (kernel)
    shared.c    string utils, field ops, comparisons
    catalog.c   catalog/table file access, view catalog
    where.c     WHERE evaluation, row_source struct
  sql/
    sql.c       SQL parser (parse phase)
    program.c   execution-tree builder + view expansion (parse phase)
  sqlopt/
    optimize.c  catalog-driven optimizer (optimize phase)
  sqlexec/
    sqlexec.c   node arena management (kernel)
    execute.c   dispatch: routes to ddl / select / mutate
    execute_ddl.c      DDL executors
    execute_select.c   SELECT executor
    execute_mutate.c   INSERT, UPDATE, DELETE executors
    execute_scan.c     ndx_scan callbacks for index-driven scans
    execute_subquery.c run_subquery + delete_temp + built-in views
    exec_impl.h  internal env struct, entry-point declarations
```

### Shared helpers (lib/shared/)

Code needed by more than one phase lives in `lib/shared/` so it is
compiled once and referenced by all phases without duplication.

| File | Contents | Used by |
|---|---|---|
| `shared.c` | string utils, field ops, parse_integer_text | all phases |
| `catalog.c` | ensure_catalog, find_database_path, open_table_file, view catalog | optimizer, executor |
| `where.c` | row_source, where_matches, resolve_field_ref | executor |

---

## Target CP/M Architecture

On a Z80 machine with 48 KB effective TPA, the executable is split
into a resident kernel and loadable module binaries.

### Memory budget

| Region | Size | Notes |
|---|---|---|
| CP/M system (BDOS + BIOS) | ~8 KB | fixed, at top |
| Resident kernel | ~6 KB | always present |
| Communication area (sql_context) | ~5 KB | fixed address |
| Module slot | ~8 KB | one module at a time |
| Data / stack | ~21 KB | |

### Residency

The kernel is always loaded and contains:
- shell loop + dispatcher + module loader
- platform I/O (read_char / write_char)
- DBF library
- NDX library
- lib/shared (all helpers)
- sqlexec.c arena management

Everything else loads on demand.

### Module map

| Module | Source | Loaded for |
|---|---|---|
| `MOD_PARSE.BIN` | lib/sql (sql.c + program.c) | every statement |
| `MOD_OPT.BIN` | lib/sqlopt (optimize.c) | every statement |
| `MOD_DDL.BIN` | execute_ddl.c | CREATE/DROP/USE/SHOW + VIEW |
| `MOD_SELECT.BIN` | execute_select.c + execute_scan.c | SELECT |
| `MOD_MUTATE.BIN` | execute_mutate.c + execute_scan.c | INSERT/UPDATE/DELETE |
| `MOD_SUBQ.BIN` | execute_subquery.c | view/subquery materialisation |

### Load sequence

```
read line (kernel)
  load MOD_PARSE  → sql_run  → unload
  load MOD_OPT    → sqlopt_run → unload
  if DDL:     load MOD_DDL    → sqlexec_run → unload
  if SELECT:  load MOD_SUBQ (if view/subquery) → unload
              load MOD_SELECT → sqlexec_run → unload
  if mutate:  load MOD_SUBQ (if view/subquery) → unload
              load MOD_MUTATE → sqlexec_run → unload
output (kernel)
```

Three disk loads per plain statement. For subquery/view statements,
four or five. On a CP/M hard disk this is acceptable.

### Module entry convention

Every module binary exports one symbol at offset zero:

```c
int module_run(sql_context *ctx);   /* 0 = ok, -1 = error */
```

The kernel writes `ctx->text`, calls `module_run`, checks `ctx->result`.
The `sqlexec_program` arena is reused across phases; only `subquery_text`
is overwritten per-query.

### Fixed slot address

CP/M `.COM` files load at 0x0100 and are not relocatable. The simplest
approach: compile all modules with a fixed `--code-loc` matching the
slot start address (e.g. `0x2000`). The kernel occupies
`0x0100`–`0x1FFF`.

---

## Optimisation: skip reload for consecutive same-type statements

The dispatcher tracks which module is currently loaded. If two
consecutive statements map to the same module (two INSERTs, a sequence
of DDL), the module is not unloaded and reloaded between statements.
This is the single most impactful optimisation for interactive use.
