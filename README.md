# sql

`sql` is the hosted development program for the project. It runs
an interactive SQL shell backed by the small SQL parser and DBF storage
layer. The same source code compiles for CP/M with SDCC.

The root README describes the main program only. Library details live in
[lib/dbf/README.md](lib/dbf/README.md) and [lib/sql/README.md](lib/sql/README.md).

## What The Program Does

The current program can:

- create a database entry in the system catalog
- list databases from the system catalog
- create a table inside the currently selected database
- insert rows into a table
- select rows from a table with one simple `WHERE` condition
- update rows with one simple `WHERE` condition
- delete rows with one simple `WHERE` condition

## Project Layout

- `src/` contains the main program and platform I/O layer
- `include/` contains public headers
- `lib/dbf/` contains the DBF storage library
- `lib/sql/` contains the SQL parser library
- `tests/` contains automated tests
- `docs/` contains project notes
- `build/` contains compiler outputs
- `bin/` contains built programs and test binaries

## Build

```sh
make
```

This produces `bin/sql`.

Run the automated tests with:

```sh
make test
```

The hosted debug build uses `-std=c11 -Wall -Wextra -pedantic -g
-fsanitize=address,undefined`.

## Running The Shell

```sh
./bin/sql <root>
```

`<root>` is the path to the directory where all databases are stored.
The directory is created on first run if it does not already exist.

Example using `./db` as the storage root:

```sh
./bin/sql ./db
```

The shell prompt shows `> ` when no database is selected, and
`dbname> ` after a database has been created or is in use.

Exit the shell with **Ctrl+C**.

## Session Example

```
> CREATE DATABASE demo;
created demo
demo> CREATE TABLE people (name CHAR(16), age NUMERIC(3), born DATE);
created people
demo> INSERT INTO people VALUES ('alice', 18, 19900101);
inserted 1
demo> SHOW DATABASES;
 1 ./db/1 demo
demo> SELECT * FROM people;
alice | 18 | 19900101
1 row
demo> ^C
```

## Shell Editing

| Key       | Effect                     |
|-----------|----------------------------|
| Any key   | Echo and add to buffer     |
| Backspace | Erase last character       |
| Enter     | Submit and execute         |
| Ctrl+C    | Exit the shell             |

## Storage Layout

Under the root path the program creates:

```
<root>/
  sys/
    db.dbf        system catalog of all databases
  1/              first database (slot number assigned automatically)
    people.dbf    a table inside that database
  2/              second database
  ...
```

Slot numbers run from 1 to 15.

## Platform I/O

All terminal access is routed through two functions declared in
`src/platform.h`:

```c
int  read_char(void);
void write_char(char c);
```

The hosted build (`platform.c`) sets the terminal to raw mode so
characters arrive one at a time without echo.  The CP/M build uses BDOS
calls 8 (console input without echo) and 2 (console output) directly.

## Dependencies

- GCC for hosted development
- SDCC for CP/M compatibility checking
- GDB for the VS Code debug workflow

## Debugging

Press `F5` in VS Code to build and run the hosted debug binary.

## Notes

- Development and testing happen under GCC first.
- SDCC is used only after the hosted build is working.
- The SQL grammar is intentionally very small at this stage; see
  [lib/sql/README.md](lib/sql/README.md) for the full EBNF.
