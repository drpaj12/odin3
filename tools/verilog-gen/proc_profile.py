"""The ``--proc`` profile of verilog-gen (2E spec §18.6, plan Task 12).

``generate(seed)`` writes one small synthesisable module whose processes exercise Odin III's
``proc`` against Yosys 0.55's: ``always`` blocks with nested ``if``/``case``/``casez``/``casex``,
``full_case``/``parallel_case`` metacomments, case items with ``x`` and ``?``, every reset form of
§5.4, ``negedge``, latches (default mode only), blocking temporaries and memories. Each feature
is a counted production: ``Profile.features`` names the ones a seed used, and the first line of the
file lists them (``// features: ...``).

The module avoids what 2E rejects or documents as a divergence: one clock and edge per module (E1a),
constant reset values (E2), and case items that are always constants (the two Yosys quirks of §20
#17 and #18 need a non-constant item after an empty select). A module with ``latch`` in its
features infers latches, so only the default latch mode and no bridge run apply to it. The output
is a function of the seed alone.
"""

from __future__ import annotations

import random
import re
from dataclasses import dataclass

# Every counted production; test_proc_gen_diff asserts each appears in seeds 1-50.
FEATURES = (
    "nested_if", "case", "casez", "casex", "full_case", "parallel_case", "xz_items",
    "arst_pos", "arst_neg", "arst_not", "arst_eq", "arst_two", "sync_reset", "vhdl_style",
    "negedge", "latch", "blocking_temp", "memory", "enable_ff",
)
WIDTH = 4
SEL_BITS = 3
ADDR_BITS = 2
MAX_DEPTH = 3
COND_REDRAWS = 8
EXPR_REDRAWS = 8
LABEL_REDRAWS = 6
BINARY_OPS = ("&", "|", "^", "+", "-")
CASE_KINDS = ("case", "casez", "casex")
CASE_WEIGHTS = (3, 3, 2)


@dataclass(frozen=True)
class Profile:
    text: str
    features: frozenset[str]


