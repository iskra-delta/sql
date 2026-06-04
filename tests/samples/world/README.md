# world

- Source: [MySQL world sample database](https://downloads.mysql.com/docs/world-db.zip)
- License / provenance: Sample data from Statistics Finland via MySQL

## Usage

```sh
./bin/sql tests/samples/world
```

Then run:

```sql
USE world;
SHOW VIEWS;
```

## Notes

- This is the official MySQL world sample.
- Floating-point country metrics are stored as text because the current SQL numeric path is integer-only.
- `bootstrap.sql` creates the database slot and catalogs.
- `schema.sql` is the imported logical schema reference.
- Table names were shortened to fit the shell identifier limit.
- DBF field names were shortened to 11 bytes when needed.

## Tables

| Source | Imported | Rows | Indexed columns |
|---|---|---:|---|
| `city` | `city` | 4079 | id, countrycode, name |
| `country` | `country` | 239 | code, name |
| `countrylanguage` | `countrylanguage` | 984 | countrycode |

## Column Mapping

### `city`

| Source | Imported | Type |
|---|---|---|
| `ID` | `id` | `NUMERIC(4)` |
| `Name` | `name` | `CHAR(34)` |
| `CountryCode` | `countrycode` | `CHAR(3)` |
| `District` | `district` | `CHAR(22)` |
| `Population` | `population` | `NUMERIC(8)` |

### `country`

| Source | Imported | Type |
|---|---|---|
| `Code` | `code` | `CHAR(3)` |
| `Name` | `name` | `CHAR(44)` |
| `Continent` | `continent` | `CHAR(13)` |
| `Region` | `region` | `CHAR(25)` |
| `SurfaceArea` | `surfacearea` | `CHAR(11)` |
| `IndepYear` | `indepyear` | `NUMERIC(5)` |
| `Population` | `population` | `NUMERIC(10)` |
| `LifeExpectancy` | `lifeexpecta` | `CHAR(4)` |
| `GNP` | `gnp` | `CHAR(10)` |
| `GNPOld` | `gnpold` | `CHAR(10)` |
| `LocalName` | `localname` | `CHAR(44)` |
| `GovernmentForm` | `governmentf` | `CHAR(44)` |
| `HeadOfState` | `headofstate` | `CHAR(36)` |
| `Capital` | `capital` | `NUMERIC(4)` |
| `Code2` | `code2` | `CHAR(2)` |

### `countrylanguage`

| Source | Imported | Type |
|---|---|---|
| `CountryCode` | `countrycode` | `CHAR(3)` |
| `Language` | `language` | `CHAR(25)` |
| `IsOfficial` | `isofficial` | `LOGICAL` |
| `Percentage` | `percentage` | `CHAR(5)` |
