// minimal repro: two sign-extended operands added in a 11-bit context
module sx_add (clk, a, b, y);
  input clk;
  input [8:0] a;
  input [8:0] b;
  output [10:0] y;
  reg [8:0] ra;
  reg [8:0] rb;
  reg [10:0] y;
  always @(posedge clk) begin
    ra <= a;
    rb <= b;
    y <= {ra[8], ra[8], ra} + {rb[8], rb[8], rb};
  end
endmodule
