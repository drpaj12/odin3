"""Sphinx configuration of the Odin III user documentation (7.3, spec 2.2).

Build: ``sphinx-build -W -b html docs/user build/docs`` (CI job ``docs``). The pass reference is
generated from docs/PASSES.md and ``odin3 --help`` by _ext/passes_ref.py, so the build needs an
odin3 binary (``$ODIN3_CLI``, else build/release/odin3 or build/debug/odin3).
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "_ext"))

project = "Odin III"
author = "Odin III contributors"
copyright = "Odin III contributors"

extensions = ["myst_parser", "sphinxcontrib.mermaid", "passes_ref"]
source_suffix = {".md": "markdown"}
root_doc = "index"
exclude_patterns = ["_build", "_generated"]
myst_enable_extensions = ["colon_fence"]
myst_heading_anchors = 3
html_theme = "alabaster"

# Shared Yggdrasil III look: the "Manuals" block (hub link on top, links to every manual) leads
# the sidebar on every page. Keep _templates/manuals.html identical across the four manuals.
templates_path = ["_templates"]
html_sidebars = {
    "**": ["manuals.html", "about.html", "navigation.html", "relations.html", "searchbox.html"],
}
