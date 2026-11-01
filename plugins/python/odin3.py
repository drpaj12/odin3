"""cffi binding to the Odin III C ABI (include/odin3/odin3.h), with a thin Pythonic layer.

The cdef is generated at import time from the block between ODIN3_CDEF_BEGIN and
ODIN3_CDEF_END in odin3.h, so the binding never drifts from the header.

The shared library is located (find_library) as the bundled libodin3.so beside this module (the
pip wheel), else $ODIN3_LIB, else build/{debug,release}/libodin3.so in the repository. The header
is the bundled odin3.h beside this module, else include/odin3/odin3.h in the repository.

The Pythonic layer (spec docs/specs/2026-10-09-1D-abi-design.md, "Python"):

    design = Design.read_blif("a.blif")
    for module in design.modules():
        for node in module.nodes():
            print(node.type_name, [pin.net() for pin in node.pins()], node.sources())

The AST a read kept (ABI v4; read-side helpers only):

    tree = design.ast()  # the latest read run's store, elaborated else parsed; None if none
    for unit in tree.roots():
        for child in unit.children():
            print(child.kind, child.name, child.loc)  # e.g. MODULE top f.v:1:1

A Simulation steps the top module one cycle at a time (ABI v5; run `proc; opt; lower` first):

    with design.simulate() as sim:
        sim["rst"] = 1
        sim.step()
        print(sim["light_ns"], sim.cycle)

Objects (Module, Node, Pin, Net, Wire; AstNode) are small value types naming an IR object (an
AST node) by ID through the design that owns it; they compare equal when they name the same
object. Every string is copied out of the library at once, so it outlives later mutations. IDs
stay valid until a `compact` pass renumbers a module (ABI conventions in odin3.h). A failing ABI
call raises Odin3Error carrying its odin3_status; the library has already logged why.
"""

from __future__ import annotations

import importlib
import logging
import os
import re
import sys
import threading
import weakref
from collections.abc import Iterator, Sequence
from contextlib import contextmanager
from dataclasses import dataclass, field
from pathlib import Path
from types import TracebackType
from typing import Any, ClassVar, NamedTuple, TypeVar, Union

import cffi

PACKAGE_DIR = Path(__file__).resolve().parent
REPO_ROOT = PACKAGE_DIR.parents[1]
_BUNDLED_HEADER = PACKAGE_DIR / "odin3.h"
HEADER = (
    _BUNDLED_HEADER if _BUNDLED_HEADER.is_file() else REPO_ROOT / "include" / "odin3" / "odin3.h"
)
LOG = logging.getLogger("odin3")
GRAPHLET_MAX_WORK = 20_000_000_000  # the CLI default bound of the graphlets pass (--max-work)


def cdef_from_header(header: Path = HEADER) -> str:
    """Return the cffi-parsable declarations from odin3.h, comments stripped."""
    text = header.read_text(encoding="utf-8")
    match = re.search(r"/\* ODIN3_CDEF_BEGIN \*/(.*)/\* ODIN3_CDEF_END \*/", text, re.DOTALL)
    if match is None:
        raise RuntimeError(f"{header}: ODIN3_CDEF markers not found")
    return re.sub(r"/\*.*?\*/", "", match.group(1), flags=re.DOTALL)


def find_library() -> Path:
    """Locate libodin3.so: the library bundled beside this module (the pip wheel) first, then
    $ODIN3_LIB, then the debug and release build trees. Logs (logger "odin3", INFO) which one it
    chose and returns its path."""
    env = os.environ.get("ODIN3_LIB")
    candidates = [("bundled", PACKAGE_DIR / "libodin3.so")]
    if env:
        candidates.append(("$ODIN3_LIB", Path(env)))
    candidates += [
        (f"build/{p}", REPO_ROOT / "build" / p / "libodin3.so") for p in ("debug", "release")
    ]
    for origin, path in candidates:
        if path.is_file():
            LOG.info("libodin3: using the %s library %s", origin, path)
            return path
    raise FileNotFoundError("libodin3.so not found; build first or set ODIN3_LIB")


_ABC_TRIED = False  # the abc adapter load was attempted in this process


class Odin3Error(RuntimeError):
    """An ABI call returned a failure status (status: the odin3_status value)."""

    def __init__(self, function: str, status: int, name: str) -> None:
        super().__init__(f"{function}: {name}")
        self.function = function
        self.status = status
        # set by Node.set_cover / set_param_int when the old node was replaced and a later step
        # failed: the twin node (otherwise None)
        self.twin: Any = None


class LogRecord(NamedTuple):
    """One message delivered by the library's log (level: an odin3_log_level value)."""

    level: int
    message: str


class Odin3:
    """A loaded libodin3 with typed wrappers for the ABI functions in odin3.h."""

    def __init__(self, library: Path | None = None) -> None:
        self.ffi = cffi.FFI()
        self.ffi.cdef(cdef_from_header())
        self.lib: Any = self.ffi.dlopen(str(library or find_library()))
        if self.abi_version() != int(self.lib.ODIN3_ABI_VERSION):
            raise RuntimeError("libodin3 ABI version does not match odin3.h")
        # One callback per visitor kind; each call passes the list to fill as a handle.
        self._source_cb = self.ffi.callback("odin3_source_visit", _on_source(self))
        self._object_cb = self.ffi.callback("odin3_prov_object_visit", _on_object(self))
        self._log_cb = self.ffi.callback("odin3_log_sink", _on_log(self))
        self._u32_cb = self.ffi.callback("odin3_u32_visit", _on_u32(self))
        self._construct_cb = self.ffi.callback("odin3_construct_visit", _on_construct(self))
        self._captures: list[Any] = []  # handles of the open capture_log blocks, innermost last
        self._library = Path(str(library or find_library()))
        self._register_abc(self._library)

    def _register_abc(self, library: Path) -> None:
        """Register the in-process ABC engine, as the CLI does at start-up: load the adapter
        libodin3_abc.so beside the library as a plugin (its odin3_plugin_init registers the
        engine). Once per process (the registry is process-wide); without the adapter library,
        or when an engine is already registered, the ``abc`` pass is simply unavailable."""
        global _ABC_TRIED
        adapter = library.resolve().parent / "libodin3_abc.so"
        if _ABC_TRIED or not adapter.is_file():
            return
        _ABC_TRIED = True
        try:
            self.load_plugin(adapter)
        except Odin3Error as exc:
            LOG.warning("odin3: the abc engine was not registered: %s", exc)

    def version(self) -> str:
        return str(self.ffi.string(self.lib.odin3_version_string()).decode())

    def commit(self) -> str:
        """The Git commit libodin3 was built from (40 hex digits or "unknown"), plus "-dirty" if
        the sources had uncommitted changes."""
        sha = str(self.ffi.string(self.lib.odin3_version_commit()).decode())
        return sha + ("-dirty" if self.lib.odin3_version_commit_dirty() else "")

    def abi_version(self) -> int:
        return int(self.lib.odin3_abi_version())

    def status_string(self, status: int) -> str:
        return str(self.ffi.string(self.lib.odin3_status_string(status)).decode())

    def graphlet_counts(
        self,
        n: int,
        edges: Sequence[tuple[int, int]],
        directed: bool = False,
        size: int = 5,
        max_work: int = GRAPHLET_MAX_WORK,
    ) -> list[list[int]]:
        """Graphlet orbit counts of any graph, no design needed (odin3_graphlets_count).

        The graph has nodes 0..n-1 and the directed edges (u, v). Undirected (default): the simple
        undirected graph, size 4 (15 orbits) or 5 (73 orbits, Przulj numbering). Directed: 2 to 4
        node directed graphlets, 129 orbits (size is ignored). Returns n rows of orbit counts.
        max_work bounds the connected node sets visited (default: the CLI bound, 2e10; 0: none).
        """
        if not hasattr(self.lib, "odin3_graphlets_count"):
            raise Odin3Error(
                "odin3_graphlets_count",
                -1,
                "libodin3 too old: needs odin3_graphlets_count, Odin III > v1.2.0",
            )
        n_orbits = 129 if directed else {4: 15, 5: 73}.get(size, 0)
        if n_orbits == 0:
            raise ValueError("size must be 4 or 5")
        src = self.ffi.new("uint32_t[]", [u for u, _ in edges] or [0])
        dst = self.ffi.new("uint32_t[]", [v for _, v in edges] or [0])
        out = self.ffi.new("int64_t[]", max(n * n_orbits, 1))
        graph = self.ffi.new(
            "odin3_graphlet_graph *", {"n": n, "n_edges": len(edges), "from": src, "to": dst}
        )
        opts = self.ffi.new(
            "odin3_graphlet_opts *", {"directed": int(directed), "size": size, "max_work": max_work}
        )
        status = self.lib.odin3_graphlets_count(graph, opts, out, n * n_orbits)
        self.check("odin3_graphlets_count", status)
        return [list(out[row * n_orbits : (row + 1) * n_orbits]) for row in range(n)]

    def check(self, function: str, status: int) -> None:
        """Raise Odin3Error unless status is ODIN3_OK."""
        if status != self.lib.ODIN3_OK:
            raise Odin3Error(function, int(status), self.status_string(status))

    def call(self, function: str, *args: Any) -> None:
        """Call ABI function `function` with args and check its status."""
        self.check(function, getattr(self.lib, function)(*args))

    def get(self, function: str, ctype: str, *args: Any) -> Any:
        """Call `function(*args, &out)` (out of C type ctype), check it, return out."""
        out = self.ffi.new(ctype + " *")
        self.call(function, *args, out)
        return out[0]

    def text(self, ptr: Any) -> str:
        """Copy a NUL-terminated C string (never NULL here) into a Python str."""
        return str(self.ffi.string(ptr).decode("utf-8", "surrogateescape"))

    def enum_name(self, ctype: str, value: int) -> str:
        """The lower-case suffix of an enum constant, e.g. ODIN3_DIR_INOUT -> "inout"."""
        name = str(self.ffi.typeof(ctype).elements[int(value)])
        return name.split("_", 2)[2].lower()

    def passes(self) -> list[str]:
        """Names of the registered passes, built-ins first."""
        return [
            self.text(self.get("odin3_pass_get_name", "const char *", i))
            for i in range(int(self.lib.odin3_pass_get_count()))
        ]

    def load_plugin(self, path: str | Path) -> None:
        """Load a shared-object plugin (odin3_plugin_load)."""
        self.call("odin3_plugin_load", os.fsencode(path))

    def load_abc(self, library: str | Path | None = None) -> None:
        """Register the linked ABC engine, which `abc` and `cone_check` need (the odin3 CLI does
        this at start-up; a plain library does not). library: libodin3_abc.so, default
        $ODIN3_ABC_LIB, else the file next to libodin3. Once per process: a second call does
        nothing. Odin3Error with the plugin's status when it cannot be loaded."""
        global _ABC_TRIED
        if _ABC_TRIED:
            return  # registered at construction (or by an earlier call)
        env = os.environ.get("ODIN3_ABC_LIB")
        path = library or env or self._library.with_name("libodin3_abc.so")
        self.load_plugin(path)
        _ABC_TRIED = True

    @contextmanager
    def capture_log(self) -> Iterator[list[LogRecord]]:
        """Collect the log's messages (INFO and above) while the block runs.

        Nestable: an inner capture takes the messages while it runs, then the enclosing one
        resumes. After the outermost block messages go to stderr again (the ABI has no sink
        getter, so a sink installed outside this binding is not restored). The level is raised
        to INFO for the block if it was lower, and restored after it.
        """
        records: list[LogRecord] = []
        handle = self.ffi.new_handle(records)
        old_level = self.lib.odin3_log_get_level()
        self.call("odin3_log_set_level", max(int(old_level), int(self.lib.ODIN3_LOG_INFO)))
        self._captures.append(handle)
        self.lib.odin3_log_set_sink(self._log_cb, handle)
        try:
            yield records
        finally:
            self._captures.remove(handle)
            outer = self._captures[-1] if self._captures else self.ffi.NULL
            self.lib.odin3_log_set_sink(self._log_cb if self._captures else self.ffi.NULL, outer)
            self.call("odin3_log_set_level", old_level)


