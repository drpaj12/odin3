"""The ``--opt`` profile of verilog-gen (2F spec 20.4, plan Task 18).

``generate(seed)`` writes one small synthesisable module for the ``opt`` differential
(``tools/opt-check/opt-gen-diff``): word-level expressions and processes made of what ``opt`` has
rules for. It is a weighting of the same kind as ``--proc``: each production is a counted feature,
``Profile.features`` names the ones a seed used, and the first line of the file lists them
(``// features: ...``):

- ``const_operand``: a constant operand (``a + 4'd3``, ``a & 4'hF``, ``a | 4'd0``);
- ``x_operand``: an ``x`` bit in a constant operand (``a & 4'b1x01``, ``s == 2'bx1``);
- ``case_default`` / ``case_no_default``: a ``case`` with a ``default`` item, and one whose items
  cover every value of the selector so that none is needed (every target is assigned before the
  ``case`` either way: no latch, no path left to ``--nolatches``);
- ``repeated_subexpr``: one expression used in two places (``opt_merge``'s food);
- ``enable_nest`` / ``reset_nest``: a register behind ``if (en)`` and behind a synchronous or
  asynchronous reset (``opt_dff``'s ``$dffe``/``$sdff``/``$adff`` inference), nested one in
  the other;
- ``const_shift``: a shift by a constant amount (``a << 2``, ``a >> 1``, ``a >>> 1``);
- ``pow2_mul``: a multiply by a power of two (``a * 4``, ``8 * b``).

The module is always ``t``, with inputs ``clk rst en`` (one bit), ``s`` (two bits) and
``a b c d`` (four bits); every output is driven, so nothing needs a latch. The output is a function
of the seed alone.
"""

from __future__ import annotations

import random
from dataclasses import dataclass

# Every counted production; test_opt_gen_diff asserts each appears in seeds 1-50.
FEATURES = (
    "const_operand", "x_operand", "case_default", "case_no_default", "repeated_subexpr",
    "enable_nest", "reset_nest", "const_shift", "pow2_mul",
)
WIDTH = 4
SEL_BITS = 2
INPUTS = ("a", "b", "c", "d")
BINARY_OPS = ("&", "|", "^", "+", "-")
SHIFTS = ("<<", ">>", ">>>")
POW2 = (2, 4, 8)
MAX_SHIFT = 3
WEIGHT_CONST = 0.2
WEIGHT_X = 0.1
WEIGHT_SHIFT = 0.12
WEIGHT_MUL = 0.12
WEIGHT_REPEAT = 0.2
MAX_DEPTH = 2


@dataclass(frozen=True)
class Profile:
    text: str
    features: frozenset[str]


