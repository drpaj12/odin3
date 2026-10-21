"""write_blif --vtr keeps every primary output and hard block through VTR's ABC step.

A primary output driven straight by a hard block's output pin is dropped by VTR's ABC script
(strash; ifraig; scorr; dc2; dch -f; if -K 6; mfs2; write_hie) together with the hard blocks
behind it. The writer buffers such an output with a `.names`. The test needs the odin3 binary
(ODIN3_CLI) and VTR's own abc (VTR_ROOT/build/abc/abc); it is skipped without them.
"""

from __future__ import annotations

import os
import subprocess
import sys
import unittest
from pathlib import Path

from . import helpers

DEFAULT_VTR_ROOT = Path.home() / "odin3-ws" / "external" / "vtr-verilog-to-routing"
SCRIPT = (
    "read in.blif; strash; ifraig; scorr; dc2; dch -f; if -K 6; mfs2; "
    "write_hie in.blif out.blif"
)
SOURCE = """.model top
.inputs c d0 d1 a w
.outputs o1 o2 r
.subckt single_port_ram clk=c data[0]=d0 addr[0]=a we=w out[0]=o1
.subckt single_port_ram clk=c data[0]=d1 addr[0]=a we=w out[0]=o2
.names a w r
11 1
.end

.model single_port_ram
.inputs clk data[0] addr[0] we
.outputs out[0]
.blackbox
.end
"""


def abc_binary() -> Path | None:
    root = Path(os.environ.get("VTR_ROOT", str(DEFAULT_VTR_ROOT)))
    path = root / "build" / "abc" / "abc"
    return path if path.is_file() else None


def top_outputs(text: str) -> list[str]:
    """The names on the first `.outputs` line (with its continuations) of the file."""
    joined = text.replace("\\\n", " ")
    for line in joined.splitlines():
        if line.startswith(".outputs"):
            return line.split()[1:]
    return []


class VtrAbcHardBlocks(unittest.TestCase):
    def setUp(self) -> None:
        self.odin3 = os.environ.get("ODIN3_CLI")
        self.abc = abc_binary()
        if not self.odin3 or self.abc is None:
            why = "needs ODIN3_CLI and VTR's abc (build/abc/abc under VTR_ROOT)"
            if os.environ.get("ODIN3_REQUIRE_VTR_ABC") == "1":
                self.fail(f"vtr_abc_hardblocks: {why}; ODIN3_REQUIRE_VTR_ABC=1 forbids a skip")
            print(f"SKIP vtr_abc_hardblocks: {why}", file=sys.stderr)
            self.skipTest(why)
        self.work = helpers.scratch(self, "vtr-abc-hb-")

    def run_abc(self, blif: str) -> str:
        assert self.abc is not None
        (self.work / "in.blif").write_text(blif)
        done = subprocess.run(
            [str(self.abc), "-c", SCRIPT], cwd=self.work, capture_output=True, check=False,
            timeout=120,
        )
        self.assertEqual(done.returncode, 0, done.stdout.decode(errors="replace")[-300:])
        return (self.work / "out.blif").read_text()

    def test_odin3_blif_keeps_outputs_and_rams(self) -> None:
        assert self.odin3 is not None
        (self.work / "src.blif").write_text(SOURCE)
        script = (
            f'read_techlib "{helpers.REPO_ROOT / "lib" / "vtr.o3lib"}"; read_blif "src.blif"; '
            'write_blif --vtr "written.blif"'
        )
        done = subprocess.run(
            [self.odin3, "-p", script], cwd=self.work, capture_output=True, check=False,
            timeout=120,
        )
        self.assertEqual(done.returncode, 0, done.stderr.decode(errors="replace")[-300:])
        written = (self.work / "written.blif").read_text()
        after = self.run_abc(written)
        self.assertEqual(sorted(top_outputs(after)), ["o1", "o2", "r"])
        self.assertEqual(after.count(".subckt single_port_ram"), 2)

    def test_unbuffered_blif_loses_them(self) -> None:
        """The failure the buffer prevents: the same netlist with the RAMs on the outputs."""
        after = self.run_abc(SOURCE)
        lost = len(top_outputs(after)) < 3 or after.count(".subckt single_port_ram") < 2
        self.assertTrue(lost, "VTR's ABC no longer drops unbuffered hard-block outputs")


if __name__ == "__main__":
    unittest.main()