def _on_source(odin3: Odin3) -> Any:
    def visit(src: Any, user: Any) -> None:
        odin3.ffi.from_handle(user).append(
            Source(odin3.text(src.file), src.line, src.col, src.end_line, src.end_col)
        )

    return visit


def _on_object(odin3: Odin3) -> Any:
    def visit(found: Any, user: Any) -> None:
        obj = found.obj
        odin3.ffi.from_handle(user).append(
            (int(obj.module), int(obj.kind), int(obj.id), bool(found.live))
        )

    return visit


def _on_u32(odin3: Odin3) -> Any:
    def visit(ident: int, user: Any) -> None:
        odin3.ffi.from_handle(user).append(int(ident))

    return visit


def _on_construct(odin3: Odin3) -> Any:
    def visit(construct: Any, leaf: int, user: Any) -> None:
        odin3.ffi.from_handle(user).append((_construct_of(odin3, construct), int(leaf)))

    return visit


def _construct_kind(odin3: Odin3, kind: int) -> str:
    """The AST kind name (BLOCKING_ASSIGN), or IF_THEN, IF_ELSE, CALL_SITE for a synthesized row."""
    lib = odin3.lib
    synthesized = {
        int(lib.ODIN3_CONSTRUCT_IF_THEN): "IF_THEN",
        int(lib.ODIN3_CONSTRUCT_IF_ELSE): "IF_ELSE",
        int(lib.ODIN3_CONSTRUCT_CALL_SITE): "CALL_SITE",
    }
    return synthesized.get(int(kind)) or odin3.enum_name("odin3_ast_kind", kind).upper()


def _construct_of(odin3: Odin3, construct: Any) -> Construct:
    return Construct(
        int(construct.index),
        int(construct.parent),
        _construct_kind(odin3, construct.kind),
        odin3.enum_name("odin3_level", construct.level),
        odin3.text(construct.file),
        int(construct.line),
        int(construct.col),
        int(construct.end_line),
        int(construct.end_col),
        odin3.text(construct.name),
    )


def _on_log(odin3: Odin3) -> Any:
    def sink(level: int, msg: Any, user: Any) -> None:
        odin3.ffi.from_handle(user).append(LogRecord(int(level), odin3.text(msg)))

    return sink


_DEFAULT: list[Odin3] = []


_PROV_AST_FLAG = " --prov-ast refs"
_READ_VERILOG_CMD = re.compile(r"(^|[;\n])([ \t]*)read_verilog(?=[ \t;\n]|$)")


def _with_prov_ast(args: str) -> str:
    """The arguments of a read_verilog pass with `--prov-ast refs` added, unless they name a mode
    or the user set ODIN3_PROV_AST (the environment is read, never written)."""
    if "--prov-ast" in args or "ODIN3_PROV_AST" in os.environ:
        return args
    return _PROV_AST_FLAG.strip() + " " + args


def _script_with_prov_ast(text: str) -> str:
    """The script with every read_verilog command given `--prov-ast refs` (same exceptions)."""
    if "--prov-ast" in text or "ODIN3_PROV_AST" in os.environ:
        return text
    return _READ_VERILOG_CMD.sub(
        lambda m: f"{m.group(1)}{m.group(2)}read_verilog{_PROV_AST_FLAG}", text
    )


def default() -> Odin3:
    """The process's shared Odin3 (loaded on first use)."""
    if not _DEFAULT:
        _DEFAULT.append(Odin3())
    return _DEFAULT[0]


class Source(NamedTuple):
    """A source location (odin3_source): file as the reader recorded it, 1-based line/col."""

    file: str
    line: int
    col: int
    end_line: int
    end_col: int


class Construct(NamedTuple):
    """One construct of the design's construct table (odin3_construct): where in the HDL an object
    comes from, at one granularity. kind is an AST kind name or IF_THEN, IF_ELSE, CALL_SITE; level
    is module, instance, block, branch, stmt or expr; parent is the row of the enclosing construct
    (0 for a module); row is its own row number (1-based). file and name are "" when unknown."""

    row: int
    parent: int
    kind: str
    level: str
    file: str
    line: int
    col: int
    end_line: int
    end_col: int
    name: str


class ProvRecord(NamedTuple):
    """One provenance record (odin3_prov_info) with its parents, construct and joined constructs.

    kind is source, imported or derived; dir is origin, refine, rewrite, coarsen or context, and
    from_level and to_level are the levels of a derived record's class (none otherwise); flags is a
    frozenset of join, join_instance, lossy_bulk, lossy_cap, span_only (V1.2.0 3.3); parents are
    record IDs in stored order, the last n_context of them context (instance) parents; construct is
    a SOURCE record's construct; joined holds the constructs of J (at most 16, n_joined is the
    total) of a coarsen record."""

    record: int
    kind: str
    run: int
    op: int
    dir: str
    from_level: str
    to_level: str
    flags: frozenset[str]
    construct: Construct | None
    parents: tuple[int, ...]
    n_context: int
    n_joined: int
    joined: tuple[Construct, ...]
    hier: str


_PROV_KINDS = ("source", "imported", "derived")
_PROV_FLAGS = (
    (1, "join"),
    (2, "join_instance"),
    (4, "lossy_bulk"),
    (8, "lossy_cap"),
    (16, "span_only"),
)


IRObject = Union["Module", "Node", "Net", "Wire"]


class Found(NamedTuple):
    """An object found from a source location; obj is None when compact has freed it."""

    module: int
    kind: str
    obj: IRObject | None
    live: bool


def _quote(path: str | Path) -> str:
    text = os.fspath(path)
    if '"' in text:
        raise ValueError(f"{text}: a pass argument cannot hold a double quote")
    return f'"{text}"'


def _design_destructor(lib: Any, open_sims: dict[int, Any]) -> Any:
    """The gc destructor of a design: close the simulations still open, then destroy the design.
    Closing here, not only in the simulations' own destructors, keeps the order right when the
    garbage collector finalises a design and its simulations in one cycle in any order (SP-4)."""

    def destroy(ptr: Any) -> None:
        for raw in list(open_sims.values()):
            lib.odin3_simulation_close(raw)
        open_sims.clear()
        lib.odin3_design_destroy(ptr)

    return destroy


