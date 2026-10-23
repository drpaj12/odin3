"""CFG similarity (Gate-Breaker Algorithm 3) over the Odin III AST, in Python.

Probe for the queued Gate-Breaker experiment: build a statement-level control-flow graph per
module from the AST the odin3 binding walks (plugins/python/odin3.py: Design.ast, Ast.roots,
AstNode.kind/sub/name/loc/children) and compare two designs with the paper's metric.

Node set (one CFG per module; expression trees included, as the earlier pyverilog run in
work/gatebreaker-exp/scripts/metrics.py did):

  Entry, Exit                              one each
  Block                                    one per always/initial, an edge from Entry
  Assign                                   one per continuous assign, an edge from Entry
  If, Then, Else, Join                     per if statement (Join merges the branches)
  Case, CaseWhen, CaseDefault, CaseJoin    per case statement and per case item
  Loop, LoopBody, LoopExit                 per for/while/repeat/forever; the exit edge leaves the
                                           body's end (no edge from the loop head to the exit)
  the kind name (BINARY, IDENT, ...)       one per expression-tree node under an assign rhs, a
                                           condition, a case selector or a case item expression

Edges are the statement-level flow; every node with no outgoing edge (except Entry) gets one to
Exit. Labels do not enter the metric: a node's feature vector is the histogram of its
shortest-path distances to all CFG nodes (raw counts), and two CFGs compare as

  SimCFG = KSP(G, R) / sqrt(KSP(G, G) * KSP(R, R))
  KSP(A, B) = sum over node pairs of exp(-||phi(a) - phi(b)||^2 / (2 sigma^2)), sigma = 0.5,

so SimCFG is in [0, 1]: 1.0 for the same control structure whatever the identifier names, lower
when the statement/expression structure differs.

Representation: the elaborated AST store (read_verilog --keep-ast for Verilog; the slang adapter's
store for SystemVerilog), so both languages are read post-parameter and post-generate. A parsed
Verilog store (--parse-only) works too; a generate block is then flattened to its items.

Usage:
    python3 tools/cfg-probe/odin3_cfg.py REF GEN [--top NAME] [--sigma S] [--edges] [--json]

The binding needs libodin3: $ODIN3_LIB, else build/debug or build/release under the repository; a
debug build needs its sanitizer runtime preloaded, so this script re-execs itself once with
LD_PRELOAD set ($ODIN3_SLANG_LIB names the SV adapter when it is not beside libodin3).
"""

from __future__ import annotations

import argparse
import ctypes
import importlib
import json
import math
import os
import subprocess
import sys
from collections import deque
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path
from types import ModuleType
from typing import Any, Protocol

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[1]
BINDING_DIR = REPO_ROOT / "plugins" / "python"

SIGMA = 0.5
SV_SUFFIXES = (".sv", ".svh")

EXPRESSION_KINDS = frozenset({
    "NUMBER", "REAL", "STRING", "IDENT", "HIER_NAME", "SELECT", "CONCAT", "REPLICATE",
    "UNARY", "BINARY", "TERNARY", "CALL", "MINTYPMAX",
})
BLOCK_KINDS = frozenset({"SEQ_BLOCK", "PAR_BLOCK"})
ASSIGN_KINDS = frozenset({"BLOCKING_ASSIGN", "NONBLOCKING_ASSIGN", "PROC_CONT_ASSIGN"})
LOOP_KINDS = frozenset({"FOR", "WHILE", "REPEAT", "FOREVER"})
STATEMENT_KINDS = BLOCK_KINDS | ASSIGN_KINDS | LOOP_KINDS | frozenset({
    "IF", "CASE", "TASK_CALL", "TIMING_STMT", "WAIT", "DISABLE", "EVENT_TRIGGER", "NULL_STMT",
})
ITEM_KINDS = frozenset({"ALWAYS", "INITIAL", "CONT_ASSIGN"})
GENERATE_KINDS = frozenset({"GENERATE", "GEN_BLOCK"})

_SLANG: Any = None  # the loaded libodin3_slang.so, kept alive after registration


class AstNode(Protocol):
    """What build_cfg needs of an AST node (odin3.AstNode satisfies it)."""

    @property
    def kind(self) -> str: ...

    @property
    def sub(self) -> str: ...

    @property
    def name(self) -> str: ...

    def children(self) -> list[AstNode]: ...


