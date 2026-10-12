"""vtr-models: the arch's ``<models>`` for CI, and the library check (spec 3E §15.2, VB-16).

Usage::

    vtr-models extract ARCH.xml [--out FILE] [--vtr-root DIR]
    vtr-models check FILE [--arch ARCH.xml]
    vtr-models check-lib LIB.o3lib [--models FILE] [--vtr-root DIR]

``extract`` writes the models JSON (``schema`` 1, ``arch``, ``arch_sha256``, ``vtr_commit``,
``lut_size``, ``ram_addr_width``, ``models``). A model port's width follows R11: the largest
``num_pins`` over the pb_types whose ``blif_model`` is ``.subckt <model>``, where the
``port_class="data*"`` ports of a ``class="memory"`` pb_type count 1 (VPR's ProcessMemoryClass);
a pb_type port whose direction or ``is_clock`` differs from its model is refused, as VPR does.
``lut_size`` and ``ram_addr_width`` come from VTR's own ``vtr.log_parse`` (decision 5: VTR's
readers, not ours), run as ``$VTR_ROOT/.venv/bin/python -I``; ``extract`` refuses when they
disagree with its own pb_type count.

``check`` checks the JSON's schema, and with ``--arch`` also that the arch's sha256 and content
(the models, ``lut_size`` and ``ram_addr_width``, counted from the XML) still match.

``check-lib`` reads the ``cell``/``in``/``out``/``param``/``end`` statements of a ``.o3lib``
(the others are skipped) and requires, for each ``hard`` cell: a model of that name; the same
port names and directions; ``clock`` flags equal to the model's ``is_clock``; every port's
default width at most the model's size. ``--models`` is the models JSON (default: the committed
k6_frac one), or an arch XML, which is then extracted with ``--vtr-root``.

``--vtr-root`` defaults to ``$VTR_ROOT``, else ``~/odin3-ws/external/vtr-verilog-to-routing``.
Every message is ``vtr-models: <subcommand>: <cell or key>: <reason>`` on stderr (``extract``
writes its JSON to stdout without ``--out``), except extract's two VTR-disagreement lines,
which are ``vtr-models: lut_size: VTR says <a>, the pb_types give <b>`` (and the same for
``ram_addr_width``). Exit 0 clean, 1 a violation, 2 usage or I/O.
"""

from __future__ import annotations

import argparse
import ast
import hashlib
import json
import operator
import os
import re
import subprocess
import sys
import xml.etree.ElementTree as ET
from collections.abc import Callable, Sequence
from dataclasses import dataclass, field
from pathlib import Path

PROG = "vtr-models"
REPO = Path(__file__).resolve().parents[2]
SCHEMA = 1
DEFAULT_MODELS = REPO / "tests" / "vtr" / "arch" / "k6_frac_N10_frac_chain_mem32K_40nm.models.json"
DEFAULT_VTR_ROOT = Path.home() / "odin3-ws" / "external" / "vtr-verilog-to-routing"
VTR_PYTHON = Path(".venv") / "bin" / "python"
VTR_PYTHON_LIBS = Path("vtr_flow") / "scripts" / "python_libs"
RUN_TIMEOUT = 300  # seconds for VTR's Python and git; both take well under one
SUBCKT_PREFIX = ".subckt "
PB_PORT_TAGS = ("input", "output", "clock")
TOP_KEYS = ("arch", "arch_sha256", "lut_size", "models", "ram_addr_width", "schema",
            "vtr_commit")
MODEL_KEYS = ("clocks", "inputs", "outputs")
SHA256_RE = re.compile(r"[0-9a-f]{64}")
COMMIT_RE = re.compile(r"[0-9a-f]{40}")
PORT_FLAGS = ("signed", "clock")

# VTR's own readers (vtr.log_parse at 3c9a4d23). -I ignores PYTHONPATH, so the library path goes
# in through sys.path; no bytecode is written, since $VTR_ROOT is read only.
_VTR_SNIPPET = """\
import sys
sys.dont_write_bytecode = True
sys.path.insert(0, {libs!r})
from vtr.log_parse import determine_lut_size, determine_memory_addr_width
print(determine_lut_size(sys.argv[1]))
print(determine_memory_addr_width(sys.argv[1]))
"""


