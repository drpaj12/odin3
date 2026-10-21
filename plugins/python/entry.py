"""Console entry points of the odin3 wheel (installed as odin3/_entry.py).

``odin3`` runs the bundled CLI (odin3/bin/odin3); ``odin3-flow`` runs the flow driver
(odin3/data/tools/flow/odin3_flow.py) on the bundled data (odin3/data/{lib,tools}).
"""

from __future__ import annotations

import importlib
import os
import sys
from pathlib import Path

PACKAGE_DIR = Path(__file__).resolve().parent
DATA = PACKAGE_DIR / "data"


def odin3_main() -> None:
    """Replace this process by the bundled odin3 CLI, same arguments."""
    cli = PACKAGE_DIR / "bin" / "odin3"
    os.execv(cli, [str(cli), *sys.argv[1:]])


def flow_main() -> int:
    """Run odin3_flow.main with the bundled flow and vtr-qor modules importable."""
    sys.path[:0] = [str(DATA / "tools" / "flow"), str(DATA / "tools" / "vtr-qor")]
    return int(importlib.import_module("odin3_flow").main())
