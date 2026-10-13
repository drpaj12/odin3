// Task 22 (C8): case items that are expressions.
module t (input a, b, output reg [1:0] y);
  always @* case (1'b1)
    a: y = 2'd1;
    b: y = 2'd2;
    default: y = 2'd3;
  endcase
endmodule
