"""The ``--lower`` profile of verilog-gen (3B spec 14.5, plan Task 9 step 5).

``generate(seed)`` writes one small synthesisable module ``t`` for the ``lower`` differential
(``tools/lower-check/lower-gen-diff``: ``lower-diff`` and ``lower-equiv`` on the same file). It is
a weighting of the same kind as ``--opt``: each production is a counted feature,
``Profile.features`` names the ones a seed used, and the first two lines of the file are
``// kind: K`` and ``// features: ...``.

One seed makes one *kind* of module, because ``lower-diff`` judges a family at a time (its tier
depends on the cell types the module holds); the kind cycles with the seed so that any ten
consecutive seeds hold every kind (``KIND_CYCLE``):

- ``comb`` (``lower-diff --family arith --tier T``): word cells over operands of mixed widths and
  signs (``$signed``/``$unsigned`` casts; outputs of other widths than the expression): bitwise
  and reduce operators, logic operators, the six comparisons, ``+ - -x``, shifts by a constant
  and by a variable amount (``<< >> <<< >>>``), ``*``, ``/``, ``%``, the conditional, ``$pmux``
  through ``case`` and a variable indexed part-select (``$shiftx``);
- ``ff`` (``--family ff --tier D``): registers behind an enable and a synchronous or asynchronous
  reset in both nesting orders (``$dffe``, ``$sdff``, ``$sdffe``, ``$sdffce``, ``$adff``,
  ``$adffe``), an asynchronous load (``$aldff``, ``$aldffe``), set and reset together (``$dffsr``,
  ``$dffsre``), and a
  falling clock edge;
- ``latch`` (``--family latch``, tier S): ``always @*`` with a path that leaves the target
  unassigned (``$dlatch``), with an active-high and an active-low enable, and a second branch;
- ``mem`` (``--family mem``): a ``reg`` array with one or two write ports, one or two read ports,
  a registered read, sizes that are and are not a power of two.

The ports are ``clk rst en`` (one bit), ``s`` (three bits), ``a`` (8, unsigned), ``b`` (5,
unsigned), ``c`` (7, signed), ``d`` (4, signed). Every output is driven. Widths are small
so that the SAT proofs of ``lower-equiv`` decide (``*``, ``/`` and ``%`` only on the narrow
operands). The output is a function of the seed alone.
"""

from __future__ import annotations

import random
from dataclasses import dataclass

KINDS = ("comb", "ff", "latch", "mem")
# Ten consecutive seeds hold every kind: comb 5, ff 3, latch 1, mem 1.
KIND_CYCLE = ("comb", "ff", "comb", "mem", "comb", "ff", "comb", "latch", "comb", "ff")
FEATURES = (
    "bitwise", "reduce", "logic", "compare", "compare_signed", "add", "sub", "neg",
    "shift_const", "shift_var", "sshift_var", "mul", "div", "mod", "mux", "pmux", "shiftx",
    "mixed_width", "mixed_sign", "signed_op", "cast",
    "dffe", "sdff", "sdffe", "sdffce", "adff", "adffe", "aldff", "aldffe", "dffsr", "dffsre",
    "negedge",
    "dlatch", "dlatch_neg", "dlatch_else",
    "mem", "mem_two_write", "mem_two_read", "mem_sync_read", "mem_non_pow2",
)
# name -> (declaration, width, signed)
PORTS = {"a": (8, False), "b": (5, False), "c": (7, True), "d": (4, True)}
NARROW = ("b", "d")  # the operands of *, / and %: keep the SAT proofs inside the 60 s limit
SHIFT_AMOUNTS = ("s", "b[2:0]", "d[1:0]")
BITWISE = ("&", "|", "^", "~^")
REDUCE = ("&", "|", "^", "~^")
COMPARES = ("==", "!=", "<", "<=", ">", ">=")
OUT_WIDTHS = (1, 3, 4, 5, 8, 10)
MAX_DEPTH = 2
WEIGHT_CAST = 0.25
WEIGHT_CONST = 0.15
MEM_SIZES = (2, 4, 6, 8, 12, 16)
MEM_WIDTHS = (1, 2, 3, 4)