class Failure(Exception):
    """A message (after ``vtr-models: ``) that ends the run with ``code`` (1 or 2)."""

    def __init__(self, message: str, code: int) -> None:
        super().__init__(message)
        self.message = message
        self.code = code


# --- the arch XML --------------------------------------------------------------------------------


@dataclass
class ArchFacts:
    """What the arch XML says, counted by this tool (not VTR's readers)."""

    models: dict[str, dict[str, object]]
    lut_size: int
    ram_addr_width: int
    problems: list[str] = field(default_factory=list)


def _int_attr(elem: ET.Element, name: str, where: str) -> int:
    text = elem.get(name)
    try:
        return int(text if text is not None else "")
    except ValueError:
        raise Failure(f"{where}: '{name}' is not an integer", 1) from None


# Each model: its input ports, output ports and clock ports, in XML order.
Declared = dict[str, tuple[list[str], list[str], list[str]]]


def _declared_models(root: ET.Element) -> Declared:
    """Each <models>/<model>: its input ports, output ports and clock ports, in XML order."""
    declared: Declared = {}
    for model in root.findall("./models/model"):
        name = model.get("name", "")
        inputs = [p.get("name", "") for p in model.findall("./input_ports/port")]
        outputs = [p.get("name", "") for p in model.findall("./output_ports/port")]
        clocks = [p.get("name", "") for p in model.findall("./input_ports/port")
                  if p.get("is_clock") == "1"]
        declared[name] = (inputs, outputs, clocks)
    return declared


def _pin_count(pb: ET.Element, port: ET.Element, where: str) -> int:
    """A pb_type port's pins as VPR sizes the model from it. A ``class="memory"`` pb_type's
    ``port_class="data*"`` ports count 1: libarchfpga's ProcessMemoryClass moves the blif_model
    to a 1-bit ``memory_slice`` child and forces its data pins to one bit before
    SyncModelsPbTypes sizes the model (so single_port_ram's data/out are 1, not 64)."""
    pins = _int_attr(port, "num_pins", where)
    if pb.get("class") == "memory" and port.get("port_class", "").startswith("data"):
        return 1
    return pins


def _sync_problem(decl: tuple[list[str], list[str], list[str]], port: ET.Element) -> str | None:
    """SyncModelsPbTypes' refusals for one pb_type port: the direction, and for an input the
    is_clock flag (a <clock> is a clock), must equal the model port's. None when it agrees or
    the port is not on the model (reported by _model_entry)."""
    inputs, outputs, clocks = decl
    name = port.get("name", "")
    if name in inputs:
        if port.tag == "output":
            return "direction differs from the model"
        if (port.tag == "clock") != (name in clocks):
            return "is_clock differs from the model"
    elif name in outputs and port.tag != "output":
        return "direction differs from the model"
    return None


def _subckt_widths(root: ET.Element, declared: Declared,
                   problems: list[str]) -> dict[str, dict[str, int]]:
    """R11: per model, per port, the largest pin count (_pin_count) over its .subckt pb_types;
    each port's direction and is_clock are checked against the model (_sync_problem)."""
    widths: dict[str, dict[str, int]] = {}
    for pb in root.iter("pb_type"):
        blif = pb.get("blif_model", "")
        if not blif.startswith(SUBCKT_PREFIX):
            continue
        model = blif[len(SUBCKT_PREFIX):].strip()
        ports = widths.setdefault(model, {})
        for child in pb:
            if child.tag not in PB_PORT_TAGS:
                continue
            port = child.get("name", "")
            where = f"{model}: pb_type '{pb.get('name', '')}' port '{port}'"
            ports[port] = max(ports.get(port, 0), _pin_count(pb, child, where))
            problem = _sync_problem(declared[model], child) if model in declared else None
            if problem is not None:
                problems.append(f"{where}: {problem}")
    return widths


