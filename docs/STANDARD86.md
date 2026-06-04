# SQL-86 Implementation Prompt

Use the prompt below when we want to move this project from its current
small CP/M-oriented SQL dialect toward a true SQL-86 implementation.

The goal of this document is not to restate the standard from memory.
The goal is to give the next implementation pass a disciplined brief:
what the engine already has, what is still missing, what is non-standard
today, and what must be delivered to claim SQL-86 coverage honestly.

Before doing any coding, obtain a real copy of the target standard
edition and treat it as the source of truth:

- ANSI X3.135-1986 SQL
- or the matching ISO text used for the same language generation

If the reference text differs from this document, follow the standard
text and update this file.

---

## Prompt

Implement a SQL-86 conformance pass for this repository.

Work in the existing CP/M-friendly architecture and storage model, but
do not confuse current project constraints with the SQL-86 language
itself. The result must clearly separate:

- standard SQL-86 behavior
- deliberate project extensions
- deliberate project deviations

### Current Engine Snapshot

The engine already supports a useful but non-standard subset:

- `CREATE DATABASE`, `USE`, `SHOW DATABASES`
- `CREATE TABLE`, `DROP TABLE`
- `CREATE INDEX`
- `CREATE VIEW`, `DROP VIEW`, `SHOW VIEWS`
- `SELECT`, `INSERT`, `UPDATE`, `DELETE`
- `WHERE` with `AND`, `OR`, `NOT`, `IN`, `BETWEEN`, `LIKE`,
  `IS NULL`, `IS NOT NULL`, `EXISTS`, quantified `ANY` / `ALL`,
  parentheses, simple comparisons, and bounded uncorrelated predicate
  subqueries
- bounded multi-source inner joins using comma-separated table lists
  and `JOIN ... ON ...`
- inline subqueries in `FROM`
- `SELECT DISTINCT`, explicit `SELECT ALL`, `GROUP BY`, `HAVING`
- aggregate functions: `COUNT(*)`, `COUNT(col)`, `MIN(col)`,
  `MAX(col)`, `SUM(col)`, `AVG(col)`
- scalar function: `TRIM(col)`

The current system is intentionally bounded:

- fixed plan arena
- fixed predicate pools
- fixed row-source count
- fixed in-memory grouped row count
- DBF/NDX storage instead of a standard relational runtime

Those bounds are acceptable as implementation limits only if they are
documented as deviations. They do not count as full SQL-86 coverage.

### Important Non-Standard Features Already Present

Keep these working, but label them as extensions unless the standard
text proves otherwise:

- `CREATE DATABASE`
- `USE`
- `SHOW DATABASES`
- `SHOW VIEWS`
- explicit `JOIN ... ON ...` syntax
- DBF-specific `DATE` and `LOGICAL` storage assumptions
- built-in `sys_*` catalog views
- inline `FROM (SELECT ...)` handling as currently implemented

### Big Picture

A real SQL-86 implementation is larger than our current interactive
query shell. If we claim full standard coverage, the implementation
must address not only query syntax, but also:

- standard naming and schema rules
- standard data typing and null semantics
- standard table expressions and predicate forms
- standard aggregation semantics
- authorization
- transaction control
- cursor and embedded-SQL behavior, if required by the edition used

If we choose to target only the query-language core, say so explicitly
and do not call the result “full SQL-86”.

---

## Gap Analysis

### 1. Standard Naming And Schema Model

Current status:

- We have database roots and active databases.
- We do not have standard SQL schema objects in the SQL-86 sense.
- Names are short, case-insensitive, and mostly unqualified.

What is still needed:

- Implement the standard schema model from the reference text.
- Support schema-qualified object names where required.
- Add `CREATE SCHEMA` and `AUTHORIZATION` behavior if required.
- Define how the current `database/root/slot` design maps onto schemas.
- Keep `CREATE DATABASE` and `USE` as project extensions, not as part of
  the SQL-86 conformance story.

### 2. Standard Type System

Current status:

- We support `CHAR`, `CHARACTER`, `NUMERIC`, `DATE`, `LOGICAL`.
- Decimal numerics are only partially real today.
- `NULL` values exist, but are currently stored as blank DBF fields.
- Types are strongly shaped by DBF field kinds.

What is still needed:

- Implement the exact SQL-86 type set required by the chosen standard
  text, including names and semantics, not just parser spelling.
- Fix exact numeric semantics, especially precision and scale.
- Add missing standard numeric and character types as required.
- Confirm whether `DATE` belongs to the target edition; if not, keep it
  as an extension.
