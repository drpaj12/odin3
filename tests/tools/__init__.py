"""Tests for tools/netlist-compare, tools/equiv-check and the shared BLIF reader.

The tools are stand-alone scripts (not installed packages); their directories are put on
sys.path here so the test modules can import them.
"""

import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
for _sub in ("tools/blif", "tools/netlist-compare", "tools/equiv-check"):
    _path = str(REPO_ROOT / _sub)
    if _path not in sys.path:
        sys.path.insert(0, _path)