def _lut_and_ram(root: ET.Element) -> tuple[int, int]:
    """The largest .names input and the largest RAM address port, as vtr.log_parse counts."""
    lut = ram = 0
    for pb in root.iter("pb_type"):
        blif = pb.get("blif_model", "")
        if blif == ".names":
            port = pb.find("input")
            if port is not None:
                lut = max(lut, _int_attr(port, "num_pins", f"pb_type '{pb.get('name', '')}'"))
        elif "port_ram" in blif:
            for port in pb.findall("input"):
                if "addr" in port.get("name", ""):
                    where = f"pb_type '{pb.get('name', '')}'"
                    ram = max(ram, _int_attr(port, "num_pins", where))
    return lut, ram


def _model_entry(name: str, decl: tuple[list[str], list[str], list[str]],
                 widths: dict[str, int], problems: list[str]) -> dict[str, object]:
    inputs, outputs, clocks = decl
    entry: dict[str, dict[str, int]] = {"inputs": {}, "outputs": {}}
    for key, ports in (("inputs", inputs), ("outputs", outputs)):
        for port in ports:
            if port not in widths:
                problems.append(f"{name}: port '{port}' is on no pb_type")
                continue
            entry[key][port] = widths[port]
    for port in sorted(set(widths) - set(inputs) - set(outputs)):
        problems.append(f"{name}: pb_type port '{port}' is not on the model")
    return {"inputs": entry["inputs"], "outputs": entry["outputs"], "clocks": sorted(clocks)}


def arch_facts(arch: Path, command: str) -> ArchFacts:
    """The models (R11 widths), K and the RAM address width that the XML gives. Raises Failure
    (prefixed with ``command``): 2 when the XML cannot be read or parsed, 1 for a bad num_pins."""
    try:
        root = ET.parse(arch).getroot()
    except (OSError, ET.ParseError) as err:
        raise Failure(f"{command}: {arch}: cannot read: {err}", 2) from None
    try:
        return _facts(root)
    except Failure as err:
        raise Failure(f"{command}: models: {err.message}", err.code) from None


def _facts(root: ET.Element) -> ArchFacts:
    problems: list[str] = []
    declared = _declared_models(root)
    widths = _subckt_widths(root, declared, problems)
    models = {
        name: _model_entry(name, decl, widths.get(name, {}), problems)
        for name, decl in sorted(declared.items())
    }
    for name in sorted(set(widths) - set(models)):
        problems.append(f"{name}: a pb_type's .subckt model is not in <models>")
    lut, ram = _lut_and_ram(root)
    return ArchFacts(models, lut, ram, problems)


# --- extract -------------------------------------------------------------------------------------


def _clean_env() -> dict[str, str]:
    """The environment without GIT_* variables (a git hook's GIT_DIR must not leak in)."""
    return {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}


def _run(argv: list[str], what: str) -> str:
    try:
        proc = subprocess.run(argv, capture_output=True, text=True, timeout=RUN_TIMEOUT,
                              check=False, env=_clean_env())
    except (OSError, subprocess.TimeoutExpired) as err:
        raise Failure(f"extract: {what}: {err}", 2) from None
    if proc.returncode != 0:
        detail = proc.stderr.strip().splitlines()
        tail = f": {detail[-1]}" if detail else ""
        raise Failure(f"extract: {what}: exited {proc.returncode}{tail}", 2)
    return proc.stdout


def vtr_counts(arch: Path, vtr_root: Path) -> tuple[int, int]:
    """(lut_size, ram_addr_width) from VTR's vtr.log_parse, run by VTR's own Python."""
    snippet = _VTR_SNIPPET.format(libs=str(vtr_root / VTR_PYTHON_LIBS))
    out = _run([str(vtr_root / VTR_PYTHON), "-I", "-c", snippet, str(arch)], "VTR's Python")
    words = out.split()
    if len(words) != 2 or not all(w.isdigit() for w in words):
        raise Failure(f"extract: VTR's Python: expected two integers, got {out.strip()!r}", 2)
    return int(words[0]), int(words[1])


def vtr_commit(vtr_root: Path) -> str:
    commit = _run(["git", "-C", str(vtr_root), "rev-parse", "HEAD"], "vtr_commit").strip()
    if COMMIT_RE.fullmatch(commit) is None:
        raise Failure(f"extract: vtr_commit: not a commit '{commit}'", 2)
    return commit


