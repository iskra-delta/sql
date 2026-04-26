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
- `SELECT` currently parses projections and one simple `WHERE`
  comparison but is not executed yet
- `INSERT` currently parses one `VALUES` list but is not executed yet
- `UPDATE` currently parses one `SET` list and one simple `WHERE`
  comparison but is not executed yet
- `DELETE` currently parses one optional simple `WHERE` comparison but
  is not executed yet
