# sql_demo

This repository contains the first DBF storage layer for a tiny SQL
server aimed at CP/M-class machines. The current code is intentionally
small and focuses only on creating, appending, and reading fixed-length
dBase III style records.

## Layout

- `src/` contains all project `.c` files, including `dbf.c`
- `include/` contains public headers
- `lib/` is reserved for third-party or prebuilt libraries
- `bin/` contains generated programs and test output files
- `build/` contains all compiler artifacts
- `docs/` stores design and implementation notes
- `tests/` stores the local test harness and test data

## Current DBF API

The public API lives in `include/dbf.h`.
The `libdbf` implementation lives in `lib/dbf/dbf.c`.

The API currently provides only:

- `dbf_create()`
- `dbf_open()`
- `dbf_close()`
- `dbf_append()`
- `dbf_read()`

Records are handled as raw fixed-length byte arrays. This keeps the
implementation small and avoids bringing in field parsers too early.

## Build

Build and run with GCC first:

```sh
make
./bin/sql_demo
```

The default build enables:

- `-std=c11`
- `-Wall -Wextra -pedantic`
- `-g`
- `-fsanitize=address,undefined`

Run the automated test harness with:

```sh
make test
```

## Dependencies

- GCC for hosted development
- SDCC for compatibility checking
- GDB if you want to use the VS Code `F5` debug profile

## Usage

The sample program in `src/main.c` creates `bin/demo.dbf`, appends one
record, reopens the file, and reads it back.

## Debugging

Press `F5` in VS Code to build the debug binary and launch `gdb`
against `bin/sql_demo`.

## SDCC

The `sdcc_check` target compiles the DBF code with SDCC so the source
stays portable while the project is still debugged under GCC.
