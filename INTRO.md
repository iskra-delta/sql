# sql — A Tiny SQL Shell

## What It Is

`sql` is an interactive SQL shell for creating and querying small
databases.  It runs on Linux and on Z80-based CP/M machines such as the
Iskra Delta Partner.  Databases are stored as ordinary dBase III DBF
files, which any tool that understands that format can also read.

## Starting the Shell

```sh
./sql <root>
```

`<root>` is the directory where all your databases will be stored.  It is
created automatically on first run.

```sh
./sql ./db
```

The prompt shows `> ` when no database is active, and `dbname> ` once you
have created or selected one.  Exit with **Ctrl+C**.

## Working with Databases

Create a database and start using it:

```
> CREATE DATABASE demo;
created demo
demo>
```

List all databases:

```
demo> SHOW DATABASES;
 1 ./db/1 demo
```

## Working with Tables

Create a table inside the active database:

```
demo> CREATE TABLE people (name CHAR(16), age NUMERIC(3), active LOGICAL);
created people
```

Supported column types:

| Type | Description |
|---|---|
| `CHAR(n)` | Fixed-width text, up to *n* characters |
| `NUMERIC(n)` | Integer, stored in *n* character positions |
| `LOGICAL` | Single character boolean flag (`T` or `F`) |

## Inserting Rows

Values must be given in the same order as the columns were defined:

```
demo> INSERT INTO people VALUES ('alice', 30, T);
inserted 1
demo> INSERT INTO people VALUES ('bob', 25, F);
inserted 1
demo> INSERT INTO people VALUES ('carol', 30, T);
inserted 1
```

## Selecting Rows

Select all columns:

```
demo> SELECT * FROM people;
alice | 30 | T
bob | 25 | F
carol | 30 | T
3 rows
```

Select specific columns:

```
demo> SELECT name, age FROM people;
alice | 30
bob | 25
carol | 30
3 rows
```

Filter with a `WHERE` clause:

```
demo> SELECT * FROM people WHERE age = 30;
alice | 30 | T
carol | 30 | T
2 rows
```

Supported comparison operators: `=`  `<>`  `<`  `<=`  `>`  `>=`

## Updating Rows

```
demo> UPDATE people SET age = 31 WHERE name = 'alice';
1 updated
```

## Deleting Rows

```
demo> DELETE FROM people WHERE active = F;
1 deleted
```

## How Data Is Stored on Disk

Under the storage root the shell creates:

```
<root>/
  sys/
    db.dbf          catalog of all databases
  1/                first database (slot number assigned automatically)
    people.dbf      a table inside that database
  2/                second database
    ...
```

Each database occupies a numbered slot directory (1 through 15).  Every
table is a single DBF file named after the table.  Deleted rows are
marked inside the file and remain on disk until the file is compacted by
an external tool; the shell itself does not reclaim deleted-record space.

## Limits

- Up to 15 databases per storage root
- Up to 16 columns per table
- Column names up to 16 characters
- Values up to 32 characters
- One `WHERE` condition per statement; no joins or subqueries
