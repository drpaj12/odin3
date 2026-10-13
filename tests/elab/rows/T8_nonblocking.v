// T8: nonblocking assignments — variants: one register, a shift chain of two registers, a vector register
module t8_nonblocking (input clk, d, input [3:0] v, output reg q1, q2, output reg [3:0] qv);
  always @(posedge clk) begin
    q1 <= d;
    q2 <= q1;
    qv <= v;
  end
endmodule
