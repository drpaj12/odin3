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
import os
import re
import shutil
import subprocess
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
_HOME = "/home" + "/"  # built from parts: the repo forbids the literal in tracked files
_PRIVATE = re.compile(r"(^|[\s=:\"'(,])(" + _HOME + r"|/Users/|/root/|[A-Za-z]:\\)")


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


class PreRow:
    """An object line already serialised (exactly as ``StepWriter`` writes a dict row), with the
    fields ``add_step`` counts. For rows with very long ``links`` lists: building one dict per
    link costs memory and time and the link text repeats, so the caller joins cached link text.
    The caller guarantees the text holds no absolute path (``add_step`` does not check it)."""

    __slots__ = ("text", "kind", "origin", "link_kinds")

    def __init__(self, text: str, kind: str, origin: str, link_kinds: Mapping[str, int]) -> None:
        self.text = text
        self.kind = kind
        self.origin = origin
        self.link_kinds = link_kinds


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
                 objects: Iterable[Mapping[str, Any] | PreRow] | None, needs: Sequence[str] = (),
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
                    if isinstance(row, PreRow):
                        fh.write(row.text + "\n")
                        kind, origin, link_kinds = row.kind, row.origin, row.link_kinds
                    else:
                        _check_private(row, f"{step} object {row.get('id')}")
                        fh.write(json.dumps(row, sort_keys=True, separators=(",", ":")) + "\n")
                        kind, origin = row["kind"], row["origin"]
                        link_kinds = {}
                        for lk in row["links"]:
                            link_kinds[lk["kind"]] = link_kinds.get(lk["kind"], 0) + 1
                    counts["objects"] += 1
                    for field, val in (("by_kind", kind), ("by_origin", origin)):
                        counts[field][val] = counts[field].get(val, 0) + 1
                    for lkind, lcount in link_kinds.items():
                        by_link = counts["by_link_kind"]
                        by_link[lkind] = by_link.get(lkind, 0) + lcount
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

def _rel_file(file: str, root: Path | None) -> str:
    """A source file name without an absolute prefix: relative to ``root`` when it lies under
    it, else the base name (the same rule as ``silicon_flow._rel``)."""
    if not (file.startswith("/") or _PRIVATE.search(file)):
        return file
    if root is not None:
        try:
            return Path(file).resolve().relative_to(root.resolve()).as_posix()
        except ValueError:
            pass
    return re.split(r"[\\/]", file)[-1]


def odin3_version(binary: Path | str | None = None) -> str:
    """First line of ``odin3 --version`` (the binary given, ``$ODIN3_CLI`` or ``odin3`` on PATH);
    empty when none can be run."""
    exe = str(binary) if binary else os.environ.get("ODIN3_CLI") or shutil.which("odin3")
    if not exe:
        return ""
    try:
        res = subprocess.run([exe, "--version"], capture_output=True, text=True, check=False,
                             timeout=30)
    except (OSError, subprocess.SubprocessError):
        return ""
    lines = res.stdout.strip().splitlines()
    return lines[0].strip() if res.returncode == 0 and lines else ""


def fpga_steps(map_path: Path, out: Path, design: str, top: str, odin3_version: str,
               blif_name: str, tools: Sequence[tuple[str, str]] = (),
               root: Path | None = None) -> Path:
    """Write ``out/prov`` for a ``--prov`` run from its name map: stages ``hdl`` (constructs),
    ``luts`` (objects of the BLIF, link ``fine`` to the constructs of their record chain), and
    ``placed`` (atoms with a site record, link ``name`` to the LUT object) when
    ``odin3-prov-post`` added ``A`` lines. The chains stay in the map (``recs``). Source file
    names are made relative to ``root`` (base name when outside it or ``root`` is None)."""
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
                  c.name or f"c{number}", "hdl", src=[{"file": _rel_file(span.file, root), "span":
                  f"{span.line}.{span.col}-{span.end_line}.{span.end_col}"}] if span else [])
        row["attrs"] = {"level": c.level, "parent": f"hdl:c{c.parent}" if c.parent else None}
        hdl_rows.append(row)
    writer.add_step("read", "hdl", tool, hdl_rows, map_=map_ref,
                    outputs=[{"role": "map", "path": map_path.name,
                              "sha256": map_ref["sha256"]}])
    number_of = {c.number: n for n, c in enumerate(nm.constructs, 1)}
    blif = map_path.with_name(blif_name)
    by_name: dict[str, str] = {}
    # a chain holds up to every construct of the design: the link text is made once per construct
    # and joined per object, and the rows are streamed to the file (never all held at once)
    fine_text = {n: json.dumps(link(f"hdl:c{n}", "fine"), sort_keys=True, separators=(",", ":"))
                 for n in number_of.values()}
    sentinel = json.dumps(["links\0"], separators=(",", ":"))

    def lut_rows() -> Iterable[PreRow]:
        for o in nm.objects:
            prov = nm.provenance(o)
            numbers = [number_of[c.number] for c in nm.chain_of(prov)]
            chain_src = []
            for r in prov:
                loc = nm.location(r)
                if loc is not None:
                    chain_src.append({"file": _rel_file(loc.file, root),
                                      "span": f"{loc.line}.{loc.col}-{loc.end_line}.{loc.end_col}"})
            kind = {"net": "net", "node": "cell", "wire": "net"}.get(o.kind, "net")
            if o.level == "lut":
                kind = "lut"
            row = obj("luts", f"{o.module}/{o.name}", kind, o.name,
                      "hdl" if numbers or chain_src else "unknown",
                      src=chain_src[:8], recs=[f"r{n}" for n in o.records],
                      attrs={"module": o.module, "level": o.level})
            row["links"] = ["links\0"]
            _check_private(row, f"map object {row['id']}")
            text = json.dumps(row, sort_keys=True, separators=(",", ":"))
            text = text.replace(sentinel, "[" + ",".join(map(fine_text.__getitem__, numbers))
                                + "]", 1)
            by_name.setdefault(o.name, row["id"])
            yield PreRow(text, kind, row["origin"], {"fine": len(numbers)} if numbers else {})

    writer.add_step("map", "luts", tool, lut_rows(), needs=["read"], map_=map_ref,
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
    try:
        fpga_steps(maps[0], flow_dir, top, top, odin3_version(), maps[0].name[:-6])
    except PrivacyError as exc:
        print(f"odin3-prov-steps: {exc}", file=sys.stderr)
        return 2
    print(f"wrote {flow_dir / 'prov' / 'index.json'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
