"""silicon_flow: Odin III to GDS and back, with provenance step files (v1.4).

Steps (docs/user/silicon-flow.md): read, elab, map, equiv, netlist, place, route, gds, extract,
lvs, recover. Odin III maps the design onto sky130 cells (``abc --gates``, ``dfflibmap``); OpenROAD
(its own Tcl, no Yosys) places and routes; KLayout writes the GDS; Magic extracts; Netgen checks
LVS; ``recover`` joins the extracted cells to the DEF placements by cell type and location and so
to the provenance, without names. Every step writes provenance step files through
``prov_steps.StepWriter``. External tools are optional: ``discover`` names what is missing.

Standard library only (KLayout runs in the Python of the tools directory).
"""

from __future__ import annotations

import csv
import json
import os
import re
import shutil
import subprocess
import sys
import time
from collections.abc import Callable, Collection, Mapping, Sequence
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "flow"))
import prov_steps  # noqa: E402

TOP_STEPS = ("read", "elab", "map", "equiv", "netlist", "place", "route", "gds", "extract", "lvs",
             "recover")
ODIN_ONLY = ("read", "elab", "map", "equiv", "netlist")
LIB_NAME = "sky130hd"
CONB = "sky130_fd_sc_hd__conb_1"
PHYS_RE = re.compile(r"__(tap|fill|decap|diode)")
SKIP_EXIT = 77


class MissingTools(Exception):
    """External tools or the PDK are not available; the message says what and how to get it."""


@dataclass
class Tools:
    odin3: Path
    openroad: Path | None = None
    magic: Path | None = None
    netgen: Path | None = None
    klayout_python: Path | None = None
    pdk: Path | None = None
    versions: dict[str, str] = field(default_factory=dict)


def _run(argv: Sequence[str | Path], cwd: Path | None = None, env: Mapping[str, str] | None = None,
         timeout: int = 3600) -> subprocess.CompletedProcess[str]:
    return subprocess.run([str(a) for a in argv], capture_output=True, text=True, cwd=cwd,
                          env=dict(os.environ, **env) if env else None, timeout=timeout,
                          stdin=subprocess.DEVNULL, check=False)


_ABS_PATH = re.compile(r"(?<![\w.~-])(?:/[^\s'\"(),:;]*|[A-Za-z]:\\[^\s'\"(),;]*)")


def redact_paths(text: str) -> str:
    """Replace absolute paths by ``<path>``; the step files refuse a path, the reason stays."""
    return _ABS_PATH.sub("<path>", text)


EQUIV_RESULT = {0: "equivalent", 1: "not-equivalent"}  # any other exit code: a tool error


def _reason(text: str) -> str:
    """The last line of an equiv-check output that is not a notice, paths redacted."""
    lines = [ln for ln in text.strip().splitlines() if "notice:" not in ln]
    return redact_paths(lines[-1] if lines else "gates are not equivalent to the RTL")[:200]


def equiv_verdict(checker: Path, rtl: Path, gates: Path,
                  run: Callable[[Sequence[str | Path]], subprocess.CompletedProcess[str]] = _run
                  ) -> tuple[dict[str, str], str | None, str]:
    """Run equiv-check on RTL against gates: (step check, WARNING line or None, raw output).
    A proven difference (exit 1) is a failure. An undriven output the tool offers to tie to 0 is
    tied (``--allow-undriven``); any other tool error or limit is a WARNING. Both are recorded
    as ``equiv: not proven: <reason>`` and the flow goes on."""
    base: list[str | Path] = [sys.executable, checker]
    eq = run([*base, rtl, gates])
    text = eq.stdout + eq.stderr
    note = ""
    first = _reason(text)
    if eq.returncode == 2 and "--allow-undriven" in text:
        note = "; undriven nets tied to 0 (--allow-undriven)"
        retry = run([*base, "--allow-undriven", rtl, gates])
        text += retry.stdout + retry.stderr
        eq = retry if retry.returncode == 0 else eq
        if retry.returncode != 0:
            note = ""
    if eq.returncode in EQUIV_RESULT and not note:
        return ({"name": "equiv-check", "result": EQUIV_RESULT[eq.returncode],
                 "detail": _reason(text)}, None, text)
    reason = "equiv: not proven: " + first + note
    return ({"name": "equiv-check", "result": "not-proven", "detail": reason}, "WARNING " + reason,
            text)


def _first_line(text: str) -> str:
    """First line of a tool's output, paths redacted, cut to 120 characters."""
    return redact_paths((text.strip().splitlines() or [""])[0])[:120]


def discover(odin3: Path, tools_dir: Path | None, pdk: Path | None) -> Tools:
    """Find the external tools: ``tools_dir`` layout (mamba/envs/{orcond,eda}/bin, orvenv/bin,
    pdk/sky130A), then PATH. Missing pieces stay None."""
    t = Tools(odin3=odin3)
    roots = [tools_dir] if tools_dir else []
    env_dir = os.environ.get("ODIN3_SILICON_TOOLS")
    if env_dir and not tools_dir:
        roots.append(Path(env_dir))

    def find(name: str, rel: Sequence[str]) -> Path | None:
        for root in roots:
            for r in rel:
                if (root / r / name).is_file():
                    return root / r / name
        found = shutil.which(name)
        return Path(found) if found else None

    t.openroad = find("openroad", ["mamba/envs/orcond/bin", "bin"])
    t.magic = find("magic", ["mamba/envs/eda/bin", "bin"])
    t.netgen = find("netgen", ["mamba/envs/eda/bin", "bin"])
    for cand in [r / "orvenv/bin/python" for r in roots] + [Path(sys.executable)]:
        if cand.is_file() and _run([cand, "-c", "import klayout.db"]).returncode == 0:
            t.klayout_python = cand
            break
    pdk_dirs = [pdk] if pdk else []
    if os.environ.get("PDK_ROOT"):
        pdk_dirs.append(Path(os.environ["PDK_ROOT"]) / "sky130A")
    pdk_dirs += [r / "pdk/sky130A" for r in roots]
    t.pdk = next((p for p in pdk_dirs if (p / "libs.ref/sky130_fd_sc_hd/lef").is_dir()), None)
    for name, exe, arg in (("openroad", t.openroad, "-version"), ("magic", t.magic, "--version"),
                           ("netgen", t.netgen, "-batch")):
        if exe is not None:
            probe = _run([exe, arg, "quit"] if name == "netgen" else [exe, arg], timeout=60)
            t.versions[name] = _first_line(probe.stdout or probe.stderr)
    return t


