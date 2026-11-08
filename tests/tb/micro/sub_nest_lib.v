module inner #(parameter K = 1) (input clk, output reg [7:0] v);
  integer calls;
  task bump;
    input [7:0] by;
    begin
      calls = calls + 1;
      v = v + by;
    end
  endtask
  initial begin calls = 0; v = 0; end
  always @(posedge clk) bump(K);
endmodule

module outer #(parameter N = 2) (input clk, output [7:0] r);
  wire [7:0] va, vb;
  inner #(N) ia (.clk(clk), .v(va));
  inner #(.K(N * 2)) ib (.clk(clk), .v(vb));
  assign r = va + vb;
  initial $display("outer N=%0d", N);
endmodule
