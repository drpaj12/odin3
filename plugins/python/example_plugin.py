"""Smallest Python plugin: load libodin3 through the cffi binding and query it.

From Phase 1 a Python pass is a function taking a module handle and walking the
IR through the same binding. Run: python3 plugins/python/example_plugin.py
"""

from __future__ import annotations

import sys

from odin3 import Odin3


def main() -> int:
    odin3 = Odin3()
    print(f"example_plugin.py: libodin3 {odin3.version()} (ABI {odin3.abi_version()})")
    if odin3.status_string(0) != "ODIN3_OK":
        print("example_plugin.py: unexpected status string", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