def missing(t: Tools, steps: Sequence[str]) -> list[str]:
    """What the requested steps need and the machine lacks, each with the install hint."""
    need: list[str] = []
    if any(s in steps for s in ("place", "route")) and t.openroad is None:
        need.append("openroad (micromamba create -n orcond -c litex-hub -c conda-forge "
                    "--override-channels openroad)")
    if any(s in steps for s in ("extract", "recover", "lvs")):
        if t.magic is None:
            need.append("magic (micromamba create -n eda -c litex-hub -c conda-forge "
                        "--override-channels magic netgen)")
        if "lvs" in steps and t.netgen is None:
            need.append("netgen (same environment as magic)")
    if "gds" in steps and t.klayout_python is None:
        need.append("a Python with the klayout module (pip install klayout)")
    if any(s not in ODIN_ONLY for s in steps) and t.pdk is None:
        need.append("the sky130A PDK, library sky130_fd_sc_hd (pip install ciel; ciel enable "
                    "--pdk sky130 -l sky130_fd_sc_hd <version>)")
    return need


# --- steps run by Odin III --------------------------------------------------------------------

def _rel(path: Path, base: Path) -> str:
    try:
        return path.resolve().relative_to(base.resolve()).as_posix()
    except ValueError:
        return path.name


def odin_script(lib: Path, design: Path, top: str, out: Path, upto: str,
                strict_tristate: bool = False) -> str:
    """The Odin III script of the ``elab`` (word level) or ``map`` (gates) run. ``tristate`` runs
    after ``flatten`` (a WARNING per conversion; ``strict_tristate``: a conversion is an error)."""
    tri = "tristate --strict" if strict_tristate else "tristate"
    head = f"read_techlib {lib}; read_verilog --nolatches {design}; hierarchy --top {top}; " \
           f"proc; flatten; {tri}; opt; lower --neg-edge keep"
    if upto == "elab":
        return f"{head}; dump_locs --types {out}/elab_locs.txt"
    return (f"{head}; write_blif --gates-as-names {out}/rtl.blif; abc --gates {LIB_NAME}; "
            f"dfflibmap {LIB_NAME}; write_blif --gates-as-names {out}/gates.blif; "
            f"dump_locs --types {out}/locs.txt; write_verilog {out}/g.v")


def set_param(text: str, top: str, name: str, value: str) -> str:
    """``text`` with the default of parameter ``name`` of module ``top`` set to ``value``.

    Odin III has no parameter override on the command line and the netlist has no parameters, so a
    testbench that instantiates ``top #(.NAME(v))`` needs the netlist built with that value. Only
    the first ``parameter NAME = ...`` in the module body or header is rewritten (the text up to the
    next ``,``, ``;`` or ``)``)."""
    mod = re.search(rf"\bmodule\s+{re.escape(top)}\b.*?\bendmodule\b", text, re.S)
    if mod is None:
        raise ValueError(f"--param: module {top} not found")
    decl = re.compile(rf"(\bparameter\s+(?:signed\s+)?(?:\[[^\]]*\]\s*)?{re.escape(name)}\s*=\s*)"
                      r"[^,;)]+")
    body, n = decl.subn(lambda m: m.group(1) + value, mod.group(0), count=1)
    if n == 0:
        raise ValueError(f"--param: module {top} has no 'parameter {name} = ...'")
    return text[:mod.start()] + body + text[mod.end():]


def parametrised_design(design: Path, top: str, params: Sequence[str], out: Path) -> Path:
    """A copy of ``design`` with the ``NAME=VALUE`` defaults set, in ``out/param_src`` beside
    symlinks to the other files of the design's directory (includes keep resolving). The copy
    has the original's file name, so provenance spans name the same file."""
    src = out / "param_src"
    src.mkdir(parents=True, exist_ok=True)
    for other in design.parent.iterdir():
        link = src / other.name
        if other.name != design.name and other.is_file() and not link.exists():
            link.symlink_to(other)
    text = design.read_text()
    for item in params:
        name, eq, value = item.partition("=")
        if not eq or not value:
            raise ValueError(f"--param {item!r}: expected NAME=VALUE")
        text = set_param(text, top, name, value)
    dst = src / design.name
    dst.write_text(text)
    return dst


def read_locs(path: Path) -> dict[str, tuple[str, list[str]]]:
    """dump_locs --types: node name to (cell type, source spans)."""
    nodes: dict[str, tuple[str, list[str]]] = {}
    for line in path.read_text().splitlines():
        parts = re.split(r"(?<!\\)\|", line, maxsplit=3)
        if len(parts) == 4 and parts[1].startswith("node:"):
            name = parts[2].replace("\\\\", "\\")
            nodes[name] = (parts[1][5:], [s for s in parts[3].split("|") if s])
    return nodes


def split_span(text: str, root: Path) -> dict[str, str]:
    """``file:L.C-EL.EC`` to {"file": relative path, "span": "L.C-EL.EC"}."""
    file, _, span = text.rpartition(":")
    return {"file": _rel(Path(file), root) if file.startswith("/") else file, "span": span}


def _legal(name: str) -> str:
    return name if re.fullmatch(r"[A-Za-z_]\w*", name) else "o3_" + re.sub(r"\W", "_", name)