class Design:
    """An odin3_design: create it empty, run passes on it, walk its modules."""

    def __init__(
        self, odin3: Odin3 | None = None, *, handle: Any = None, top: str | None = None
    ) -> None:
        """A new empty design; or, given handle (an odin3_design * that C code made with the same
        library, e.g. a host program or a test fixture), that design, which this object then owns
        and destroys. MemoryError when a new design cannot be created, ValueError for a NULL
        handle. top names the module that read_verilog, read_blif and hierarchy make the design's
        top, as the CLI's --top does (it wins over a file's own top); it is applied around every
        run_pass and run_script of this design."""
        self.odin3 = odin3 or default()
        self.top_name = top
        # The CLI keeps no construct table by default (V1.2.0 5.3); a Python design wants the chain
        # of Node.chain(), so run_pass and run_script give a read_verilog that names no mode
        # `--prov-ast refs`, per read; the process environment is never changed (a user's
        # ODIN3_PROV_AST wins, as does an explicit --prov-ast).
        if handle is None:
            ptr = self.odin3.lib.odin3_design_create()
            if ptr == self.odin3.ffi.NULL:
                raise MemoryError("odin3_design_create")
        elif handle == self.odin3.ffi.NULL:
            raise ValueError("Design: the adopted handle is NULL")
        else:
            ptr = handle
        # Raw handles of the open simulations, by address; shared with the destructors (below).
        self._open: dict[int, Any] = {}
        self.ptr: Any = self.odin3.ffi.gc(ptr, _design_destructor(self.odin3.lib, self._open))
        self._sims: weakref.WeakSet[Simulation] = weakref.WeakSet()

    @classmethod
    def read_blif(
        cls,
        path: str | Path,
        *,
        techlibs: Sequence[str | Path] = (),
        odin3: Odin3 | None = None,
        top: str | None = None,
    ) -> Design:
        """A new design holding the BLIF at path, after reading the given tech libraries."""
        design = cls(odin3, top=top)
        for lib in techlibs:
            design.run_pass("read_techlib", _quote(lib))
        design.run_pass("read_blif", _quote(path))
        return design

    def copy(self) -> Design:
        """A deep copy (odin3_design_copy): the same IDs, no memory shared with this design, so a
        pass on the copy leaves this design unchanged and either may be closed first. Held AST
        stores, the project and the architecture are not copied. Odin3Error with the status when
        a design-local cell type holds a width expression or tech-library data (INVALID_ARG) or on
        out of memory."""
        ptr = self.get("odin3_design_copy", "odin3_design *")
        return Design(self.odin3, handle=ptr)

    def close(self) -> None:
        """Destroy the design now (else when it is garbage-collected). Its open simulations are
        closed first."""
        for sim in list(self._sims):
            sim.close()
        if self.ptr is not None:
            self.odin3.ffi.release(self.ptr)
            self.ptr = None

    def __enter__(self) -> Design:
        return self

    def __exit__(
        self, kind: type[BaseException] | None, exc: BaseException | None, tb: TracebackType | None
    ) -> None:
        self.close()

    def call(self, function: str, *args: Any) -> None:
        self.odin3.call(function, self.ptr, *args)

    def get(self, function: str, ctype: str, *args: Any) -> Any:
        return self.odin3.get(function, ctype, self.ptr, *args)

    def get_text(self, function: str, *args: Any) -> str:
        return self.odin3.text(self.get(function, "const char *", *args))

    @contextmanager
    def _top_scope(self, top: str | None) -> Iterator[None]:
        """Set the requested top name (odin3_pass_set_top_name) for one run, then put back the
        top that was set before it.

        The C top name is process-wide, so the whole run holds a module-level lock: runs of this
        binding are serialised across threads (cffi releases the GIL during a pass) and never see
        each other's top. The previous value is the one this binding last set (the C API has no
        getter); a host that calls odin3_pass_set_top_name directly is not seen. A failure while
        restoring never hides the exception of the run."""
        name = top if top is not None else self.top_name
        with _TOP_LOCK:
            if name is None and _current_top is None:
                yield
                return
            previous = _current_top
            _set_top(self.odin3, name)
            try:
                yield
            finally:
                try:
                    _set_top(self.odin3, previous)
                except Odin3Error:
                    if sys.exc_info()[0] is None:
                        raise

    def run_pass(self, name: str, args: str = "", *, top: str | None = None) -> None:
        """Run one pass (odin3_design_run_pass); raises Odin3Error with its status.

        After a failed pass the design is valid but may no longer compute what its input did:
        discard it (close it) rather than keep using or writing it.
        """
        if name == "read_verilog":
            args = _with_prov_ast(args)
        with self._top_scope(top):
            self.call("odin3_design_run_pass", name.encode(), args.encode())

    def run_script(self, text: str, origin: str = "python", *, top: str | None = None) -> None:
        """Run a pass script, its errors located by command (odin3_design_run_script). top: the
        requested top module name for the whole script, set before its first command (the
        CLI's --top); default: the Design's top argument, else none.

        After a failed pass the design must be discarded, as for run_pass.
        """
        text = _script_with_prov_ast(text)
        keep = self.odin3.ffi.new("char[]", origin.encode())  # alive during the call
        src = (keep, self.odin3.lib.ODIN3_SCRIPT_BY_COMMAND)
        with self._top_scope(top):
            self.call("odin3_design_run_script", text.encode(), src)

    def graphlets(
        self,
        *,
        directed: bool = False,
        size: int = 5,
        exclude_global: bool = False,
        global_fanout: int = 16,
        per_node: bool = False,
        halo: int = 0,
        focus: Sequence[str] = (),
        cells: str | Path | None = None,
        partition: str | None = None,
        max_nodes: int | None = None,
        max_work: int | None = None,
        force: bool = False,
    ) -> dict[str, Any]:
        """Run the graphlets pass and return its JSON (schema odin3-graphlets, version 1).

        Groups (whole modules, foci, cell partitions) each have orbit_totals and gdd; with
        per_node=True also node_vectors keyed by the module-local node id and name. Whole-module
        summaries are also stored as module attributes odin3:glet_* (Module.attr). See
        docs/user/graphlets.md.
        """
        import json
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "graphlets.json"
            words = ["--out", _quote(out), "--size", str(size), "--halo", str(halo)]
            words += ["--directed"] * directed + ["--per-node"] * per_node + ["--force"] * force
            if exclude_global:
                words += ["--exclude-global", "--global-fanout", str(global_fanout)]
            for item in focus:
                words += ["--focus", _quote(item)]
            if cells is not None:
                words += ["--cells", _quote(cells)]
            if partition is not None:
                words += ["--partition", _quote(partition)]
            if max_nodes is not None:
                words += ["--max-nodes", str(max_nodes)]
            if max_work is not None:
                words += ["--max-work", str(max_work)]
            self.run_pass("graphlets", " ".join(words))
            result: dict[str, Any] = json.loads(out.read_text(encoding="utf-8"))
            return result

    def scoap(
        self,
        *,
        sequential: bool = False,
        top: int = 10,
        focus: Sequence[str] = (),
        sigprob: int = 0,
        seed: int = 1,
        threshold: float = 0.01,
    ) -> dict[str, Any]:
        """Run the scoap pass and return its JSON (schema odin3-scoap, version 2).

        Per module and net: cc0, cc1, co (and sc0, sc1, so with sequential=True; p1_ppm with
        sigprob=<vectors>), keyed by the module-local net id and name; "top" holds the `top`
        hardest-to-control and hardest-to-observe nets, "rare" (with sigprob) the nets whose
        probability of 1 or 0 is below `threshold`. INF (1000000000) means unreachable. Module
        summaries are also stored as module attributes odin3:scoap_*. See docs/user/scoap.md.
        """
        import json
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "scoap.json"
            words = ["--out", _quote(out), "--top", str(top)] + ["--sequential"] * sequential
            if sigprob > 0:
                words += ["--sigprob", str(sigprob), "--seed", str(seed)]
                words += ["--threshold", repr(threshold)]
            for item in focus:
                words += ["--focus", _quote(item)]
            self.run_pass("scoap", " ".join(words))
            result: dict[str, Any] = json.loads(out.read_text(encoding="utf-8"))
            return result

    def simulate(self, top: str | None = None) -> Simulation:
        """Open a Simulation of module `top` (default: the design's top); see Simulation."""
        return Simulation(self, top)

    def modules(self) -> Iterator[Module]:
        """Every module, in creation order."""
        for i in range(int(self.get("odin3_design_get_module_count", "uint32_t"))):
            yield Module(self, int(self.get("odin3_design_get_module_at", "uint32_t", i)))

    def top(self) -> Module | None:
        module = int(self.get("odin3_design_get_top_module", "uint32_t"))
        return Module(self, module) if module else None

    def module(self, name: str) -> Module | None:
        module = int(self.get("odin3_design_lookup_module", "uint32_t", name.encode()))
        return Module(self, module) if module else None

    def ast(self) -> Ast | None:
        """The AST of the latest read run that kept one: its elaborated store, else its parsed
        one; None when the design holds no AST (odin3_design_get_ast_count / _at)."""
        best: tuple[int, int, Any] | None = None
        for i in range(int(self.get("odin3_design_get_ast_count", "uint32_t"))):
            ptr = self.get("odin3_design_get_ast_at", "odin3_ast *", i)
            run = int(self.odin3.get("odin3_ast_get_run", "uint32_t", ptr))
            form = int(self.odin3.get("odin3_ast_get_form", "uint32_t", ptr))
            if best is None or (run, form) > best[:2]:
                best = (run, form, ptr)  # ELABORATED (2) > PARSED (1) within a run
        return Ast(self, best[2]) if best is not None else None

    def construct_count(self) -> int:
        """One past the last construct row: the table holds rows 1 .. count - 1 (ABI v7)."""
        return int(self.odin3.lib.odin3_design_construct_count(self.ptr))

    def construct(self, index: int) -> Construct:
        """Construct row `index` (1 .. construct_count() - 1); Odin3Error when out of range."""
        raw = self.get("odin3_design_get_construct", "odin3_construct", index)
        return _construct_of(self.odin3, raw)

    def prov_record(self, record: int) -> ProvRecord:
        """The provenance record `record` with its parents, construct and joined constructs."""
        info = self.get("odin3_prov_get_info", "odin3_prov_info", record)
        odin3 = self.odin3
        parents: list[int] = []
        self.call("odin3_prov_visit_parents", record, odin3._u32_cb, odin3.ffi.new_handle(parents))
        rows: list[int] = []
        if int(info.n_joined) > 0:
            self.call("odin3_prov_visit_joined", record, odin3._u32_cb, odin3.ffi.new_handle(rows))
        flags = frozenset(name for bit, name in _PROV_FLAGS if int(info.flags) & bit)
        return ProvRecord(
            record,
            _PROV_KINDS[int(info.kind)],
            int(info.run),
            int(info.op),
            odin3.enum_name("odin3_prov_dir", info.dir),
            odin3.enum_name("odin3_level", info.from_level),
            odin3.enum_name("odin3_level", info.to_level),
            flags,
            self.construct(int(info.construct)) if int(info.construct) else None,
            tuple(parents),
            int(info.n_context),
            int(info.n_joined),
            tuple(self.construct(row) for row in rows),
            odin3.text(info.hier),
        )

    def objects_at(self, file: str, line: int) -> list[Found]:
        """Every object that comes from line `line` of file (odin3_prov_visit_objects)."""
        raw: list[tuple[int, int, int, bool]] = []
        handle = self.odin3.ffi.new_handle(raw)
        self.call(
            "odin3_prov_visit_objects", os.fsencode(file), line, self.odin3._object_cb, handle
        )
        return [self._found(*hit) for hit in raw]

    def _found(self, module: int, kind: int, ident: int, live: bool) -> Found:
        name = self.odin3.enum_name("odin3_objkind", kind)
        obj: IRObject | None = None
        if name == "module":
            obj = Module(self, module)
        elif ident and name == "node":
            obj = Node(self, module, ident)
        elif ident and name == "net":
            obj = Net(self, module, ident)
        elif ident:
            obj = Wire(self, module, ident)
        return Found(module, name, obj, live)


