// Task 22: the case method: a variable bit select and a variable indexed part select as targets,
// each after a whole assignment (so no flip-flop gets an enable, which the equiv-check cannot
// read). The part select's base is one bit wide: no position straddles the end of z (spec 17 #54).
module t (input clk, input [1:0] s, input d, input b, input [1:0] d2, input [3:0] a,
          output reg [3:0] y, output reg [3:0] z);
  always @(posedge clk) begin
    y <= a;
    y[s] <= d;
    z <= ~a;
    z[{1'b0, b} +: 2] <= d2;
  end
endmodule