- Confirm whether `LOGICAL` is standard in the target edition; if not,
  keep it as an extension.
- Tighten standard nullability semantics, especially the distinction
  between blank stored values and true `NULL`.

### 3. Full Expression System

Current status:

- `SELECT` items are still column references plus bounded built-in
  function metadata, not a general scalar-expression tree.
- `WHERE` and `HAVING` are predicate trees, not full scalar expression
  trees.
- Arithmetic expressions are not supported.

What is still needed:

- Build a real scalar-expression AST shared by:
  - `SELECT` list items
  - `WHERE`
  - `HAVING`
  - `UPDATE ... SET`
  - aggregate arguments
- Support standard operators and precedence rules required by SQL-86.
- Add arithmetic expressions.
- Add unary operators where required.
- Add string operations required by the standard text.
- Move the existing built-in function family onto the shared
  scalar-expression model instead of keeping it in ad hoc select-item
  metadata.

### 4. Standard Predicate Forms

Current status:

- We support simple comparisons, `IN (...)`, `IN (SELECT ...)`, `NOT`,
  `BETWEEN`, `LIKE`, `IS NULL`, `IS NOT NULL`, `EXISTS`, and
  quantified comparisons with `ANY` / `ALL`.
- Search conditions now run with three-valued logic over the current
  null model.

What is still needed:

- Make correlated predicate subqueries work if the chosen SQL-86 text
  requires them.
- Fold predicate subqueries into the future shared scalar-expression /
  query-block model instead of keeping them as a separate cached path.
- Revisit predicate behavior after the remaining null-model work in
  section 14 is complete.

### 5. Table Expressions And Joins

Current status:

- We support bounded comma-separated table lists with correlation names.
- We also support bounded `JOIN ... ON ...` with up to 4 row sources.
- `JOIN ... ON ...` remains a project extension unless the target text
  proves otherwise.

What is still needed:

- Remove the current fixed `4`-source limit or document it as a
  conformance deviation.

### 6. Aggregation And Grouping

Current status:

- We support `COUNT(*)`, `COUNT(col)`, `MIN(col)`, `MAX(col)`,
  `SUM(col)`, `AVG(col)`, `GROUP BY`, and `HAVING`.
- Grouping is limited to 8 in-memory groups.
- `AVG` uses integer arithmetic.
- `HAVING` works only over projected output names or aliases.

What is still needed:

- Implement standard aggregate null-handling semantics.
- Implement standard behavior on empty inputs.
- Decide whether aggregate `DISTINCT` is required and implement it if
  so.
- Support `HAVING` over standard grouped expressions, not only output
  aliases.
- Remove or spill past the current `8`-group in-memory limit, or mark
  it as a deviation.
- Upgrade `AVG` to correct standard numeric semantics.

### 7. SELECT Semantics

Current status:

- We support `SELECT *`, column lists, basic functions, joins, where,
  grouping, having, explicit `SELECT ALL`, and `SELECT DISTINCT`.
- Duplicate elimination is implemented, but it is limited to 8
  in-memory distinct output rows.
- We do not support a full SQL-86 query expression model.

What is still needed:

- Remove or spill past the current `8`-row in-memory duplicate
  elimination limit, or mark it as a deviation.
- Implement all standard select-list restrictions and grouped-query
  rules exactly.
- Add standard subquery placement and semantics as required.
- Add set operations such as `UNION` if the chosen SQL-86 text requires
  them.
- Confirm whether `ORDER BY` belongs to the chosen edition before
  adding it. Do not add later-standard syntax accidentally.

### 8. INSERT

Current status:

- We support `INSERT INTO table VALUES (...)`.
- We support `INSERT INTO table (col, ...) VALUES (...)`.

What is still needed:

- Add `INSERT ... SELECT ...` if required by the standard text.
- Enforce standard type-conversion and nullability rules.
- Add standard error handling for count and type mismatches.

### 9. UPDATE And DELETE

Current status:

- We support searched `UPDATE` and searched `DELETE`.

What is still needed:

- Confirm whether positioned `UPDATE` and positioned `DELETE` are part
  of the target standard, and implement them if required.
- Route `SET` expressions through the shared scalar-expression system.
- Make correlated subqueries work in searched updates/deletes if
  required by the standard text.

### 10. Views

Current status:

- We support stored views by saving SQL text.
- View handling is still tied to current parser and temp-table rules.

What is still needed:

- Confirm standard SQL-86 view rules and enforce them.
- Support standard view name resolution and schema qualification.
- Implement standard rules for updatable views if required.
- Distinguish clearly between standard view semantics and current
  built-in `sys_*` pseudo-views.