def _sim_destructor(lib: Any, open_sims: dict[int, Any], addr: int) -> Any:
    """The gc destructor of a simulation: close it unless its design's destructor already did."""

    def close(_ptr: Any) -> None:
        raw = open_sims.pop(addr, None)
        if raw is not None:
            lib.odin3_simulation_close(raw)

    return close


class SignalInfo(NamedTuple):
    """A simulated signal (SP-1). kind: "input", "clock", "output" or "internal"; obj: the wire
    or net as (module ID, odin3_objkind, ID); first_bit: position of bit 0 within obj."""

    id: int
    name: str
    leaf: str
    width: int
    kind: str
    msb: int
    lsb: int
    scope: int
    obj: tuple[int, int, int]
    first_bit: int


class ScopeInfo(NamedTuple):
    """An expanded module instance (SP-1); scope 0 is the top. inst: (module ID, node ID). The
    index field of SP-1 is named `scope` (`index` would shadow tuple.index; mypy --strict)."""

    scope: int
    name: str
    leaf: str
    parent: int
    module: int
    inst: tuple[int, int]


_MAX_STEPS = 2**32 - 1
_MAX_SCALAR_WIDTH = 64
_CLOSED = "simulation is closed"
_NOT_FOUND = (
    "{}: not in the simulated netlist (a pass may have removed it: mark it (* keep *) in the "
    "HDL, or see Simulation.signals())"
)


class Simulation:
    """Per-cycle stepping of a design's top module (ABI odin3_simulation_*; spec SP-1 to SP-5).

    The design must be simulable first (`read_verilog; proc; opt; lower`). Values are unsigned
    Python ints, bit i of the int being bit i of the signal. Names use "." between instances.
    While a simulation is open every pass on its design is refused. Use it as a context manager
    or call close(); Design.close() closes its simulations first.
    """

    def __init__(self, design: Design, top: str | None = None) -> None:
        """Open a simulation of module `top` (default: the design's top). ValueError for an
        unknown module name; Odin3Error when the library refuses (it logged why)."""
        if design.ptr is None:
            raise RuntimeError("design is closed")
        module = 0
        if top is not None:
            found = design.module(top)
            if found is None:
                raise ValueError(f"{top}: no such module")
            module = found.id
        self.design = design
        self.odin3 = design.odin3
        raw = design.get("odin3_simulation_open", "odin3_simulation *", module)
        addr = int(self.odin3.ffi.cast("uintptr_t", raw))
        design._open[addr] = raw
        self.ptr: Any = self.odin3.ffi.gc(raw, _sim_destructor(self.odin3.lib, design._open, addr))
        self._cache: dict[str, SignalInfo] = {}
        self._u64 = self.odin3.ffi.new("uint64_t *")
        design._sims.add(self)

    def close(self) -> None:
        """Close the simulation now (else when it is garbage-collected); idempotent."""
        if self.ptr is not None:
            self.odin3.ffi.release(self.ptr)
            self.ptr = None
            self._cache.clear()

    def __enter__(self) -> Simulation:
        return self

    def __exit__(
        self, kind: type[BaseException] | None, exc: BaseException | None, tb: TracebackType | None
    ) -> None:
        self.close()

    def _live(self) -> Any:
        if self.ptr is None:
            raise RuntimeError(_CLOSED)
        return self.ptr

    def _call(self, function: str, *args: Any) -> None:
        self.odin3.call(function, self._live(), *args)

    def _count(self, function: str) -> int:
        return int(self.odin3.get(function, "uint32_t", self._live()))

    def _info(self, ident: int) -> SignalInfo:
        raw = self.odin3.ffi.new("odin3_simulation_signal *")
        self._call("odin3_simulation_get_signal", ident, raw)
        text = self.odin3.text
        return SignalInfo(
            ident,
            text(raw.name),
            text(raw.leaf),
            int(raw.width),
            self.odin3.enum_name("odin3_simulation_kind", raw.kind),
            int(raw.msb),
            int(raw.lsb),
            int(raw.scope),
            (int(raw.obj.module), int(raw.obj.kind), int(raw.obj.id)),
            int(raw.first_bit),
        )

    def signals(self) -> list[SignalInfo]:
        """The listed signals (spec SA-4), IDs 1.. in order; signals found by name later are not
        in the list."""
        count = self._count("odin3_simulation_get_signal_count")
        return [self._info(i) for i in range(1, count + 1)]

    def scopes(self) -> list[ScopeInfo]:
        """One scope per expanded instance, scope 0 (the top) first."""
        raw = self.odin3.ffi.new("odin3_simulation_scope *")
        out = []
        for i in range(self._count("odin3_simulation_get_scope_count")):
            self._call("odin3_simulation_get_scope", i, raw)
            out.append(
                ScopeInfo(
                    i,
                    self.odin3.text(raw.name),
                    self.odin3.text(raw.leaf),
                    int(raw.parent),
                    int(raw.module),
                    (int(raw.inst.module), int(raw.inst.id)),
                )
            )
        return out

    def signal(self, name: str) -> SignalInfo | None:
        """The signal named `name` (cached), or None when the simulated netlist has no such name."""
        found = self._cache.get(name)
        if found is None:
            ident = int(
                self.odin3.get(
                    "odin3_simulation_find_signal", "uint32_t", self._live(), name.encode()
                )
            )
            if ident == 0:
                return None
            found = self._cache[name] = self._info(ident)
        return found

    def _resolve(self, sig: str | SignalInfo) -> SignalInfo:
        self._live()
        if isinstance(sig, SignalInfo):
            return sig
        found = self.signal(sig)
        if found is None:
            raise KeyError(_NOT_FOUND.format(sig))
        return found

    def get(self, sig: str | SignalInfo) -> int:
        """The value of a signal as of the last settle (open, reset, settle or step)."""
        info = self._resolve(sig)
        if info.width <= _MAX_SCALAR_WIDTH:
            self._call("odin3_simulation_get", info.id, self._u64)
            return int(self._u64[0])
        buf = self.odin3.ffi.new("uint8_t[]", info.width)
        self._call("odin3_simulation_get_bits", info.id, buf, info.width)
        return sum(int(buf[i]) << i for i in range(info.width))

    def set(self, sig: str | SignalInfo, value: int) -> None:
        """Set an input signal; it keeps the value until set again. ValueError for a negative
        value or one of 2**width or more; Odin3Error for a signal that is not an input."""
        info = self._resolve(sig)
        if value < 0 or value >> info.width:
            raise ValueError(f"{info.name}: value {value} does not fit in {info.width} bit(s)")
        if info.width <= _MAX_SCALAR_WIDTH:
            self._call("odin3_simulation_set", info.id, value)
            return
        buf = self.odin3.ffi.new("uint8_t[]", [(value >> i) & 1 for i in range(info.width)])
        self._call("odin3_simulation_set_bits", info.id, buf, info.width)

    def __getitem__(self, name: str | SignalInfo) -> int:
        return self.get(name)

    def __setitem__(self, name: str | SignalInfo, value: int) -> None:
        self.set(name, value)

    def settle(self) -> None:
        """Show the combinational effect of inputs just set; no state advances."""
        self._call("odin3_simulation_settle")

    def step(self, cycles: int = 1) -> None:
        """Run `cycles` clock cycles (SC-2). ValueError outside 0 .. 2**32-1."""
        if cycles < 0 or cycles > _MAX_STEPS:
            raise ValueError(f"step: cycles {cycles} out of range 0 .. {_MAX_STEPS}")
        self._call("odin3_simulation_step", cycles)

    def reset(self) -> None:
        """Power-on reset: every value and state bit 0 (INIT applied), cycle counter 0."""
        self._call("odin3_simulation_reset")

    def set_toggle_counting(self, enable: bool = True) -> None:
        """Count per-signal bit toggles between cycle ends (the energy proxy). Tracks the signals
        known now: look up unlisted nets with signal() first. Needs the editing-era libodin3."""
        _edit_probe_sim(self, "odin3_simulation_set_toggle_counting")
        self._call("odin3_simulation_set_toggle_counting", int(enable))

    def toggles(self, sig: str | SignalInfo) -> int:
        """Bit changes of a signal, summed over its bits, since counting was turned on or reset."""
        _edit_probe_sim(self, "odin3_simulation_get_toggles")
        info = self._resolve(sig)
        try:
            self._call("odin3_simulation_get_toggles", info.id, self._u64)
        except Odin3Error as exc:
            raise Odin3Error(
                "odin3_simulation_get_toggles",
                exc.status,
                f"no toggle count for {info.name!r}: counting is off, or the signal was looked up "
                "after set_toggle_counting() (look up every signal you want first)",
            ) from exc
        return int(self._u64[0])

    def run_vectors(
        self,
        inputs: Sequence[str | SignalInfo],
        rows: Sequence[Sequence[int]],
        outputs: Sequence[str | SignalInfo],
    ) -> list[list[int]]:
        """For each row of input values (<= 64 bits each): set the inputs, step one cycle, read
        the outputs. Returns one list of output values per row."""
        _edit_probe_sim(self, "odin3_simulation_run_vectors")
        ffi = self.odin3.ffi
        ins = [self._resolve(sig).id for sig in inputs]
        outs = [self._resolve(sig).id for sig in outputs]
        flat = [value for row in rows for value in row]
        in_ids = ffi.new("uint32_t[]", ins or [0])
        out_ids = ffi.new("uint32_t[]", outs or [0])
        in_vals = ffi.new("uint64_t[]", flat or [0])
        out_vals = ffi.new("uint64_t[]", max(len(rows) * len(outs), 1))
        run = ffi.new("odin3_simulation_run *")
        run.size = ffi.sizeof("odin3_simulation_run")
        run.in_signals, run.n_in, run.in_values = in_ids, len(ins), in_vals
        run.n_vectors = len(rows)
        run.out_signals, run.n_out, run.out_values = out_ids, len(outs), out_vals
        self._call("odin3_simulation_run_vectors", run)
        width = len(outs)
        return [[int(out_vals[r * width + c]) for c in range(width)] for r in range(len(rows))]

    @property
    def cycle(self) -> int:
        """Cycles stepped since open or the last reset."""
        self._call("odin3_simulation_get_cycle", self._u64)
        return int(self._u64[0])


