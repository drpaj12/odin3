"""The ``--flatten`` profile of verilog-gen (3A spec 14.5, plan Task 5).

``generate(seed)`` writes a random module hierarchy for the ``flatten`` fuzz
(``tools/flatten-check/fuzz_flatten``). Every module has the ports ``clk rst a b y z`` (``a b y z``
are ``W`` bits wide, ``W`` a parameter), so any module can instantiate any module of the level
below. The top is called ``top``. The first line lists the counted features the seed used
(``// features: ...``):

- depth 1 to 6 levels, 1 to 3 modules per level, 1 to 4 instances per module (a generate loop
  and the bus reader count as one each); the flat design stays under ``MAX_FLAT`` leaf instances;
- ``param_override``: an instance with a narrower ``#(.W(2))`` than its parent (a second
  specialization of the same module);
- ``generate``: a generate loop of instances (names like ``g[1].u``);
- ``feedthrough``: ``assign z = a`` (an output wired straight to an input);
- ``const_out``: an output tied to a constant;
- ``unconnected``: an instance with an output port left open (``.z()``);
- ``reg``: registers (only in about a third of the designs: the simulator cannot run word
  flip-flops before ``lower``, so those designs skip the simulation leg);
- ``inout``: an ``inout`` bus read by a child and driven by a tristate in the top;
- ``keep_module`` / ``keep_inst``: ``(* keep_hierarchy *)`` on a module or on an instance.

The registers are synchronous-reset, so nothing needs a latch, and every output is driven. The
output is a function of the seed alone.
"""

from __future__ import annotations

import random
from dataclasses import dataclass

# Every counted production; test_verilog_gen asserts each appears in seeds 1-200.
FEATURES = (
    "param_override", "generate", "feedthrough", "const_out", "unconnected", "inout",
    "keep_module", "keep_inst", "reg",
)
MAX_LEVELS = 6
MAX_MODULES = 3
MAX_FANOUT = 4
MAX_FLAT = 40  # leaf instances below one module
WIDTH = 4
NARROW = 2
WEIGHT_NARROW = 0.2
WEIGHT_GENERATE = 0.2
WEIGHT_KEEP = 0.2  # a design with keep_hierarchy at all
WEIGHT_KEEP_MODULE = 0.15
WEIGHT_KEEP_INST = 0.15
WEIGHT_UNCONNECTED = 0.2
WEIGHT_THRU = 0.15
WEIGHT_CONST = 0.1
WEIGHT_INOUT = 0.25
WEIGHT_SEQ = 0.3  # a design with registers (the simulator cannot run word flip-flops yet)
HEADER = (
    "#(parameter W = {w}) (input clk, input rst, input [W-1:0] a, input [W-1:0] b, "
    "output [W-1:0] y, output [W-1:0] z{extra});"
)


@dataclass(frozen=True)
class Profile:
    text: str
    features: frozenset[str]


@dataclass
class _Module:
    name: str
    flat: int  # leaf instances in its flat form


