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
| `CHARACTER(n)` | Alias for `CHAR(n)` |
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

Insert with named columns (other columns are left blank):

```
demo> INSERT INTO people (name, age) VALUES ('dave', 28);
inserted 1
```

Select all columns:

```
demo> SELECT * FROM people;
alice | 30 | T
bob | 25 | F
carol | 30 | T
dave | 28 |
4 rows
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

The WHERE clause supports `AND`, `OR`, `NOT`, parentheses, `LIKE`,
`BETWEEN`, `IS NULL`, `IS NOT NULL`, `IN (...)`, and the comparison
operators `=`, `<>`, `!=`, `<`, `<=`, `>`, `>=`.

```sql
SELECT * FROM people WHERE age > 25 AND active = T;
SELECT * FROM people WHERE city = 'LON' OR city = 'NYC';
SELECT * FROM people WHERE age IN (25, 30, 35);
SELECT * FROM people WHERE age BETWEEN 20 AND 35;
SELECT * FROM people WHERE name LIKE 'a%';
SELECT * FROM people WHERE name IS NOT NULL;
SELECT * FROM people WHERE (age > 20 AND active = T) OR name = 'bob';
```

---

## Joins

Up to 3 JOIN clauses or comma-separated sources per SELECT:

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

## Aggregates and GROUP BY

```
demo> SELECT city, COUNT(*) FROM employees GROUP BY city;
LON | 1
NYC | 1
2 rows

demo> SELECT city, MAX(age), MIN(age) FROM employees GROUP BY city
...>  HAVING MAX(age) > 20;
LON | 30 | 30
NYC | 25 | 25
2 rows
```

Supported aggregate functions: `COUNT(*)`, `COUNT(col)`, `MIN(col)`,
`MAX(col)`, `SUM(col)`, `AVG(col)`.

`SELECT DISTINCT` is also supported:

```sql
SELECT DISTINCT city FROM employees;
```

---

## Indexes

Create an index to speed up equality and range queries:

```
demo> CREATE INDEX age_idx ON people (age);
created index age_idx
```

Create a unique index:

```
demo> CREATE UNIQUE INDEX name_idx ON people (name);
created index name_idx
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

Use a subquery in a WHERE condition:

```sql
SELECT name FROM people
WHERE age IN (SELECT age FROM employees WHERE city = 'LON');
```

---

## Transactions

Group multiple statements into an atomic unit:

```
demo> BEGIN;
demo> INSERT INTO people VALUES ('eve', 22, T);
inserted 1
demo> UPDATE people SET age = 32 WHERE name = 'alice';
1 updated
demo> COMMIT;
```

Roll back all changes since BEGIN:

```
demo> BEGIN;
demo> DELETE FROM people WHERE active = F;
1 deleted
demo> ROLLBACK;
```

While a transaction is open, SELECT shows the pending changes as if
they were already committed.  COMMIT verifies that every updated or
deleted row is unchanged since the transaction started; if another
writer modified a row the commit fails — issue ROLLBACK and retry.

DDL (CREATE, DROP, USE) is not allowed inside an open transaction.

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
| String value length | 33 characters |
| Row sources per SELECT | 4 (1 base + 3 JOINs) |
| WHERE / HAVING nodes | 24 |
| WHERE / HAVING values | 16 |
| Distinct / grouped result rows per SELECT | 8 |
| Predicate subqueries per statement | 2 |
| Subquery nesting depth | 1 |
| No `ORDER BY` | — |