class _Named:
    """Shared by every IR object: provenance and string attributes through an odin3_obj."""

    design: Design

    def obj(self) -> tuple[int, int, int]:
        """The odin3_obj naming this object (module ID, odin3_objkind, ID)."""
        raise NotImplementedError

    def sources(self) -> list[Source]:
        """Where the object comes from, the naming location first (odin3_prov_visit_sources)."""
        found: list[Source] = []
        odin3 = self.design.odin3
        self.design.call(
            "odin3_prov_visit_sources", self.obj(), odin3._source_cb, odin3.ffi.new_handle(found)
        )
        return found

    def provenance(self) -> list[ProvRecord]:
        """The provenance records of the object (ABI v7): its naming record first, then every
        record reachable through parents, depth-first in parent order, each once. Empty for an
        object without provenance. Nothing is recomputed in Python: each record is
        odin3_prov_get_info, odin3_prov_visit_parents and odin3_prov_visit_joined."""
        design = self.design
        first = int(design.get("odin3_obj_get_prov", "uint32_t", self.obj()))
        records: list[ProvRecord] = []
        seen: set[int] = set()
        stack = [first] if first else []
        while stack:
            ident = stack.pop()
            if ident in seen:
                continue
            seen.add(ident)
            record = design.prov_record(ident)
            records.append(record)
            stack.extend(reversed(record.parents))
        return records

    def chain_with_leaves(self) -> list[tuple[Construct, int]]:
        """chain() with the SOURCE record that reached each construct first: (construct, leaf
        record ID) pairs, in the same order. The leaf record is the one whose ``construct`` the
        entry is (or the first leaf under it, for an ancestor row)."""
        reached: list[tuple[Construct, int]] = []
        odin3 = self.design.odin3
        self.design.call(
            "odin3_prov_visit_chain", self.obj(), odin3._construct_cb, odin3.ffi.new_handle(reached)
        )
        return reached

    def chain(self) -> list[Construct]:
        """Where the object comes from in the HDL (odin3_prov_visit_chain): for each source leaf
        in the order of sources(), its construct and then its ancestors up to the module; each
        construct once, at its first reach. Empty when the design holds no construct table (read
        with --prov-ast off, or from a netlist) or the object has no provenance. See
        chain_with_leaves() for the leaf record of each entry."""
        return [construct for construct, _leaf in self.chain_with_leaves()]

    @property
    def edit_tag(self) -> str | None:
        """The tag of the edit that made this node, net or wire (odin3_edit_*), else None."""
        _edit_probe(self.design, "odin3_edit_get_tag")
        value = self.design.get("odin3_edit_get_tag", "const char *", self.obj())
        return None if value == self.design.odin3.ffi.NULL else self.design.odin3.text(value)

    @property
    def edit_events(self) -> list[tuple[str, str]]:
        """The edit events of this node or net, oldest first, as (kind, tag): "added", "param",
        "rewired", "removed" (odin3_edit_get_events). Empty for an object no edit touched."""
        _edit_probe(self.design, "odin3_edit_get_events")
        value = self.design.get("odin3_edit_get_events", "const char *", self.obj())
        if value == self.design.odin3.ffi.NULL:
            return []
        lines = self.design.odin3.text(value).split("\n")
        return [(kind, tag) for kind, _, tag in (ln.partition(" ") for ln in lines if ln)]

    def attr(self, key: str) -> str | None:
        """The string attribute key, None when absent."""
        value = self.design.get("odin3_attr_get_string", "const char *", self.obj(), key.encode())
        return None if value == self.design.odin3.ffi.NULL else self.design.odin3.text(value)

    def set_attr(self, key: str, value: str) -> None:
        """Set the string attribute key (an IR mutation)."""
        self.design.call("odin3_attr_set_string", self.obj(), key.encode(), value.encode())


@dataclass(frozen=True)
class Module(_Named):
    """A module, by its design-global ID."""

    design: Design = field(compare=False, repr=False)
    id: int

    def obj(self) -> tuple[int, int, int]:
        return (self.id, int(self.design.odin3.lib.ODIN3_OBJ_MODULE), 0)

    def _get(self, function: str, *args: Any) -> int:
        return int(self.design.get(function, "uint32_t", self.id, *args))

    @property
    def name(self) -> str:
        return self.design.get_text("odin3_module_get_name", self.id)

    def _live(self, store: str, cls: type[_L]) -> Iterator[_L]:
        for i in range(1, self._get(f"odin3_module_get_{store}_end")):
            item = cls(self.design, self.id, i)
            if item.is_live:
                yield item

    def nodes(self) -> Iterator[Node]:
        """Live nodes in ID order (port nodes included)."""
        return self._live("node", Node)

    def nets(self) -> Iterator[Net]:
        """Live nets in ID order."""
        return self._live("net", Net)

    def wires(self) -> Iterator[Wire]:
        """Live wires in ID order (port wires included)."""
        return self._live("wire", Wire)

    def node_count(self) -> int:
        """Live nodes, as the library counts them."""
        return self._get("odin3_module_get_node_count")

    def net_count(self) -> int:
        return self._get("odin3_module_get_net_count")

    def wire_count(self) -> int:
        return self._get("odin3_module_get_wire_count")

    def port_count(self) -> int:
        return self._get("odin3_module_get_port_count")

    def proc_count(self) -> int:
        """Live process ($proc) nodes, before the `proc` pass lowers them (ABI v4, 2D)."""
        return self._get("odin3_module_get_proc_count")

    def ports(self) -> list[ModulePort]:
        """The module's ports in declaration order: each its port node and wire."""
        return [
            ModulePort(
                Node(self.design, self.id, self._get("odin3_module_get_port_node", i)),
                Wire(self.design, self.id, self._get("odin3_module_get_port_wire", i)),
            )
            for i in range(self.port_count())
        ]

    def _lookup(self, store: str, name: str) -> int:
        return self._get(f"odin3_module_lookup_{store}", name.encode())

    def _add(
        self, function: str, tag: str, params: dict[str, int] | None = None, **fields: Any
    ) -> tuple[int, int]:
        ffi = self.design.odin3.ffi
        fields["tag"] = tag
        keep = {k: ffi.new("char[]", v.encode()) for k, v in fields.items() if isinstance(v, str)}
        what = _edit_new(ffi)
        for key, value in fields.items():
            if isinstance(value, str):
                setattr(what, key, keep[key])
            elif value is not None:
                setattr(what, key, value)
        names = [ffi.new("char[]", name.encode()) for name in params or {}]
        pnames = ffi.new("const char *[]", names or 1)
        pvalues = ffi.new("int64_t[]", list((params or {}).values()) or 1)
        if names:
            # a library from before the parameter fields ignores them (and accepts the larger
            # struct): odin3_edit_get_events arrived with them, so its absence means too old
            _edit_probe(self.design, "odin3_edit_get_events")
            what.param_names, what.param_values, what.n_params = pnames, pvalues, len(names)
        _edit_probe(self.design, function)
        return _edit_ref(self.design, function, self.id, what)

    def add_net(self, name: str | None, tag: str) -> Net:
        """A new net (odin3_edit_add_net). tag marks the edit in the provenance (edit_tag)."""
        return Net(self.design, *self._add("odin3_edit_add_net", tag, name=name))

    def add_node(
        self, cell_type: str, name: str | None, tag: str, params: dict[str, int] | None = None
    ) -> Node:
        """A new node of a registered or built-in cell type, pins open. Its parameters are the
        type's defaults except the integer ones in params, e.g. {"INIT": 0} for a flip-flop
        that starts at 0 (no twin node is made). Odin3Error for an unknown or non-integer
        parameter name."""
        ref = self._add("odin3_edit_add_node", tag, params, name=name, cell_type=cell_type)
        return Node(self.design, *ref)

    def add_sop(self, n_inputs: int, cover: str, name: str | None, tag: str) -> Node:
        """A new $sop (BLIF .names) of n_inputs inputs; cover is the BLIF rows, e.g. "11 1\\n"."""
        ref = self._add("odin3_edit_add_sop", tag, name=name, cover=cover, n_inputs=n_inputs)
        return Node(self.design, *ref)

    def node(self, name: str) -> Node | None:
        ident = self._lookup("node", name)
        return Node(self.design, self.id, ident) if ident else None

    def net(self, name: str) -> Net | None:
        """The live net named name or holding it as an alias."""
        ident = self._lookup("net", name)
        return Net(self.design, self.id, ident) if ident else None

    def wire(self, name: str) -> Wire | None:
        ident = self._lookup("wire", name)
        return Wire(self.design, self.id, ident) if ident else None


@dataclass(frozen=True)
class _Local(_Named):
    """A module-local object: its module's ID and its own ID (an odin3_ref)."""

    design: Design = field(compare=False, repr=False)
    module_id: int
    id: int

    KIND: ClassVar[str] = ""  # odin3_objkind constant
    LIVE: ClassVar[str] = ""  # is_live function

    def ref(self) -> tuple[int, int]:
        """The odin3_ref naming this object."""
        return (self.module_id, self.id)

    def obj(self) -> tuple[int, int, int]:
        return (self.module_id, int(getattr(self.design.odin3.lib, self.KIND)), self.id)

    @property
    def is_live(self) -> bool:
        return bool(self._get(self.LIVE, "bool"))

    def _get(self, function: str, ctype: str = "uint32_t", *args: Any) -> Any:
        return self.design.get(function, ctype, self.ref(), *args)

    def _text(self, function: str, *args: Any) -> str:
        return self.design.odin3.text(self._get(function, "const char *", *args))

    @property
    def module(self) -> Module:
        return Module(self.design, self.module_id)


_L = TypeVar("_L", bound=_Local)


_TOP_LOCK = threading.RLock()
_current_top: str | None = None  # the top name this binding last set (guarded by _TOP_LOCK)


def _set_top(odin3: Odin3, name: str | None) -> None:
    """odin3_pass_set_top_name(name), NULL for None; remembers the value. Hold _TOP_LOCK."""
    global _current_top
    odin3.call("odin3_pass_set_top_name", odin3.ffi.NULL if name is None else name.encode())
    _current_top = name


def _cstr(text: str | None) -> bytes | Any:
    """Text as C bytes; None stays None (cffi turns it into NULL)."""
    return None if text is None else text.encode()


