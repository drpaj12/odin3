// Task 22: the full_case metacomment: the unlisted selector value assigns x.
module t (input [1:0] s, input a, b, c, output reg y);
  always @* case (s) // synopsys full_case
    2'd0: y = a;
    2'd1: y = b;
    2'd2: y = c;
  endcase
endmodule