class AstTree(Protocol):
    """What build_cfg needs of an AST store (odin3.Ast satisfies it)."""

    def roots(self) -> list[AstNode]: ...


@dataclass(frozen=True)
class Cfg:
    """One module's control-flow graph: a node label per node and the directed edges."""

    labels: tuple[str, ...]
    edges: tuple[tuple[int, int], ...]

    def __len__(self) -> int:
        return len(self.labels)


class _Builder:
    """A CFG under construction: node 0 is Entry, added nodes follow in creation order."""

    def __init__(self) -> None:
        self.labels: list[str] = ["Entry"]
        self.edges: list[tuple[int, int]] = []

    def node(self, label: str) -> int:
        self.labels.append(label)
        return len(self.labels) - 1

    def edge(self, tail: int, head: int) -> None:
        self.edges.append((tail, head))

    def close(self) -> Cfg:
        """Add Exit, then an edge to it from every node but Entry that has no outgoing edge."""
        exit_node = self.node("Exit")
        has_out = {tail for tail, _ in self.edges}
        for ident in range(exit_node):
            if ident != 0 and ident not in has_out:
                self.edge(ident, exit_node)
        return Cfg(tuple(self.labels), tuple(self.edges))


def _statements(node: AstNode) -> list[AstNode]:
    """The statements of a block or of a single statement, block nesting flattened."""
    if node.kind not in BLOCK_KINDS:
        return [node] if node.kind in STATEMENT_KINDS else []
    out: list[AstNode] = []
    for kid in node.children():
        out.extend(_statements(kid))
    return out


def _body(node: AstNode) -> AstNode | None:
    """The statement of an always/initial: through a timing control when there is one."""
    kids = node.children()
    if not kids:
        return None
    kid = kids[0]
    if kid.kind == "TIMING_STMT":
        inner = [sub for sub in kid.children() if sub.kind in STATEMENT_KINDS]
        return inner[0] if inner else None
    return kid


def _expr_nodes(node: AstNode, parent: int, builder: _Builder) -> None:
    """Add the expression tree under node as CFG nodes hanging below parent (preorder)."""
    if node.kind not in EXPRESSION_KINDS:
        return
    ident = builder.node(node.kind)
    builder.edge(parent, ident)
    for kid in node.children():
        _expr_nodes(kid, ident, builder)


def _statement_cfg(stmt: AstNode, entry: int, builder: _Builder) -> int:
    """Append the CFG of one statement starting at node entry; return the statement's exit."""
    kind = stmt.kind
    if kind in ASSIGN_KINDS:
        return _assign(stmt, entry, builder)
    if kind == "IF":
        return _if(stmt, entry, builder)
    if kind == "CASE":
        return _case(stmt, entry, builder)
    if kind in LOOP_KINDS:
        return _loop(stmt, entry, builder)
    builder.labels[entry] = kind
    for kid in stmt.children():
        _expr_nodes(kid, entry, builder)
    return entry


def _assign(stmt: AstNode, entry: int, builder: _Builder) -> int:
    """One assignment statement: its node, then the expression tree of the right-hand side."""
    builder.labels[entry] = "Assign"
    kids = stmt.children()
    if kids:
        _expr_nodes(kids[-1], entry, builder)
    return entry


def _if(stmt: AstNode, entry: int, builder: _Builder) -> int:
    """If: the condition, a Then and an Else branch, and a Join that merges them."""
    builder.labels[entry] = "If"
    kids = stmt.children()
    if kids:
        _expr_nodes(kids[0], entry, builder)
    then_entry = builder.node("Then")
    builder.edge(entry, then_entry)
    then_exit = then_entry
    for inner in (_statements(kids[1]) if len(kids) > 1 else []):
        then_exit = _statement_cfg(inner, then_exit, builder)
    else_entry = builder.node("Else")
    builder.edge(entry, else_entry)
    else_exit = else_entry
    for inner in (_statements(kids[2]) if len(kids) > 2 else []):
        else_exit = _statement_cfg(inner, else_exit, builder)
    join = builder.node("Join")
    builder.edge(then_exit, join)
    builder.edge(else_exit, join)
    return join


