"""The ``--elab`` profile of verilog-gen (2D spec 14.5, plan Task 28).

``generate(seed)`` writes one synthesisable module ``gtop`` (and a small ``gsub``) built to hit the
traps of 2D's elaboration against Yosys 0.55's: random-width, random-sign expression trees into
random-width targets, unsized parameters and literals, ``===`` against x patterns, ``#()``
overrides and ``defparam``, generate loops, conditionals and cases, ``always`` blocks that mix
``=`` and ``<=``, ``case``/``casez``/``casex``, loops, functions and tasks, and memories whose
low index is not zero. Every register starts at a constant (an uninitialised register reads x in
one tool and 0 in the other: a false NOT-equivalent) and every combinational output is assigned
at the top of its block. The first line of the file lists the features it used
(``// features: a, b``); each is a counted production, and ``test_elab_diff`` asserts that each
appears in seeds 1-50. The output is a function of the seed alone.

The shape is the PR 2D-6 review's random ``always`` generator (three blocks: a clocked one that
owns ``y0``, ``y1`` and the memory, a combinational one that owns ``y2`` and ``y3``, and a second
clocked one that owns ``y4``), grown by the expression trees and the generate/instance forms.
"""

from __future__ import annotations

import random
import re
from dataclasses import dataclass

# Each counted production and the text that shows the generator emitted it.
FEATURE_PATTERNS: dict[str, str] = {
    "expr_tree": r"assign e0 =",
    "signed_mix": r"\$signed|\$unsigned|\bsa\b|\bsc\b",
    "unsized_param": r"parameter P = \d+;",
    "unsized_literal": r"\b(?:P|LQ) \* \d|\(-\d+\)|~0|[-+] \d+\b",
    "case_eqx": r"[!=]==\s*\d+'b[01]*[xz?]",
    "param_override": r"gsub #\(",
    "defparam": r"\bdefparam\b",
    "gen_for": r"genvar gi;[^\n]*\n\s*for ",
    "gen_if": r"\bif \(P ",
    "gen_case": r"\bcase \(P\)",
    "mixed_assign": r"\bt0 = ",
    "case": r"\bcase \((?!P)",
    "casez": r"\bcasez \(",
    "casex": r"\bcasex \(",
    "for_loop": r"\bfor \(i\d",
    "function": r"\bf[123]\(",
    "task": r"\btk\(",
    "mem_low": r"reg \[3:0\] mem \[[1-9]\d*:\d+\]|reg \[3:0\] mem \[\d+:[1-9]\d*\]",
}
FEATURES = tuple(FEATURE_PATTERNS)

SCALARS = ("e", "f")
MAX_WIDTH = 16  # widest target of an expression tree
TREE_DEPTH = 3
BLOCK_DEPTH = 2
COMPARE_OPS = ("==", "!=", "<", "<=", ">", ">=")
BIT_OPS = ("&", "|", "^", "~^")
ARITH_OPS = ("+", "-", "*")
SHIFT_OPS = ("<<", ">>", "<<<", ">>>")
REDUCE_OPS = ("&", "|", "^", "~&", "~|", "~^")
SIGNED_LEAVES = ("sa", "sc")
UNSIGNED_LEAVES = ("a", "b", "c", "s", "e", "f")

FUNCS = """
  function [7:0] f1(input [7:0] x, input [7:0] z);
    begin f1 = x ^ (z + 8'd1); end
  endfunction
  function [7:0] f2(input [7:0] x, input [7:0] z);
    begin
      if (x[0]) f2 = x & z; else f2 = x | z;
      case (z[1:0]) 2'd0: f2 = f2 + 8'd3; 2'd2: f2 = ~f2; default: ; endcase
    end
  endfunction
  function [7:0] f3(input [7:0] x, input [7:0] z);
    integer j;
    begin f3 = z; for (j = 0; j < 3; j = j + 1) f3 = f3 + (x >> j); end
  endfunction
  task tk(input [7:0] x, output [7:0] z);
    begin z = ~x; if (x[1]) z = z + 8'd1; end
  endtask
"""

SUB = """module gsub #(parameter W = 4, parameter K = 1) (input [W-1:0] x, output [W-1:0] y);
  assign y = x + K;
endmodule
"""


