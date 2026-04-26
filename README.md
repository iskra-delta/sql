# sql_demo

`sql_demo` is the hosted development program for the project. It uses
the small SQL parser and DBF storage layer to manage a tiny catalog of
databases and create DBF-backed tables.

The root README describes the main program only. Library details live in
`lib/dbf/README.md` and `lib/sql/README.md`.

## What The Program Does

The current program can:

- create a database entry in the system catalog
- list databases from the system catalog
- create a table inside a selected database
- parse a small `SELECT ... FROM ... WHERE ...` statement

The current program cannot yet execute `SELECT`. It reports:

```text
select execution is not implemented yet
```

## Project Layout

- `src/` contains the main program
- `include/` contains public headers
- `lib/dbf/` contains the DBF storage library
- `lib/sql/` contains the SQL parser library
- `tests/` contains automated tests
- `docs/` contains project notes
- `build/` contains compiler outputs
- `bin/` contains built programs and test binaries

## Build

Build with GCC first:

```sh
make
```

This produces:

- `bin/sql_demo`

Run the automated tests with:

```sh
make test
```

The hosted debug build uses:

- `-std=c11`
- `-Wall -Wextra -pedantic`
- `-g`
- `-fsanitize=address,undefined`

## Command Line Usage

The program accepts SQL text on the command line:

```sh
./bin/sql_demo [--root path] [--database name] "SQL;"
```

Options:

- `--root path`
  Sets the database root directory. The default is `db`.
- `--database name`
  Selects the database used by statements such as `CREATE TABLE`.

## Examples

Create a database:

```sh
./bin/sql_demo "CREATE DATABASE demo;"
```

Show known databases:

```sh
./bin/sql_demo "SHOW DATABASES;"
```

Create a table in one database:

```sh
./bin/sql_demo --database demo \
    "CREATE TABLE people (name CHAR(16), age NUMERIC(3));"
```

Parse a simple select:

```sh
./bin/sql_demo --database demo \
    "SELECT name, age FROM people WHERE age >= 18;"
```

This last command parses successfully today, but execution is not yet
implemented.

## Storage Layout

Under the root path, the program creates:

- `sys/db.dbf`
  The system catalog of databases
- numbered database directories such as `1/`, `2/`, and so on
- table files such as `people.dbf` inside the selected database folder

## Dependencies

- GCC for hosted development
- SDCC for compatibility checking
- GDB for the VS Code debug workflow

## Debugging

Press `F5` in VS Code to build and run the hosted debug binary.

## Notes

- Development and testing happen under GCC first.
- SDCC is used only after the hosted build is working.
- The SQL grammar is intentionally very small at this stage.