def _edit_probe_sim(sim: Simulation, function: str) -> None:
    if not hasattr(sim.odin3.lib, function):
        raise Odin3Error(function, -1, f"libodin3 too old: needs {function} (after v1.2.0)")


def _edit_new(ffi: Any) -> Any:
    """A zeroed odin3_edit_new with size set (the ABI's leading size field)."""
    what = ffi.new("odin3_edit_new *")
    what.size = ffi.sizeof("odin3_edit_new")
    return what


def _edit_probe(design: Design, function: str) -> None:
    """Raise a clear error when libodin3 predates the netlist-editing ABI."""
    if not hasattr(design.odin3.lib, function):
        raise Odin3Error(
            function, -1, f"libodin3 too old: needs {function} (netlist editing, after v1.2.0)"
        )


def _edit_ref(design: Design, function: str, *args: Any) -> tuple[int, int]:
    """Call an odin3_edit_* function whose last argument is an odin3_ref out; return its pair."""
    _edit_probe(design, function)
    out = design.get(function, "odin3_ref", *args)
    return (int(out.module), int(out.id))


def _edit_call(design: Design, function: str, *args: Any) -> None:
    _edit_probe(design, function)
    design.call(function, *args)


def _span(design: Design, module: int, span: Any) -> list[Pin]:
    return [Pin(design, module, int(span.first) + i) for i in range(int(span.count))]


class Port(NamedTuple):
    """One port of a node's cell type (port: its index), with the node's pins (LSB first)."""

    port: int
    name: str
    direction: str
    width: int
    pins: list[Pin]


class ModulePort(NamedTuple):
    """A module port: its port node ($port_in/$port_out/$port_inout) and its wire."""

    node: Node
    wire: Wire


@dataclass(frozen=True)
class Node(_Local):
    """A cell instance."""

    KIND: ClassVar[str] = "ODIN3_OBJ_NODE"
    LIVE: ClassVar[str] = "odin3_node_is_live"

    @property
    def type_name(self) -> str:
        return self._text("odin3_node_get_type_name")

    @property
    def granularity(self) -> str:
        """word, bit, hard, blackbox, module or port."""
        gran = self._get("odin3_node_get_granularity", "odin3_granularity")
        return self.design.odin3.enum_name("odin3_granularity", gran)

    @property
    def name(self) -> str:
        return self._text("odin3_node_get_name")

    def param_kind(self, index: int) -> str:
        """The value kind of parameter index: INT, BITS, STRING, COVER or PROC (a $proc's BODY;
        ABI v4, 2D), the odin3_value_kind constant's name."""
        kind = self._get("odin3_node_get_param_kind", "odin3_value_kind", index)
        return self.design.odin3.enum_name("odin3_value_kind", kind).upper()

    def params(self) -> dict[str, int | str]:
        """Parameter name -> value: an int for INT, else the ABI's text form (a PROC's text is
        its RTLIL process block)."""
        lib = self.design.odin3.lib
        out: dict[str, int | str] = {}
        for i in range(int(self._get("odin3_node_get_param_count"))):
            name = self._text("odin3_node_get_param_name", i)
            if self._get("odin3_node_get_param_kind", "odin3_value_kind", i) == lib.ODIN3_VAL_INT:
                out[name] = int(self._get("odin3_node_get_param_int", "int64_t", i))
            else:
                out[name] = self._text("odin3_node_get_param_text", i)
        return out

    def _twin(self, function: str, what: Any) -> Node:
        """Run a twin-making edit; the twin's ref comes back through an out parameter."""
        _edit_probe(self.design, function)
        out = self.design.odin3.ffi.new("odin3_ref *")
        try:
            self.design.call(function, self.ref(), what, out)
        except Odin3Error as exc:
            if out.id:  # replaced, then a later step failed: the error carries the twin
                exc.twin = Node(self.design, int(out.module), int(out.id))
            raise
        return Node(self.design, int(out.module), int(out.id))

    def set_cover(self, cover: str, tag: str) -> Node:
        """Replace a $sop's cover (same width). Returns the twin node that takes over this
        node's pins, name and attributes; this node is then dead. The twin's provenance is
        derived from this node's and the edit's (odin3_edit_set_cover)."""
        ffi = self.design.odin3.ffi
        keep = (ffi.new("char[]", cover.encode()), ffi.new("char[]", tag.encode()))
        what = _edit_new(ffi)
        what.cover, what.tag = keep
        return self._twin("odin3_edit_set_cover", what)

    def set_param_int(self, index: int, value: int, tag: str) -> Node:
        """Change an integer parameter; returns the twin node (as set_cover)."""
        ffi = self.design.odin3.ffi
        keep = ffi.new("char[]", tag.encode())
        what = _edit_new(ffi)
        what.tag, what.index, what.value = keep, index, value
        return self._twin("odin3_edit_set_param_int", what)

    def remove(self, allow_undriven: bool = False) -> None:
        """Remove this node (odin3_edit_remove_node). Refused (Odin3Error) when it drives a net
        that has other pins, unless allow_undriven: the net is then left undriven."""
        _edit_call(self.design, "odin3_edit_remove_node", self.ref(), int(allow_undriven))

    def pins(self) -> list[Pin]:
        """All pins: port order, then bit order (LSB first)."""
        return _span(self.design, self.module_id, self._get("odin3_node_get_pins", "odin3_span"))

    def port_dir(self, port: int) -> str:
        """in, out or inout."""
        dir_ = self._get("odin3_node_get_port_dir", "odin3_dir", port)
        return self.design.odin3.enum_name("odin3_dir", dir_)

    def ports(self) -> list[Port]:
        """The cell type's ports in definition order, with this node's pins."""
        return [
            Port(
                i,
                self._text("odin3_node_get_port_name", i),
                self.port_dir(i),
                int(self._get("odin3_node_get_port_width", "uint32_t", i)),
                _span(
                    self.design,
                    self.module_id,
                    self._get("odin3_node_get_port_pins", "odin3_span", i),
                ),
            )
            for i in range(int(self._get("odin3_node_get_port_count")))
        ]


@dataclass(frozen=True)
class Pin(_Local):
    """One bit of one port of a node. Its provenance and attributes are its node's."""

    def obj(self) -> tuple[int, int, int]:
        return self.node().obj()

    @property
    def is_live(self) -> bool:
        """A pin lives while its node does."""
        return self.node().is_live

    def node(self) -> Node:
        return Node(self.design, self.module_id, int(self._get("odin3_pin_get_node")))

    @property
    def port(self) -> int:
        return int(self._get("odin3_pin_get_port"))

    @property
    def bit(self) -> int:
        return int(self._get("odin3_pin_get_bit"))

    def direction(self) -> str:
        """in, out or inout (its port's direction)."""
        return self.node().port_dir(self.port)

    def connect(self, net: Net) -> None:
        """Connect this unconnected pin to net (odin3_edit_connect)."""
        _edit_call(self.design, "odin3_edit_connect", self.ref(), net.ref())

    def disconnect(self, tag: str | None = None) -> None:
        """Disconnect this pin. The pin's node gets the edit event "removed" with tag (default
        "edit"; odin3_edit_disconnect[_tagged])."""
        if tag is None:
            _edit_call(self.design, "odin3_edit_disconnect", self.ref())
        else:
            _edit_call(self.design, "odin3_edit_disconnect_tagged", self.ref(), tag.encode())

    def rewire(self, net: Net, tag: str | None = None) -> None:
        """Move this pin to net in one step. The pin's node gets the edit event "rewired" with tag
        (default "edit"; odin3_edit_rewire[_tagged]); works on an unconnected pin too."""
        if tag is None:
            _edit_call(self.design, "odin3_edit_rewire", self.ref(), net.ref())
        else:
            _edit_call(self.design, "odin3_edit_rewire_tagged", self.ref(), net.ref(), tag.encode())

    def net(self) -> Net | None:
        """The net the pin is connected to, None when unconnected."""
        net = int(self._get("odin3_pin_get_net"))
        return Net(self.design, self.module_id, net) if net else None


@dataclass(frozen=True)
class Net(_Local):
    """A net: drivers first among its pins."""

    KIND: ClassVar[str] = "ODIN3_OBJ_NET"
    LIVE: ClassVar[str] = "odin3_net_is_live"

    @property
    def name(self) -> str:
        return self._text("odin3_net_get_name")

    def remove(self) -> None:
        """Remove this net, which must have no pins (odin3_edit_remove_net)."""
        _edit_call(self.design, "odin3_edit_remove_net", self.ref())

    def pins(self) -> list[Pin]:
        """Every pin on the net, its drivers first."""
        count = int(self._get("odin3_net_get_pin_count"))
        return [
            Pin(self.design, self.module_id, int(self._get("odin3_net_get_pin_at", "uint32_t", i)))
            for i in range(count)
        ]

    def drivers(self) -> list[Pin]:
        """The OUT and INOUT pins on the net."""
        return self.pins()[: int(self._get("odin3_net_get_driver_count"))]

    def driver(self) -> Pin | None:
        """The first driver, None when the net has none."""
        pin = int(self._get("odin3_net_get_driver"))
        return Pin(self.design, self.module_id, pin) if pin else None

    def aliases(self) -> list[str]:
        """Names the net was also known by, oldest first."""
        count = int(self._get("odin3_net_get_alias_count"))
        return [self._text("odin3_net_get_alias_name", i) for i in range(count)]


@dataclass(frozen=True)
class Wire(_Local):
    """A named bundle of nets (a declared signal)."""

    KIND: ClassVar[str] = "ODIN3_OBJ_WIRE"
    LIVE: ClassVar[str] = "odin3_wire_is_live"

    @property
    def name(self) -> str:
        return self._text("odin3_wire_get_name")


