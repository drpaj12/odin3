module t(input c, input [3:0] a, output [3:0] q);
reg [3:0] r = 4'd3;
task tk(input [3:0] x);
  begin r <= x; end
endtask
always @(posedge c) tk(a);
assign q = r;
endmodule