def clean_netlist(gv: Path, clean: Path, locs: Mapping[str, tuple[str, list[str]]]
                  ) -> list[dict[str, str]]:
    """Strip attributes and comments, legalise names, give every constant a ``conb_1`` tie cell.
    Returns the prov rows: inst, odin_name, master, src_chain, tie (value or "")."""
    text = re.sub(r"\(\*.*?\*\)", "", gv.read_text(), flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    names: dict[str, str] = {}

    def esc(m: re.Match[str]) -> str:
        names.setdefault(m.group(1), _legal(m.group(1)))
        return names[m.group(1)] + " "

    text = re.sub(r"\\(\S+) ", esc, text)
    ties: list[tuple[str, str]] = []

    def tie_pin(m: re.Match[str]) -> str:
        net = f"o3_tie_net{len(ties)}"
        ties.append((net, m.group(2)))
        return f".{m.group(1)}({net})"

    text = re.sub(r"\.(\w+)\(1'b([01])\)", tie_pin, text)
    extra: list[str] = []
    for net, val in ties:
        extra.append(f"  wire {net};\n  {CONB} o3_tie_{net[11:]} (.{'HI' if val == '1' else 'LO'}"
                     f"({net}));\n")
    for m in re.finditer(r"^\s*assign\s+(\S+)\s*=\s*1'b([01])\s*;", text, re.M):
        idx = len(ties) + len(extra)
        pin = "HI" if m.group(2) == "1" else "LO"
        extra.append(f"  {CONB} o3_tie_a{idx} (.{pin}({m.group(1)}));\n")
        ties.append((m.group(1), m.group(2)))
    text = re.sub(r"^\s*assign\s+\S+\s*=\s*1'b[01]\s*;\n?", "", text, flags=re.M)
    text = text.replace("endmodule", "".join(extra) + "endmodule", 1)
    clean.write_text(text)
    const_src: dict[str, list[str]] = {"0": [], "1": []}
    for ctype, spans in locs.values():
        if ctype in ("$_CONST0_", "$_CONST1_"):
            const_src[ctype[7]] += spans
    rows: list[dict[str, str]] = []
    inv = {v: k for k, v in names.items()}
    for m in re.finditer(r"^\s*(sky130_fd_sc_hd__\w+)\s+(\S+)\s*\(", text, re.M):
        master, inst = m.groups()
        if inst.startswith("o3_tie_"):
            val = "1" if ".HI(" in text[m.end():m.end() + 20] else "0"
            rows.append({"inst": inst, "odin_name": "", "master": master,
                         "src_chain": "|".join(const_src[val]), "tie": val})
            continue
        orig = inv.get(inst, inst)
        rows.append({"inst": inst, "odin_name": orig, "master": master,
                     "src_chain": "|".join(locs.get(orig, ("", []))[1]), "tie": ""})
    return rows


def relative_chains(rows: Sequence[Mapping[str, str]], root: Path) -> list[dict[str, str]]:
    """The rows with every source span's file relative to root (no absolute path is kept)."""
    out = []
    for r in rows:
        parts = [split_span(x, root) for x in r["src_chain"].split("|") if x]
        out.append({**r, "src_chain": "|".join(f"{p['file']}:{p['span']}" for p in parts)})
    return out


def write_prov_csv(path: Path, rows: Sequence[Mapping[str, str]]) -> None:
    with path.open("w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=["inst", "odin_name", "master", "src_chain", "tie"],
                           lineterminator="\n")
        w.writeheader()
        w.writerows(rows)


# --- OpenROAD, KLayout, Magic, Netgen ---------------------------------------------------------

OUT_OF_SCOPE_EXIT = 3
NEEDS = {"elab": ["read"], "map": ["elab"], "equiv": ["map"], "netlist": ["map"],
         "place": ["netlist"], "route": ["place"], "gds": ["route"], "extract": ["gds"],
         "lvs": ["extract"], "recover": ["extract"]}


class OutOfScope(RuntimeError):
    """The design does not fit what the flow can place and route; the reason is recorded in the
    step files and printed as a WARNING, never dropped silently."""


def precheck_warnings(log: str) -> list[str]:
    """The ``WARNING precheck`` lines the Tcl driver printed when it adapted the floorplan."""
    return [ln.strip() for ln in log.splitlines() if ln.startswith("WARNING precheck")]


def openroad_reason(log: str) -> str:
    """The first ``[ERROR code] text`` line of an OpenROAD log, paths redacted."""
    for ln in log.splitlines():
        if ln.startswith("[ERROR"):
            return redact_paths(ln.strip())[:200]
    return ""


def run_openroad(t: Tools, out: Path, top: str, clock: str) -> None:
    assert t.openroad is not None and t.pdk is not None
    env = {"PDK": str(t.pdk), "OUT": str(out), "TOP": top, "CLK": clock}
    (out / "or").mkdir(exist_ok=True)
    r = _run([t.openroad, "-no_splash", "-exit", HERE / "openroad_flow.tcl"], cwd=out / "or",
             env=env)  # OpenROAD (2022) writes route.guide into its cwd
    (out / "openroad.log").write_text(r.stdout + r.stderr)
    if r.returncode != 0 or not (out / "or" / "route.def").is_file():
        reason = openroad_reason(r.stdout + r.stderr)
        if reason:
            raise OutOfScope(reason)
        raise RuntimeError(f"openroad failed (rc {r.returncode}); see openroad.log")


def run_klayout(t: Tools, out: Path, top: str) -> None:
    assert t.klayout_python is not None and t.pdk is not None
    r = _run([t.klayout_python, "-I", HERE / "klayout_gds.py", t.pdk, out, top])
    (out / "klayout.log").write_text(r.stdout + r.stderr)
    if r.returncode != 0:
        raise RuntimeError("klayout failed; see klayout.log")


def run_magic(t: Tools, out: Path, top: str) -> None:
    assert t.magic is not None and t.pdk is not None
    for mode in ("flat", "hier"):
        wd = out / f"magic_{mode}"
        wd.mkdir(exist_ok=True)
        env = {"PDK_ROOT": str(t.pdk.parent), "PDK": "sky130A", "OUT": str(out), "TOP": top,
               "MODE": mode}
        r = _run([t.magic, "-dnull", "-noconsole", "-rcfile",
                  t.pdk / "libs.tech/magic/sky130A.magicrc", HERE / "magic_extract.tcl"],
                 cwd=wd, env=env)
        (wd / "magic.log").write_text(r.stdout + r.stderr)
        if r.returncode != 0 or not (out / f"ext_{mode}.spice").is_file():
            raise RuntimeError(f"magic ({mode}) failed; see magic_{mode}/magic.log")


def ref_instance_line(inst: str, master: str, pins: Sequence[str],
                      conn: Mapping[str, str]) -> str:
    """One instance line of the reference SPICE. A pin the netlist leaves open (an unused
    output, such as the HI pin of a tie cell) gets a net of its own: an empty field would drop
    the pin from the line and move every later pin one place up."""
    nets = [conn.get(p) or f"nc_{inst}_{p}" for p in pins]
    return f"X{inst} " + " ".join(nets) + f" {master}"


def port_aliases(netlist: str, ports: Sequence[str]) -> tuple[dict[str, str], list[list[str]]]:
    """Nets that ``assign a = b;`` joins in final.v. The layout is one net with one port label
    per group, so the reference must use that name: every non-port member maps to the first
    port of its group (sorted order). Returns (rename, groups); a group without a layout
    port is left alone."""
    parent: dict[str, str] = {}

    def find(x: str) -> str:
        while parent.setdefault(x, x) != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    for m in re.finditer(r"^\s*assign\s+(\S+)\s*=\s*(\S+?)\s*;", netlist, re.M):
        parent[find(m.group(1))] = find(m.group(2))
    members: dict[str, list[str]] = {}
    for n in list(parent):
        members.setdefault(find(n), []).append(n)
    groups = [sorted(g) for g in members.values()]
    rename: dict[str, str] = {}
    for g in groups:
        named = [n for n in g if n in ports]
        if named:
            rename.update({n: named[0] for n in g if n not in ports})
    return rename, groups


def run_lvs(t: Tools, out: Path, top: str) -> dict[str, str]:
    """Netgen LVS of the hierarchical extraction against the OpenROAD netlist."""
    assert t.netgen is not None and t.pdk is not None
    cells = (t.pdk / "libs.ref/sky130_fd_sc_hd/spice/sky130_fd_sc_hd.spice").read_text()
    pins: dict[str, list[str]] = {}
    for m in re.finditer(r"^\.subckt (\S+)((?:[^\n]*)(?:\n\+[^\n]*)*)", cells, re.M):
        pins[m.group(1)] = m.group(2).replace("\n+", " ").split()
    ext = (out / "ext_hier.spice").read_text()
    head = re.search(rf"^\.subckt {re.escape(top)}((?:[^\n]*)(?:\n\+[^\n]*)*)", ext, re.M)
    if head is None:
        raise RuntimeError("extracted SPICE has no top subckt")
    ports = head.group(1).replace("\n+", " ").split()
    netlist = re.sub(r"//[^\n]*", "", (out / "or" / "final.v").read_text())
    pg = {"VPWR": "VDD", "VPB": "VDD", "VGND": "VSS", "VNB": "VSS"}
    rename, groups = port_aliases(netlist, ports)
    lines = [f".subckt {top} {' '.join(ports)}"]
    for im in re.finditer(r"^\s*(sky130_fd_sc_hd__\w+)\s+(\S+)\s*\((.*?)\);", netlist,
                          re.M | re.S):
        master, inst, body = im.groups()
        if PHYS_RE.search(master):
            continue
        conn = {k: rename.get(v.strip(), v.strip())
                for k, v in re.findall(r"\.(\w+)\(([^)]*)\)", body)}
        for p, n in pg.items():
            conn.setdefault(p, n)
        lines.append(ref_instance_line(inst, master, pins[master], conn))
    lines.append(f".ends {top}")
    if len(lines) == 2:  # no cell: the reference has no element and netgen refuses to compare
        return {"name": "netgen_lvs", "result": "not-applicable",
                "detail": "no cells in the design; nothing to compare"}
    (out / "ref.spice").write_text(cells + "\n" + "\n".join(lines) + "\n")
    setup = t.pdk / "libs.tech/netgen/sky130A_setup.tcl"
    (out / "lvs.tcl").write_text(f'lvs "{out}/ext_hier.spice {top}" "{out}/ref.spice {top}" '
                                 f"{setup} {out}/lvs.log\nquit\n")
    r = _run([t.netgen, "-batch", "source", out / "lvs.tcl"])
    (out / "netgen.out").write_text(r.stdout + r.stderr)
    log = (out / "lvs.log").read_text() if (out / "lvs.log").is_file() else ""
    final = [ln for ln in log.splitlines() if ln.startswith("Final result")]
    result = "match" if final and "match uniquely" in final[-1] else "mismatch"
    detail = redact_paths(final[-1]) if final else "no result"
    shared = [g for g in groups if any(n in rename for n in g)]
    if shared:
        detail += ("; WARNING: ports sharing one net are one label in the layout: "
                   + " ".join("{" + ",".join(g) + "}" for g in shared))
    return {"name": "netgen_lvs", "result": result, "detail": detail}


# --- census, join -----------------------------------------------------------------------------

def kind_of(master: str, step: str) -> str:
    m = master.split("__", 1)[-1]
    if m.startswith("tap"):
        return "tap"
    if m.startswith(("fill", "decap")):
        return "fill" if m.startswith("fill") else "decap"
    if m.startswith("diode"):
        return "antenna"
    if m.startswith("conb"):
        return "tie"
    if step == "s07_cts":
        return "cts_buffer"
    if step == "s08_repair_timing":
        return "hold_buffer"
    if m.startswith(("buf", "clkbuf", "inv")):
        return "repair_buffer"
    return "other"


def load_dump(path: Path) -> dict[str, dict[str, str]]:
    with path.open() as fh:
        return {r["inst"]: r for r in csv.DictReader(fh)}


def census(out: Path) -> list[dict[str, Any]]:
    """Per-step instance census against the previous dump (kept, moved, resized, inserted by
    kind, removed, renamed). Also writes census.csv."""
    files = sorted(p for p in (out / "or").glob("s[0-9][0-9]_*.csv"))
    rows: list[dict[str, Any]] = []
    prev: dict[str, dict[str, str]] | None = None
    for p in files:
        step = p.stem
        cur = load_dump(p)
        c: dict[str, Any] = {"step": step, "total": len(cur), "kept": 0, "moved": 0,
                             "resized": 0, "inserted": 0, "removed": 0, "renamed": 0,
                             "inserted_kinds": {}}
        if prev is None:
            c["kept"] = len(cur)
        else:
            rem = {n: r for n, r in prev.items() if n not in cur}
            add = {n: r for n, r in cur.items() if n not in prev}
            for n, r in cur.items():
                if n in prev:
                    same_pos = (prev[n]["x"], prev[n]["y"]) == (r["x"], r["y"])
                    if prev[n]["master"] != r["master"]:
                        c["resized"] += 1
                    elif not same_pos:
                        c["moved"] += 1
                    else:
                        c["kept"] += 1
            where = {(r["master"], r["x"], r["y"]): n for n, r in rem.items()}
            for n, r in list(add.items()):
                key = (r["master"], r["x"], r["y"])
                if key in where:
                    c["renamed"] += 1
                    del rem[where[key]]
                    del add[n]
            c["removed"], c["inserted"] = len(rem), len(add)
            for r in add.values():
                k = kind_of(r["master"], step)
                c["inserted_kinds"][k] = c["inserted_kinds"].get(k, 0) + 1
        rows.append(c)
        prev = cur
    with (out / "census.csv").open("w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=list(rows[0]), lineterminator="\n")
        w.writeheader()
        for r in rows:
            w.writerow({**r, "inserted_kinds": json.dumps(r["inserted_kinds"], sort_keys=True)})
    return rows


def lef_sizes(lef: Path) -> dict[str, tuple[int, int]]:
    text = lef.read_text()
    return {m.group(1): (round(float(m.group(2)) * 1000) // 5, round(float(m.group(3)) * 1000) // 5)
            for m in re.finditer(r"MACRO (\S+).*?SIZE ([\d.]+) BY ([\d.]+)", text, re.S)}


def def_components(path: Path) -> dict[tuple[str, int, int], str]:
    comps: dict[tuple[str, int, int], str] = {}
    pat = r"- (\S+) (\S+)(?: \+ SOURCE \w+)? \+ (?:FIXED|PLACED) \( (-?\d+) (-?\d+) \) (\w+)"
    for m in re.finditer(pat, path.read_text()):
        comps[(m.group(2), int(m.group(3)), int(m.group(4)))] = m.group(1)
    return comps


def extracted_cells(ext: Path, sizes: Mapping[str, tuple[int, int]]) -> list[dict[str, Any]]:
    """Cells of a Magic hierarchical ``.ext``: master, id, lower-left corner in nm. Magic units
    are 5 nm; the transform ``a b c d e f`` maps the cell box to the parent."""
    rows: list[dict[str, Any]] = []
    for m in re.finditer(r"^use (\S+) (\S+) (-?\d+) (-?\d+) (-?\d+) (-?\d+) (-?\d+) (-?\d+)",
                         ext.read_text(), re.M):
        master = m.group(1)
        if not master.startswith("sky130_fd_sc_hd__") or master not in sizes:
            continue
        a, b, c, d, e, f = (int(m.group(i)) for i in range(3, 9))
        w, h = sizes[master]
        xs = [a * x + b * y + c for x in (0, w) for y in (0, h)]
        ys = [d * x + e * y + f for x in (0, w) for y in (0, h)]
        rows.append({"id": m.group(2), "master": master, "x": min(xs) * 5, "y": min(ys) * 5})
    return rows


def name_survival(out: Path, top: str, prov: Sequence[Mapping[str, str]]) -> dict[str, int]:
    gds = (out / f"{top}.gds").read_bytes()
    named = (out / f"{top}.named.gds").read_bytes() if (out / f"{top}.named.gds").is_file() else b""
    spice = (out / "ext_hier.spice").read_text() + (out / "ext_flat.spice").read_text()
    names = [p["inst"] for p in prov]
    return {"odin_instances": len(names),
            "names_in_gds": sum(1 for n in names if n.encode() in gds),
            "names_in_gds_with_klayout_properties": sum(1 for n in names if n.encode() in named),
            "names_in_extracted_spice": sum(
                1 for n in names if re.search(rf"\b{re.escape(n)}\b", spice))}


# --- provenance objects per stage -------------------------------------------------------------

def _spans(chain: Sequence[str]) -> list[dict[str, str]]:
    return [{"file": s.rpartition(":")[0], "span": s.rpartition(":")[2]} for s in chain if s]


def hdl_objects(ir: Mapping[str, tuple[str, list[str]]], root: Path
                ) -> tuple[list[dict[str, Any]], dict[str, str]]:
    seen: dict[str, str] = {}
    rows: list[dict[str, Any]] = []
    for _, spans in ir.values():
        for s in spans:
            sp = split_span(s, root)
            key = f"{sp['file']}:{sp['span']}"
            if key not in seen:
                seen[key] = f"hdl:{key}"
                rows.append(prov_steps.obj("hdl", key, "stmt", key, "hdl", src=[sp]))
    return rows, seen


def ir_objects(ir: Mapping[str, tuple[str, list[str]]], hdl: Mapping[str, str], root: Path
               ) -> list[dict[str, Any]]:
    rows = []
    for name, (ctype, spans) in sorted(ir.items()):
        sps = [split_span(s, root) for s in spans]
        links = [prov_steps.link(hdl[f"{s['file']}:{s['span']}"], "fine") for s in sps]
        kind = "port" if ctype.startswith("$port") else "cell"
        rows.append(prov_steps.obj("ir", name, kind, name, "hdl" if links else "unknown", links,
                                   type=ctype, src=sps))
    return rows


def gates_objects(prov: Sequence[Mapping[str, str]], ir: Mapping[str, tuple[str, list[str]]],
                  root: Path) -> list[dict[str, Any]]:
    """Gate rows. A gate's ``cone`` links go to the HDL statement objects of its source chain
    (those that some IR cell also carries), one link per span. The IR cells that share a span
    are the other end of their ``fine`` links to the same object, so the IR set of a gate is
    recoverable and the number of links is linear in gates (all-pairs gate/IR links took more than
    6 GiB on a 17,565-gate design)."""
    known: set[str] = set()
    for _, spans in ir.values():
        for s in spans:
            sp = split_span(s, root)
            known.add(f"{sp['file']}:{sp['span']}")
    rows = []
    for r in prov:
        chain = [x for x in r["src_chain"].split("|") if x]
        targets = sorted({c for c in chain if c in known})
        links = [prov_steps.link(f"hdl:{c}", "cone", "medium") for c in targets]
        origin = "hdl" if links else ("physical:tie" if r["tie"] else "unknown")
        rows.append(prov_steps.obj("gates", r["inst"], "cell", r["inst"], origin, links,
                                   type=r["master"], src=_spans(chain),
                                   attrs={"odin_name": r["odin_name"], "tie": r["tie"]}))
    return rows


def placed_objects(stage: str, dump: Mapping[str, Mapping[str, str]],
                   prev: Mapping[str, Mapping[str, str]], prov: Mapping[str, Mapping[str, str]],
                   prev_stage: str, first_seen: Mapping[str, str]) -> list[dict[str, Any]]:
    rows = []
    for name, r in sorted(dump.items()):
        attrs = {"master": r["master"], "x": int(r["x"]), "y": int(r["y"]),
                 "orient": r["orient"], "status": r["status"]}
        if name in prov:
            links = []
            gm = prov[name]["master"]
            kind = "name" if r["master"] == gm else "resize"
            if prev_stage == "gates":
                links.append(prov_steps.link(f"gates:{name}", kind))
                if kind == "resize":
                    attrs["resized_from"] = gm
            else:
                links.append(prov_steps.link(f"placed:{name}", "id"))
            rows.append(prov_steps.obj(stage, name, "cell", name, "hdl", links, type=r["master"],
                                       attrs=attrs))
        elif name in prev and prev_stage != "gates":
            rows.append(prov_steps.obj(stage, name, "cell", name,
                                       f"physical:{kind_of(r['master'], first_seen.get(name, ''))}",
                                       [prov_steps.link(f"placed:{name}", "id")], type=r["master"],
                                       attrs=attrs))
        else:
            origin = f"physical:{kind_of(r['master'], first_seen.get(name, stage))}"
            rows.append(prov_steps.obj(stage, name, "cell", name, origin, [], type=r["master"],
                                       attrs=attrs))
    return rows


def gds_objects(comps: Mapping[tuple[str, int, int], str], hdl_names: Collection[str],
                first_seen: Mapping[str, str]) -> list[dict[str, Any]]:
    """Rows of the gds step. A physical cell is labelled from the step that first showed it, as
    at ``placed`` and ``routed``, so a clock-tree buffer is ``physical:cts_buffer`` here too."""
    return [prov_steps.obj("gds", f"{m}@{x},{y}", "cell", name,
                           "hdl" if name in hdl_names
                           else f"physical:{kind_of(m, first_seen.get(name, ''))}",
                           [prov_steps.link(f"routed:{name}", "id")], type=m,
                           attrs={"master": m, "x": x, "y": y})
            for (m, x, y), name in sorted(comps.items())]


# --- the runner -------------------------------------------------------------------------------

@dataclass
class Run:
    design: Path
    top: str
    out: Path
    lib: Path
    tools: Tools
    clock: str = ""
    steps: tuple[str, ...] = TOP_STEPS
    root: Path = field(default_factory=Path)
    log: list[str] = field(default_factory=list)
    seconds: dict[str, float] = field(default_factory=dict)  # wall clock per step (not in log)
    strict_tristate: bool = False  # --strict-tristate: a tri-state conversion is an error


def _tool_entry(t: Tools, name: str) -> dict[str, str]:
    return {"id": name, "version": t.versions.get(name, "")}


def _version(odin3: Path) -> str:
    return _first_line(_run([odin3, "--version"]).stdout)


def _first_seen(out: Path) -> dict[str, str]:
    seen: dict[str, str] = {}
    for p in sorted((out / "or").glob("s[0-9][0-9]_*.csv")):
        for name in load_dump(p):
            seen.setdefault(name, p.stem)
    return seen


def run(r: Run) -> int:
    """Run the requested steps in order; return 0, or the exit code of the first failure."""
    out, t, top = r.out, r.tools, r.top
    out.mkdir(parents=True, exist_ok=True)
    root = r.root or r.design.parent
    writer = prov_steps.StepWriter(out, "silicon-sky130", r.design.name, top, _version(t.odin3))
    writer.add_design_file(r.design.name, r.design)
    for name, version in t.versions.items():
        writer.add_tool(name, version)
    writer.add_tool("odin3", _version(t.odin3))
    state: dict[str, Any] = {}
    todo = [s for s in TOP_STEPS if s in r.steps]
    clock = {"id": "odin3", "version": _version(t.odin3)}
    for step in todo:
        started = time.time()
        try:
            _do_step(r, writer, step, state, root, clock)
        except OutOfScope as exc:
            return _out_of_scope(r, writer, step, str(exc), started)
        except (RuntimeError, OSError, ValueError, subprocess.TimeoutExpired) as exc:
            # ValueError includes prov_steps.PrivacyError (an absolute path about to be written)
            r.log.append(f"{step}: FAILED {exc}")
            r.seconds[step] = round(time.time() - started, 1)
            return 1
        r.log.append(f"{step}: ok")
        r.seconds[step] = round(time.time() - started, 1)
    writer.finish()
    return 0


def _out_of_scope(r: Run, w: prov_steps.StepWriter, step: str, reason: str,
                  started: float) -> int:
    """Record 'out of scope: <reason>' as a check-only step, finish the index, print a WARNING."""
    text = f"out of scope: {reason}"
    w.add_step(step, "gates", {"id": "silicon-flow", "version": ""}, None,
               needs=NEEDS.get(step, []),
               check={"name": "scope", "result": "out-of-scope", "detail": text})
    w.finish()
    print(f"WARNING: {r.design.name}: {step}: {text}", file=sys.stderr)
    r.log.append(f"{step}: OUT OF SCOPE {reason}")
    r.seconds[step] = round(time.time() - started, 1)
    return OUT_OF_SCOPE_EXIT


def _odin(r: Run, upto: str) -> None:
    script = odin_script(r.lib, r.design, r.top, r.out, upto, r.strict_tristate)
    res = _run([r.tools.odin3, "-p", script], cwd=r.design.parent)
    (r.out / f"odin_{upto}.log").write_text(res.stdout + res.stderr)
    if res.returncode != 0:
        raise RuntimeError(f"odin3 {upto} failed; see odin_{upto}.log")


def _do_step(r: Run, w: prov_steps.StepWriter, step: str, st: dict[str, Any], root: Path,
             odin: Mapping[str, str]) -> None:
    out, t, top = r.out, r.tools, r.top
    needs = NEEDS.get(step, [])
    if step in ("read", "elab"):
        if "ir" not in st:
            _odin(r, "elab")
            st["ir"] = read_locs(out / "elab_locs.txt")
        rows, st["hdl"] = hdl_objects(st["ir"], root)
        if step == "read":
            w.add_step("read", "hdl", odin, rows,
                       inputs=[{"role": "design", "path": r.design.name,
                                "sha256": prov_steps.sha256_file(r.design)}])
        else:
            w.add_step("elab", "ir", odin, ir_objects(st["ir"], st["hdl"], root), needs=["read"],
                       links=[{"from_step": "read", "kind": "fine", "key": "src",
                               "confidence": "exact", "scope": None}])
    elif step == "map":
        _odin(r, "map")
        st["locs"] = read_locs(out / "locs.txt")
        st["prov"] = relative_chains(clean_netlist(out / "g.v", out / "clean.v", st["locs"]), root)
        write_prov_csv(out / "prov.csv", st["prov"])
        w.add_step("map", "gates", odin, gates_objects(st["prov"], st.get("ir", {}), root),
                   needs=needs, outputs=_outs(out, ["gates.blif", "g.v"]),
                   links=[{"from_step": "read", "kind": "cone", "key": "src", "scope": None,
                           "confidence": "medium"}])
    elif step == "equiv":
        checker = HERE.parent / "equiv-check" / "equiv_check.py"
        check, warning, text = equiv_verdict(checker, out / "rtl.blif", out / "gates.blif")
        (out / "equiv.txt").write_text(text)
        w.add_step("equiv", "gates", {"id": "equiv-check", "version": ""}, None, needs=needs,
                   check=check)
        if warning:
            print(warning, file=sys.stderr)
        elif check["result"] != "equivalent":
            raise RuntimeError(f"equiv-check: {check['detail']}")
    elif step == "netlist":
        w.add_step("netlist", "gates", {"id": "clean_netlist", "version": ""}, None,
                   needs=needs, outputs=_outs(out, ["clean.v", "prov.csv"]),
                   check={"name": "ties", "result": "ok",
                          "detail": f"{sum(1 for p in st.get('prov', []) if p['tie'])} tie cells"})
    elif step == "place" or step == "route":
        _openroad(r, w, step, st, needs)
    elif step == "gds":
        run_klayout(t, out, top)
        comps = def_components(out / "or" / "route.def")
        rows = gds_objects(comps, {p["inst"] for p in st.get("prov", [])}, _first_seen(out))
        w.add_step("gds", "gds", {"id": "klayout", "version": t.versions.get("klayout", "")},
                   rows, needs=needs, outputs=_outs(out, [f"{top}.gds"]))
    elif step == "extract":
        _extract(r, w, st, needs)
    elif step == "lvs":
        res = run_lvs(t, out, top)
        w.add_step("lvs", "extracted", {"id": "netgen", "version": t.versions.get("netgen", "")},
                   None, needs=needs, check=res)
        if res["result"] == "mismatch":
            raise RuntimeError(f"LVS: {res['detail']}")
    elif step == "recover":
        _recover(r, w, st, needs)


def _outs(out: Path, names: Sequence[str]) -> list[dict[str, str]]:
    return [{"role": n.rsplit(".", 1)[-1], "path": n, "sha256": prov_steps.sha256_file(out / n)}
            for n in names if (out / n).is_file()]


def _openroad(r: Run, w: prov_steps.StepWriter, step: str, st: dict[str, Any],
              needs: Sequence[str]) -> None:
    out, t = r.out, r.tools
    if "or_done" not in st:
        run_openroad(t, out, r.top, r.clock)
        st["or_done"] = True
        st["census"] = census(out)
        st["seen"] = _first_seen(out)
        st["precheck"] = precheck_warnings((out / "openroad.log").read_text())
        for warning in st["precheck"]:
            print(warning, file=sys.stderr)
    seen: Mapping[str, str] = st["seen"]
    prov = {p["inst"]: p for p in st.get("prov", [])}
    if not prov:  # the netlist step was not run in this process: read the side table
        with (out / "prov.csv").open() as fh:
            prov = {p["inst"]: p for p in csv.DictReader(fh)}
    dumps = sorted((out / "or").glob("s[0-9][0-9]_*.csv"))
    if step == "place":
        last = [p for p in dumps if p.stem < "s09"][-1]
        rows = placed_objects("placed", load_dump(last), {}, prov, "gates", seen)
        cen = [c for c in st["census"] if c["step"] < "s09"]
        w.add_step("place", "placed", _tool_entry(t, "openroad"), rows, needs=needs,
                   census={"steps": cen}, outputs=_outs(out, []),
                   check={"name": "precheck", "result": "adapted",
                          "detail": "; ".join(st["precheck"])} if st["precheck"] else None,
                   links=[{"from_step": "map", "kind": "name", "key": "instance", "scope": None,
                           "confidence": "exact"},
                          {"from_step": "place", "kind": "physical_origin", "key": None,
                           "scope": "inserted", "confidence": "exact"}])
    else:
        prev = load_dump([p for p in dumps if p.stem < "s09"][-1])
        rows = placed_objects("routed", load_dump(dumps[-1]), prev, prov, "placed", seen)
        cen = [c for c in st["census"] if c["step"] >= "s09"]
        w.add_step("route", "routed", _tool_entry(t, "openroad"), rows, needs=needs,
                   census={"steps": cen},
                   links=[{"from_step": "place", "kind": "id", "key": "instance", "scope": None,
                           "confidence": "exact"},
                          {"from_step": "route", "kind": "physical_origin", "key": None,
                           "scope": "inserted", "confidence": "exact"}])
        (out / "census.json").write_text(json.dumps(st["census"], indent=1, sort_keys=True) + "\n")


def _extract(r: Run, w: prov_steps.StepWriter, st: dict[str, Any], needs: Sequence[str]) -> None:
    out, t, top = r.out, r.tools, r.top
    assert t.pdk is not None
    run_magic(t, out, top)
    sizes = lef_sizes(t.pdk / "libs.ref/sky130_fd_sc_hd/lef/sky130_fd_sc_hd.lef")
    st["ext"] = extracted_cells(out / "magic_hier" / f"{top}.ext", sizes)
    rows = [prov_steps.obj("extracted", f"{c['id']}@{c['x']},{c['y']}", "cell", c["id"],
                           "unknown", [prov_steps.link(f"gds:{c['master']}@{c['x']},{c['y']}",
                                                       "location_match", "high")],
                           type=c["master"],
                           attrs={"master": c["master"], "x": c["x"], "y": c["y"],
                                  "ext_id": c["id"]}) for c in st["ext"]]
    w.add_step("extract", "extracted", _tool_entry(t, "magic"), rows, needs=needs,
               outputs=_outs(out, ["ext_hier.spice", "ext_flat.spice"]),
               links=[{"from_step": "gds", "kind": "location_match", "key": "master,x,y",
                       "scope": None, "confidence": "high"}])


def _recover(r: Run, w: prov_steps.StepWriter, st: dict[str, Any], needs: Sequence[str]) -> None:
    out, t, top = r.out, r.tools, r.top
    assert t.pdk is not None
    if "ext" not in st:
        sizes = lef_sizes(t.pdk / "libs.ref/sky130_fd_sc_hd/lef/sky130_fd_sc_hd.lef")
        st["ext"] = extracted_cells(out / "magic_hier" / f"{top}.ext", sizes)
    comps = def_components(out / "or" / "route.def")
    with (out / "prov.csv").open() as fh:
        prov = {p["inst"]: p for p in csv.DictReader(fh)}
    rows = []
    matched = logic = 0
    for c in st["ext"]:
        name = comps.get((c["master"], c["x"], c["y"]))
        phys = PHYS_RE.search(c["master"]) is not None
        logic += not phys
        matched += (name is not None) and not phys
        links = [prov_steps.link(f"extracted:{c['id']}@{c['x']},{c['y']}", "id")]
        if name:
            links.append(prov_steps.link(f"routed:{name}", "location_match", "high"))
        origin = "hdl" if name in prov else f"physical:{kind_of(c['master'], 'x')}"
        if name is None:
            origin = "unknown"
        rows.append(prov_steps.obj("recovered", name or f"unjoined:{c['master']}@{c['x']},{c['y']}",
                                   "cell", name or "", origin, links, type=c["master"],
                                   attrs={"def_comp": name or "", "join": "matched" if name
                                          else "unmatched", "master": c["master"],
                                          "x": c["x"], "y": c["y"]}))
    names = name_survival(out, top, list(prov.values()))
    summary = {"extracted_cells": len(st["ext"]), "logic_cells": logic, "join_matched": matched,
               "join_rate": round(matched / logic, 4) if logic else None,
               "logic_with_hdl_chain": sum(1 for x in rows if x["origin"] == "hdl"
                                           and x["attrs"]["join"] == "matched"
                                           and PHYS_RE.search(x["type"]) is None),
               **names}
    (out / "join.json").write_text(json.dumps(summary, indent=1, sort_keys=True) + "\n")
    w.add_step("recover", "recovered", {"id": "join", "version": ""}, rows, needs=needs,
               check={"name": "name_free_join", "result": "ok" if matched == logic else "partial",
                      "detail": f"{matched}/{logic} logic cells joined"},
               links=[{"from_step": "extract", "kind": "id", "key": None, "scope": None,
                       "confidence": "exact"},
                      {"from_step": "route", "kind": "location_match", "key": "master,x,y",
                       "scope": None, "confidence": "high"}])


def default_lib() -> Path:
    """lib/sky130hd.o3lib of this checkout, or of the installed data directory."""
    for cand in (HERE.parents[1] / "lib" / "sky130hd.o3lib",
                 HERE.parents[1] / "data" / "lib" / "sky130hd.o3lib"):
        if cand.is_file():
            return cand
    return HERE.parents[1] / "lib" / "sky130hd.o3lib"


def main(argv: Sequence[str] | None = None) -> int:
    """CLI: silicon_flow.py DESIGN.v --top T --out DIR [--tools DIR] [--pdk DIR] [--odin3 PATH]
    [--steps a,b] [--clock PORT] [--param NAME=VALUE]. Exit 0 ok, 1 a step failed, 2 usage or
    missing tools."""
    import argparse

    ap = argparse.ArgumentParser(prog="silicon_flow", description=(__doc__ or "").split("\n")[0])
    ap.add_argument("design", type=Path)
    ap.add_argument("--top", required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--tools", type=Path, help="user-space tools directory (see the manual)")
    ap.add_argument("--pdk", type=Path, help="the sky130A directory of the PDK")
    ap.add_argument("--odin3", type=Path, default=None)
    ap.add_argument("--steps", default=",".join(TOP_STEPS), help="comma list of steps")
    ap.add_argument("--clock", default="", help="clock port (enables CTS)")
    ap.add_argument("--lib", type=Path, default=None)
    ap.add_argument("--param", action="append", default=[], metavar="NAME=VALUE",
                    help="set the default of a parameter of the top module (a testbench that "
                         "overrides it needs the netlist built with its value); repeatable")
    ns = ap.parse_args(list(sys.argv[1:] if argv is None else argv))
    steps = tuple(x for x in ns.steps.split(",") if x)
    bad = [x for x in steps if x not in TOP_STEPS]
    if bad:
        print(f"silicon_flow: unknown step(s) {', '.join(bad)} ({', '.join(TOP_STEPS)})",
              file=sys.stderr)
        return 2
    odin3 = (ns.odin3 or Path(shutil.which("odin3") or HERE.parents[1] / "build/release/odin3"))
    if not odin3.is_file():
        print(f"silicon_flow: no odin3 binary at {odin3}", file=sys.stderr)
        return 2
    t = discover(odin3.resolve(), ns.tools, ns.pdk)
    gone = missing(t, steps)
    if gone:
        print("silicon_flow: missing tools for the requested steps:\n  " + "\n  ".join(gone),
              file=sys.stderr)
        return SKIP_EXIT if os.environ.get("ODIN3_SILICON_SKIP") else 2
    design = ns.design.resolve()
    if ns.param:
        try:
            design = parametrised_design(design, ns.top, ns.param, ns.out.resolve())
        except ValueError as exc:
            print(f"silicon_flow: {exc}", file=sys.stderr)
            return 2
    run_ = Run(design, ns.top, ns.out.resolve(), (ns.lib or default_lib()).resolve(),
               t, ns.clock, steps)
    code = run(run_)
    for line in run_.log:
        print(line)
    return code


if __name__ == "__main__":
    sys.exit(main())
