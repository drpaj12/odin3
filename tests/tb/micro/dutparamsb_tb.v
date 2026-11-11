// Non-ANSI DUT (parameters declared in the body): the override equals the default, signed -1 too.
module dutparamsb_tb;
  reg [3:0] a = 3;
  wire [3:0] q1, q2;
  dutparamsb #(4, -1) u1 (.a(a), .q(q1));
  dutparamsb #(.W(4), .N(-1)) u2 (.a(a), .q(q2));
  initial begin #1 $display("%0d %0d", q1, q2); end
endmodule
