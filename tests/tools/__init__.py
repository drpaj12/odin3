"""Tests for tools/netlist-compare, tools/equiv-check, tools/golden-sample, tools/golden-blackboxes,
tools/project-fixtures, tools/parse-check, tools/elab-check, tools/opt-check, tools/exit-check,
tools/proc-check, tools/heavy-run, tools/build-locked, tools/blif-vtr-check (the checker and the
fixture driver), tools/vtr-models, tools/vtr-qor, tools/vtr-read-check, tools/verilog-gen,
the BLIF reader and the
Python plugin binding (plugins/python).

The tools are stand-alone scripts (not installed packages); their directories are put on
sys.path here so the test modules can import them.
"""

import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
for _sub in (
    "tools/blif",
    "tools/blif-vtr-check",
    "tools/vtr-models",
    "tools/netlist-compare",
    "tools/equiv-check",
    "tools/golden-sample",
    "tools/golden-blackboxes",
    "tools/project-fixtures",
    "tools/pp-fuzz",
    "tools/pp-diff",
    "tools/parse-check",
    "tools/verilog-gen",
    "tools/token-cost",
    "tools/elab-check",
    "tools/opt-check",
    "tools/flatten-check",
    "tools/exit-check",
    "tools/proc-check",
    "tools/vtr-qor",
    "tools/vtr-read-check",
    "plugins/python",
):
    _path = str(REPO_ROOT / _sub)
    if _path not in sys.path:
        sys.path.insert(0, _path)