def extract(arch: Path, vtr_root: Path) -> dict[str, object]:
    """The §15.2 models JSON of ``arch``. Raises Failure: 1 when the arch is inconsistent
    (a port on no pb_type, VTR's counts against ours), 2 for I/O and tool failures."""
    try:
        data = arch.read_bytes()
    except OSError as err:
        raise Failure(f"extract: {arch}: cannot read: {err.strerror or err}", 2) from None
    facts = arch_facts(arch, "extract")
    if facts.problems:
        raise Failure(f"extract: models: {facts.problems[0]}", 1)
    lut, ram = vtr_counts(arch, vtr_root)
    if lut != facts.lut_size:
        raise Failure(f"lut_size: VTR says {lut}, the pb_types give {facts.lut_size}", 1)
    if ram != facts.ram_addr_width:
        raise Failure(f"ram_addr_width: VTR says {ram}, the pb_types give "
                      f"{facts.ram_addr_width}", 1)
    return {
        "schema": SCHEMA,
        "arch": arch.name,
        "arch_sha256": hashlib.sha256(data).hexdigest(),
        "vtr_commit": vtr_commit(vtr_root),
        "lut_size": lut,
        "ram_addr_width": ram,
        "models": facts.models,
    }


def dumps(obj: object) -> str:
    """The committed JSON form: one-space indent, sorted keys, a final newline."""
    return json.dumps(obj, indent=1, sort_keys=True) + "\n"


# --- check ---------------------------------------------------------------------------------------


def _is_count(value: object, least: int) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and value >= least


def _check_ports(name: str, key: str, value: object) -> list[str]:
    if not isinstance(value, dict):
        return [f"models: {name}: {key}: not an object"]
    return [f"models: {name}: {key}: port '{port}' needs a positive width"
            for port, width in value.items() if not _is_count(width, 1)]


def _check_model(name: str, value: object) -> list[str]:
    if not isinstance(value, dict):
        return [f"models: {name}: not an object"]
    found = [f"models: {name}: {key}: missing" for key in MODEL_KEYS if key not in value]
    found += [f"models: {name}: {key}: unknown key" for key in sorted(value)
              if key not in MODEL_KEYS]
    for key in ("inputs", "outputs"):
        if key in value:
            found += _check_ports(name, key, value[key])
    inputs, outputs, clocks = value.get("inputs"), value.get("outputs"), value.get("clocks")
    if isinstance(inputs, dict) and isinstance(outputs, dict):
        found += [f"models: {name}: port '{port}' is both an input and an output"
                  for port in sorted(set(inputs) & set(outputs))]
    if "clocks" in value and (not isinstance(clocks, list)
                              or not all(isinstance(c, str) for c in clocks)):
        found.append(f"models: {name}: clocks: not a list of names")
    elif isinstance(clocks, list) and isinstance(inputs, dict):
        found += [f"models: {name}: clocks: '{c}' is not an input" for c in clocks
                  if c not in inputs]
    return found


def check_schema(data: object) -> list[str]:
    """The schema violations of a parsed models JSON, as ``<key>: <reason>``."""
    if not isinstance(data, dict):
        return ["schema: not a JSON object"]
    found = [f"{key}: missing" for key in TOP_KEYS if key not in data]
    found += [f"{key}: unknown key" for key in sorted(data) if key not in TOP_KEYS]
    tests: dict[str, tuple[Callable[[object], bool], str]] = {
        "schema": (lambda v: v == SCHEMA and not isinstance(v, bool), f"not {SCHEMA}"),
        "arch": (lambda v: isinstance(v, str) and v != "", "not a file name"),
        "arch_sha256": (lambda v: isinstance(v, str) and SHA256_RE.fullmatch(v) is not None,
                        "not a sha256 in lower-case hex"),
        "vtr_commit": (lambda v: isinstance(v, str) and COMMIT_RE.fullmatch(v) is not None,
                       "not a 40-digit commit"),
        "lut_size": (lambda v: _is_count(v, 1), "not a positive integer"),
        "ram_addr_width": (lambda v: _is_count(v, 0), "not a non-negative integer"),
        "models": (lambda v: isinstance(v, dict) and len(v) > 0, "not a non-empty object"),
    }
    for key, (ok, reason) in tests.items():
        if key in data and not ok(data[key]):
            found.append(f"{key}: {reason}")
    models = data.get("models")
    if isinstance(models, dict):
        for name, value in sorted(models.items()):
            found += _check_model(name, value)
    return found


