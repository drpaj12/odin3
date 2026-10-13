module t(input c, input [1:0] s, input [1:0] wa, ra, input [7:0] d, output [7:0] q);
reg [7:0] m [0:3];
task wr(input [1:0] a, input [7:0] v);
  case (s)
    2'd1: m[a] <= v;
    2'd2: begin m[a] <= v; m[a+1] <= ~v; end
    default: ;
  endcase
endtask
always @(posedge c) begin wr(wa, d); if (s == 2'd3) wr(wa+1, d+1); end
assign q = m[ra];
endmodule
