// T16: procedural assign/deassign and force/release (REJECT) — variants: assign r = a, deassign r, force q = b, release q
module t16_proc_cont (input a, b, output reg q, r);
  initial begin
    assign r = a;
    deassign r;
    force q = b;
    release q;
  end
endmodule
