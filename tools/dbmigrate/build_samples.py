#!/usr/bin/env python3
"""
Build sample SQL roots under tests/samples/ from small public datasets.

The script uses the project's hosted sql shell to create the catalog and
empty DBF files, then writes DBF records directly so it can preserve text
values longer than the parser's 33-character literal limit.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import sqlite3
import subprocess
from dataclasses import dataclass, field
from decimal import Decimal, ROUND_HALF_UP
from pathlib import Path
from typing import Iterable


REPO_ROOT = Path(__file__).resolve().parents[2]
BIN_SQL = REPO_ROOT / "bin" / "sql"
SAMPLES_ROOT = REPO_ROOT / "tests" / "samples"
DB_SLOT = "1"
EOF_MARK = b"\x1a"

DATE_RE = re.compile(r"^\d{4}-\d{2}-\d{2}(?:[ T]\d{2}:\d{2}:\d{2})?$")
COMPACT_RE = re.compile(r"[^a-z0-9]")
SNAKE_1_RE = re.compile(r"(.)([A-Z][a-z]+)")
SNAKE_2_RE = re.compile(r"([a-z0-9])([A-Z])")

MONEY_HINTS = (
    "amount",
    "price",
    "cost",
    "rate",
    "total",
    "fee",
)


@dataclass
class SourceColumn:
    name: str
    declared_type: str
    primary_key: bool = False


@dataclass
class SourceTable:
    name: str
    columns: list[SourceColumn]
    rows: list[tuple]
    foreign_keys: set[str] = field(default_factory=set)


@dataclass
class SampleSource:
    name: str
    root_dir: Path
    db_name: str
    source_label: str
    source_url: str
    license_label: str
    notes: list[str]
    tables: list[SourceTable]


@dataclass
class BuiltColumn:
    source_name: str
    name: str
    sql_type: str
    field_type: str
    length: int
    decimals: int
    storage_kind: str


@dataclass
class BuiltTable:
    source_name: str
    name: str
    columns: list[BuiltColumn]
    rows: list[tuple[str | None, ...]]
    indexes: list[str]


def snake_case(name: str) -> str:
    text = name.replace(" ", "_").replace("-", "_")
    text = SNAKE_1_RE.sub(r"\1_\2", text)
    text = SNAKE_2_RE.sub(r"\1_\2", text)
    text = text.lower()
    text = re.sub(r"[^a-z0-9_]", "_", text)
    text = re.sub(r"_+", "_", text)
    return text.strip("_")


def compact_name(name: str) -> str:
    return COMPACT_RE.sub("", snake_case(name))


def dedupe_name(base: str, used: set[str], limit: int) -> str:
    if base not in used:
        used.add(base)
        return base
    counter = 2
    while True:
        suffix = str(counter)
        candidate = f"{base[:limit - len(suffix)]}{suffix}"
        if candidate not in used:
            used.add(candidate)
            return candidate
        counter += 1


def shorten_table_name(name: str, used: set[str]) -> str:
    tokens = [t for t in snake_case(name).split("_") if t]
    if not tokens:
        tokens = ["table"]
    for width in (99, 5, 4, 3, 2):
        if width == 99:
            candidate = "_".join(tokens)
        else:
            candidate = "_".join(t if len(t) <= width else t[:width]
                for t in tokens)
        if len(candidate) <= 16:
            return dedupe_name(candidate, used, 16)
    candidate = "".join(t[:2] for t in tokens)[:16]
    if not candidate:
        candidate = "table"
    return dedupe_name(candidate, used, 16)


def shorten_column_name(name: str, used: set[str], cents: bool) -> str:
    base = compact_name(name)
    if not base:
        base = "col"
    if cents and not base.endswith("c"):
        base = f"{base}c"
    if len(base) > 11:
        if cents:
            base = f"{base[:10]}c"
        elif base.endswith("id"):
            base = f"{base[:9]}id"
        else:
            base = base[:11]
    return dedupe_name(base, used, 11)


def sql_literal(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def run_sql_script(root_dir: Path, text: str) -> None:
    try:
        root_arg = str(root_dir.relative_to(REPO_ROOT))
    except ValueError:
        root_arg = str(root_dir)
    env = os.environ.copy()
    env["ASAN_OPTIONS"] = "detect_leaks=0"
    result = subprocess.run(
        [str(BIN_SQL), root_arg],
        input=text.encode("utf-8"),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        env=env,
        cwd=REPO_ROOT,
        check=False,
    )
    output = result.stdout.decode("utf-8", "replace")
    if result.returncode != 0:
        raise RuntimeError(output)
    for line in output.splitlines():
        stripped = line.strip()
        if stripped in {"error", "parse error"}:
            raise RuntimeError(output)


def dbf_field_descriptor(name: str, field_type: str, length: int,
    decimals: int) -> bytes:
    descriptor = bytearray(32)
    name_bytes = name.encode("ascii", "ignore")[:11]
    descriptor[:len(name_bytes)] = name_bytes
    descriptor[11] = ord(field_type)
    descriptor[16] = length
    descriptor[17] = decimals
    return bytes(descriptor)


def create_dbf(
    path: Path,
    fields: list[tuple[str, str, int, int]],
    rows: list[bytes],
) -> None:
    payload_length = sum(length for _, _, length, _ in fields)
    header_length = 32 + (32 * len(fields)) + 1
    record_length = payload_length + 1
    header = bytearray(32)
    header[0] = 0x03
    header[4:8] = len(rows).to_bytes(4, "little")
    header[8:10] = header_length.to_bytes(2, "little")
    header[10:12] = record_length.to_bytes(2, "little")
    with path.open("wb") as handle:
        handle.write(header)
        for name, field_type, length, decimals in fields:
            handle.write(
                dbf_field_descriptor(name, field_type, length, decimals)
            )
        handle.write(b"\x0d")
        for row in rows:
            if len(row) != payload_length:
                raise ValueError(
                    f"{path.name}: payload length {len(row)} "
                    f"!= {payload_length}"
                )
            handle.write(b" ")
            handle.write(row)
        handle.write(EOF_MARK)


def parse_sqlite_source(
    path: Path,
    name: str,
    source_label: str,
    source_url: str,
    license_label: str,
    notes: list[str],
) -> SampleSource:
    con = sqlite3.connect(path)
    cur = con.cursor()
    table_names = [
        row[0]
        for row in cur.execute(
            "select name from sqlite_master "
            "where type='table' and name not like 'sqlite_%' "
            "order by name"
        )
    ]
    tables: list[SourceTable] = []
    for table_name in table_names:
        column_rows = cur.execute(
            f'pragma table_info("{table_name}")'
        ).fetchall()
        columns = [
            SourceColumn(
                name=row[1],
                declared_type=row[2] or "",
                primary_key=bool(row[5]),
            )
            for row in column_rows
        ]
        rows = cur.execute(
            f'select * from "{table_name}"'
        ).fetchall()
        foreign_keys = {
            row[3]
            for row in cur.execute(
                f'pragma foreign_key_list("{table_name}")'
            ).fetchall()
        }
        tables.append(SourceTable(
            name=table_name,
            columns=columns,
            rows=rows,
            foreign_keys=foreign_keys,
        ))
    con.close()
    return SampleSource(
        name=name,
        root_dir=SAMPLES_ROOT / name,
        db_name=name,
        source_label=source_label,
        source_url=source_url,
        license_label=license_label,
        notes=notes,
        tables=tables,
    )


def parse_world_value_list(text: str) -> tuple:
    values = []
    index = 0
    limit = len(text)
    while index < limit:
        while index < limit and text[index] in " \t,":
            index += 1
        if index >= limit:
            break
        if text[index] == "'":
            index += 1
            chars = []
            while index < limit:
                ch = text[index]
                if ch == "\\" and index + 1 < limit:
                    chars.append(text[index + 1])
                    index += 2
                    continue
                if ch == "'":
                    if index + 1 < limit and text[index + 1] == "'":
                        chars.append("'")
                        index += 2
                        continue
                    index += 1
                    break
                chars.append(ch)
                index += 1
            values.append("".join(chars))
            continue
        start = index
        while index < limit and text[index] != ",":
            index += 1
        token = text[start:index].strip()
        if token.upper() == "NULL":
            values.append(None)
        elif "." in token:
            values.append(Decimal(token))
        else:
            values.append(int(token))
    return tuple(values)


def parse_world_source(
    path: Path,
    source_url: str,
    notes: list[str],
) -> SampleSource:
    text = path.read_text("utf-8")
    tables: dict[str, SourceTable] = {}
    current: SourceTable | None = None
    for raw_line in text.splitlines():
        line = raw_line.rstrip()
        start_match = re.match(r"CREATE TABLE `([^`]+)` \($", line)
        if start_match:
            current = SourceTable(
                name=start_match.group(1),
                columns=[],
                rows=[],
                foreign_keys=set(),
            )
            tables[current.name] = current
            continue
        if current is not None:
            col_match = re.match(
                r"\s*`([^`]+)`\s+([a-zA-Z]+(?:\([^)]+\))?)",
                line,
            )
            if col_match:
                current.columns.append(SourceColumn(
                    name=col_match.group(1),
                    declared_type=col_match.group(2),
                ))
                continue
            pk_match = re.match(r"\s*PRIMARY KEY \(`([^`]+)`\)", line)
            if pk_match:
                for column in current.columns:
                    if column.name == pk_match.group(1):
                        column.primary_key = True
                continue
            fk_match = re.match(
                r"\s*CONSTRAINT `[^`]+` FOREIGN KEY \(`([^`]+)`\)",
                line,
            )
            if fk_match:
                current.foreign_keys.add(fk_match.group(1))
                continue
            if line.startswith(") ENGINE="):
                current = None
                continue
        insert_match = re.match(
            r"INSERT INTO `([^`]+)` VALUES \((.*)\);$",
            line,
        )
        if insert_match:
            table_name = insert_match.group(1)
            tables[table_name].rows.append(
                parse_world_value_list(insert_match.group(2))
            )
    return SampleSource(
        name="world",
        root_dir=SAMPLES_ROOT / "world",
        db_name="world",
        source_label="MySQL world sample database",
        source_url=source_url,
        license_label="Sample data from Statistics Finland via MySQL",
        notes=notes,
        tables=[tables[name] for name in sorted(tables.keys())],
    )


def is_boolish(values: Iterable[object]) -> bool:
    seen = set()
    for value in values:
        if value is None:
            continue
        if isinstance(value, str):
            seen.add(value.upper())
        else:
            seen.add(value)
    return bool(seen) and seen <= {0, 1, "0", "1", "T", "F", "Y", "N"}


def is_dateish(values: Iterable[object]) -> bool:
    any_value = False
    for value in values:
        if value is None:
            continue
        any_value = True
        if not isinstance(value, str):
            return False
        if not DATE_RE.match(value):
            return False
    return any_value


def wants_cents(sample_name: str, table_name: str, column_name: str) -> bool:
    column = compact_name(column_name)
    if sample_name == "world":
        return False
    return any(hint in column for hint in MONEY_HINTS)


def world_decimal_as_text(table_name: str, column_name: str) -> bool:
    if table_name.lower() == "country":
        return compact_name(column_name) in {
            "surfacearea",
            "lifeexpectancy",
            "gnp",
            "gnpold",
        }
    if table_name.lower() == "countrylanguage":
        return compact_name(column_name) == "percentage"
    return False


def classify_storage(
    sample_name: str,
    table_name: str,
    column: SourceColumn,
    values: list[object],
) -> str:
    declared = column.declared_type.lower()
    if sample_name == "world" and compact_name(column.name) == "isofficial":
        return "logical"
    if sample_name == "world" and world_decimal_as_text(
        table_name, column.name
    ):
        return "char"
    if "int" in declared:
        return "int"
    if wants_cents(sample_name, table_name, column.name):
        any_decimal = any(
            value is not None and Decimal(str(value)) !=
            Decimal(int(Decimal(str(value))))
            for value in values
        )
        if any_decimal:
            return "scaled100"
    if is_dateish(values):
        return "date"
    if "bool" in declared or is_boolish(values):
        return "logical"
    if any(isinstance(value, int) for value in values if value is not None):
        if all(
            value is None or isinstance(value, int)
            for value in values
        ):
            return "int"
    return "char"


def encode_char_value(text: str, length: int) -> bytes:
    value = text
    encoded = value.encode("utf-8")
    while len(encoded) > length and value:
        value = value[:-1]
        encoded = value.encode("utf-8")
    return encoded.ljust(length, b" ")


def to_date_text(value: object) -> str | None:
    if value is None:
        return None
    text = str(value)
    digits = re.sub(r"[^0-9]", "", text)
    if len(digits) >= 8:
        return digits[:8]
    return None


def to_logical_text(value: object) -> str | None:
    if value is None:
        return None
    if isinstance(value, str):
        text = value.upper()
    else:
        text = str(int(value))
    if text in {"1", "T", "Y", "TRUE"}:
        return "T"
    if text in {"0", "F", "N", "FALSE"}:
        return "F"
    return None


def to_int_text(value: object) -> str | None:
    if value is None:
        return None
    return str(int(value))


def to_scaled_cents_text(value: object) -> str | None:
    if value is None:
        return None
    cents = (Decimal(str(value)) * Decimal("100")).quantize(
        Decimal("1"),
        rounding=ROUND_HALF_UP,
    )
    return str(int(cents))


def build_table(sample: SampleSource, table: SourceTable) -> BuiltTable:
    table_name = table.name
    used_columns: set[str] = set()
    built_columns: list[BuiltColumn] = []
    name_to_built: dict[str, BuiltColumn] = {}

    for index, source_column in enumerate(table.columns):
        values = [row[index] for row in table.rows]
        storage_kind = classify_storage(
            sample.name, table.name, source_column, values
        )
        mapped_name = shorten_column_name(
            source_column.name,
            used_columns,
            cents=(storage_kind == "scaled100"),
        )
        transformed: list[str] = []
        if storage_kind == "date":
            transformed = [
                text for text in
                (to_date_text(value) for value in values)
                if text is not None
            ]
            length = 8
            sql_type = "DATE"
            field_type = "D"
            decimals = 0
        elif storage_kind == "logical":
            transformed = [
                text for text in
                (to_logical_text(value) for value in values)
                if text is not None
            ]
            length = 1
            sql_type = "LOGICAL"
            field_type = "L"
            decimals = 0
        elif storage_kind == "int":
            transformed = [
                text for text in
                (to_int_text(value) for value in values)
                if text is not None
            ]
            length = max((len(text) for text in transformed), default=1)
            sql_type = f"NUMERIC({length})"
            field_type = "N"
            decimals = 0
        elif storage_kind == "scaled100":
            transformed = [
                text for text in
                (to_scaled_cents_text(value) for value in values)
                if text is not None
            ]
            length = max((len(text) for text in transformed), default=1)
            sql_type = f"NUMERIC({length})"
            field_type = "N"
            decimals = 0
        else:
            transformed = [
                "" if value is None else str(value)
                for value in values
            ]
            length = max(
                (len(text.encode("utf-8")) for text in transformed),
                default=1,
            )
            if length > 250:
                length = 250
            if length < 1:
                length = 1
            sql_type = f"CHAR({length})"
            field_type = "C"
            decimals = 0

        built = BuiltColumn(
            source_name=source_column.name,
            name=mapped_name,
            sql_type=sql_type,
            field_type=field_type,
            length=length,
            decimals=decimals,
            storage_kind=storage_kind,
        )
        built_columns.append(built)
        name_to_built[source_column.name] = built

    transformed_rows: list[tuple[str | None, ...]] = []
    for row in table.rows:
        out_row: list[str | None] = []
        for index, value in enumerate(row):
            built = built_columns[index]
            if built.storage_kind == "date":
                out_row.append(to_date_text(value))
            elif built.storage_kind == "logical":
                out_row.append(to_logical_text(value))
            elif built.storage_kind == "int":
                out_row.append(to_int_text(value))
            elif built.storage_kind == "scaled100":
                out_row.append(to_scaled_cents_text(value))
            else:
                out_row.append(None if value is None else str(value))
        transformed_rows.append(tuple(out_row))

    index_targets: list[str] = []
    for source_column in table.columns:
        built = name_to_built[source_column.name]
        if built.field_type not in {"C", "N", "D"}:
            continue
        if built.field_type == "C" and built.length > 100:
            continue
        if source_column.primary_key or source_column.name in table.foreign_keys:
            if built.name not in index_targets:
                index_targets.append(built.name)
    for built in built_columns:
        if (
            built.field_type == "C"
            and built.length <= 100
            and built.name in {
            "name", "lastname", "title"
        }
        ):
            if built.name not in index_targets:
                index_targets.append(built.name)

    return BuiltTable(
        source_name=table.name,
        name="",
        columns=built_columns,
        rows=transformed_rows,
        indexes=index_targets,
    )


def build_sample(source: SampleSource) -> tuple[list[BuiltTable], dict[str, str]]:
    used_tables: set[str] = set()
    built_tables: list[BuiltTable] = []
    table_map: dict[str, str] = {}
    for table in source.tables:
        built = build_table(source, table)
        built.name = shorten_table_name(table.name, used_tables)
        built_tables.append(built)
        table_map[table.name] = built.name
    return built_tables, table_map


def build_schema_sql(source: SampleSource, tables: list[BuiltTable]) -> str:
    lines = [
        f"CREATE DATABASE {source.db_name};",
        f"USE {source.db_name};",
    ]
    for table in tables:
        lines.append(f"CREATE TABLE {table.name} (")
        for index, column in enumerate(table.columns):
            suffix = "," if index + 1 < len(table.columns) else ""
            lines.append(f"    {column.name} {column.sql_type}{suffix}")
        lines.append(");")
    lines.append("")
    return "\n".join(lines)


def build_index_sql(source: SampleSource, tables: list[BuiltTable]) -> str:
    lines = [f"USE {source.db_name};"]
    used_names: set[str] = set()
    for table in tables:
        for column_name in table.indexes:
            base = f"idx_{table.name}_{column_name}"
            base = compact_name(base)[:16] or "idx"
            index_name = dedupe_name(base, used_names, 16)
            lines.append(
                f"CREATE INDEX {index_name} ON {table.name} ({column_name});"
            )
    lines.append("")
    return "\n".join(lines)


def encode_field(column: BuiltColumn, value: str | None) -> bytes:
    if value is None:
        return b" " * column.length
    if column.field_type == "C":
        return encode_char_value(value, column.length)
    if column.field_type == "N":
        return value.encode("ascii").rjust(column.length, b" ")
    if column.field_type == "D":
        return value.encode("ascii").ljust(column.length, b" ")
    if column.field_type == "L":
        return value.encode("ascii")[:1].ljust(column.length, b" ")
    raise ValueError(column.field_type)


def build_dbf_rows(table: BuiltTable) -> list[bytes]:
    return [
        b"".join(
            encode_field(column, value)
            for column, value in zip(table.columns, row)
        )
        for row in table.rows
    ]


def create_table_dbf(root_dir: Path, table: BuiltTable) -> None:
    create_dbf(
        root_dir / DB_SLOT / f"{table.name}.dbf",
        [
            (column.name, column.field_type, column.length, column.decimals)
            for column in table.columns
        ],
        build_dbf_rows(table),
    )


def create_empty_view_catalog(root_dir: Path) -> None:
    create_dbf(
        root_dir / "sys" / "vw.dbf",
        [
            ("db_name", "C", 16, 0),
            ("name", "C", 16, 0),
            ("type", "C", 1, 0),
            ("statement", "C", 240, 0),
        ],
        [],
    )


def write_sample_readme(
    source: SampleSource,
    tables: list[BuiltTable],
    table_map: dict[str, str],
) -> None:
    lines = [
        f"# {source.name}",
        "",
        f"- Source: [{source.source_label}]({source.source_url})",
        f"- License / provenance: {source.license_label}",
        "",
        "## Usage",
        "",
        "```sh",
        f"./bin/sql tests/samples/{source.name}",
        "```",
        "",
        "Then run:",
        "",
        "```sql",
        f"USE {source.db_name};",
        "SHOW VIEWS;",
        "```",
        "",
        "## Notes",
        "",
    ]
    for note in source.notes:
        lines.append(f"- {note}")
    lines.extend([
        "- `bootstrap.sql` creates the database slot and catalogs.",
        "- `schema.sql` is the imported logical schema reference.",
        "- Table names were shortened to fit the shell identifier limit.",
        "- DBF field names were shortened to 11 bytes when needed.",
        "",
        "## Tables",
        "",
        "| Source | Imported | Rows | Indexed columns |",
        "|---|---|---:|---|",
    ])
    for table in tables:
        indexes = ", ".join(table.indexes) if table.indexes else "-"
        lines.append(
            f"| `{table.source_name}` | `{table.name}` | "
            f"{len(table.rows)} | {indexes} |"
        )
    lines.extend([
        "",
        "## Column Mapping",
        "",
    ])
    for table in tables:
        lines.append(f"### `{table.name}`")
        lines.append("")
        lines.append("| Source | Imported | Type |")
        lines.append("|---|---|---|")
        for column in table.columns:
            lines.append(
                f"| `{column.source_name}` | `{column.name}` | "
                f"`{column.sql_type}` |"
            )
        lines.append("")
    (source.root_dir / "README.md").write_text(
        "\n".join(lines),
        encoding="utf-8",
    )


def write_top_readme(samples: list[SampleSource]) -> None:
    lines = [
        "# Sample Databases",
        "",
        "These roots are ready to open with `./bin/sql <root>`.",
        "",
        "| Sample | Domain | Source |",
        "|---|---|---|",
    ]
    for sample in samples:
        lines.append(
            f"| [{sample.name}]({sample.name}/README.md) | "
            f"{sample.source_label} | "
            f"[link]({sample.source_url}) |"
        )
    lines.extend([
        "",
        "Rebuild them with:",
        "",
        "```sh",
        "./tests/samples/build_samples.py",
        "```",
    ])
    (SAMPLES_ROOT / "README.md").write_text(
        "\n".join(lines),
        encoding="utf-8",
    )


def ensure_source_exists(path: Path) -> None:
    if not path.exists():
        raise FileNotFoundError(path)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--chinook-src",
        default="/tmp/chinook-src/ChinookDatabase/DataSources/"
                "Chinook_Sqlite.sqlite",
    )
    parser.add_argument(
        "--employee-src",
        default="/tmp/employee-sample-src/sqlite/dataset_small/employee.db",
    )
    parser.add_argument(
        "--world-src",
        default="/tmp/world-db/world-db/world.sql",
    )
    args = parser.parse_args()

    chinook_src = Path(args.chinook_src)
    employee_src = Path(args.employee_src)
    world_src = Path(args.world_src)

    ensure_source_exists(chinook_src)
    ensure_source_exists(employee_src)
    ensure_source_exists(world_src)

    samples = [
        parse_sqlite_source(
            chinook_src,
            name="chinook",
            source_label="Chinook sample database",
            source_url=(
                "https://github.com/lerocha/chinook-database/releases/"
                "download/v1.4.5/Chinook_Sqlite.sqlite"
            ),
            license_label="See lerocha/chinook-database LICENSE.md",
            notes=[
                "Invoice and track price fields are stored as integer cents.",
                "Timestamp values were normalized to YYYYMMDD.",
            ],
        ),
        parse_sqlite_source(
            employee_src,
            name="employee",
            source_label="Bytebase employee sample database",
            source_url="https://github.com/bytebase/employee-sample-database",
            license_label="MIT",
            notes=[
                "This uses the repository's small SQLite dataset.",
                "Date values were normalized to YYYYMMDD.",
            ],
        ),
        parse_world_source(
            world_src,
            source_url="https://downloads.mysql.com/docs/world-db.zip",
            notes=[
                "This is the official MySQL world sample.",
                "Floating-point country metrics are stored as text because "
                "the current SQL numeric path is integer-only.",
            ],
        ),
    ]

    for sample in samples:
        built_tables, table_map = build_sample(sample)
        if sample.root_dir.exists():
            shutil.rmtree(sample.root_dir)
        sample.root_dir.mkdir(parents=True)
        schema_sql = build_schema_sql(sample, built_tables)
        (sample.root_dir / "schema.sql").write_text(
            schema_sql,
            encoding="utf-8",
        )
        bootstrap_sql = (
            f"CREATE DATABASE {sample.db_name};\n"
            f"USE {sample.db_name};\n"
        )
        (sample.root_dir / "bootstrap.sql").write_text(
            bootstrap_sql,
            encoding="utf-8",
        )
        run_sql_script(sample.root_dir, bootstrap_sql)
        create_empty_view_catalog(sample.root_dir)
        for table in built_tables:
            create_table_dbf(sample.root_dir, table)
        index_sql = build_index_sql(sample, built_tables)
        (sample.root_dir / "indexes.sql").write_text(
            index_sql,
            encoding="utf-8",
        )
        run_sql_script(sample.root_dir, index_sql)
        write_sample_readme(sample, built_tables, table_map)

    write_top_readme(samples)


if __name__ == "__main__":
    main()
