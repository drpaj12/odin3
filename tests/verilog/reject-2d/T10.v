// T10: one variable assigned in two always blocks (REJECT).
module m (input c, a, b, output reg [3:0] r);
  always @(posedge c) r[2] <= a;
  always @(posedge c) r[2] <= b;
endmodule