def _check_content(data: dict[str, object], arch: Path) -> list[str]:
    """Where a schema-clean JSON no longer matches ``arch``."""
    found: list[str] = []
    if data["arch"] != arch.name:
        found.append(f"arch: '{data['arch']}' is not '{arch.name}'")
    try:
        sha = hashlib.sha256(arch.read_bytes()).hexdigest()
    except OSError as err:
        raise Failure(f"check: {arch}: cannot read: {err.strerror or err}", 2) from None
    if data["arch_sha256"] != sha:
        found.append(f"arch_sha256: differs from {arch.name} ({sha})")
    facts = arch_facts(arch, "check")
    found += [f"models: {problem}" for problem in facts.problems]
    for key, value in (("lut_size", facts.lut_size), ("ram_addr_width", facts.ram_addr_width)):
        if data[key] != value:
            found.append(f"{key}: {data[key]}, but the arch gives {value}")
    models = data["models"]
    assert isinstance(models, dict)
    for name in sorted(set(models) | set(facts.models)):
        if name not in facts.models:
            found.append(f"models: {name}: not in the arch")
        elif name not in models:
            found.append(f"models: {name}: missing (the arch has it)")
        else:
            found += [f"models: {name}: {key}: differs from the arch" for key in MODEL_KEYS
                      if models[name].get(key) != facts.models[name][key]]
    return found


def check(json_path: Path, arch: Path | None) -> list[str]:
    """The violations of a models JSON: its schema, and with ``arch`` also the sha256 and the
    content. Each is ``<key>: <reason>``. Raises Failure(2) when the file cannot be read."""
    try:
        data = json.loads(json_path.read_text(encoding="utf-8"))
    except OSError as err:
        raise Failure(f"check: {json_path}: cannot read: {err.strerror or err}", 2) from None
    except ValueError as err:
        raise Failure(f"check: {json_path}: not JSON: {err}", 2) from None
    found = check_schema(data)
    if not found and arch is not None:
        found = _check_content(data, arch)
    return found


# --- check-lib -----------------------------------------------------------------------------------

_BINOPS: dict[type[ast.operator], Callable[[int, int], int]] = {
    ast.Add: operator.add, ast.Sub: operator.sub, ast.Mult: operator.mul,
    ast.FloorDiv: operator.floordiv, ast.Div: operator.floordiv, ast.Mod: operator.mod,
    ast.Pow: operator.pow, ast.LShift: operator.lshift, ast.RShift: operator.rshift,
    ast.BitAnd: operator.and_, ast.BitOr: operator.or_, ast.BitXor: operator.xor,
}
_UNOPS: dict[type[ast.unaryop], Callable[[int], int]] = {
    ast.USub: operator.neg, ast.UAdd: operator.pos, ast.Invert: operator.invert,
}


def eval_width(expr: str, params: dict[str, int]) -> int | None:
    """An integer width expression over the parameters (`A_WIDTH + B_WIDTH`, `2 ** N`), or None
    when it is not one this checker evaluates."""
    try:
        tree = ast.parse(expr.strip(), mode="eval").body
    except SyntaxError:
        return None
    stack: list[tuple[ast.AST, bool]] = [(tree, False)]
    values: list[int] = []
    while stack:  # post-order, without recursion
        node, done = stack.pop()
        if isinstance(node, ast.Constant) and isinstance(node.value, int) \
                and not isinstance(node.value, bool):
            values.append(node.value)
        elif isinstance(node, ast.Name) and node.id in params:
            values.append(params[node.id])
        elif isinstance(node, ast.BinOp) and type(node.op) in _BINOPS:
            if done:
                right, left = values.pop(), values.pop()
                try:
                    values.append(_BINOPS[type(node.op)](left, right))
                except (ArithmeticError, ValueError):
                    return None
            else:
                stack += [(node, True), (node.right, False), (node.left, False)]
        elif isinstance(node, ast.UnaryOp) and type(node.op) in _UNOPS:
            if done:
                values.append(_UNOPS[type(node.op)](values.pop()))
            else:
                stack += [(node, True), (node.operand, False)]
        else:
            return None
    return values[0] if len(values) == 1 else None


