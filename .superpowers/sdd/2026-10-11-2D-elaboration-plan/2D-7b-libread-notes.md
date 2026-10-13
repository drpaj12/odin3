# 2D-7b: loose ends for the library-unit read task (review M5)

For the queued "library-unit read" mode (before 2H). Not implemented in 2D-7b.

1. Text: the `adder #(W)` error (`elab1_expand.c` too_many, `elab1_inst.c` odin3_elab1_fixed_width_why), spec 17 #78
   and PHASE2 #20 say "3E/Phase 4"; progress.md queues the read mode "before 2H". Update all three when it lands.
2. EL-6 resolves a library-unit MODULE (step 3) before a registered cell type (step 5), but also rejects a design
   module that has the name of a registered cell type ("module 'adder' has the name of cell type 'adder'").
   The spec must say a library unit is exempt from that check; otherwise every 1-bit `adder u(...)` in a design
   that reads `vtr_primitives.v` silently switches from the hard cell to the soft `$add`.
3. The two micros (the `adder #(W)` ones) would then be equivalent to Yosys but not structurally equal to the
   Parmys goldens (a chain of 1-bit hard adders) until 3E.
