"""prov_steps: provenance step files (v1.4; docs/user/provenance-files.md).

Every flow step that creates or changes objects writes a header ``<NN>-<step>.json`` and an
objects file ``<NN>-<step>.objects.jsonl`` into ``<out>/prov``; ``index.json`` lists them with
their sha256. ``StepWriter`` writes these files; ``fpga_steps`` derives them from a version 2 name
map (``--prov`` runs) so the LUT/VTR flows write the same format as the silicon flow.

Standard library only. No absolute path, home path or user name is ever written: a string that
looks like one raises ``PrivacyError`` (the same rule as the run record).
"""

from __future__ import annotations

import hashlib
import json
import re
import sys
from collections.abc import Iterable, Mapping, Sequence
from pathlib import Path
from typing import Any

SCHEMA = 1
STAGES = ("hdl", "ir", "gates", "luts", "packed", "placed", "routed", "gds", "extracted",
          "recovered")
LINK_KINDS = ("id", "name", "cone", "fine", "join", "resize", "physical_origin",
              "location_match", "structural_match")
CONFIDENCES = ("exact", "high", "medium", "low", "ambiguous")
OBJECT_KINDS = ("module", "block", "stmt", "net", "port", "cell", "lut", "ble", "clb", "site",
                "device")
_PRIVATE = re.compile(r"(^|[\s=:\"'(,])(/home/|/Users/|/root/|[A-Za-z]:\\\\)")


class PrivacyError(ValueError):
    """A value that would write an absolute or home path."""


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _check_private(value: Any, where: str) -> None:
    if isinstance(value, str):
        if _PRIVATE.search(value) or value.startswith("/"):
            raise PrivacyError(f"{where}: absolute or home path in '{value[:60]}'")
    elif isinstance(value, Mapping):
        for k, v in value.items():
            _check_private(v, f"{where}.{k}")
    elif isinstance(value, (list, tuple)):
        for v in value:
            _check_private(v, where)


def link(to: str, kind: str, confidence: str = "exact", **extra: Any) -> dict[str, Any]:
    """One entry of an object's ``links``."""
    if kind not in LINK_KINDS:
        raise ValueError(f"unknown link kind '{kind}'")
    if confidence not in CONFIDENCES:
        raise ValueError(f"unknown confidence '{confidence}'")
    return {"to": to, "kind": kind, "confidence": confidence, **extra}


def obj(stage: str, local: str, kind: str, name: str, origin: str,
        links: Sequence[Mapping[str, Any]] = (), **fields: Any) -> dict[str, Any]:
    """One object line. ``fields`` may hold ``type``, ``attrs``, ``src``, ``recs``."""
    if stage not in STAGES:
        raise ValueError(f"unknown stage '{stage}'")
    if kind not in OBJECT_KINDS:
        raise ValueError(f"unknown object kind '{kind}'")
    row: dict[str, Any] = {"id": f"{stage}:{local}", "stage": stage, "kind": kind, "name": name,
                           "origin": origin, "links": [dict(x) for x in links]}
    row.update(fields)
    return row


