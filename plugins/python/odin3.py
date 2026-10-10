"""cffi binding to the Odin III C ABI (include/odin3/odin3.h), with a thin Pythonic layer.

The cdef is generated at import time from the block between ODIN3_CDEF_BEGIN and
ODIN3_CDEF_END in odin3.h, so the binding never drifts from the header.

The shared library is located via $ODIN3_LIB, else build/{debug,release}/libodin3.so
in the repository.

The Pythonic layer (spec docs/specs/2026-10-09-1D-abi-design.md, "Python"):

    design = Design.read_blif("a.blif")
    for module in design.modules():
        for node in module.nodes():
            print(node.type_name, [pin.net() for pin in node.pins()], node.sources())

Objects (Module, Node, Pin, Net, Wire) are small value types naming an IR object by ID through
the design that owns it; they compare equal when they name the same object. Every string is
copied out of the library at once, so it outlives later mutations. IDs stay valid until a
`compact` pass renumbers a module (ABI conventions in odin3.h). A failing ABI call raises
Odin3Error carrying its odin3_status; the library has already logged why.
"""

from __future__ import annotations

import os
import re
from collections.abc import Iterator, Sequence
from contextlib import contextmanager
from dataclasses import dataclass, field
from pathlib import Path
from types import TracebackType
from typing import Any, ClassVar, NamedTuple, TypeVar, Union

import cffi

REPO_ROOT = Path(__file__).resolve().parents[2]
HEADER = REPO_ROOT / "include" / "odin3" / "odin3.h"


def cdef_from_header(header: Path = HEADER) -> str:
    """Return the cffi-parsable declarations from odin3.h, comments stripped."""
    text = header.read_text(encoding="utf-8")
    match = re.search(r"/\* ODIN3_CDEF_BEGIN \*/(.*)/\* ODIN3_CDEF_END \*/", text, re.DOTALL)
    if match is None:
        raise RuntimeError(f"{header}: ODIN3_CDEF markers not found")
    return re.sub(r"/\*.*?\*/", "", match.group(1), flags=re.DOTALL)


def find_library() -> Path:
    """Locate libodin3.so: $ODIN3_LIB, then the debug and release build trees."""
    env = os.environ.get("ODIN3_LIB")
    candidates = [Path(env)] if env else []
    candidates += [REPO_ROOT / "build" / p / "libodin3.so" for p in ("debug", "release")]
    for path in candidates:
        if path.is_file():
            return path
    raise FileNotFoundError("libodin3.so not found; build first or set ODIN3_LIB")


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

    @contextmanager
    def capture_log(self) -> Iterator[list[LogRecord]]:
        """Collect the log's messages (INFO and above) while the block runs; stderr afterwards."""
        records: list[LogRecord] = []
        handle = self.ffi.new_handle(records)
        old_level = self.lib.odin3_log_get_level()
        self.call("odin3_log_set_level", max(int(old_level), int(self.lib.ODIN3_LOG_INFO)))
        self.lib.odin3_log_set_sink(self._log_cb, handle)
        try:
            yield records
        finally:
            self.lib.odin3_log_set_sink(self.ffi.NULL, self.ffi.NULL)
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


class Design:
    """An odin3_design: create it empty, run passes on it, walk its modules."""

    def __init__(self, odin3: Odin3 | None = None) -> None:
        self.odin3 = odin3 or default()
        ptr = self.odin3.lib.odin3_design_create()
        if ptr == self.odin3.ffi.NULL:
            raise MemoryError("odin3_design_create")
        self.ptr: Any = self.odin3.ffi.gc(ptr, self.odin3.lib.odin3_design_destroy)

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

    def close(self) -> None:
        """Destroy the design now (else when it is garbage-collected)."""
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
        """Run one pass (odin3_design_run_pass); raises Odin3Error with its status."""
        self.call("odin3_design_run_pass", name.encode(), args.encode())

    def run_script(self, text: str, origin: str = "python") -> None:
        """Run a pass script, its errors located by command (odin3_design_run_script)."""
        keep = self.odin3.ffi.new("char[]", origin.encode())  # alive during the call
        src = (keep, self.odin3.lib.ODIN3_SCRIPT_BY_COMMAND)
        self.call("odin3_design_run_script", text.encode(), src)

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

    def params(self) -> dict[str, int | str]:
        """Parameter name -> value: an int for INT, else the ABI's text form."""
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
