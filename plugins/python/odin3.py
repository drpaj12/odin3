"""cffi binding to the Odin III C ABI (include/odin3/odin3.h).

The cdef is generated at import time from the block between ODIN3_CDEF_BEGIN and
ODIN3_CDEF_END in odin3.h, so the binding never drifts from the header.

The shared library is located via $ODIN3_LIB, else build/{debug,release}/libodin3.so
in the repository.
"""

from __future__ import annotations

import os
import re
from pathlib import Path
from typing import Any

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


class Odin3:
    """A loaded libodin3 with typed wrappers for the ABI functions in odin3.h."""

    def __init__(self, library: Path | None = None) -> None:
        self.ffi = cffi.FFI()
        self.ffi.cdef(cdef_from_header())
        self.lib: Any = self.ffi.dlopen(str(library or find_library()))
        if self.abi_version() != int(self.lib.ODIN3_ABI_VERSION):
            raise RuntimeError("libodin3 ABI version does not match odin3.h")

    def version(self) -> str:
        return str(self.ffi.string(self.lib.odin3_version_string()).decode())

    def abi_version(self) -> int:
        return int(self.lib.odin3_abi_version())

    def status_string(self, status: int) -> str:
        return str(self.ffi.string(self.lib.odin3_status_string(status)).decode())
