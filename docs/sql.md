# SQL Notes

Current SQL support is intentionally tiny.

Grammar:

- `CREATE DATABASE <name>;`
- `SHOW DATABASES;`

Near-term behavior:

- `CREATE DATABASE` allocates one numeric folder from `1` to `15`
- catalog data is stored in `db/sys/db.dbf`
- `SHOW DATABASES` reads and prints the catalog
