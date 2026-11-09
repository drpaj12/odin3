# Reverse-engineering workflow

This page shows one round of the loop in `docs/RE-ARCHITECTURE.md` section 4 on a small design:
read a netlist with no useful names, guess a word-level structure, prove the guess, replace it
and write the result. The example is `plugins/python/re_round.py`; it is short on purpose, so
that it can be the starting point of a larger tool. It uses only the public ABI through
`plugins/python/odin3.py`.

## The loop

1. **Read level 0** with `Design.read_blif`. Level 0 is never edited.
2. **Guess.** The script looks for a ripple-carry adder: a sum cell (XOR or XNOR) and a carry
   cell (majority, any inversion) per bit, chained by the carry. This is a plain Python rule over
   the nodes and nets of the IR; the tool replaces it with its own hypotheses.
3. **Mark.** Each node of the guess gets the attribute `grp=add`. Each boundary net gets
   `odin3:bind=A[i]`, `B[i]` or `Y[i]`, the port bit of the `$add` cell it stands for.
4. **Prove.** `cone_check -attr grp=add -cell $add -param A_WIDTH=4 ...` compares the group with
   the template using the linked ABC. The module attribute `odin3:cone_check` is `equivalent`,
   `differs` (with `odin3:cone_cex`, an input pattern that tells them apart) or `unproven`
   (the time limit ran out).
5. **Commit.** `raise` replaces the group by one `$add`. It refuses a group that was not proved,
   or that changed since the proof. The new cell's provenance lists the source lines of every
   replaced node, so the word-level cell still traces to the BLIF.
6. **Write.** `write_verilog` (and `write_json --odin3` with all provenance).

The round works on `Design.copy()` of level 0, so a refused or wrong guess costs nothing: close
the copy and try the next hypothesis.

## Run it

An 8-bit adder is mapped to 3-input LUTs and made anonymous (every signal renamed `n0`, `n1`, ...).
Set `ODIN3_LIB` as in {doc}`python-api` and use a Python with `cffi`. `anonymize.py` below is the
renaming of `tests/tools/test_re_round.py`:

```python
# anonymize.py: python3 anonymize.py IN.blif OUT.blif
import sys
names = {}
out = []
for line in open(sys.argv[1]).read().splitlines():
    w = line.split()
    if w and w[0] in (".inputs", ".outputs", ".names"):
        for i, x in enumerate(w[1:], 1):
            w[i] = names.setdefault(x, f"n{len(names)}")
        line = " ".join(w)
    out.append(line)
open(sys.argv[2], "w").write("\n".join(out) + "\n")
```

```bash
echo 'module add(input [7:0] a, input [7:0] b, output [7:0] y); assign y = a + b; endmodule' > add.v
odin3 -p "read_verilog add.v; proc; opt; lower; abc --lut 3; write_blif add.blif"
python3 anonymize.py add.blif anon.blif
python3 plugins/python/re_round.py anon.blif out.v --json out.json
```

```text
re_round: guess: 8-bit adder, 15 cells
re_round: cone_check: equivalent
re_round: raise: 15 cells replaced by one $add
re_round: wrote out.v
```

`out.v` holds one line of logic:

```verilog
assign {n23, n22, n21, n20, n19, n18, n17, n16} = {n7, n6, n5, n4, n3, n2, n1, n0} + {n15, n14, n13, n12, n11, n10, n9, n8};
```

When no adder is found, or the proof fails, the script prints why and exits 1; it never raises
an unproved group.

## Notes for a tool built on this

- The binding does not register the linked ABC; call `odin3.default().load_abc()` once before
  `cone_check` or `abc`.
- Node and net IDs are module-local and stay the same in a copy. After `submod` the nodes live in
  a new module with new IDs; keep a map by provenance (`Node.sources()`).
- `cone_check` keeps one result per module: read `odin3:cone_check` after each call.
- `submod -bus BUSKEY` (v1.0.5) makes vector ports: set the string attribute `BUSKEY` of each
  boundary net to `NAME[BIT]` (`h12[0]`, `h12[1]`, ...) before the call; a complete set of one
  direction becomes one port `NAME`, an incomplete one stays scalar with a warning. The port sits at the position of its lowest-net-ID
  bit, among the other ports in net ID order.
- `cone_check` and `raise` take `-const PORT=V` (v1.0.5): a template input port tied to a
  constant, so `A == 13` and `Q + 1` cones prove against `$eq` and `$add` with `-const B=13` and
  `-const B=1`. Give `raise` the same options, `-const` spelling included.
- Gaps found while writing the example are listed in `docs/RE-ARCHITECTURE.md` section 5.
