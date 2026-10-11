// T11: delays (IGNORE) — variants: intra-assignment q <= #1 d and q = #(2) d, #D stmt with a parameter, #5 stmt, #(a+b) stmt, #(1:2:3) stmt, #5; with no statement
module t11_delays (input clk, a, b, d, output reg q1, q2, q3, output reg [3:0] r);
  parameter D = 2;
  always @(posedge clk)
    q1 <= #1 d;
  always @(posedge clk)
    q2 = #(2) d;
  always @(posedge clk)
    #D q3 <= d;
  initial begin
    r = 4'd0;
    #5 r = 4'd1;
    #(a+b) r = 4'd2;
    #(1:2:3) r = 4'd3;
    #5;
  end
endmodule
