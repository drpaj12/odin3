module show #(parameter W = 4, parameter D = 2) (input clk, output [3:0] q);
  localparam M = W * D;
  reg [W-1:0] r;
  initial begin
    r = {W{1'b1}};
    $display("W=%0d D=%0d M=%0d r=%b", W, D, M, r);
  end
  assign q = W + D;
endmodule
