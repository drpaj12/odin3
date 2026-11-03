#!/usr/bin/env python3
"""Validate a provenance directory (prov/) against docs/schemas/*.schema.json.

A small stdlib checker for the subset of JSON Schema the schemas use: type (a name or a list),
const, enum, required, properties, items, pattern. It also checks the rules the schemas cannot say:
the sha256 of every file in the index, link targets that exist, and ``origin: hdl`` only with a
link (stage ``hdl`` excepted).

Usage: schema_check.py PROV_DIR [SCHEMA_DIR]   exit 0 valid, 1 invalid, 2 usage
"""

from __future__ import annotations

import hashlib
import json
import re
import sys
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import Any

TYPES: dict[str, tuple[type, ...]] = {
    "object": (dict,), "array": (list,), "string": (str,), "integer": (int,),
    "number": (int, float), "boolean": (bool,), "null": (type(None),)}


def validate(value: Any, schema: Mapping[str, Any], where: str = "$") -> list[str]:
    """Problems of value against schema, each prefixed with its path."""
    errs: list[str] = []
    typ = schema.get("type")
    if typ is not None:
        names = [typ] if isinstance(typ, str) else list(typ)
        ok = any(isinstance(value, TYPES[n]) and not (n == "integer" and isinstance(value, bool))
                 for n in names)
        if not ok:
            return [f"{where}: expected {'/'.join(names)}, got {type(value).__name__}"]
    if "const" in schema and value != schema["const"]:
        errs.append(f"{where}: expected {schema['const']!r}")
    if "enum" in schema and value not in schema["enum"]:
        errs.append(f"{where}: {value!r} is not one of {schema['enum']}")
    if "pattern" in schema and isinstance(value, str) and not re.search(schema["pattern"], value):
        errs.append(f"{where}: {value!r} does not match {schema['pattern']}")
    if isinstance(value, dict):
        for key in schema.get("required", []):
            if key not in value:
                errs.append(f"{where}: missing '{key}'")
        for key, sub in schema.get("properties", {}).items():
            if key in value:
                errs += validate(value[key], sub, f"{where}.{key}")
    if isinstance(value, list) and "items" in schema:
        for i, item in enumerate(value):
            errs += validate(item, schema["items"], f"{where}[{i}]")
    return errs


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def check_dir(prov: Path, schemas: Path) -> list[str]:
    """All problems of the run directory ``prov`` (empty: valid)."""
    load = {n: json.loads((schemas / f"prov-{n}.schema.json").read_text())
            for n in ("index", "step", "object")}
    index = json.loads((prov / "index.json").read_text())
    errs = validate(index, load["index"], "index")
    ids: set[str] = set()
    rows_by_step: list[tuple[str, list[dict[str, Any]]]] = []
    for entry in index.get("steps", []):
        head_path = prov / entry["file"]
        if sha(head_path) != entry["sha256"]:
            errs.append(f"{entry['file']}: sha256 differs from the index")
        errs += validate(json.loads(head_path.read_text()), load["step"], entry["file"])
        rows: list[dict[str, Any]] = []
        if entry["objects"]:
            path = prov / entry["objects"]
            if sha(path) != entry["objects_sha256"]:
                errs.append(f"{entry['objects']}: sha256 differs from the index")
            for n, line in enumerate(path.read_text().splitlines(), 1):
                row = json.loads(line)
                errs += validate(row, load["object"], f"{entry['objects']}:{n}")
                rows.append(row)
                ids.add(row.get("id", ""))
        rows_by_step.append((entry["stage"], rows))
    for stage, rows in rows_by_step:
        for row in rows:
            if row.get("origin") == "hdl" and stage != "hdl" and not row.get("links"):
                errs.append(f"{row['id']}: origin hdl without a link")
            for lk in row.get("links", []):
                if lk["to"] not in ids:
                    errs.append(f"{row['id']}: link target {lk['to']} is not an object")
    return errs


def main(argv: Sequence[str]) -> int:
    if len(argv) not in (1, 2):
        print("usage: schema_check.py PROV_DIR [SCHEMA_DIR]", file=sys.stderr)
        return 2
    default = Path(__file__).resolve().parents[2] / "docs" / "schemas"
    schemas = Path(argv[1]) if len(argv) == 2 else default
    errs = check_dir(Path(argv[0]), schemas)
    for e in errs[:50]:
        print(e)
    print(f"schema_check: {'invalid' if errs else 'valid'} ({len(errs)} problems)")
    return 1 if errs else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