@dataclass
class LibPort:
    name: str
    direction: str  # "in" or "out"
    width: str  # the expression as written
    clock: bool


@dataclass
class LibCell:
    name: str
    kind: str
    line: int
    params: dict[str, int] = field(default_factory=dict)
    ports: list[LibPort] = field(default_factory=list)
    bad_params: list[str] = field(default_factory=list)


def _statements(text: str) -> list[tuple[int, list[str]]]:
    """(line, words) of every `;`-separated statement, comments dropped."""
    found = []
    for number, line in enumerate(text.splitlines(), start=1):
        for stmt in line.split("#", 1)[0].split(";"):
            words = stmt.split()
            if words:
                found.append((number, words))
    return found


def _port(words: list[str]) -> LibPort:
    rest = words[2:]
    flags: set[str] = set()
    while rest and rest[-1] in PORT_FLAGS:
        flags.add(rest.pop())
    return LibPort(words[1], words[0], " ".join(rest), "clock" in flags)


def _cell_statement(cell: LibCell, words: list[str]) -> None:
    if words[0] in ("in", "out") and len(words) >= 3:
        cell.ports.append(_port(words))
    elif words[0] == "param" and len(words) >= 4:
        value = eval_width(" ".join(words[3:]), cell.params)
        if value is None:
            cell.bad_params.append(words[1])
        else:
            cell.params[words[1]] = value


def parse_lib(text: str, path: str) -> list[LibCell]:
    """The cells of a .o3lib: only cell/in/out/param/end are read. Raises Failure(2) on a
    statement out of place."""
    cells: list[LibCell] = []
    cell: LibCell | None = None
    for line, words in _statements(text):
        head = words[0]
        if head == "cell":
            if cell is not None:
                raise Failure(f"check-lib: {path}:{line}: 'cell' inside cell '{cell.name}'", 2)
            if len(words) < 3:
                raise Failure(f"check-lib: {path}:{line}: 'cell' takes a name and a kind", 2)
            cell = LibCell(words[1], words[2], line)
        elif head == "end":
            if cell is None:
                raise Failure(f"check-lib: {path}:{line}: 'end' outside a cell", 2)
            cells.append(cell)
            cell = None
        elif head in ("in", "out", "param"):
            if cell is None:
                raise Failure(f"check-lib: {path}:{line}: '{head}' outside a cell", 2)
            _cell_statement(cell, words)
    if cell is not None:
        raise Failure(f"check-lib: {path}:{cell.line}: cell '{cell.name}' has no 'end'", 2)
    return cells


def _port_problem(port: LibPort, cell: LibCell, model: dict[str, object]) -> str | None:
    inputs, outputs, clocks = model.get("inputs"), model.get("outputs"), model.get("clocks")
    assert isinstance(inputs, dict) and isinstance(outputs, dict) and isinstance(clocks, list)
    own, other = (inputs, outputs) if port.direction == "in" else (outputs, inputs)
    if port.name not in own:
        if port.name in other:
            return f"port '{port.name}' direction differs from the model"
        return f"port '{port.name}' is not on the model"
    if port.clock != (port.name in clocks):
        return f"port '{port.name}' clock flag differs from the model"
    width = eval_width(port.width, cell.params)
    if width is None:
        return f"port '{port.name}' default width '{port.width}' cannot be evaluated"
    if width > own[port.name]:
        return f"port '{port.name}' default width {width} exceeds the model's {own[port.name]}"
    return None


