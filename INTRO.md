# sql — Shell Guide

## Starting the Shell

```sh
./bin/sql <root>
```

`<root>` is the directory where all your databases are stored.

```sh
./bin/sql ./db
```

The prompt shows `> ` when no database is active, and `dbname> ` once
you have created or selected one. Exit with **Ctrl+C**.

---

## Databases

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

Switch to an existing database:

```
> USE demo;
using demo
demo>
```

Delete a database (the slot directory remains on disk):

```
demo> DROP DATABASE demo;
dropped demo
>
```

---

## Tables

Create a table inside the active database:

```
demo> CREATE TABLE people (name CHAR(16), age NUMERIC(3), active LOGICAL);
created people
```

Supported column types:

| Type | Description |
|---|---|
| `CHAR(n)` | Fixed-width text, up to *n* characters |
| `NUMERIC(n[,d])` | Number stored in *n* character positions, *d* decimals |
| `DATE` | Date in `YYYYMMDD` format |
| `LOGICAL` | Single character flag (`T` or `F`) |

Drop a table (also removes its registered indexes):

```
demo> DROP TABLE people;
dropped people
```

---

## Rows

Insert rows — values must match the column order:

```
demo> INSERT INTO people VALUES ('alice', 30, T);
inserted 1
demo> INSERT INTO people VALUES ('bob', 25, F);
inserted 1
demo> INSERT INTO people VALUES ('carol', 30, T);
inserted 1
```

Select all columns:

```
demo> SELECT * FROM people;
alice | 30 | T
bob | 25 | F
carol | 30 | T
3 rows
```

Select specific columns with a filter:

```
demo> SELECT name, age FROM people WHERE age = 30;
alice | 30
carol | 30
2 rows
```

Count rows:

```
demo> SELECT COUNT(*) FROM people WHERE active = T;
2
```

Update matching rows:

```
demo> UPDATE people SET age = 31 WHERE name = 'alice';
1 updated
```

Delete matching rows:

```
demo> DELETE FROM people WHERE active = F;
1 deleted
```

---

## WHERE Expressions

The WHERE clause supports `AND`, `OR`, `IN (...)`, parentheses, and
the operators `=`, `<>`, `!=`, `<`, `<=`, `>`, `>=`.

```sql
SELECT * FROM people WHERE age > 25 AND active = T;
SELECT * FROM people WHERE city = 'LON' OR city = 'NYC';
SELECT * FROM people WHERE age IN (25, 30, 35);
SELECT * FROM people WHERE (age > 20 AND active = T) OR name = 'bob';
```

---

## Joins

One inner join per SELECT:

```
demo> CREATE TABLE cities (code CHAR(3), name CHAR(16));
demo> INSERT INTO cities VALUES ('LON', 'London');
demo> INSERT INTO cities VALUES ('NYC', 'New York');
demo> CREATE TABLE employees (name CHAR(16), city CHAR(3), age NUMERIC(3));
demo> INSERT INTO employees VALUES ('alice', 'LON', 30);
demo> INSERT INTO employees VALUES ('bob', 'NYC', 25);

demo> SELECT e.name, c.name FROM employees AS e
...>  JOIN cities c ON e.city = c.code;
alice | London
bob | New York
2 rows
```

Aliases work in both `SELECT` and `WHERE`:

```sql
SELECT e.name AS person, c.name AS location
FROM employees AS e JOIN cities c ON e.city = c.code
WHERE c.code = 'LON';
```

---

## Indexes

Create an index to speed up equality and range queries:

```
demo> CREATE INDEX age_idx ON people (age);
created index age_idx
```

Create a composite character index:

```
demo> CREATE UNIQUE INDEX name_city ON people (name, city);
created index name_city
```

The optimizer automatically uses registered indexes for single-field
equality and range conditions. Indexes are rebuilt after every INSERT,
UPDATE, or DELETE.

---

## Views

Create a named view that stores a SELECT query:

```
demo> CREATE VIEW active_people AS SELECT name, age FROM people WHERE active = T;
created view active_people
```

Query a view exactly like a table:

```
demo> SELECT * FROM active_people;
alice | 31
carol | 30
2 rows
```

Filter the view with an outer WHERE:

```
demo> SELECT name FROM active_people WHERE age > 30;
alice
1 row
```

List views in the current database:

```
demo> SHOW VIEWS;
active_people
```

Drop a view:

```
demo> DROP VIEW active_people;
dropped view active_people
```

---

## Nested SELECT

Use an inline subquery as the FROM source:

```
demo> SELECT name FROM (SELECT name, age FROM people WHERE active = T) AS p
...>  WHERE age > 28;
alice
1 row
```

---

## System Views

Query live metadata through built-in views. A database must be
selected for `sys_tables`, `sys_fields`, `sys_indexes`, and
`sys_views`.

List all tables in the current database:

```
demo> SELECT * FROM sys_tables;
people
1 row
```

Inspect a table's columns:

```
demo> SELECT name, type, length FROM sys_fields WHERE table_name = 'people';
name   | C | 16
age    | N |  3
active | L |  1
3 rows
```

List registered indexes:

```
demo> SELECT * FROM sys_indexes;
age_idx | people | age  | N
1 row
```

List all databases (no active database required):

```
> SELECT * FROM sys_databases;
demo | ./db/1 | 1
1 row
```

---

## Storage on Disk

```text
<root>/
  sys/
    db.dbf     catalog of all databases
    ndx.dbf    catalog of all registered indexes
    vw.dbf     catalog of all user-defined views
  1/            first database slot
    table.dbf
    index.ndx
  2/
  ...
```

- Up to 15 databases per storage root (slots 1–15)
- Up to 16 columns per table
- Up to 16 characters per identifier
- Deleted rows are soft-deleted; space is not reclaimed by the shell

---

## Limits

| Resource | Limit |
|---|---|
| Databases per root | 15 |
| Columns per table | 16 |
| Column / identifier length | 16 characters |
| String value length | 32 characters |
| WHERE conditions (nodes) | 16 |
| One JOIN per SELECT | — |
| One WHERE per statement | — |
| No `ORDER BY`, `GROUP BY`, aggregates beyond `COUNT(*)` | — |
