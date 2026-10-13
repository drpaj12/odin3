// P3: parameter with a range or signed — variants: parameter [3:0], parameter signed [7:0], parameter signed without a range, localparam signed [3:0]
module p3_ranged_signed (output [7:0] y);
  parameter [3:0] P = 4'd9;
  parameter signed [7:0] S = 8'shFD;
  parameter signed Q = 1;
  localparam signed [3:0] LS = 4'sb1000;
  assign y = S;
endmodule