class StepWriter:
    """Writes the step files of one run into ``out/prov``."""

    def __init__(self, out: Path, flow: str, design: str, top: str, odin3_version: str,
                 project: str | None = None) -> None:
        self.dir = out / "prov"
        self.dir.mkdir(parents=True, exist_ok=True)
        self.index: dict[str, Any] = {
            "schema": SCHEMA, "kind": "odin3-prov-index", "flow": flow, "design": design,
            "top": top, "odin3_version": odin3_version, "project": project, "tools": [],
            "inputs": [], "steps": []}
        self._n = 0

    def add_tool(self, tool_id: str, version: str, path_alias: str = "") -> None:
        self.index["tools"].append({"id": tool_id, "version": version, "path_alias": path_alias})

    def add_design_file(self, name: str, path: Path) -> None:
        self.index["inputs"].append({"role": "design", "path": name, "sha256": sha256_file(path)})

    def add_step(self, step: str, stage: str, tool: Mapping[str, str],
                 objects: Iterable[Mapping[str, Any]] | None, needs: Sequence[str] = (),
                 inputs: Sequence[Mapping[str, str]] = (),
                 outputs: Sequence[Mapping[str, str]] = (),
                 links: Sequence[Mapping[str, Any]] = (),
                 census: Mapping[str, Any] | None = None,
                 check: Mapping[str, Any] | None = None,
                 map_: Mapping[str, str] | None = None) -> dict[str, Any]:
        """Write one step; ``objects`` None for a check-only step. Returns the index entry."""
        if stage not in STAGES:
            raise ValueError(f"unknown stage '{stage}'")
        self._n += 1
        base = f"{self._n:02d}-{step}"
        counts: dict[str, Any] = {"objects": 0, "by_kind": {}, "by_origin": {}, "by_link_kind": {}}
        objects_file: str | None = None
        if objects is not None:
            objects_file = f"{base}.objects.jsonl"
            with (self.dir / objects_file).open("w", encoding="utf-8") as fh:
                for row in objects:
                    _check_private(row, f"{step} object {row.get('id')}")
                    fh.write(json.dumps(row, sort_keys=True, separators=(",", ":")) + "\n")
                    counts["objects"] += 1
                    for field, val in (("by_kind", row["kind"]), ("by_origin", row["origin"])):
                        counts[field][val] = counts[field].get(val, 0) + 1
                    for lk in row["links"]:
                        counts["by_link_kind"][lk["kind"]] = \
                            counts["by_link_kind"].get(lk["kind"], 0) + 1
        header = {"schema": SCHEMA, "step": step, "stage": stage, "tool": dict(tool),
                  "objects_file": objects_file, "needs": list(needs),
                  "inputs": [dict(x) for x in inputs], "outputs": [dict(x) for x in outputs],
                  "links": [dict(x) for x in links], "counts": counts,
                  "census": dict(census) if census is not None else None,
                  "check": dict(check) if check is not None else None,
                  "map": dict(map_) if map_ is not None else None}
        _check_private(header, f"{step} header")
        (self.dir / f"{base}.json").write_text(
            json.dumps(header, sort_keys=True, indent=1) + "\n", encoding="utf-8")
        entry = {"id": step, "stage": stage, "file": f"{base}.json", "objects": objects_file,
                 "sha256": sha256_file(self.dir / f"{base}.json"),
                 "objects_sha256": sha256_file(self.dir / objects_file) if objects_file else None,
                 "count": counts["objects"], "needs": list(needs), "tool": dict(tool)}
        self.index["steps"].append(entry)
        return entry

    def finish(self) -> Path:
        """Write ``index.json`` (last, so a partial run has none)."""
        path = self.dir / "index.json"
        _check_private(self.index, "index")
        path.write_text(json.dumps(self.index, sort_keys=True, indent=1) + "\n", encoding="utf-8")
        return path


# --- the FPGA flows: step files from a version 2 name map --------------------------------------

def _span_text(file: str, span: str) -> str:
    return f"{file}:{span}"