class Ast:
    """An AST store the design holds (odin3_ast): its run, form and UNIT roots."""

    def __init__(self, design: Design, ptr: Any) -> None:
        self.design = design  # keeps the owning design (and so the store) alive
        self.ptr = ptr

    def _get(self, function: str, *args: Any) -> int:
        return int(self.design.odin3.get(function, "uint32_t", self.ptr, *args))

    @property
    def run(self) -> int:
        """The pass run of the read that built it."""
        return self._get("odin3_ast_get_run")

    @property
    def form(self) -> str:
        """ "parsed" or "elaborated"."""
        parsed = self._get("odin3_ast_get_form") == self.design.odin3.lib.ODIN3_AST_FORM_PARSED
        return "parsed" if parsed else "elaborated"

    def roots(self) -> list[AstNode]:
        """The UNIT nodes, in creation order."""
        count = self._get("odin3_ast_get_root_count")
        return [AstNode(self, self._get("odin3_ast_get_root_at", i)) for i in range(count)]

    def node(self, ident: int) -> AstNode:
        """The node with store-local ID ident (1 .. node end - 1)."""
        return AstNode(self, ident)

    def __len__(self) -> int:
        """The number of nodes (IDs run from 1 to this)."""
        return self._get("odin3_ast_get_node_end") - 1


@dataclass(frozen=True)
class AstNode:
    """One AST node: a store and a node ID."""

    tree: Ast = field(compare=False, repr=False)
    id: int

    def _get(self, function: str, ctype: str = "uint32_t", *args: Any) -> Any:
        return self.tree.design.odin3.get(function, ctype, self.tree.ptr, self.id, *args)

    def _text(self, function: str) -> str:
        return self.tree.design.odin3.text(self._get(function, "const char *"))

    @property
    def kind(self) -> str:
        """The kind's name, e.g. "MODULE" (odin3_ast_get_kind_name)."""
        return self._text("odin3_ast_get_kind_name")

    @property
    def sub(self) -> str:
        """The sub-kind's spelling ("+", "posedge", "input", …), "" when the kind has none."""
        return self._text("odin3_ast_get_sub_name")

    @property
    def name(self) -> str:
        """The node's name, "" when it has none."""
        return self._text("odin3_ast_get_name")

    @property
    def loc(self) -> str:
        """Where the node starts, as "file:line:col" ("<unknown>:0:0" when not recorded)."""
        design = self.tree.design
        odin3 = design.odin3
        raw = int(self._get("odin3_ast_get_loc"))
        srcman = design.get("odin3_design_get_srcman", "odin3_srcman *")
        return odin3.text(odin3.get("odin3_srcman_get_format", "const char *", srcman, raw))

    def children(self) -> list[AstNode]:
        """The present children, slots first then the tail (absent optional slots skipped)."""
        count = int(self._get("odin3_ast_get_nchild"))
        kids = (int(self._get("odin3_ast_get_child", "uint32_t", i)) for i in range(count))
        return [AstNode(self.tree, kid) for kid in kids if kid]


# --- name maps (V1.2.0 3.2, 3.5): a file reader that restores nothing into the IR ---------------

_MAP_ESCAPES_V1 = {"%09": "\t", "%0A": "\n", "%5C": "\\", "%25": "%", "%23": "#"}
_MAP_ESCAPES = {**_MAP_ESCAPES_V1, "%2C": ",", "%7C": "|"}
_MAP_MIN_FIELDS = {"O": 9, "N": 5, "R": 9, "C": 7, "F": 2, "T": 4, "A": 3, "Q": 12, "K": 7}
_MAP_MAX_FIELDS = 12  # the fields the C parse keeps (ODIN3_NM2_MAX_FIELDS)
_MAP_MAX_LINE = 1 << 20  # the longest line kept, bytes (ODIN3_NAME_MAP_MAX_LINE); longer: ignored
_MAP_MAX_FILE = 1 << 30  # the largest map file read, bytes (the C `names` pass has the same cap)
_MAP_KINDS = ("net", "node", "wire")
_MAP_DECIMAL = re.compile(r"[0-9]{1,10}")  # as the C parse_decimal: ASCII digits, at most 10
_MAP_UINT32_MAX = 0xFFFFFFFF
_MAP_HEADER = re.compile(
    r"# odin3-names (?P<version>[12]) hash=sha256 width=(?P<width>\d{1,2})"
    r"(?: prov=1 ast=(?P<ast>refs|constructs|full|none))?"
)


def _map_unescape(text: str, v2: bool = True) -> str:
    """The text of a name map field with its escapes undone: %09 %0A %5C %25 %23 and, in a
    version 2 map, %2C and %7C."""
    table = _MAP_ESCAPES if v2 else _MAP_ESCAPES_V1

    def one(match: re.Match[str]) -> str:
        try:
            return table[match.group(0)]
        except KeyError:
            raise ValueError(f"bad escape '{match.group(0)}'") from None

    return re.sub(r"%.{0,2}", one, text)


def _map_decimal(text: str) -> int:
    """A decimal number as the C parse reads it: 1 to 10 ASCII digits, at most 2**32 - 1."""
    if _MAP_DECIMAL.fullmatch(text) is None or int(text) > _MAP_UINT32_MAX:
        raise ValueError(f"'{text}' is not a decimal number")
    return int(text)


def _map_ref(text: str, prefix: str) -> int:
    """The number in a reference like r12 (0 for an empty field)."""
    if not text:
        return 0
    number = _map_decimal(text[1:]) if text[0] == prefix else 0
    if number == 0:
        raise ValueError(f"'{text}' is not a {prefix}<n> reference")
    return number


def _map_refs(field: str, prefix: str, sep: str = ",") -> tuple[int, ...]:
    """The numbers in a reference list. Only a wholly empty field means no references. An empty
    element of a longer list is kept as 0 (it keeps the places of the others) and every walk
    skips it, as the C walk does: the C parse does not ignore such a line, `names --check` counts
    it as a reference that does not resolve."""
    return tuple(_map_ref(item, prefix) for item in field.split(sep)) if field else ()


@dataclass(frozen=True)
class MapObject:
    """An O or N line: an object of a netlist and its provenance record. type is O (renamed by a
    hash: name is the hash name and full its full name) or N (kept its name; full is empty);
    kind is net, node or wire; records are record numbers (Odin III writes one); level is the
    granularity of the record. Version 1 maps give O objects without records. raw is the line
    as the file holds it."""

    type: str
    module: str
    kind: str
    name: str
    full: str
    hier: str
    file: str
    line: int
    records: tuple[int, ...]
    level: str
    raw: str


@dataclass(frozen=True)
class MapRecord:
    """An R line: a provenance record. kind is S (source), I (imported) or D (derived); dir is
    origin or <refine|rewrite|coarsen|context>:<from level>; step is t<n>.<op>; parents and
    constructs are record and construct numbers; origins are f<n>:span texts; flags are the
    comma-separated flags (hier=... unescaped)."""

    number: int
    kind: str
    level: str
    dir: str
    step: str
    parents: tuple[int, ...]
    origins: tuple[str, ...]
    constructs: tuple[int, ...]
    flags: tuple[str, ...]
    raw: str


@dataclass(frozen=True)
class MapConstruct:
    """A C line: a construct of the AST layer. parent is a construct number (0 for a module) and
    file a file number (0 when empty)."""

    number: int
    kind: str
    level: str
    parent: int
    file: int
    span: str
    name: str
    raw: str


def _map_list(text: str) -> tuple[str, ...]:
    """A comma list of a Q or K field, each element unescaped (an empty field is no elements)."""
    return tuple(_map_unescape(item) for item in text.split(",")) if text else ()


def _map_ref_lenient(text: str, prefix: str) -> int:
    """The number in a reference like r12, 0 when the field is not one (a Q or K line is kept even
    then; `names --check` reports the id)."""
    try:
        return _map_ref(text, prefix)
    except ValueError:
        return 0


@dataclass(frozen=True)
class MapWhy:
    """A Q line (spec 3.6): why the join record ``record`` was made and how critical it was.
    ``why`` and ``gain`` are ``name:share`` elements, ``near`` and ``npaths`` ``margin:value``
    elements, ``basis`` ``field=r|i|-`` elements; ``pcrit``, ``rho``, ``slack`` and ``members`` are
    the field texts (empty when absent). ``stale`` is ``-`` or ``t<n>``. ``record`` is 0 when the
    id field is not an ``r<n>``."""

    record: int
    why: tuple[str, ...]
    objective: str
    gain: tuple[str, ...]
    pcrit: str
    rho: str
    slack: str
    near: tuple[str, ...]
    npaths: tuple[str, ...]
    members: str
    basis: tuple[str, ...]
    stale: str
    raw: str


@dataclass(frozen=True)
class MapMember:
    """A K line (spec 3.6): one member (an atom) of the join record ``record`` with its own
    criticality; fields as in MapWhy (``basis`` lists ``pcrit``, ``rho`` and ``slack``)."""

    record: int
    member: str
    pcrit: str
    rho: str
    slack: str
    basis: tuple[str, ...]
    stale: str
    raw: str


@dataclass
class NameMap:
    """A parsed name map. version is 1 or 2; ast is the header's ast word (version 2). Lines of
    a type this reader does not know are skipped with one warning per type; A lines are kept
    unparsed in atoms (fields after the type letter); Q lines are whys and K lines members (spec
    3.6). Hash names are not verified here
    (`names --check` does)."""

    version: int
    width: int
    ast: str
    objects: list[MapObject] = field(default_factory=list)
    records: list[MapRecord] = field(default_factory=list)
    constructs: list[MapConstruct] = field(default_factory=list)
    files: list[str] = field(default_factory=list)
    runs: list[tuple[str, str, str]] = field(default_factory=list)
    atoms: list[tuple[str, ...]] = field(default_factory=list)
    whys: list[MapWhy] = field(default_factory=list)
    members: list[MapMember] = field(default_factory=list)
    ignored: int = 0
    _index: dict[str, list[MapObject]] = field(
        default_factory=dict, init=False, repr=False, compare=False
    )
    _indexed: int = field(default=-1, init=False, repr=False, compare=False)

    def find(self, name: str) -> list[MapObject]:
        """The O lines whose hash name, and the N lines whose name, is name, in file order. The
        first call builds an index by name (rebuilt when objects has grown or shrunk since)."""
        if self._indexed != len(self.objects):
            self._index = {}
            for obj in self.objects:
                self._index.setdefault(obj.name, []).append(obj)
            self._indexed = len(self.objects)
        return list(self._index.get(name, ()))

    def why_of(self, record: int) -> MapWhy | None:
        """The current Q line of a join record (the last whose stale field is ``-``), None when it
        has none. A stale line is history: ask ``whys`` for it."""
        found = [w for w in self.whys if w.record == record and w.stale == "-"]
        return found[-1] if found else None

    def members_of(self, record: int) -> list[MapMember]:
        """The current K lines (stale ``-``) of a join record, in file order."""
        return [m for m in self.members if m.record == record and m.stale == "-"]

    def provenance(self, obj: MapObject) -> list[MapRecord]:
        """The records of obj, as Node.provenance(): its naming record first, then every record
        reachable through parents, depth first in parent order, each once."""
        records: list[MapRecord] = []
        seen: set[int] = set()
        stack = list(obj.records[:1])
        while stack:
            number = stack.pop()
            if number in seen or not 1 <= number <= len(self.records):
                continue
            seen.add(number)
            record = self.records[number - 1]
            records.append(record)
            stack.extend(reversed(record.parents))
        return records

    def chain(self, obj: MapObject) -> list[MapConstruct]:
        """The HDL constructs of obj, as Node.chain(): for each source leaf of provenance() that
        has a construct, the construct and then its ancestors up to the module, each once."""
        chain: list[MapConstruct] = []
        reached: set[int] = set()
        for record in self.provenance(obj):
            number = record.constructs[0] if record.kind != "D" and record.constructs else 0
            for _ in range(len(self.constructs)):
                if number in reached or not 1 <= number <= len(self.constructs):
                    break
                reached.add(number)
                construct = self.constructs[number - 1]
                chain.append(construct)
                number = construct.parent
        return chain


