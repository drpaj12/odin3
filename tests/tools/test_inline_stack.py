"""A call inside a call costs no C stack (PR 2D-6 review I6, fixed in 2D-7).

A chain of 1000 functions, each called by the one before it inside an expression, elaborates under
a 2 MiB stack (``ulimit -s 2048``) with the default caps: the calls are inlined from an explicit
frame stack (src/frontends/elab/pg_call.c), not by C recursion. A function that calls itself is
still refused, with its located error. CTest ``python_tools`` runs this with ODIN3_CLI set to the
odin3 executable; without it the tests are skipped.
"""

from __future__ import annotations

import os
import resource
import subprocess
import tempfile
import unittest
from pathlib import Path

ODIN3 = os.environ.get("ODIN3_CLI", "")
STACK_BYTES = 2048 * 1024
CHAIN = 1000


def _limit_stack() -> None:
    resource.setrlimit(resource.RLIMIT_STACK, (STACK_BYTES, STACK_BYTES))


def _chain(length: int) -> str:
    lines = ["module t(input [7:0] a, output reg [7:0] y);"]
    for k in range(length, 0, -1):
        body = "x + 1" if k == length else f"f{k + 1}(x) + 1"
        lines.append(f"  function [7:0] f{k}(input [7:0] x); f{k} = {body}; endfunction")
    lines.append("  always @* y = f1(a);")
    lines.append("endmodule")
    return "\n".join(lines) + "\n"


def _run(source: str) -> subprocess.CompletedProcess[str]:
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "t.v"
        path.write_text(source, encoding="ascii")
        return subprocess.run(
            [ODIN3, "--elab-only", str(path)],
            capture_output=True,
            text=True,
            check=False,
            preexec_fn=_limit_stack,
        )


@unittest.skipUnless(ODIN3, "ODIN3_CLI not set (CTest python_tools)")
class InlineStackTests(unittest.TestCase):
    def test_thousand_deep_chain_elaborates_on_a_small_stack(self) -> None:
        run = _run(_chain(CHAIN))
        self.assertEqual(run.returncode, 0, run.stderr[-2000:])
        self.assertNotIn("error", run.stderr)

    def test_recursion_is_still_refused(self) -> None:
        source = (
            "module t(input [3:0] a, output reg [3:0] y);\n"
            "  function [3:0] f(input [3:0] n);\n"
            "    if (n == 0) f = 0; else f = f(n - 1);\n"
            "  endfunction\n"
            "  always @* y = f(a);\n"
            "endmodule\n"
        )
        run = _run(source)
        self.assertNotEqual(run.returncode, 0)
        self.assertIn("recursive call of 'f' cannot be inlined", run.stderr)


if __name__ == "__main__":
    unittest.main()