@dataclass(frozen=True)
class Profile:
    text: str
    kind: str
    features: frozenset[str]


def kind_of(seed: int) -> str:
    """The kind of module a seed makes."""
    return KIND_CYCLE[(seed - 1) % len(KIND_CYCLE)]


class _Module:
    def __init__(self, seed: int) -> None:
        self.rng = random.Random(seed)
        self.kind = kind_of(seed)
        self.features: set[str] = set()
        self.body: list[str] = []
        self.outputs: list[tuple[str, int, bool]] = []  # name, width, is a reg
        self.count = 0
        self.readable = list(PORTS)
        # one clock edge per module: equiv-check (lower-diff tier D) takes one clock domain
        self.edge = "negedge clk" if self.chance(0.2) else "posedge clk"

    def chance(self, p: float) -> bool:
        return self.rng.random() < p

    def fresh(self, prefix: str) -> str:
        self.count += 1
        return f"{prefix}{self.count}"

    # -- operands and expressions --

    def operand(self, names: tuple[str, ...] | None = None) -> str:
        if self.chance(WEIGHT_CONST):
            width = self.rng.choice((1, 3, 4, 8))
            return f"{width}'d{self.rng.randrange(1 << width)}"
        name = self.rng.choice(names or tuple(self.readable))
        if self.chance(WEIGHT_CAST):
            self.features.add("cast")
            return f"$signed({name})" if self.chance(0.5) else f"$unsigned({name})"
        return name

    def note_mix(self, left: str, right: str) -> None:
        """Counts mixed widths and signs of two plain operands."""
        if left in PORTS and right in PORTS:
            if PORTS[left][0] != PORTS[right][0]:
                self.features.add("mixed_width")
            if PORTS[left][1] != PORTS[right][1]:
                self.features.add("mixed_sign")
            if PORTS[left][1] and PORTS[right][1]:
                self.features.add("signed_op")

    def binary(self, depth: int) -> str:
        left, right = self.expr(depth + 1), self.expr(depth + 1)
        self.note_mix(left, right)
        roll = self.rng.random()
        if roll < 0.25:
            self.features.add("bitwise")
            return f"({left} {self.rng.choice(BITWISE)} {right})"
        if roll < 0.45:
            op = self.rng.choice(COMPARES)
            self.features.add("compare_signed" if "$signed" in left + right else "compare")
            return f"({left} {op} {right})"
        if roll < 0.6:
            self.features.add("add")
            return f"({left} + {right})"
        if roll < 0.7:
            self.features.add("sub")
            return f"({left} - {right})"
        if roll < 0.78:
            self.features.add("logic")
            return f"({left} {self.rng.choice(('&&', '||'))} {right})"
        return self.shift(left)

    def shift(self, operand: str) -> str:
        if self.chance(0.3):
            self.features.add("shift_const")
            op = self.rng.choice(("<<", ">>", "<<<", ">>>"))
            return f"({operand} {op} {self.rng.randint(0, 4)})"
        op = self.rng.choice(("<<", ">>", "<<<", ">>>"))
        self.features.add("sshift_var" if op in ("<<<", ">>>") else "shift_var")
        return f"({operand} {op} {self.rng.choice(SHIFT_AMOUNTS)})"

    def unary(self, depth: int) -> str:
        sub = self.expr(depth + 1)
        roll = self.rng.random()
        if roll < 0.3:
            self.features.add("bitwise")
            return f"(~{sub})"
        if roll < 0.55:
            self.features.add("reduce")
            return f"({self.rng.choice(REDUCE)}{sub})"
        if roll < 0.75:
            self.features.add("neg")
            return f"(-{sub})"
        if roll < 0.85:
            self.features.add("logic")
            return f"(!{sub})"
        self.features.add("mux")
        return f"({self.cond()} ? {sub} : {self.expr(depth + 1)})"

    def cond(self) -> str:
        roll = self.rng.random()
        if roll < 0.4:
            return f"s == 3'd{self.rng.randrange(8)}"
        if roll < 0.6:
            return "en"
        return f"{self.rng.choice(list(PORTS))}[{self.rng.randint(0, 3)}]"

    def arith(self) -> str:
        """``*``, ``/`` or ``%`` of narrow operands, possibly with a width of the result."""
        left, right = self.operand(NARROW), self.operand(NARROW)
        roll = self.rng.random()
        self.note_mix(left, right)
        if roll < 0.4:
            self.features.add("mul")
            return f"({left} * {right})"
        if roll < 0.7:
            self.features.add("div")
            return f"({left} / {right})"
        self.features.add("mod")
        return f"({left} % {right})"

    def expr(self, depth: int = 0) -> str:
        roll = self.rng.random()
        if depth >= MAX_DEPTH or roll < 0.2:
            return self.operand()
        if roll < 0.36 and depth == 0:
            return self.arith()
        if roll < 0.42 and depth == 0:
            self.features.add("shiftx")
            name = self.rng.choice(("a", "c"))
            return f"{name}[s +: {self.rng.randint(1, 3)}]"
        if roll < 0.7:
            return self.binary(depth)
        return self.unary(depth)

    # -- comb --

    def out_width(self) -> int:
        return self.rng.choice(OUT_WIDTHS)

    def assign(self) -> None:
        name = self.fresh("w")
        self.body.append(f"  assign {name} = {self.expr()};")
        width = self.out_width()
        self.outputs.append((name, width, False))
        self.readable.append(name)

    def pmux(self) -> None:
        """``case`` over ``s``: a ``$pmux`` after ``proc``; the items are not exhaustive."""
        self.features.add("pmux")
        name = self.fresh("y")
        items = self.rng.randint(3, 6)
        lines = ["  always @* begin", f"    {name} = {self.expr(1)};", "    case (s)"]
        for value in self.rng.sample(range(8), items):
            lines.append(f"      3'd{value}: {name} = {self.expr(1)};")
        lines += [f"      default: {name} = {self.expr(1)};", "    endcase", "  end"]
        self.body += lines
        self.outputs.append((name, self.out_width(), True))

    def build_comb(self) -> None:
        for _ in range(self.rng.randint(3, 6)):
            if self.chance(0.25):
                self.pmux()
            else:
                self.assign()

    # -- ff --

    def register(self) -> None:
        name = self.fresh("q")
        width = self.rng.choice((1, 2, 4, 5, 8))
        data = self.expr(1)
        value = f"{width}'d{self.rng.randrange(1 << width)}"
        edge = self.edge
        form = self.rng.choice(("dffe", "sdff", "sdffe", "sdffce", "adff", "adffe", "aldff",
                                "aldffe", "dffsr", "dffsre"))
        self.features.add(form)
        self.body.append(self.ff_text(form, name, (edge, data, value)))
        self.outputs.append((name, width, True))
        self.readable.append(name)

    def ff_text(self, form: str, name: str, parts: tuple[str, str, str]) -> str:
        edge, data, value = parts
        if form == "dffe":
            return f"  always @({edge}) if (en) {name} <= {data};"
        if form == "sdff":
            return f"  always @({edge}) if (rst) {name} <= {value}; else {name} <= {data};"
        if form == "sdffe":
            return (f"  always @({edge}) if (rst) {name} <= {value}; "
                    f"else if (en) {name} <= {data};")
        if form == "sdffce":
            return (f"  always @({edge}) if (en) begin if (rst) {name} <= {value}; "
                    f"else {name} <= {data}; end")
        if form == "adff":
            polarity = "posedge" if self.chance(0.5) else "negedge"
            test = "rst" if polarity == "posedge" else "!rst"
            return (f"  always @({edge} or {polarity} rst) if ({test}) {name} <= {value}; "
                    f"else {name} <= {data};")
        if form == "adffe":
            return (f"  always @({edge} or posedge rst) if (rst) {name} <= {value}; "
                    f"else if (en) {name} <= {data};")
        if form == "aldff":
            return (f"  always @({edge} or posedge rst) if (rst) {name} <= {self.expr(1)}; "
                    f"else {name} <= {data};")
        if form == "aldffe":
            return (f"  always @({edge} or posedge rst) if (rst) {name} <= {self.expr(1)}; "
                    f"else if (en) {name} <= {data};")
        if form == "dffsre":
            return (f"  always @({edge} or posedge rst or posedge s[0]) "
                    f"if (rst) {name} <= 0; else if (s[0]) {name} <= {value}; "
                    f"else if (en) {name} <= {data};")
        return (f"  always @({edge} or posedge rst or posedge en) "
                f"if (rst) {name} <= 0; else if (en) {name} <= {value}; else {name} <= {data};")

    def build_ff(self) -> None:
        if self.edge == "negedge clk":
            self.features.add("negedge")
        for _ in range(self.rng.randint(2, 4)):
            self.register()
        if self.chance(0.5):
            self.assign()

    # -- latch --

    def latch(self) -> None:
        name = self.fresh("l")
        width = self.rng.choice((1, 2, 4, 6))
        self.features.add("dlatch")
        test = "en"
        if self.chance(0.3):
            test = "!en"
            self.features.add("dlatch_neg")
        # data is a plain operand: a latch is judged gate for gate (tier S), no word cell feeds it
        lines = [f"  always @* begin if ({test}) {name} = {self.operand()};"]
        if self.chance(0.4):
            self.features.add("dlatch_else")
            lines.append(f"    else if (s[0]) {name} = {self.operand()};")
        lines.append("  end")
        self.body.append("\n".join(lines))
        self.outputs.append((name, width, True))

    def build_latch(self) -> None:
        for _ in range(self.rng.randint(1, 3)):
            self.latch()

    # -- mem --

    def build_mem(self) -> None:
        size = self.rng.choice(MEM_SIZES)
        width = self.rng.choice(MEM_WIDTHS)
        abits = max(1, (size - 1).bit_length())
        self.features.add("mem")
        if size & (size - 1):
            self.features.add("mem_non_pow2")
        self.body.append(f"  reg [{width - 1}:0] m [0:{size - 1}];")
        addr = [f"s[{abits - 1}:0]" if abits <= 3 else "a[3:0]", f"b[{abits - 1}:0]"]
        self.body.append(f"  always @(posedge clk) if (en) m[{addr[0]}] <= d[{width - 1}:0];")
        if self.chance(0.4):
            self.features.add("mem_two_write")
            self.body.append(f"  always @(posedge clk) if (rst) m[{addr[1]}] <= c[{width - 1}:0];")
        self.add_read(width, addr[1], "r1")
        if self.chance(0.4):
            self.features.add("mem_two_read")
            self.add_read(width, addr[0], "r2")

    def add_read(self, width: int, address: str, name: str) -> None:
        if self.chance(0.4):
            self.features.add("mem_sync_read")
            self.body.append(f"  always @(posedge clk) {name} <= m[{address}];")
            self.outputs.append((name, width, True))
        else:
            self.body.append(f"  assign {name} = m[{address}];")
            self.outputs.append((name, width, False))

    # -- output --

    def build(self) -> str:
        getattr(self, f"build_{self.kind}")()
        return self.render()

    def render(self) -> str:
        ports = ["input clk", "input rst", "input en", "input [2:0] s"]
        ports += [f"input {'signed ' if signed else ''}[{width - 1}:0] {name}"
                  for name, (width, signed) in PORTS.items()]
        for name, width, is_reg in self.outputs:
            span = f"[{width - 1}:0] " if width > 1 else ""
            ports.append(f"output {'reg ' if is_reg else ''}{span}{name}")
        features = " ".join(sorted(self.features))
        return "\n".join([f"// kind: {self.kind}", f"// features: {features}",
                          "module t (" + ", ".join(ports) + ");", *self.body, "endmodule", ""])


def generate(seed: int) -> Profile:
    """The module for seed: its text, its kind and the features it used."""
    module = _Module(seed)
    text = module.build()
    return Profile(text, module.kind, frozenset(module.features))