def _case(stmt: AstNode, entry: int, builder: _Builder) -> int:
    """Case: the selector, one branch per item, and a CaseJoin all branches end in."""
    builder.labels[entry] = "Case"
    kids = stmt.children()
    if kids:
        _expr_nodes(kids[0], entry, builder)
    join = builder.node("CaseJoin")
    for item in kids[1:]:
        if item.kind != "CASE_ITEM":
            continue
        _case_item(item, entry, join, builder)
    return join


def _case_item(item: AstNode, entry: int, join: int, builder: _Builder) -> None:
    """One case item: its branch node with the item expressions, body, then the edge to join."""
    kids = item.children()
    exprs = [kid for kid in kids if kid.kind in EXPRESSION_KINDS]
    branch = builder.node("CaseWhen" if exprs else "CaseDefault")
    builder.edge(entry, branch)
    for expr in exprs:
        _expr_nodes(expr, branch, builder)
    body = [kid for kid in kids if kid.kind in STATEMENT_KINDS]
    cur = branch
    for inner in (body[0:1] and _statements(body[0]) or []):
        cur = _statement_cfg(inner, cur, builder)
    builder.edge(cur, join)


def _loop(stmt: AstNode, entry: int, builder: _Builder) -> int:
    """Loop: body node (condition expressions hang below it), back edge, then the exit."""
    builder.labels[entry] = "Loop"
    kids = stmt.children()
    cond: AstNode | None = None
    if stmt.kind == "FOR" and len(kids) >= 4:
        cond, body = kids[1], kids[3]
    elif len(kids) >= 2:
        cond, body = kids[0], kids[1]
    elif kids:
        body = kids[0]
    else:
        body = None
    loop_body = builder.node("LoopBody")
    builder.edge(entry, loop_body)
    if cond is not None:
        _expr_nodes(cond, loop_body, builder)
    cur = loop_body
    for inner in (_statements(body) if body is not None else []):
        cur = _statement_cfg(inner, cur, builder)
    exit_node = builder.node("LoopExit")
    builder.edge(cur, loop_body)
    builder.edge(cur, exit_node)
    return exit_node


def _items(node: AstNode) -> list[AstNode]:
    """The module-level items that make statements: blocks, assigns; generate blocks flattened."""
    out: list[AstNode] = []
    for kid in node.children():
        if kid.kind in ITEM_KINDS:
            out.append(kid)
        elif kid.kind in GENERATE_KINDS:
            out.extend(_items(kid))
    return out


def build_cfg(module: AstNode) -> Cfg:
    """The statement-level CFG of one AST module node (see the node set in the docstring)."""
    builder = _Builder()
    for item in _items(module):
        if item.kind == "CONT_ASSIGN":
            for net in item.children():
                if net.kind == "NET_ASSIGN":
                    assign = builder.node("Assign")
                    builder.edge(0, assign)
                    kids = net.children()
                    if kids:
                        _expr_nodes(kids[-1], assign, builder)
            continue
        block = builder.node("Block")
        builder.edge(0, block)
        body = _body(item)
        cur = block
        for stmt in (_statements(body) if body is not None else []):
            cur = _statement_cfg(stmt, cur, builder)
    return builder.close()


def modules(tree: AstTree) -> list[AstNode]:
    """Every MODULE node of an AST store, in creation order."""
    out: list[AstNode] = []
    stack = list(reversed(tree.roots()))
    while stack:
        node = stack.pop()
        if node.kind == "MODULE":
            out.append(node)
        else:
            stack.extend(reversed(node.children()))
    return out


def top_module(tree: AstTree, name: str | None = None) -> AstNode:
    """The module CFG to compare: the named one, else the only one no module instantiates."""
    found = modules(tree)
    if not found:
        raise ValueError("the AST holds no module")
    if name:
        for module in found:
            if module.name == name or module.name.split(".")[-1] == name:
                return module
        raise ValueError(f"no module named {name!r} (have {[m.name for m in found]})")
    instantiated: set[str] = set()
    for module in found:
        stack = list(module.children())
        while stack:
            node = stack.pop()
            if node.kind == "INSTANTIATION":
                instantiated.add(node.name)
            stack.extend(node.children())
    tops = [module for module in found if module.name not in instantiated]
    if len(tops) == 1:
        return tops[0]
    raise ValueError(
        f"{len(tops)} modules no module instantiates ({[m.name for m in tops]}); name one with "
        f"--top (have {[m.name for m in found]})")