class _Gen:
    def __init__(self, seed: int) -> None:
        self.rng = random.Random(seed)
        self.features: set[str] = set()
        self.text: list[str] = []
        self.levels: list[list[_Module]] = []
        self.seq = self.chance(WEIGHT_SEQ)
        self.keeping = self.chance(WEIGHT_KEEP)

    def chance(self, p: float) -> bool:
        return self.rng.random() < p

    def keep(self, p: float, feature: str) -> str:
        if self.keeping and self.chance(p):
            self.features.add(feature)
            return "(* keep_hierarchy *) "
        return ""

    def leaf(self, name: str) -> _Module:
        """A module without instances: combinational, registered, feed-through or constant."""
        kind = self.rng.choice(("comb", "reg", "thru", "const") if self.seq else
                               ("comb", "thru", "const"))
        body = [f"  wire [W-1:0] t = a {self.rng.choice('&|^')} b;"]
        if kind == "reg":
            self.features.add("reg")
            body += [
                "  reg [W-1:0] r;",
                "  always @(posedge clk) begin if (rst) r <= {W{1'b0}}; else r <= t; end",
                "  assign y = r + a;",
                "  assign z = r ^ b;",
            ]
        else:
            body += ["  assign y = t + b;"]
            if kind == "thru":
                self.features.add("feedthrough")
                body += ["  assign z = a;"]
            elif kind == "const":
                self.features.add("const_out")
                body += ["  assign z = {W{1'b1}};"]
            else:
                body += ["  assign z = ~t;"]
        keep = self.keep(WEIGHT_KEEP_MODULE, "keep_module")
        self.text += [f"{keep}module {name} " + HEADER.format(w=WIDTH, extra=""), *body,
                      "endmodule", ""]
        return _Module(name, 1)

    def pick_children(self, below: list[_Module], limit: int) -> list[_Module]:
        """1 to ``limit`` modules of the level below, keeping the flat size in check."""
        chosen: list[_Module] = []
        total = 0
        for _ in range(self.rng.randint(1, limit)):
            child = self.rng.choice(below)
            if chosen and total + child.flat > MAX_FLAT:
                break
            chosen.append(child)
            total += child.flat
        return chosen

    def inner(self, name: str, below: list[_Module], extra: str = "") -> _Module:
        looped = self.chance(WEIGHT_GENERATE)
        kids = self.pick_children(below, MAX_FANOUT - int(looped) - int(bool(extra)))
        pool = ["a", "b"]
        ys: list[str] = []
        zs: list[str] = []
        lines: list[str] = []
        for i, kid in enumerate(kids):
            lines += self.instance(i, kid, pool, ys, zs)
        if looped:
            lines += self.generate_loop(self.rng.choice(kids), pool, ys, zs)
        if extra:
            lines += self.inout_bus(pool, ys)
        y_expr = " ^ ".join(ys) if ys else "a"
        z_expr = " & ".join(zs) if zs else "b"
        if self.chance(WEIGHT_THRU):
            self.features.add("feedthrough")
            z_expr = self.rng.choice(("a", "b"))
        lines += [f"  assign y = {y_expr};", f"  assign z = {z_expr};"]
        keep = self.keep(WEIGHT_KEEP_MODULE, "keep_module") if name != "top" else ""
        self.text += [f"{keep}module {name} " + HEADER.format(w=WIDTH, extra=extra), *lines,
                      "endmodule", ""]
        return _Module(name, sum(k.flat for k in kids) + (1 if name == "top" else 0))

    def instance(self, i: int, kid: _Module, pool: list[str], ys: list[str], zs: list[str]
                 ) -> list[str]:
        """One instance u<i>; its outputs join the pool for later instances."""
        a, b = self.rng.choice(pool), self.rng.choice(pool)
        keep = self.keep(WEIGHT_KEEP_INST, "keep_inst")
        lines = [f"  wire [W-1:0] y{i}, z{i};"]
        if self.chance(WEIGHT_NARROW):
            self.features.add("param_override")
            n = NARROW
            lines = [f"  wire [{n - 1}:0] ny{i}, nz{i};",
                     f"  wire [W-1:0] y{i} = {{{{(W-{n}){{1'b0}}}}, ny{i}}};",
                     f"  wire [W-1:0] z{i} = {{{{(W-{n}){{1'b0}}}}, nz{i}}};",
                     f"  {keep}{kid.name} #(.W({n})) u{i} (.clk(clk), .rst(rst), "
                     f".a({a}[{n - 1}:0]), .b({b}[{n - 1}:0]), .y(ny{i}), .z(nz{i}));"]
        elif self.chance(WEIGHT_UNCONNECTED):
            self.features.add("unconnected")
            lines.append(f"  {keep}{kid.name} #(.W(W)) u{i} (.clk(clk), .rst(rst), .a({a}), "
                         f".b({b}), .y(y{i}), .z());")
            ys.append(f"y{i}")
            pool.append(f"y{i}")
            return lines
        else:
            lines.append(f"  {keep}{kid.name} #(.W(W)) u{i} (.clk(clk), .rst(rst), .a({a}), "
                         f".b({b}), .y(y{i}), .z(z{i}));")
        ys.append(f"y{i}")
        zs.append(f"z{i}")
        pool += [f"y{i}", f"z{i}"]
        return lines

    def generate_loop(self, kid: _Module, pool: list[str], ys: list[str], zs: list[str]
                      ) -> list[str]:
        """A two-iteration generate loop; the outputs are the halves of gy and gz."""
        self.features.add("generate")
        a, b = self.rng.choice(pool), self.rng.choice(pool)
        ys += ["gy[W-1:0]", "gy[2*W-1:W]"]
        zs += ["gz[W-1:0]", "gz[2*W-1:W]"]
        return [
            "  wire [2*W-1:0] gy, gz;",
            "  genvar gi;",
            "  generate for (gi = 0; gi < 2; gi = gi + 1) begin : g",
            f"    {kid.name} #(.W(W)) u (.clk(clk), .rst(rst), .a({a}), .b({b}), "
            "    .y(gy[gi*W +: W]), .z(gz[gi*W +: W]));",
            "  end endgenerate",
        ]

    def inout_bus(self, pool: list[str], ys: list[str]) -> list[str]:
        """The top's tristate driver on io and the child ``bus_rd`` that reads it."""
        self.features.add("inout")
        a, b = self.rng.choice(pool), self.rng.choice(pool)
        ys.append("by")
        return [
            "  assign io = en ? a[0] : 1'bz;",
            "  wire [W-1:0] by, bz;",
            f"  bus_rd #(.W(W)) ub (.clk(clk), .rst(rst), .a({a}), .b({b}), .y(by), .z(bz), "
            ".p(io));",
        ]

    def build(self) -> str:
        use_bus = self.chance(WEIGHT_INOUT)
        if use_bus:
            self.text += [
                "module bus_rd " + HEADER.format(w=WIDTH, extra=", inout p"),
                "  assign y = a & {W{p}};",
                "  assign z = b;",
                "endmodule",
                "",
            ]
        depth = self.rng.randint(1, MAX_LEVELS)
        self.levels = [[self.leaf(f"m0_{i}") for i in range(self.rng.randint(1, MAX_MODULES))]]
        for level in range(1, depth):
            count = self.rng.randint(1, MAX_MODULES)
            self.levels.append([self.inner(f"m{level}_{i}", self.levels[-1])
                                for i in range(count)])
        extra = ", input en, inout io" if use_bus else ""
        self.inner("top", self.levels[-1], extra)
        header = f"// features: {' '.join(sorted(self.features))}"
        return "\n".join([header, f"// levels: {depth}", *self.text])


def generate(seed: int) -> Profile:
    """The hierarchy for seed: its text and the features it used."""
    gen = _Gen(seed)
    text = gen.build()
    return Profile(text, frozenset(gen.features))
