// T9 (accepted, 2D-7b): `=` and `<=` to one bit in different arms of an if and a case.
module t9_arms (input clk, input [1:0] s, input a, b, output reg q, output reg [1:0] w);
  always @(posedge clk) begin
    if (a) q = b;
    else q <= s[0];
    case (s)
      2'd0: w[0] = a;
      2'd1: w[0] <= b;
      default: w <= {a, b};
    endcase
  end
endmodule
