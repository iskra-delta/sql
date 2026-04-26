# SQL Notes

Current SQL support is intentionally tiny.

Grammar:

- `CREATE DATABASE <name>;`
- `SHOW DATABASES;`
- `CREATE TABLE <name> (...);`
- `SELECT * FROM <table>;`
- `SELECT <column>[, <column> ...] FROM <table> [WHERE <column> <op> <value>];`
- `INSERT INTO <table> VALUES (<value>[, <value> ...]);`
- `UPDATE <table> SET <column> = <value>[, <column> = <value> ...] [WHERE <column> <op> <value>];`
- `DELETE FROM <table> [WHERE <column> <op> <value>];`

Near-term behavior:

- `CREATE DATABASE` allocates one numeric folder from `1` to `15`
- catalog data is stored in `db/sys/db.dbf`
- `SHOW DATABASES` reads and prints the catalog
- `CREATE TABLE` maps a small SQL type set onto DBF fields
- `SELECT` scans rows from the current database table and prints
  projected values with one simple `WHERE` comparison
- `INSERT` appends one row by position using one `VALUES` list
- `UPDATE` rewrites matching rows using one `SET` list and one simple
  `WHERE` comparison
- `DELETE` marks matching rows deleted using one optional simple
  `WHERE` comparison