def _map_header(line: str, path: str) -> NameMap:
    match = _MAP_HEADER.fullmatch(line)
    if (
        match is None
        or not 1 <= int(match["width"]) <= 52
        or ((match["version"] == "2") != (match["ast"] is not None))
    ):
        raise ValueError(f"{path}:1: not a name map header: '{line}'")
    return NameMap(int(match["version"]), int(match["width"]), match["ast"] or "")


def _map_object_o(fields: list[str], text: str, v2: bool) -> MapObject:
    """The O line (version 1: the seven fields, no records) as the C `build_entry` reads it: the
    module, short, full, hier and file fields are unescaped, kind and line are not."""
    esc = [_map_unescape(item, v2) for item in fields[:6]]
    if fields[1] not in _MAP_KINDS:
        raise ValueError("unknown kind (want net, node or wire)")
    line = _map_decimal(fields[6])
    if not v2:
        return MapObject("O", esc[0], fields[1], esc[2], esc[3], esc[4], esc[5], line, (), "", text)
    return MapObject(
        "O",
        esc[0],
        fields[1],
        esc[2],
        esc[3],
        esc[4],
        esc[5],
        line,
        _map_refs(fields[7], "r"),
        _map_unescape(fields[8]),
        text,
    )


def _map_line_v1(nm: NameMap, text: str) -> None:
    """Adds one version 1 line (seven fields: module kind short full hier file line)."""
    fields = text.split("\t")
    if len(fields) != 7:
        raise ValueError("not 7 fields")
    nm.objects.append(_map_object_o(fields, text, False))


def _map_line(nm: NameMap, text: str) -> None:
    """Adds one version 2 line (type letter, TAB, fields); ValueError when it is malformed. Only
    the first 12 fields are read, as the C parse keeps no more."""
    kind, fields = text[0], text[2:].split("\t")
    if len(fields) < _MAP_MIN_FIELDS[kind]:
        raise ValueError("too few fields for its type")
    fields = fields[:_MAP_MAX_FIELDS]
    if kind == "O":
        nm.objects.append(_map_object_o(fields, text, True))
        return
    raw = [_map_unescape(item) for item in fields]
    if kind == "N":
        nm.objects.append(
            MapObject(
                "N", raw[0], raw[1], raw[2], "", "", "", 0, _map_refs(fields[3], "r"), raw[4], text
            )
        )
    elif kind == "R":
        nm.records.append(
            MapRecord(
                _map_ref(fields[0], "r"),
                raw[1],
                raw[2],
                raw[3],
                raw[4],
                _map_refs(fields[5], "r"),
                tuple(_map_unescape(item) for item in fields[6].split("|")) if fields[6] else (),
                _map_refs(fields[7], "c"),
                tuple(_map_unescape(item) for item in fields[8].split(",")) if fields[8] else (),
                text,
            )
        )
    elif kind == "C":
        nm.constructs.append(
            MapConstruct(
                _map_ref(fields[0], "c"),
                raw[1],
                raw[2],
                _map_ref(fields[3], "c"),
                _map_ref(fields[4], "f"),
                raw[5],
                raw[6],
                text,
            )
        )
    elif kind == "F":
        nm.files.append(raw[1])
    elif kind == "T":
        nm.runs.append((raw[1], raw[2], raw[3]))
    elif kind == "Q":
        nm.whys.append(
            MapWhy(
                _map_ref_lenient(fields[0], "r"),
                _map_list(fields[1]),
                raw[2],
                _map_list(fields[3]),
                raw[4],
                raw[5],
                raw[6],
                _map_list(fields[7]),
                _map_list(fields[8]),
                raw[9],
                _map_list(fields[10]),
                raw[11],
                text,
            )
        )
    elif kind == "K":
        nm.members.append(
            MapMember(
                _map_ref_lenient(fields[0], "r"),
                raw[1],
                raw[2],
                raw[3],
                raw[4],
                _map_list(fields[5]),
                raw[6],
                text,
            )
        )
    else:
        nm.atoms.append(tuple(raw))


def _map_add(nm: NameMap, raw: bytes, path: str, number: int, warned: set[str]) -> None:
    """Parses one line (bytes, no LF) into nm; a line the reader cannot use is counted in
    nm.ignored with a warning naming path:number."""
    try:
        if len(raw) > _MAP_MAX_LINE:
            raise ValueError("line is too long")
        line = raw.decode("utf-8")
        if line.endswith("\r"):
            raise ValueError("CR at the end of the line (a name map uses LF line ends)")
        if nm.version == 1:
            _map_line_v1(nm, line)
        elif len(line) < 2 or line[1] != "\t":
            raise ValueError("no type letter and TAB at the start of the line")
        elif line[0] in _MAP_MIN_FIELDS:
            _map_line(nm, line)
        else:
            if line[0] not in warned:
                warned.add(line[0])
                LOG.warning(
                    "%s:%d: name map line type '%s' is unknown and ignored", path, number, line[0]
                )
            nm.ignored += 1
    except ValueError as err:  # includes UnicodeDecodeError
        LOG.warning("%s:%d: name map entry ignored: %s", path, number, err)
        nm.ignored += 1


def read_name_map(path: str | Path) -> NameMap:
    """Read a name map (version 1 or 2, `<netlist>.names`) from a file. Nothing is restored into
    the IR and no design is needed; lines the reader cannot use (too long, not UTF-8, ending in
    CR, malformed) are skipped with a warning naming path:line and counted in ignored, as the C
    parse does. Raises ValueError (with path:line) for a bad header or a file over 1 GiB and
    OSError when the file cannot be read. See NameMap.provenance() and NameMap.chain()."""
    size = Path(path).stat().st_size
    if size > _MAP_MAX_FILE:
        raise ValueError(f"{path}: the name map is over {_MAP_MAX_FILE} bytes")
    lines = Path(path).read_bytes().split(b"\n")
    if lines and lines[-1] == b"":
        lines.pop()
    if not lines:
        raise ValueError(f"{path}:1: not a name map header: ''")
    try:
        head = lines[0].decode("utf-8")
    except UnicodeDecodeError:
        head = lines[0].decode("utf-8", errors="replace")
    nm = _map_header(head, str(path))
    warned: set[str] = set()
    for number, raw in enumerate(lines[1:], start=2):
        if raw and not raw.startswith(b"#"):
            _map_add(nm, raw, str(path), number, warned)
    return nm


def timing_attribution(flow_dir: str | Path, paths: int | None = None) -> dict[str, Any]:
    """Attribute the critical path of an ``odin3-flow --timing-attr`` run to HDL statements,
    modules and words (docs/user/timing-attribution.md). ``flow_dir`` is the flow's output
    directory; ``paths`` keeps the first N setup paths of the VPR report (default: all). Returns
    the dict that ``timing/attribution.json`` holds (``schema_version`` 1). Raises
    ``RuntimeError`` (``timing_attr.AttributionError``) when an input is missing, unreadable or
    inconsistent (the flow was run without ``--timing-attr``, the VPR report is gone or cut off,
    an input is not valid JSON or text, the two BLIFs do not line up) or ``paths`` is below 1."""
    here = Path(__file__).resolve().parent
    for flow in (here / "data" / "tools" / "flow", here.parents[1] / "tools" / "flow"):
        if (flow / "timing_attr.py").is_file():
            if str(flow) not in sys.path:
                sys.path.insert(0, str(flow))
            report: dict[str, Any] = importlib.import_module("timing_attr").attribute_flow_dir(
                Path(flow_dir), paths
            )
            return report
    raise RuntimeError("timing_attr.py not found: the install has no tools/flow data")


def graphlet_counts(
    n: int,
    edges: Sequence[tuple[int, int]],
    directed: bool = False,
    size: int = 5,
    max_work: int = GRAPHLET_MAX_WORK,
) -> list[list[int]]:
    """Graphlet orbit counts of a plain edge list (see Odin3.graphlet_counts)."""
    return default().graphlet_counts(n, edges, directed, size, max_work)


def graphlet_compare(
    a: str | Path, b: str | Path, calibration: str | Path | None = None
) -> dict[str, Any]:
    """Compare two graphlet signatures (files written by the graphlets pass; `FILE#NAME` picks a
    group). Returns gdd_agreement and cosine_log, and the Bayesian posterior(s) when a
    calibration file is given (otherwise only the similarities: uncalibrated)."""
    import json
    import tempfile

    with tempfile.TemporaryDirectory() as tmp, Design() as design:
        out = Path(tmp) / "compare.json"
        words = ["--compare", _quote(a), _quote(b), "--out", _quote(out)]
        if calibration is not None:
            words += ["--calibration", _quote(calibration)]
        design.run_pass("graphlets", " ".join(words))
        result: dict[str, Any] = json.loads(out.read_text(encoding="utf-8"))
        return result