class _Module:
    def __init__(self, seed: int) -> None:
        self.rng = random.Random(seed)
        self.features: set[str] = set()
        self.readable = list(INPUTS)
        self.body: list[str] = []
        self.outputs: list[str] = []
        self.regs: list[str] = []
        self.count = 0
        self.used: list[str] = []  # compound expressions drawn so far, for repeated_subexpr

    def chance(self, p: float) -> bool:
        return self.rng.random() < p

    def fresh(self, prefix: str) -> str:
        self.count += 1
        return f"{prefix}{self.count}"

    def const(self, with_x: bool = False) -> str:
        bits = [self.rng.choice("01") for _ in range(WIDTH)]
        if with_x:
            bits[self.rng.randrange(WIDTH)] = "x"
            self.features.add("x_operand")
            return f"{WIDTH}'b{''.join(bits)}"
        self.features.add("const_operand")
        form = self.rng.random()
        if form < 0.3:
            return f"{WIDTH}'d{self.rng.choice((0, 1, 15))}"  # neutral and absorbing values
        return f"{WIDTH}'d{self.rng.randrange(1 << WIDTH)}"

    def operand(self) -> str:
        roll = self.rng.random()
        if roll < WEIGHT_X:
            return self.const(with_x=True)
        if roll < WEIGHT_X + WEIGHT_CONST:
            return self.const()
        return self.rng.choice(self.readable)

    def expr(self, depth: int = 0) -> str:
        if self.used and self.chance(WEIGHT_REPEAT):
            self.features.add("repeated_subexpr")
            return self.rng.choice(self.used)
        text = self.draw(depth)
        if text[0] == "(":
            self.used.append(text)
        return text

    def draw(self, depth: int) -> str:
        roll = self.rng.random()
        if depth >= MAX_DEPTH or roll < 0.25:
            return self.operand()
        sub = self.expr(depth + 1)
        if roll < 0.25 + WEIGHT_SHIFT:
            self.features.add("const_shift")
            return f"({sub} {self.rng.choice(SHIFTS)} {self.rng.randint(1, MAX_SHIFT)})"
        if roll < 0.25 + WEIGHT_SHIFT + WEIGHT_MUL:
            self.features.add("pow2_mul")
            factor = self.rng.choice(POW2)
            return f"({sub} * {factor})" if self.chance(0.5) else f"({factor} * {sub})"
        if roll < 0.7:
            return f"({sub} {self.rng.choice(BINARY_OPS)} {self.expr(depth + 1)})"
        if roll < 0.8:
            return f"(~{sub})"
        return f"({self.cond()} ? {sub} : {self.expr(depth + 1)})"

    def cond(self) -> str:
        roll = self.rng.random()
        a, b = self.rng.choice(self.readable), self.rng.choice(self.readable)
        if roll < 0.3:
            return f"s == {SEL_BITS}'d{self.rng.randrange(1 << SEL_BITS)}"
        if roll < 0.4:
            self.features.add("x_operand")
            return f"s == {SEL_BITS}'b{self.rng.choice('01')}x"
        if roll < 0.6:
            return f"{a} == {b}"
        if roll < 0.75:
            return f"{a} < {b}"
        if roll < 0.85:
            return "en"
        return f"|{a}"

    # -- processes --

    def assign(self) -> None:
        name = self.fresh("w")
        self.body.append(f"  assign {name} = {self.expr()};")
        self.outputs.append(name)
        self.readable.append(name)

    def comb_case(self) -> None:
        """``always @*``: the target is assigned first, then a ``case`` over ``s``."""
        name = self.fresh("y")
        total = self.chance(0.5)
        self.features.add("case_default" if not total else "case_no_default")
        lines = ["  always @* begin", f"    {name} = {self.expr()};", "    case (s)"]
        values = list(range(1 << SEL_BITS))
        if not total:
            values = values[: self.rng.randint(1, len(values) - 1)]
        for value in values:
            lines.append(f"      {SEL_BITS}'d{value}: {name} = {self.expr()};")
        if not total:
            lines.append(f"      default: {name} = {self.expr()};")
        lines += ["    endcase", "  end"]
        self.body += lines
        self.outputs.append(name)
        self.regs.append(name)
        self.readable.append(name)

    def register(self) -> None:
        """A register behind an enable, a reset, or both, in either nesting order."""
        name = self.fresh("q")
        value = self.expr()
        reset = self.const()
        roll = self.rng.random()
        sens = "posedge clk"
        if roll < 0.35:
            sens += " or posedge rst"
            self.features.add("reset_nest")
            head = f"if (rst) {name} <= {reset}; else"
        elif roll < 0.7:
            self.features.add("reset_nest")
            head = f"if (rst) {name} <= {reset}; else"
        else:
            head = ""
        if self.chance(0.6):
            self.features.add("enable_nest")
            update = f"if ({'en' if self.chance(0.6) else self.cond()}) {name} <= {value};"
        else:
            update = f"{name} <= {value};"
        if head and sens == "posedge clk" and self.chance(0.3):  # if (en) if (rst) ...
            self.features.add("enable_nest")
            self.body.append(f"  always @({sens}) if (en) begin {head} {name} <= {value}; end")
        else:
            self.body.append(f"  always @({sens}) begin {head} {update} end")
        self.outputs.append(name)
        self.regs.append(name)
        self.readable.append(name)

    def build(self) -> str:
        for _ in range(self.rng.randint(3, 6)):
            roll = self.rng.random()
            if roll < 0.4:
                self.assign()
            elif roll < 0.65:
                self.comb_case()
            else:
                self.register()
        return self.render()

    def render(self) -> str:
        ports = ["input clk", "input rst", "input en", f"input [{SEL_BITS - 1}:0] s",
                 f"input [{WIDTH - 1}:0] a, b, c, d"]
        ports += [f"output [{WIDTH - 1}:0] {o}" if o.startswith("w") else
                  f"output reg [{WIDTH - 1}:0] {o}" for o in self.outputs]
        header = f"// features: {' '.join(sorted(self.features))}"
        return "\n".join([header, "module t (" + ", ".join(ports) + ");", *self.body,
                          "endmodule", ""])


def generate(seed: int) -> Profile:
    """The module for seed: its text and the features it used."""
    module = _Module(seed)
    text = module.build()
    return Profile(text, frozenset(module.features))