@dataclass
class Generated:
    """One generated file: its text and the counted features it used."""

    text: str
    features: tuple[str, ...]


class Gen:
    """One seed's module."""

    def __init__(self, seed: int) -> None:
        self.r = random.Random(seed)
        self.p_value = self.r.randrange(1, 8)
        self.mem_lo = self.r.choice((1, 2, 3, 5))
        self.mem_desc = self.r.random() < 0.3

    # -- helpers --
    def ch(self, items: tuple[str, ...] | list[str]) -> str:
        return self.r.choice(items)

    def p(self, prob: float) -> bool:
        return self.r.random() < prob

    # -- expression trees (random widths and signs) --
    def literal(self) -> str:
        kind = self.r.randrange(7)
        width = self.r.randrange(1, 13)
        if kind == 0:
            return f"{width}'d{self.r.randrange(1 << width)}"
        if kind == 1:
            return f"{width}'sd{self.r.randrange(1 << (width - 1)) if width > 1 else 0}"
        if kind == 2:
            return str(self.r.randrange(0, 40))
        if kind == 3:
            return "(-" + str(self.r.randrange(1, 9)) + ")"
        if kind == 4:
            return self.ch(("P", "LQ", "(P * 3)", "(LQ - 1)"))
        if kind == 5:
            return "(~0)"
        return f"{width}'h{self.r.randrange(1 << width):x}"

    def leaf(self) -> str:
        k = self.r.random()
        if k < 0.45:
            return self.ch(UNSIGNED_LEAVES)
        if k < 0.65:
            return self.ch(SIGNED_LEAVES)
        if k < 0.80:
            return self.literal()
        if k < 0.90:
            return self.ch(("a[5:2]", "b[7:3]", "c[3:1]", "sa[6:1]", "sa[3:0]", "sc[2:0]"))
        return self.ch(("a[c[2:0]]", "sa[s]", "c[s]", "b[2]", "sa[7]"))

    def tree(self, depth: int) -> str:
        if depth <= 0 or self.p(0.2):
            return self.leaf()
        k = self.r.random()
        if k < 0.34:
            op = self.ch(ARITH_OPS + BIT_OPS)
            return f"({self.tree(depth - 1)} {op} {self.tree(depth - 1)})"
        if k < 0.48:
            op = self.ch(COMPARE_OPS + ("&&", "||"))
            return f"({self.tree(depth - 1)} {op} {self.tree(depth - 1)})"
        if k < 0.58:
            amount = self.ch(("c", "s", str(self.r.randrange(0, 5)), "e"))
            return f"({self.tree(depth - 1)} {self.ch(SHIFT_OPS)} {amount})"
        if k < 0.68:
            return self.unary(depth)
        if k < 0.77:
            return f"({self.tree(depth - 1)} ? {self.tree(depth - 1)} : {self.tree(depth - 1)})"
        if k < 0.85:
            return self.concat(depth)
        if k < 0.93:
            return f"{self.ch(('$signed', '$unsigned'))}({self.tree(depth - 1)})"
        return f"(~{self.tree(depth - 1)})"

    def unary(self, depth: int) -> str:
        sub = self.tree(depth - 1)
        k = self.r.random()
        if k < 0.3:
            return f"(-{sub})"
        if k < 0.45:
            return f"(!{sub})"
        return f"({self.ch(REDUCE_OPS)}{sub})"

    def concat(self, depth: int) -> str:
        if self.p(0.3):
            return f"{{{self.r.randrange(1, 4)}{{{self.tree(depth - 1)}}}}}"
        return f"{{{self.tree(depth - 1)}, {self.tree(depth - 1)}}}"

    def outputs(self) -> tuple[list[str], list[str]]:
        """The port lines and the assigns of the expression-tree outputs e0..eN."""
        ports: list[str] = []
        assigns: list[str] = []
        for idx in range(self.r.randrange(2, 5)):
            width = self.r.randrange(1, MAX_WIDTH + 1)
            ports.append(f"output [{width - 1}:0] e{idx}")
            assigns.append(f"  assign e{idx} = {self.tree(TREE_DEPTH)};")
        return ports, assigns

    # -- ===, parameters, instances, generate --
    def eqx_assigns(self) -> list[str]:
        pat4 = "".join(self.r.choice("01xz?") for _ in range(4))
        pat8 = "".join(self.r.choice("01x") for _ in range(8))
        pat4b = "".join(self.r.choice("01xz") for _ in range(4))
        return [
            f"  assign q1 = (c === 4'b{pat4});",
            f"  assign q2 = (a !== 8'b{pat8}) & (c[1:0] === 2'b{pat4b[:2]});",
            f"  assign q3 = (sa === 8'bxxxxxxxx) | (b == {self.literal()});",
        ]

    def params_and_instances(self) -> list[str]:
        k_value = self.r.randrange(1, 6)
        lines = [
            f"  parameter P = {self.p_value};",
            f"  localparam LQ = P * {self.r.randrange(2, 5)} - 1;",
            "  wire [7:0] sy0;",
            "  wire [3:0] sy1;",
            "  wire [3:0] sy2;",
            "  gsub #(.W(8), .K(P)) u0 (.x(a), .y(sy0));",
            f"  gsub #(4, {k_value}) u1 (.x(c), .y(sy1));",
            "  gsub u2 (.x(c), .y(sy2));",
            f"  defparam u2.K = {self.r.randrange(1, 9)};",
            "  assign zs = {sy0, sy1, sy2};",
        ]
        return lines

    def generate_blocks(self) -> list[str]:
        loops = self.r.randrange(2, 9)
        return [
            f"  wire [{loops - 1}:0] gw;",
            "  wire [7:0] gz;",
            "  wire [7:0] gc;",
            "  generate",
            "  genvar gi;",
            f"  for (gi = 0; gi < {loops}; gi = gi + 1) begin : gl",
            "    assign gw[gi] = a[gi] ^ b[7 - gi];",
            "  end",
            f"  if (P > {self.r.randrange(2, 6)}) begin : gi1",
            f"    assign gz = a + b + {self.r.randrange(4)};",
            "  end else begin : gi2",
            "    assign gz = a - b;",
            "  end",
            "  case (P)",
            f"    {self.r.randrange(1, 4)}: begin : gc1 assign gc = a & b; end",
            f"    {self.r.randrange(4, 8)}: begin : gc2 assign gc = a | b; end",
            "    default: begin : gc3 assign gc = a ^ b; end",
            "  endcase",
            "  endgenerate",
            "  assign zg = (8'd0 | gw) ^ gz ^ gc;",
        ]

    # -- always blocks (the PR 2D-6 review's generator) --
    def idx2(self) -> str:
        """A 2-bit index expression."""
        return self.ch(("s", "a[1:0]", "c[1:0]"))

    def mem_index(self, ctx: dict[str, object]) -> str:
        """An address of mem inside its range [lo, lo+3]."""
        loop = ctx.get("memvar")
        if loop is not None and self.p(0.5):
            return str(loop)
        if self.p(0.3):
            return str(self.mem_lo + self.r.randrange(4))
        return f"({{2'b0, {self.idx2()}}} + 4'd{self.mem_lo})"

    def leaf8(self, scope: dict[str, list[str]]) -> str:
        k = self.r.random()
        regs = scope["rd"]
        if k < 0.35:
            return self.ch(("a", "b"))
        if k < 0.5 and regs:
            return self.ch(regs)
        if k < 0.6:
            return f"8'd{self.r.randrange(256)}"
        if k < 0.7:
            return "{4'b0, c}"
        if k < 0.78:
            return "a[3:0]"
        if k < 0.85:
            return f"{{7'b0, {self.ch(SCALARS)}}}"
        if k < 0.93 and scope.get("mem"):
            return f"{{4'b0, mem[{self.mem_index({})}]}}"
        return self.ch(("a", "b"))

    def expr8(self, depth: int, scope: dict[str, list[str]]) -> str:
        if depth <= 0 or self.p(0.25):
            return self.leaf8(scope)
        k = self.r.random()

        def sub() -> str:
            return self.expr8(depth - 1, scope)

        if k < 0.45:
            return f"({sub()} {self.ch(('+', '-', '&', '|', '^'))} {sub()})"
        if k < 0.55:
            return f"(~{sub()})"
        if k < 0.65:
            cond = self.ch(("e", "f", "s[0]", "(a > b)"))
            return f"({cond} ? {sub()} : {sub()})"
        if k < 0.70:
            return f"({sub()} << {self.r.randrange(1, 3)})"
        if k < 0.75:
            return f"({sub()} >> {self.r.randrange(1, 3)})"
        if k < 0.90 and scope.get("fn"):
            return f"{self.ch(scope['fn'])}({sub()}, {sub()})"
        return f"({sub()} * 2)"

    def lvalue(self, own: list[str], ctx: dict[str, object]) -> str:
        # Yosys 0.55 refuses a block that writes a variable (non-constant) part of a register and
        # also a concatenation holding it ("incompatible mix of lookahead and non-lookahead
        # IDs in LHS expression"): a block does one or the other.
        if ctx["concat"]:
            kinds = ["whole", "whole", "cbit", "cpart", "concat", "concat"]
        else:
            kinds = ["whole", "whole", "cbit", "vbit", "cpart", "ipart", "ipartm"]
        if ctx.get("mem"):
            kinds += ["memw", "memw", "memp", "memc"]
        kind = self.ch(kinds)
        var = self.ch(own)
        if kind == "whole":
            return var
        if kind == "cbit":
            return f"{var}[{self.r.randrange(8)}]"
        if kind == "vbit":
            return f"{var}[{self.ch(('s', 'c', 'a[2:0]', 'b[3:1]'))}]"
        if kind == "cpart":
            hi = self.r.randrange(1, 8)
            return f"{var}[{hi}:{self.r.randrange(0, hi)}]"
        if kind == "ipart":
            return f"{var}[{self.ch(('s', 'c', 'a[2:0]', '2'))} +: {self.r.randrange(1, 4)}]"
        if kind == "ipartm":
            base = self.ch(("{1'b1,s}", "(c|4'd4)", "5"))
            return f"{var}[{base} -: {self.r.randrange(1, 4)}]"
        if kind == "concat" and len(own) >= 2:
            first, second = self.r.sample(own, 2)
            return f"{{{first}, {second}}}"
        return self.mem_lvalue(kind, var, ctx)

    def mem_lvalue(self, kind: str, var: str, ctx: dict[str, object]) -> str:
        if kind == "memw":
            return f"mem[{self.mem_index(ctx)}]"
        if kind == "memp":
            return f"mem[{self.mem_index(ctx)}][{self.ch(('0', '2', '3'))}]"
        if kind == "memc":
            return f"mem[{self.mem_index(ctx)}][{self.ch(('3', '2'))}:{self.ch(('0', '1'))}]"
        return var

    def stmt(self, depth: int, ctx: dict[str, object], ind: int) -> str:
        space = " " * ind
        kinds = ["assign"] * 4
        if depth > 0:
            kinds += ["if", "if", "case", "case", "for"]
        if ctx.get("tasks"):
            kinds += ["task"]
        if ctx["seq"] and ctx.get("temps"):
            kinds += ["temp"] * 2
        kind = self.ch(kinds)
        scope: dict[str, list[str]] = ctx["scope"]  # type: ignore[assignment]
        own: list[str] = ctx["own"]  # type: ignore[assignment]
        if kind == "assign":
            op = "<=" if ctx["seq"] else "="
            return f"{space}{self.lvalue(own, ctx)} {op} {self.expr8(2, scope)};"
        if kind == "temp":
            temps: list[str] = ctx["temps"]  # type: ignore[assignment]
            return f"{space}{self.ch(temps)} = {self.expr8(2, scope)};"
        if kind == "task":
            return f"{space}tk({self.expr8(1, scope)}, {self.ch(own)});"
        if kind == "if":
            return self.if_stmt(depth, ctx, ind)
        if kind == "case":
            return self.case_stmt(depth, ctx, ind)
        return self.for_stmt(ctx, ind)

    def block(self, depth: int, ctx: dict[str, object], ind: int) -> str:
        body = [self.stmt(depth, ctx, ind + 2) for _ in range(self.r.randrange(1, 4))]
        return "begin\n" + "\n".join(body) + "\n" + " " * ind + "end"

    def if_stmt(self, depth: int, ctx: dict[str, object], ind: int) -> str:
        cond = self.ch(("e", "f", "s[0]", "(a > b)", "(s == 2'd2)", "(c[2] & e)"))
        text = f"{' ' * ind}if ({cond}) {self.block(depth - 1, ctx, ind)}"
        if self.p(0.6):
            text += f"\n{' ' * ind}else {self.block(depth - 1, ctx, ind)}"
        return text

    def case_item(self, kind: str, value: int, depth: int, ctx: dict[str, object], ind: int) -> str:
        pad = " " * (ind + 2)
        body = self.stmt(depth - 1, ctx, 0)
        if kind != "case" and self.p(0.3):
            pattern = self.ch(("2'b1?", "2'b?1", "2'b0?", "2'b??"))
            return f"{pad}{pattern}: {body}"
        if self.p(0.1):
            return f"{pad}2'd{value}, 2'd{(value + 1) % 4}: {body}"
        return f"{pad}2'd{value}: {body}"

    def case_stmt(self, depth: int, ctx: dict[str, object], ind: int) -> str:
        kind = self.ch(("case", "case", "casez", "casex"))
        sel = self.ch(("s", "c[1:0]", "a[1:0]"))
        items: list[str] = []
        used: set[int] = set()
        for _ in range(self.r.randrange(1, 4)):
            value = self.r.randrange(4)
            if value not in used:
                used.add(value)
                items.append(self.case_item(kind, value, depth, ctx, ind))
        if self.p(0.6):
            default = f"{' ' * (ind + 2)}default: {self.stmt(depth - 1, ctx, 0)}"
            items.insert(self.r.randrange(len(items) + 1) if self.p(0.3) else len(items), default)
        attr = ""
        if self.p(0.12) and not ctx["seq"]:
            attr = self.ch(("(* full_case *) ", "(* parallel_case *) ",
                            "(* full_case, parallel_case *) "))
        return f"{' ' * ind}{attr}{kind} ({sel})\n" + "\n".join(items) + f"\n{' ' * ind}endcase"

    def for_stmt(self, ctx: dict[str, object], ind: int) -> str:
        var = str(ctx["iv"])
        scope: dict[str, list[str]] = ctx["scope"]  # type: ignore[assignment]
        own: list[str] = ctx["own"]  # type: ignore[assignment]
        count = self.r.randrange(1, 5)
        use_mem = bool(ctx.get("mem")) and self.p(0.4)
        sub = dict(ctx)
        if use_mem:
            sub["memvar"] = var
        body = [self.loop_body(sub, own, scope, var, use_mem, ind + 2)
                for _ in range(self.r.randrange(1, 3))]
        space = " " * ind
        if use_mem:
            lo = self.mem_lo
            head = f"for ({var} = {lo}; {var} < {lo + count}; {var} = {var} + 1)"
        else:
            head = self.ch((
                f"for ({var} = 0; {var} < {count}; {var} = {var} + 1)",
                f"for ({var} = 1; {var} <= {count}; {var} = {var} + 1)",
                f"for ({var} = {count}; {var} >= 0; {var} = {var} - 1)",
            ))
        return f"{space}{head} begin\n" + "\n".join(body) + f"\n{space}end"

    def loop_body(self, ctx: dict[str, object], own: list[str], scope: dict[str, list[str]],
                  var: str, use_mem: bool, ind: int) -> str:
        space = " " * ind
        if not self.p(0.6):
            return self.stmt(0, ctx, ind)
        op = "<=" if ctx["seq"] else "="
        target = self.ch(own)
        if use_mem:
            return f"{space}mem[{var}] {op} {self.expr8(1, scope)};"
        lval = self.ch((f"{target}[{var}]", f"{target}[{var}[2:0]]", f"{target}[{var}[1:0] +: 2]"))
        rhs = self.ch((f"a[{var}[2:0]]", f"(b[{var}[2:0]] ^ e)", self.expr8(1, scope),
                       f"{var}", f"(a >> {var})"))
        return f"{space}{lval} {op} {rhs};"

    # -- the module --
    def always_blocks(self) -> list[str]:
        fns = ["f1", "f2", "f3"]
        sc0 = {"rd": ["y0", "y1", "y2", "t0"], "fn": fns, "mem": ["mem"]}
        ctx0: dict[str, object] = {"seq": True, "own": ["y0", "y1"], "temps": ["t0"],
                                   "scope": sc0, "iv": "i0", "mem": True, "tasks": False,
                                   "concat": self.p(0.5)}
        pre = ["    y0 <= y0; y1 <= y1;", f"    if (e) mem[{self.mem_lo + 1}] <= b[3:0];"]
        if self.p(0.7):
            pre.append(f"    t0 = {self.expr8(1, sc0)};")
        out = ["  always @(posedge clk) begin"] + pre
        out += [self.stmt(BLOCK_DEPTH, ctx0, 4) for _ in range(self.r.randrange(1, 4))]
        out.append("  end")
        sc1 = {"rd": ["y0", "y1", "t1"], "fn": fns, "mem": ["mem"]}
        ctx1: dict[str, object] = {"seq": False, "own": ["y2", "y3"], "scope": sc1,
                                   "iv": "i1", "mem": False, "tasks": True,
                                   "concat": self.p(0.5)}
        sens = self.ch(("*", "*", "a or b or c or s or e or f or y0 or y1"))
        guard = self.ch(("y2 = 8'd0; y3 = 8'd0;", "y2 = y0; y3 = b;"))
        out += [f"  always @({sens}) begin", f"    {guard}", "    t1 = 8'd0;"]
        out += [self.stmt(BLOCK_DEPTH, ctx1, 4) for _ in range(self.r.randrange(1, 4))]
        out.append("  end")
        sc2 = {"rd": ["y0", "y2", "y4"], "fn": fns}
        ctx2: dict[str, object] = {"seq": True, "own": ["y4"], "scope": sc2, "iv": "i2",
                                   "mem": False, "tasks": False, "concat": False}
        out += ["  always @(posedge clk) begin", "    y4 <= y4;"]
        out += [self.stmt(BLOCK_DEPTH, ctx2, 4) for _ in range(self.r.randrange(1, 3))]
        out.append("  end")
        return out

    def module(self) -> tuple[str, str]:
        """The header and the body (without the functions) of gtop."""
        eports, eassigns = self.outputs()
        hi, lo = self.mem_lo + 3, self.mem_lo
        mem_range = f"{lo}:{hi}" if not self.mem_desc else f"{hi}:{lo}"
        header = [
            "module gtop(input clk, input [7:0] a, input [7:0] b, input [3:0] c, input [1:0] s,",
            "  input e, input f, input signed [7:0] sa, input signed [3:0] sc,",
            "  output reg [7:0] y0 = 8'h3c, output reg [7:0] y1 = 8'ha5, output reg [7:0] y2,",
            "  output reg [7:0] y3, output reg [7:0] y4 = 8'h5a, output [3:0] q, output [7:0] z,",
            "  output q1, output q2, output q3, output [15:0] zs, output [7:0] zg,",
            "  " + ",\n  ".join(eports) + ");",
        ]
        body = [
            f"  reg [3:0] mem [{mem_range}];",
            "  reg [7:0] t0 = 8'h11, t1;",
            f"  integer ii; initial for (ii = {lo}; ii <= {hi}; ii = ii + 1) mem[ii] = ii + 1;",
            "  integer i0, i1, i2;",
        ]
        body += self.params_and_instances() + self.generate_blocks()
        body += self.eqx_assigns() + eassigns + self.always_blocks()
        zfun = self.ch(("f1(a, b)", "f2(a, b)", "f3(a, b)", "f1(f2(a, b), y0)"))
        body += [f"  assign q = mem[{{2'b0, s}} + 4'd{lo}];", f"  assign z = {zfun};"]
        return "\n".join(header) + "\n", "\n".join(body) + "\n"


def generate(seed: int) -> Generated:
    """The module for seed and the counted features it used."""
    header, body = Gen(seed).module()
    used = tuple(name for name, pat in FEATURE_PATTERNS.items() if re.search(pat, body))
    text = f"// features: {', '.join(used)}\n{SUB}{header}{FUNCS}{body}endmodule\n"
    return Generated(text, used)