def fpga_steps(map_path: Path, out: Path, design: str, top: str, odin3_version: str,
               blif_name: str, tools: Sequence[tuple[str, str]] = ()) -> Path:
    """Write ``out/prov`` for a ``--prov`` run from its name map: stages ``hdl`` (constructs),
    ``luts`` (objects of the BLIF, link ``fine`` to the constructs of their record chain), and
    ``placed`` (atoms with a site record, link ``name`` to the LUT object) when
    ``odin3-prov-post`` added ``A`` lines. The chains stay in the map (``recs``)."""
    import name_map_v2

    nm = name_map_v2.read_name_map(map_path)
    writer = StepWriter(out, "fpga-vtr", design, top, odin3_version)
    for tid, ver in tools:
        writer.add_tool(tid, ver)
    tool = {"id": "odin3", "version": odin3_version}
    map_ref = {"file": map_path.name, "sha256": sha256_file(map_path)}
    hdl_rows: list[dict[str, Any]] = []
    for number, c in enumerate(nm.constructs, 1):
        span = name_map_v2.span_of(c.span, nm.files, c.file) if c.span else None
        row = obj("hdl", f"c{number}", "block" if c.level in ("module", "block") else "stmt",
                  c.name or f"c{number}", "hdl", src=[{"file": span.file, "span":
                  f"{span.line}.{span.col}-{span.end_line}.{span.end_col}"}] if span else [])
        row["attrs"] = {"level": c.level, "parent": f"hdl:c{c.parent}" if c.parent else None}
        hdl_rows.append(row)
    writer.add_step("read", "hdl", tool, hdl_rows, map_=map_ref,
                    outputs=[{"role": "map", "path": map_path.name,
                              "sha256": map_ref["sha256"]}])
    number_of = {c.number: n for n, c in enumerate(nm.constructs, 1)}
    blif = map_path.with_name(blif_name)
    lut_rows: list[dict[str, Any]] = []
    by_name: dict[str, str] = {}
    for o in nm.objects:
        links: list[dict[str, Any]] = []
        for c in nm.chain(o):
            links.append(link(f"hdl:c{number_of[c.number]}", "fine"))
        chain_src = []
        for r in nm.provenance(o):
            loc = nm.location(r)
            if loc is not None:
                chain_src.append({"file": loc.file,
                                  "span": f"{loc.line}.{loc.col}-{loc.end_line}.{loc.end_col}"})
        kind = {"net": "net", "node": "cell", "wire": "net"}.get(o.kind, "net")
        if o.level == "lut":
            kind = "lut"
        row = obj("luts", f"{o.module}/{o.name}", kind, o.name,
                  "hdl" if links or chain_src else "unknown", links, src=chain_src[:8],
                  recs=[f"r{n}" for n in o.records], attrs={"module": o.module, "level": o.level})
        lut_rows.append(row)
        by_name.setdefault(o.name, row["id"])
    writer.add_step("map", "luts", tool, lut_rows, needs=["read"], map_=map_ref,
                    links=[{"from_step": "read", "kind": "fine", "key": "recs",
                            "confidence": "exact", "scope": None}],
                    outputs=[{"role": "blif", "path": blif_name, "sha256":
                              sha256_file(blif) if blif.is_file() else ""}])
    if nm.atoms:
        placed: list[dict[str, Any]] = []
        for atom in nm.atoms:
            name = atom[0]
            target = by_name.get(name)
            placed.append(obj("placed", name, "site", name,
                              "hdl" if target else "unknown",
                              [link(target, "name")] if target else [],
                              recs=[x for x in atom[1].split(",") if x] if len(atom) > 1 else [],
                              attrs={"stale": atom[2] if len(atom) > 2 else "-"}))
        writer.add_step("place", "placed", {"id": "vpr", "version": ""}, placed, needs=["map"],
                        map_=map_ref,
                        links=[{"from_step": "map", "kind": "name", "key": "atom",
                                "confidence": "exact", "scope": None}])
    return writer.finish()


def main(argv: Sequence[str] | None = None) -> int:
    """odin3-prov-steps FLOW_DIR: convert the name map of a ``--prov`` run into step files."""
    args = list(sys.argv[1:] if argv is None else argv)
    if len(args) != 1 or args[0] in ("-h", "--help"):
        print("usage: odin3-prov-steps FLOW_DIR   (a --prov run directory)", file=sys.stderr)
        return 2
    flow_dir = Path(args[0])
    maps = sorted(flow_dir.glob("odin3/*.names"))
    if not maps:
        print(f"odin3-prov-steps: no odin3/*.names in {flow_dir} (run with --prov)",
              file=sys.stderr)
        return 1
    top = maps[0].name.split(".")[0]
    fpga_steps(maps[0], flow_dir, top, top, "", maps[0].name[:-6])
    print(f"wrote {flow_dir / 'prov' / 'index.json'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