class _Module:
    """One module under construction: ports, regs, processes and the features counted."""

    def __init__(self, seed: int, twins: bool = False) -> None:
        self.rng = random.Random(seed)
        # twins: no redraws, so repeated compares and operators (twin cells) stay in (hidden).
        self.twins = twins
        self.cond_redraws, self.expr_redraws, self.label_redraws = (
            (0, 0, 1) if twins else (COND_REDRAWS, EXPR_REDRAWS, LABEL_REDRAWS))
        self.features: set[str] = set()
        self.edge = "negedge" if self.rng.random() < 0.3 else "posedge"
        self.latchy = self.rng.random() < 0.25
        if self.edge == "negedge":
            self.features.add("negedge")
        if self.latchy:
            self.features.add("latch")
        self.inputs = ["a", "b", "c", "d"]  # WIDTH bits each
        self.readable = list(self.inputs)  # data signals a later process may read
        self.outputs: list[str] = []  # output regs, in declaration order
        self.temps: list[str] = []
        self.extra_ports: list[str] = []  # 1-bit inputs: en, resets, memory control
        self.body: list[str] = []
        self.mem_decls: list[str] = []
        self.count = 0
        self.compares: set[str] = set()  # the compares used so far (see cond)
        self.exprs: set[str] = set()  # the compound expressions used so far (see expr)
        self.latch_left = self.latchy  # the first comb block leaves a path undriven
        self.addr_ports: list[str] = []  # 2-bit memory address inputs
        self.wires: list[str] = []  # asynchronous memory reads (output wires)

    # -- small helpers --

    def chance(self, p: float) -> bool:
        return self.rng.random() < p

    def fresh(self, prefix: str) -> str:
        self.count += 1
        return f"{prefix}{self.count}"

    def const(self, width: int = WIDTH) -> str:
        return f"{width}'d{self.rng.randrange(1 << width)}"

    def bit_input(self, prefix: str) -> str:
        name = self.fresh(prefix)
        self.extra_ports.append(name)
        return name

    # -- expressions --

    def operand(self) -> str:
        if self.chance(0.2):
            return self.const()
        return self.rng.choice(self.readable)

    def expr(self, depth: int = 0) -> str:
        """An expression whose operator applications are new to the module (equal ones are twin
        cells on both sides; see ``cond``): a repeated one is drawn again, a few times."""
        text = self.draw_expr(depth)
        for _ in range(self.expr_redraws):
            if text[0] not in "(~" or text not in self.exprs:
                break
            text = self.draw_expr(depth)
        self.exprs.add(text)
        return text

    def draw_expr(self, depth: int) -> str:
        roll = self.rng.random()
        if depth >= 2 or roll < 0.4:
            return self.operand()
        if roll < 0.55:
            return f"~{self.operand()}"
        if roll < 0.7:
            return f"({self.cond()} ? {self.expr(depth + 1)} : {self.expr(depth + 1)})"
        return f"({self.expr(depth + 1)} {self.rng.choice(BINARY_OPS)} {self.expr(depth + 1)})"

    def cond(self) -> str:
        """A condition not used before in the module: a repeated compare is a twin cell on both
        sides, and netlist-compare's canonical labelling can tie twins (see ``case_stmt``)."""
        text = self.draw_cond()
        for _ in range(self.cond_redraws):
            key = self.compare_key(text)
            if key not in self.compares:
                self.compares.add(key)
                return text
            text = self.draw_cond()
        return text

    @staticmethod
    def compare_key(cond: str) -> str:
        """``s == 3'd5`` and the case item ``3'd5`` / ``3'b101`` lower to the same compare."""
        match = re.fullmatch(rf"s == {SEL_BITS}'d(\d+)", cond)
        if match is None:
            return cond
        return _Module.label_key(f"{SEL_BITS}'b{int(match.group(1)):0{SEL_BITS}b}", "case")

    @staticmethod
    def label_key(label: str, kind: str) -> str:
        """A case item as the compare it lowers to: the bits that are tested and their values."""
        base, _, digits = label.partition("'")
        if digits[0] == "d":
            digits = "b" + format(int(digits[1:]), f"0{SEL_BITS}b")
        wild = "?z" if kind == "casez" else "?zx" if kind == "casex" else ""
        return "".join("-" if c in wild else c for c in digits[1:])

    def draw_cond(self) -> str:
        roll = self.rng.random()
        a, b = self.rng.choice(self.readable), self.rng.choice(self.readable)
        if roll < 0.25:
            return f"s[{self.rng.randrange(SEL_BITS)}]"
        if roll < 0.45:
            return f"{a} == {b}"
        if roll < 0.6:
            return f"{a} > {b}"
        if roll < 0.75:
            return f"&{a}"
        if roll < 0.9:
            return f"s == {SEL_BITS}'d{self.rng.randrange(1 << SEL_BITS)}"
        return f"!{a}"

    # -- statements --

    def pattern(self, kind: str) -> str:
        """A case item: bits of 0/1, with ``?``/``z`` (casez, casex) and ``x`` (casex, and now and
        then casez) wildcards; a plain case takes plain constants."""
        if kind == "case":
            return f"{SEL_BITS}'d{self.rng.randrange(1 << SEL_BITS)}"
        marks = "01?z" if kind == "casez" else "01?zx"
        weights = (3, 3, 2, 1) if kind == "casez" else (3, 3, 2, 1, 2)
        digits = "".join(self.rng.choices(marks, weights)[0] for _ in range(SEL_BITS))
        if kind == "casez" and self.chance(0.1):
            digits = "x" + digits[1:]
        if any(c in digits for c in "xz?"):
            self.features.add("xz_items")
        return f"{SEL_BITS}'b{digits}"

    def fresh_label(self, kind: str) -> str | None:
        """A case item whose compare the module has not used (None after a few redraws)."""
        for _ in range(self.label_redraws):
            label = self.pattern(kind)
            key = self.label_key(label, kind)
            if self.twins or key not in self.compares:
                self.compares.add(key)
                return label
        return None

    def assign(self, targets: list[str], indent: str, op: str) -> list[str]:
        return [f"{indent}{t} {op} {self.expr()};" for t in targets]

    def case_stmt(self, targets: list[str], depth: int, indent: str, op: str,
                  total: bool) -> list[str]:
        kind = self.rng.choices(CASE_KINDS, CASE_WEIGHTS)[0]
        self.features.add(kind)
        pragma = ""
        if self.chance(0.3):
            words = self.rng.choice(("full_case", "parallel_case", "full_case parallel_case"))
            pragma = f" // synopsys {words}"
            self.features.update(words.split())
        lines = [f"{indent}{kind} (s){pragma}"]
        # A compare once per module: two equal compares are twin cells, which netlist-compare's
        # canonical labelling can tie and order differently on the two sides (a false difference
        # the bridge does not confirm).
        for _ in range(self.rng.randint(2, 4)):
            label = self.fresh_label(kind)
            if label is not None:
                lines += self.item(f"{label}:", targets, depth, indent, op, total)
        if total or self.chance(0.6):
            lines += self.item("default:", targets, depth, indent, op, total)
        lines.append(f"{indent}endcase")
        return lines

    def item(self, label: str, targets: list[str], depth: int, indent: str, op: str,
             total: bool) -> list[str]:
        inner = self.stmts(targets, depth + 1, indent + "    ", op, total)
        return [f"{indent}  {label} begin", *inner, f"{indent}  end"]

    def if_stmt(self, targets: list[str], depth: int, indent: str, op: str,
                total: bool) -> list[str]:
        self.features.add("nested_if")
        lines = [f"{indent}if ({self.cond()}) begin",
                 *self.stmts(targets, depth + 1, indent + "  ", op, total), f"{indent}end"]
        if total or self.chance(0.6):
            lines[-1] = f"{indent}end else begin"
            lines += [*self.stmts(targets, depth + 1, indent + "  ", op, total), f"{indent}end"]
        return lines

    def stmts(self, targets: list[str], depth: int, indent: str, op: str,
              total: bool) -> list[str]:
        """Assignments to some of the targets and, below MAX_DEPTH, nested ``if``/``case``.
        ``total`` asks every ``if`` for an ``else`` and every ``case`` for a ``default``."""
        picked = [t for t in targets if self.chance(0.7)] or targets[:1]
        lines = self.assign(picked, indent, op)
        if depth < MAX_DEPTH and self.chance(0.6):
            nested = self.case_stmt if self.chance(0.5) else self.if_stmt
            lines += nested(targets, depth, indent, op, total)
        return lines

    # -- processes --

    def comb(self) -> None:
        """``always @*``: every target driven by a default first, so no latch (unless this is
        the latch module's latchy block, where the defaults are left out)."""
        outs = [self.fresh("y") for _ in range(self.rng.randint(1, 2))]
        lines = ["  always @* begin"]
        latchy = self.latch_left
        self.latch_left = False
        if self.chance(0.4):
            temp = self.fresh("t")
            self.temps.append(temp)
            self.features.add("blocking_temp")
            lines.append(f"    {temp} = {self.expr()};")
            self.readable.append(temp)
        if not latchy:
            lines += self.assign(outs, "    ", "=")
        lines += self.stmts(outs, 1, "    ", "=", not latchy)
        if self.temps and not latchy and self.chance(0.5):
            lines.append(f"    {outs[0]} = {outs[0]} ^ {self.temps[-1]};")
        lines.append("  end")
        self.outputs += outs
        self.readable += outs
        self.body += lines
        if self.temps and self.temps[-1] in self.readable:
            self.readable.remove(self.temps[-1])  # a temp is local to its block

    def reset_form(self) -> tuple[str, str, str]:
        """(sensitivity extra, test, polarity words) of one asynchronous reset."""
        rst = self.bit_input("r")
        form = self.rng.choice(("pos", "neg", "not", "eq1", "eq0", "tilde"))
        if form == "pos":
            self.features.add("arst_pos")
            return f"posedge {rst}", rst, rst
        if form == "neg":
            self.features.add("arst_neg")
            return f"negedge {rst}", f"!{rst}", rst
        if form == "not":
            self.features.add("arst_not")
            return f"posedge {rst}", f"!(!{rst})", rst
        self.features.add("arst_eq")
        if form == "eq1":
            return f"posedge {rst}", f"{rst} == 1'b1", rst
        if form == "eq0":
            return f"negedge {rst}", f"{rst} == 1'b0", rst
        self.features.add("arst_neg")
        return f"negedge {rst}", f"~{rst}", rst

    def seq(self) -> None:
        outs = [self.fresh("q") for _ in range(self.rng.randint(1, 2))]
        sens = f"{self.edge} clk"
        head: list[str] = []
        roll = self.rng.random()
        if roll < 0.4:  # asynchronous reset(s)
            event, test, _ = self.reset_form()
            sens += f" or {event}"
            first = [f"{o} <= {self.const()};" for o in outs]
            if self.chance(0.25):  # a second reset, same or another value
                self.features.add("arst_two")
                event2, test2, _ = self.reset_form()
                sens += f" or {event2}"
                head = [f"    if ({test}) begin {' '.join(first)} end else if ({test2}) begin "
                        + " ".join(f"{o} <= {self.const()};" for o in outs) + " end else begin"]
            else:
                head = [f"    if ({test}) begin {' '.join(first)} end else begin"]
            tail = ["    end"]
        elif roll < 0.55:  # synchronous reset
            self.features.add("sync_reset")
            rst = self.bit_input("r")
            head = [f"    if ({rst}) begin "
                    + " ".join(f"{o} <= {self.const()};" for o in outs) + " end else begin"]
            tail = ["    end"]
        elif roll < 0.62:  # VHDL-style edge test
            self.features.add("vhdl_style")
            head = ["    if (clk) begin"]
            tail = ["    end"]
        else:
            head, tail = ["    begin"], ["    end"]
        enabled = self.chance(0.4)
        if enabled:
            self.features.add("enable_ff")
            head.append(f"      if ({self.cond()}) begin")
        body = self.stmts(outs, 2, "      ", "<=", False)
        self.body += [f"  always @({sens})", *head, *body]
        if enabled:
            self.body.append("      end")
        self.body += [*tail]
        self.outputs += outs
        self.readable += outs

    def memory(self) -> None:
        mem, rd = self.fresh("m"), self.fresh("rd")
        we, wa, ra = self.bit_input("we"), self.fresh("wa"), self.fresh("ra")
        self.mem_decls.append(f"  reg [{WIDTH - 1}:0] {mem} [0:{(1 << ADDR_BITS) - 1}];")
        self.addr_ports += [wa, ra]
        self.features.add("memory")
        data = self.expr()
        self.body.append(f"  always @({self.edge} clk) if ({we}) {mem}[{wa}] <= {data};")
        if self.chance(0.5):  # an asynchronous read
            self.body.append(f"  assign {rd} = {mem}[{ra}];")
            self.wires.append(rd)
        else:  # a registered read
            self.body.append(f"  always @({self.edge} clk) {rd} <= {mem}[{ra}];")
            self.outputs.append(rd)
        self.readable.append(rd)

    def build(self) -> str:
        for _ in range(self.rng.randint(2, 4)):
            roll = self.rng.random()
            if roll < 0.4:
                self.comb()
            elif roll < 0.85:
                self.seq()
            else:
                self.memory()
        if self.latchy and self.latch_left:
            self.comb()
        return self.render()

    def render(self) -> str:
        wires, addr_ports = self.wires, self.addr_ports
        ports = ["input clk", "input [2:0] s", f"input [{WIDTH - 1}:0] a, b, c, d"]
        ports += [f"input {p}" for p in self.extra_ports if p not in addr_ports]
        ports += [f"input [{ADDR_BITS - 1}:0] {p}" for p in addr_ports]
        ports += [f"output reg [{WIDTH - 1}:0] {o}" for o in self.outputs]
        ports += [f"output [{WIDTH - 1}:0] {w}" for w in wires]
        decls = [f"  reg [{WIDTH - 1}:0] {t};" for t in self.temps]
        header = f"// features: {' '.join(sorted(self.features))}"
        return "\n".join([header, "module t (" + ", ".join(ports) + ");", *decls,
                          *self.mem_decls, *self.body, "endmodule", ""])


def generate(seed: int, twins: bool = False) -> Profile:
    """The module for seed: its text and the features it used.  ``twins`` (a test hook) turns
    the twin-cell avoidance off."""
    module = _Module(seed, twins)
    text = module.build()
    return Profile(text, frozenset(module.features))
