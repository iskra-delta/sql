# libsql

`libsql` is a tiny SQL parser for the project command shell.

Current statements:

- `CREATE DATABASE name;`
- `SHOW DATABASES;`

The parser uses:

- a minimal hand-written lexer
- a minimal recursive descent parser

Public header:

- `include/sql.h`

Implementation:

- `lib/sql/sql.c`
