// parallel_case_nonconst_append: an all-'?' item, then a non-constant item (2E spec 20, #17)
// yosys -V: Yosys 0.55 (git sha1 60f126cd0, g++ 13.3.0-6ubuntu2~24.04.1 -fPIC -O3)
// Yosys 0.55 asserts (ctrl_sig.size() == 1, proc_mux.cc:264) on this; 2E lowers it.
module t (input [1:0] s, k, input a, b, output reg y);
  always @* casez (s) // synopsys parallel_case
    2'b??: y = a;
    k: y = b;
  endcase
endmodule