def _check_cell(cell: LibCell, models: dict[str, object]) -> list[str]:
    model = models.get(cell.name)
    if not isinstance(model, dict):
        return [f"{cell.name}: no model of this name"]
    found = [f"{cell.name}: param '{p}' default cannot be evaluated" for p in cell.bad_params]
    for port in cell.ports:
        problem = _port_problem(port, cell, model)
        if problem is not None:
            found.append(f"{cell.name}: {problem}")
    declared = {port.name for port in cell.ports}
    for key in ("inputs", "outputs"):
        ports = model.get(key)
        assert isinstance(ports, dict)
        found += [f"{cell.name}: model port '{p}' is not on the cell" for p in ports
                  if p not in declared]
    return found


def check_lib(lib: Path, models: dict[str, object]) -> list[str]:
    """Every hard cell of ``lib`` against ``models`` (the JSON's ``models`` object, schema-clean),
    as ``<cell>: <reason>``. Raises Failure(2) when the library cannot be read."""
    try:
        text = lib.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as err:
        raise Failure(f"check-lib: {lib}: cannot read: {err}", 2) from None
    found: list[str] = []
    for cell in parse_lib(text, str(lib)):
        if cell.kind == "hard":
            found += _check_cell(cell, models)
    return found


def load_models(path: Path, vtr_root: Path) -> dict[str, object]:
    """The ``models`` object of a models JSON (schema-checked), or of an arch XML (extracted)."""
    if path.suffix == ".xml":
        data: object = extract(path, vtr_root)
    else:
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as err:
            raise Failure(f"check-lib: {path}: cannot read models: {err}", 2) from None
        problems = check_schema(data)
        if problems:
            raise Failure(f"check-lib: {path}: not a models file: {problems[0]}", 2)
    assert isinstance(data, dict) and isinstance(data["models"], dict)
    return data["models"]


# --- CLI -----------------------------------------------------------------------------------------


def default_vtr_root() -> Path:
    env = os.environ.get("VTR_ROOT", "")
    return Path(env) if env else DEFAULT_VTR_ROOT


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog=PROG, description=__doc__.split("\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    ext = sub.add_parser("extract", help="write the arch's models JSON")
    ext.add_argument("arch", type=Path, metavar="ARCH.xml")
    ext.add_argument("--out", type=Path, help="the JSON file (default: stdout)")
    ext.add_argument("--vtr-root", type=Path, help="VTR checkout (default: $VTR_ROOT)")
    chk = sub.add_parser("check", help="check a models JSON (and its freshness with --arch)")
    chk.add_argument("file", type=Path, metavar="FILE")
    chk.add_argument("--arch", type=Path, metavar="ARCH.xml")
    lib = sub.add_parser("check-lib", help="check a .o3lib's hard cells against the models")
    lib.add_argument("lib", type=Path, metavar="LIB.o3lib")
    lib.add_argument("--models", type=Path, default=DEFAULT_MODELS,
                     help="models JSON, or an arch XML to extract (default: the k6_frac JSON)")
    lib.add_argument("--vtr-root", type=Path, help="VTR checkout (default: $VTR_ROOT)")
    return parser


def _report(command: str, found: list[str]) -> int:
    for message in found:
        print(f"{PROG}: {command}: {message}", file=sys.stderr)
    return 1 if found else 0


def _run_command(args: argparse.Namespace) -> int:
    vtr_root = getattr(args, "vtr_root", None) or default_vtr_root()
    if args.command == "extract":
        text = dumps(extract(args.arch, vtr_root))
        if args.out is None:
            sys.stdout.write(text)
            return 0
        try:
            args.out.write_text(text, encoding="utf-8")
        except OSError as err:
            raise Failure(f"extract: {args.out}: cannot write: {err.strerror or err}", 2) from None
        return 0
    if args.command == "check":
        return _report("check", check(args.file, args.arch))
    return _report("check-lib", check_lib(args.lib, load_models(args.models, vtr_root)))


def main(argv: Sequence[str] | None = None) -> int:
    try:
        args = _parser().parse_args(argv)
    except SystemExit as exc:
        return exc.code if isinstance(exc.code, int) else 2
    try:
        return _run_command(args)
    except Failure as err:
        print(f"{PROG}: {err.message}", file=sys.stderr)
        return err.code


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