def shortest_features(cfg: Cfg) -> list[list[float]]:
    """Each node's feature vector: the histogram of its shortest-path distances to all nodes."""
    count = len(cfg.labels)
    adj: list[list[int]] = [[] for _ in range(count)]
    for tail, head in cfg.edges:
        if 0 <= tail < count and 0 <= head < count:
            adj[tail].append(head)
    feats: list[list[float]] = []
    for start in range(count):
        dist = [-1] * count
        dist[start] = 0
        queue = deque([start])
        while queue:
            here = queue.popleft()
            for nxt in adj[here]:
                if dist[nxt] < 0:
                    dist[nxt] = dist[here] + 1
                    queue.append(nxt)
        reach = max(dist) if max(dist) > 0 else 1
        hist = [0] * (reach + 1)
        for distance in dist:
            if distance >= 0:
                hist[distance] += 1
        feats.append([float(cell) for cell in hist])
    return feats


def _pad(feats: list[list[float]], width: int) -> list[list[float]]:
    return [feat + [0.0] * (width - len(feat)) for feat in feats]


def cfg_kernel(ref: Cfg, gen: Cfg, sigma: float = SIGMA) -> float:
    """The normalized shortest-path kernel between two CFGs, in [0, 1].

    Both graphs' feature vectors are padded to one common width (the longest histogram of the
    two), so a pair always compares the same feature dimensions. (The earlier pyverilog code in
    work/gatebreaker-exp padded each graph to its own width and so silently truncated the longer
    vector in the kernel; numbers from this script and that one are not directly comparable.)
    """
    if len(ref.labels) < 2 or len(gen.labels) < 2:
        return 0.0
    fr = shortest_features(ref)
    fg = shortest_features(gen)
    width = max(len(feat) for feat in fr + fg)
    fr = _pad(fr, width)
    fg = _pad(fg, width)

    def kernel(a: list[float], b: list[float]) -> float:
        squared = sum((x - y) ** 2 for x, y in zip(a, b, strict=True))
        return math.exp(-squared / (2.0 * sigma * sigma))

    cross = sum(kernel(a, b) for a in fr for b in fg)
    self_ref = sum(kernel(a, b) for a in fr for b in fr)
    self_gen = sum(kernel(a, b) for a in fg for b in fg)
    if self_ref <= 0 or self_gen <= 0:
        return 0.0
    return max(0.0, min(1.0, cross / math.sqrt(self_ref * self_gen)))


def cfg_similarity(ref: Cfg, gen: Cfg, sigma: float = SIGMA) -> float:
    """Algorithm 3's SimCFG between two module CFGs."""
    return cfg_kernel(ref, gen, sigma)


# --- reading designs -------------------------------------------------------------------------

def load_binding() -> ModuleType:
    """The odin3 Python binding (plugins/python/odin3.py, plugins/python on sys.path)."""
    if str(BINDING_DIR) not in sys.path:
        sys.path.insert(0, str(BINDING_DIR))
    return importlib.import_module("odin3")


def default_library(binding: ModuleType) -> Path:
    """libodin3 as the binding finds it ($ODIN3_LIB, else build/debug, else build/release)."""
    return Path(str(binding.find_library()))


def ensure_sanitizer_preload(binding: ModuleType) -> None:
    """Re-exec once with LD_PRELOAD when the library is the ASan/UBSan debug build."""
    if os.environ.get("ODIN3_LIB") or "libasan" in os.environ.get("LD_PRELOAD", ""):
        return
    library = default_library(binding)
    if library.parent.name != "debug":
        return
    preload: list[str] = []
    for name in ("libasan.so", "libubsan.so"):
        found = subprocess.run(
            ["gcc", "-print-file-name=" + name], capture_output=True, text=True, check=False)
        path = found.stdout.strip()
        if found.returncode != 0 or not Path(path).is_file():
            return
        preload.append(path)
    env = dict(os.environ)
    env["LD_PRELOAD"] = ":".join(preload)
    env.setdefault("ASAN_OPTIONS", "detect_leaks=0")
    os.execve(sys.executable, [sys.executable, *sys.argv], env)


