# employee

- Source: [Bytebase employee sample database](https://github.com/bytebase/employee-sample-database)
- License / provenance: MIT

## Usage

```sh
./bin/sql tests/samples/employee
```

Then run:

```sql
USE employee;
SHOW VIEWS;
```

## Notes

- This uses the repository's small SQLite dataset.
- Date values were normalized to YYYYMMDD.
- `bootstrap.sql` creates the database slot and catalogs.
- `schema.sql` is the imported logical schema reference.
- Table names were shortened to fit the shell identifier limit.
- DBF field names were shortened to 11 bytes when needed.

## Tables

| Source | Imported | Rows | Indexed columns |
|---|---|---:|---|
| `department` | `department` | 9 | deptno |
| `dept_emp` | `dept_emp` | 1103 | empno, deptno |
| `dept_manager` | `dept_manager` | 16 | empno, deptno |
| `employee` | `employee` | 1000 | empno, lastname |
| `expected_value` | `expected_value` | 6 | tablename |
| `found_value` | `found_value` | 0 | tablename |
| `salary` | `salary` | 9488 | empno, fromdate |
| `tchecksum` | `tchecksum` | 0 | - |
| `title` | `title` | 1470 | empno, title, fromdate |

## Column Mapping

### `department`

| Source | Imported | Type |
|---|---|---|
| `dept_no` | `deptno` | `CHAR(4)` |
| `dept_name` | `deptname` | `CHAR(18)` |

### `dept_emp`

| Source | Imported | Type |
|---|---|---|
| `emp_no` | `empno` | `NUMERIC(5)` |
| `dept_no` | `deptno` | `CHAR(4)` |
| `from_date` | `fromdate` | `DATE` |
| `to_date` | `todate` | `DATE` |

### `dept_manager`

| Source | Imported | Type |
|---|---|---|
| `emp_no` | `empno` | `NUMERIC(5)` |
| `dept_no` | `deptno` | `CHAR(4)` |
| `from_date` | `fromdate` | `DATE` |
| `to_date` | `todate` | `DATE` |

### `employee`

| Source | Imported | Type |
|---|---|---|
| `emp_no` | `empno` | `NUMERIC(5)` |
| `birth_date` | `birthdate` | `DATE` |
| `first_name` | `firstname` | `CHAR(13)` |
| `last_name` | `lastname` | `CHAR(14)` |
| `gender` | `gender` | `CHAR(1)` |
| `hire_date` | `hiredate` | `DATE` |

### `expected_value`

| Source | Imported | Type |
|---|---|---|
| `table_name` | `tablename` | `CHAR(12)` |
| `recs` | `recs` | `NUMERIC(4)` |
| `crc_md5` | `crcmd5` | `CHAR(32)` |

### `found_value`

| Source | Imported | Type |
|---|---|---|
| `table_name` | `tablename` | `CHAR(1)` |
| `recs` | `recs` | `NUMERIC(1)` |
| `crc_md5` | `crcmd5` | `CHAR(1)` |

### `salary`

| Source | Imported | Type |
|---|---|---|
| `emp_no` | `empno` | `NUMERIC(5)` |
| `amount` | `amount` | `NUMERIC(6)` |
| `from_date` | `fromdate` | `DATE` |
| `to_date` | `todate` | `DATE` |

### `tchecksum`

| Source | Imported | Type |
|---|---|---|
| `chk` | `chk` | `CHAR(1)` |

### `title`

| Source | Imported | Type |
|---|---|---|
| `emp_no` | `empno` | `NUMERIC(5)` |
| `title` | `title` | `CHAR(18)` |
| `from_date` | `fromdate` | `DATE` |
| `to_date` | `todate` | `DATE` |
