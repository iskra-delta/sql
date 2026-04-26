# libsql

`libsql` is the tiny SQL parser used by the main program. It parses one
SQL string into a fixed C structure that later code can inspect and
execute.

Public header:

- `include/sql.h`

Implementation:

- `lib/sql/sql.c`

## Public Interface

The public entry point is:

- `int sql_parse(const char *text, sql_statement *statement);`

It parses one SQL statement from `text` into `statement` and returns:

- `0` on success
- `-1` on parse failure

## Public Data Structures

The parser writes into `sql_statement`, which contains:

- `type`
  The parsed statement kind
- `name`
  Used for object names such as database or table names
- `column_count` and `columns`
  Used by `CREATE TABLE`
- `select_all`
  Set for `SELECT *`
- `select_count` and `select_names`
  Used by projected `SELECT` column lists
- `where`
  Stores one optional `WHERE` predicate

The `where` field is a `sql_where` structure with:

- `active`
- `column_name`
- `operator`
- `value`

The value is stored as `sql_value`, which contains:

- `type`
- `text`

This library represents parsed syntax only. Execution happens in the
main program after parsing succeeds.

## Supported Statements

- `CREATE DATABASE name;`
- `SHOW DATABASES;`
- `CREATE TABLE name (...);`
- `SELECT * FROM table;`
- `SELECT column[, column ...] FROM table [WHERE column op value];`
- `INSERT INTO table VALUES (value[, value ...]);`
- `UPDATE table SET column = value[, column = value ...] [WHERE column op value];`
- `DELETE FROM table [WHERE column op value];`

## Supported Column Types

For `CREATE TABLE`, the parser understands:

- `CHAR(n)`
- `CHARACTER(n)`
- `NUMERIC(n[,d])`
- `DATE`
- `LOGICAL`

These are mapped into DBF-style field metadata inside `sql_column`.

## Supported WHERE Forms

The current `WHERE` support is intentionally small:

- one column only
- one comparison only
- no `AND`
- no `OR`
- no parentheses
- no aliases
- no grouping

Supported operators:

- `=`
- `!=`
- `<>`
- `<`
- `<=`
- `>`
- `>=`

Supported value kinds:

- quoted strings such as `'London'`
- unsigned numbers such as `18`
- identifiers

## How It Works

The parser is hand-written and intentionally small:

- it skips ASCII whitespace
- it matches keywords case-insensitively
- it reads identifiers and numbers into fixed-size buffers
- it uses simple recursive descent functions for each statement form

The output structure is fixed-size so the parser stays predictable and
friendly to small systems.

## Examples

### Example: Parse CREATE DATABASE

```c
#include "sql.h"

sql_statement statement;

if (sql_parse("CREATE DATABASE demo;", &statement) != 0) {
    return 1;
}

if (statement.type == sql_statement_create_database) {
    /* statement.name == "demo" */
}
```

### Example: Parse SHOW DATABASES

```c
#include "sql.h"

sql_statement statement;

if (sql_parse("SHOW DATABASES;", &statement) != 0) {
    return 1;
}

if (statement.type == sql_statement_show_databases) {
    /* no extra payload is needed */
}
```

### Example: Parse CREATE TABLE

```c
#include "sql.h"

sql_statement statement;

if (sql_parse("CREATE TABLE people (name CHAR(16), age NUMERIC(3));",
    &statement) != 0) {
    return 1;
}

if (statement.type == sql_statement_create_table) {
    /* statement.name == "people" */
    /* statement.column_count == 2 */
    /* statement.columns[0].name == "name" */
    /* statement.columns[1].name == "age" */
}
```

### Example: Parse SELECT *

```c
#include "sql.h"

sql_statement statement;

if (sql_parse("SELECT * FROM people;", &statement) != 0) {
    return 1;
}

if (statement.type == sql_statement_select && statement.select_all) {
    /* statement.name == "people" */
}
```

### Example: Parse SELECT With WHERE

```c
#include "sql.h"

sql_statement statement;

if (sql_parse("SELECT name, age FROM people WHERE age >= 18;",
    &statement) != 0) {
    return 1;
}

if (statement.type == sql_statement_select) {
    /* statement.name == "people" */
    /* statement.select_count == 2 */
    /* statement.select_names[0] == "name" */
    /* statement.select_names[1] == "age" */
    /* statement.where.active != 0 */
    /* statement.where.column_name == "age" */
    /* statement.where.value.text == "18" */
}
```

### Example: Parse INSERT

```c
#include "sql.h"

sql_statement statement;

if (sql_parse("INSERT INTO people VALUES ('alice', 18);",
    &statement) != 0) {
    return 1;
}

if (statement.type == sql_statement_insert) {
    /* statement.name == "people" */
    /* statement.value_count == 2 */
    /* statement.values[0].text == "alice" */
    /* statement.values[1].text == "18" */
}
```

### Example: Parse UPDATE

```c
#include "sql.h"

sql_statement statement;

if (sql_parse("UPDATE people SET age = 19 WHERE age = 18;",
    &statement) != 0) {
    return 1;
}

if (statement.type == sql_statement_update) {
    /* statement.name == "people" */
    /* statement.assignment_count == 1 */
    /* statement.assignments[0].column_name == "age" */
    /* statement.assignments[0].value.text == "19" */
}
```

### Example: Parse DELETE

```c
#include "sql.h"

sql_statement statement;

if (sql_parse("DELETE FROM people WHERE age < 18;", &statement) != 0) {
    return 1;
}

if (statement.type == sql_statement_delete) {
    /* statement.name == "people" */
    /* statement.where.active != 0 */
}
```

## Notes

- The parser expects one statement ending in `;`
- Extra trailing text causes a parse failure
- All names are stored in fixed-size buffers
- The parser is intentionally strict and small

## Grammar

```ebnf
statement       = create-database
                | show-databases
                | create-table
                | select
                | insert
                | update
                | delete
                ;

create-database = "CREATE" "DATABASE" name ";" ;

show-databases  = "SHOW" "DATABASES" ";" ;

create-table    = "CREATE" "TABLE" name "(" column-def { "," column-def } ")" ";" ;

column-def      = name column-type { constraint } ;

column-type     = "CHAR" "(" number ")"
                | "CHARACTER" "(" number ")"
                | "NUMERIC" "(" number [ "," number ] ")"
                | "DATE"
                | "LOGICAL"
                ;

select          = "SELECT" select-list "FROM" name [ where-clause ] ";" ;

select-list     = "*"
                | name { "," name }
                ;

insert          = "INSERT" "INTO" name "VALUES" "(" value { "," value } ")" ";" ;

update          = "UPDATE" name "SET" assignment { "," assignment } [ where-clause ] ";" ;

assignment      = name "=" value ;

delete          = "DELETE" "FROM" name [ where-clause ] ";" ;

where-clause    = "WHERE" name operator value ;

operator        = "=" | "!=" | "<>" | "<" | "<=" | ">" | ">=" ;

value           = string | number | name ;

string          = "'" { character } "'" ;

number          = digit { digit } ;

name            = letter { letter | digit | "_" } ;

letter          = "A" | ... | "Z" | "a" | ... | "z" | "_" ;

digit           = "0" | ... | "9" ;
```