### 11. Authorization

Current status:

- No real SQL privilege system exists.

What is still needed:

- Implement `GRANT` and any required matching privilege semantics from
  the standard.
- Implement `REVOKE` if it is part of the target edition.
- Add catalog structures for privileges.
- Add executor checks for authorization at statement time.
- Map any CP/M single-user limitations into explicit documented
  deviations, not silent omission.

### 12. Transactions

Current status:

- No SQL transaction control exists.

What is still needed:

- Implement `COMMIT` and `ROLLBACK` if required by the target edition.
- Define transaction boundaries in the hosted build.
- Decide what “transaction” means over DBF/NDX files and make the
  semantics explicit.
- If full transactional guarantees are impossible, document the exact
  deviation.

### 13. Cursors, Modules, And Embedded SQL

Current status:

- The project is an interactive shell, not an embedded-SQL runtime.

What is still needed:

- Check the SQL-86 reference text for required cursor features:
  - `DECLARE CURSOR`
  - `OPEN`
  - `FETCH`
  - `CLOSE`
  - positioned `UPDATE` / `DELETE`
- Check whether module language or embedded host-variable features are
  required for a “full” claim.
- If yes, decide whether to:
  - implement them
  - or explicitly narrow the project goal to “SQL-86 query-language
    subset” and stop claiming full conformance

This is the single biggest scope decision in the whole effort.

### 14. NULL And Three-Valued Logic

Current status:

- `NULL` values now exist in stored rows and predicate values.
- `WHERE` and `HAVING` search conditions use three-valued logic.
- `NULL` is currently represented as an all-space DBF field, so blank
  character data and `NULL` are not yet distinct stored values.

What is still needed:

- Add a physical null representation compatible with DBF storage or add
  a documented abstraction layer over DBF limitations.
- Distinguish blank stored character data from true `NULL` values.
- Update aggregate behavior around null inputs.
- Add explicit nullability metadata and enforcement if the target SQL-86
  edition requires it.

### 15. Diagnostics And Conformance Behavior

Current status:

- Error reporting is intentionally tiny.

What is still needed:

- Implement reproducible parser and runtime diagnostics.
- Add clear standard-vs-extension labeling in docs and possibly in the
  parser mode.
- Build a conformance matrix that maps each SQL-86 clause implemented
  here to:
  - parser coverage
  - lowering coverage
  - executor coverage
  - tests
- Add negative tests for every unsupported standard construct until it
  is implemented.

---

## Required Refactors

Do not try to bolt full SQL-86 onto the current parser one keyword at a
time. The following structural refactors are needed first or very early:

1. Introduce a real scalar-expression tree.
2. Introduce a query-block model richer than “project + filter + scan”.
3. Decouple standard schemas from current database-root navigation.
4. Make grouping, distinct, and set operations able to spill to disk.
5. Unify function handling so scalar and aggregate functions are just
   different expression node kinds.
6. Add a formal standard/extension boundary in docs and tests.

---

## Deliverables

The implementation pass is complete only when it delivers all of the
following:

1. A conformance matrix in this file or a sibling doc.
2. Parser support for every chosen SQL-86 feature in scope.
3. Executor support with tests.
4. Explicit documentation for every deviation from the standard.
5. Updated `README.md` and `docs/SQL.md`.
6. Automated tests for:
   - parser
   - lowering
   - runtime execution
   - negative cases
7. A final summary stating one of:
   - “full SQL-86 implemented”
   - or “SQL-86 core subset implemented, with these deviations”

---

## Acceptance Criteria

Do not claim success unless all of these are true:

- every implemented SQL-86 feature is backed by tests
- every missing standard feature is explicitly documented
- every project extension is labeled as an extension
- every hard engine bound that violates standard expectations is either
  removed or documented as a deviation
- the implementation does not silently replace SQL-86 with SQL-89/92+
  features without justification from the chosen reference text

---

## Recommended Execution Order

Implement in this order:

1. Conformance matrix and scope decision
2. Null model and scalar-expression tree
3. Standard predicate forms
4. Standard `SELECT` table-expression model
5. Full aggregate set and grouped-query semantics
6. `INSERT ... SELECT`
7. Authorization and schema model
8. Transaction control
9. Cursors / embedded SQL if full conformance still remains the goal

If the cursor / embedded-SQL step is rejected for project-scope
reasons, change the goal text everywhere from “full SQL-86” to
“interactive SQL-86 subset”.