def register_sv_adapter(odin3: Any) -> None:
    """Load libodin3_slang.so and register odin3_read_slang (the binding does not, yet).

    The registration is process-wide and final (a second call is refused), so it runs once.
    """
    global _SLANG
    if _SLANG is not None:
        return
    path = Path(os.environ.get("ODIN3_SLANG_LIB", ""))
    if not path.is_file():
        path = Path(str(odin3._library)).resolve().parent / "libodin3_slang.so"
    if not path.is_file():
        raise RuntimeError(
            f"the SystemVerilog adapter is not built ({path}); set ODIN3_SLANG_LIB")
    slang = ctypes.CDLL(str(path))
    address = ctypes.cast(slang.odin3_read_slang, ctypes.c_void_p).value
    if address is None:
        raise RuntimeError(f"{path}: odin3_read_slang not found")
    function = odin3.ffi.cast("odin3_sv_read_fn", address)
    odin3.call("odin3_sv_reader_register", function)
    _SLANG = slang


def read_design(binding: ModuleType, path: Path) -> Any:
    """A design holding path's elaborated AST (SystemVerilog through the slang adapter)."""
    odin3 = binding.Odin3(default_library(binding))
    odin3.call("odin3_log_set_level", int(odin3.lib.ODIN3_LOG_ERROR))
    design = binding.Design(odin3)
    if path.suffix.lower() in SV_SUFFIXES:
        register_sv_adapter(odin3)
    else:
        design.call("odin3_design_set_keep_ast", int(odin3.lib.ODIN3_KEEP_AST_ELABORATED))
    try:
        design.run_pass("read_verilog", f'"{path}"')
    except Exception:
        design.close()
        raise
    return design


def design_cfg(design: Any, top: str | None = None) -> Cfg:
    """The CFG of the design's top module (or the module named top)."""
    tree = design.ast()
    if tree is None:
        raise ValueError("the design holds no AST")
    return build_cfg(top_module(tree, top))


def similarity(binding: ModuleType, ref: Path, gen: Path, top: str | None = None,
               sigma: float = SIGMA) -> tuple[float, Cfg, Cfg]:
    """SimCFG between the top modules of two source files, with both CFGs."""
    designs: list[Any] = []
    try:
        ref_design = read_design(binding, ref)
        designs.append(ref_design)
        ref_cfg = design_cfg(ref_design, top)
        gen_design = read_design(binding, gen)
        designs.append(gen_design)
        gen_cfg = design_cfg(gen_design, top)
    finally:
        for design in designs:
            design.close()
    return cfg_similarity(ref_cfg, gen_cfg, sigma), ref_cfg, gen_cfg


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("ref", type=Path, help="the reference RTL file (.v or .sv)")
    parser.add_argument("gen", type=Path, help="the generated RTL file")
    parser.add_argument("--top", help="the module to compare (default: the only top module)")
    parser.add_argument("--sigma", type=float, default=SIGMA, help=f"RBF width (default {SIGMA})")
    parser.add_argument("--edges", action="store_true", help="print the reference CFG's edges")
    parser.add_argument("--json", action="store_true", help="print the result as JSON")
    args = parser.parse_args(argv)

    binding = load_binding()
    ensure_sanitizer_preload(binding)
    sim, ref_cfg, gen_cfg = similarity(binding, args.ref, args.gen, args.top, args.sigma)
    if args.json:
        print(json.dumps({
            "simcfg": sim,
            "ref": {"nodes": len(ref_cfg), "edges": len(ref_cfg.edges)},
            "gen": {"nodes": len(gen_cfg), "edges": len(gen_cfg.edges)},
        }, indent=1))
    else:
        print(f"ref {args.ref}: {len(ref_cfg)} nodes, {len(ref_cfg.edges)} edges")
        print(f"gen {args.gen}: {len(gen_cfg)} nodes, {len(gen_cfg.edges)} edges")
        print(f"SimCFG {sim:.6f}")
    if args.edges:
        for ident, label in enumerate(ref_cfg.labels):
            print(f"{ident} {label}")
        for tail, head in ref_cfg.edges:
            print(f"{tail} -> {head}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
