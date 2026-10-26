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
HEADER = (_BUNDLED_HEADER if _BUNDLED_HEADER.is_file()
          else REPO_ROOT / "include" / "odin3" / "odin3.h")
LOG = logging.getLogger("odin3")


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
    candidates += [(f"build/{p}", REPO_ROOT / "build" / p / "libodin3.so")
                   for p in ("debug", "release")]
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

    def abi_version(self) -> int:
        return int(self.lib.odin3_abi_version())

    def status_string(self, status: int) -> str:
        return str(self.ffi.string(self.lib.odin3_status_string(status)).decode())

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

    def __init__(self, odin3: Odin3 | None = None, *, handle: Any = None) -> None:
        """A new empty design; or, given handle (an odin3_design * that C code made with the same
        library, e.g. a host program or a test fixture), that design, which this object then owns
        and destroys. MemoryError when a new design cannot be created, ValueError for a NULL
        handle."""
        self.odin3 = odin3 or default()
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
        cls, path: str | Path, *, techlibs: Sequence[str | Path] = (), odin3: Odin3 | None = None
    ) -> Design:
        """A new design holding the BLIF at path, after reading the given tech libraries."""
        design = cls(odin3)
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

    def run_pass(self, name: str, args: str = "") -> None:
        """Run one pass (odin3_design_run_pass); raises Odin3Error with its status.

        After a failed pass the design is valid but may no longer compute what its input did:
        discard it (close it) rather than keep using or writing it.
        """
        self.call("odin3_design_run_pass", name.encode(), args.encode())

    def run_script(self, text: str, origin: str = "python") -> None:
        """Run a pass script, its errors located by command (odin3_design_run_script).

        After a failed pass the design must be discarded, as for run_pass.
        """
        keep = self.odin3.ffi.new("char[]", origin.encode())  # alive during the call
        src = (keep, self.odin3.lib.ODIN3_SCRIPT_BY_COMMAND)
        self.call("odin3_design_run_script", text.encode(), src)

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
        self.ptr: Any = self.odin3.ffi.gc(
            raw, _sim_destructor(self.odin3.lib, design._open, addr)
        )
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
            ident, text(raw.name), text(raw.leaf), int(raw.width),
            self.odin3.enum_name("odin3_simulation_kind", raw.kind),
            int(raw.msb), int(raw.lsb), int(raw.scope),
            (int(raw.obj.module), int(raw.obj.kind), int(raw.obj.id)), int(raw.first_bit),
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
            out.append(ScopeInfo(
                i, self.odin3.text(raw.name), self.odin3.text(raw.leaf), int(raw.parent),
                int(raw.module), (int(raw.inst.module), int(raw.inst.id)),
            ))
        return out

    def signal(self, name: str) -> SignalInfo | None:
        """The signal named `name` (cached), or None when the simulated netlist has no such name."""
        found = self._cache.get(name)
        if found is None:
            ident = int(self.odin3.get("odin3_simulation_find_signal", "uint32_t",
                                       self._live(), name.encode()))
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

    def chain(self) -> list[Construct]:
        """Where the object comes from in the HDL (odin3_prov_visit_chain): for each source leaf
        in the order of sources(), its construct and then its ancestors up to the module; each
        construct once, at its first reach. Empty when the design holds no construct table (read
        with --prov-ast off, or from a netlist) or the object has no provenance."""
        reached: list[tuple[Construct, int]] = []
        odin3 = self.design.odin3
        self.design.call(
            "odin3_prov_visit_chain", self.obj(), odin3._construct_cb, odin3.ffi.new_handle(reached)
        )
        return [construct for construct, _leaf in reached]

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
        """"parsed" or "elaborated"."""
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
                Path(flow_dir), paths)
            return report
    raise RuntimeError("timing_attr.py not found: the install has no tools/flow data")
