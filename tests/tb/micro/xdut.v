module xdut(input clk, input rst, input [1:0] sel, input [7:0] d, output reg [7:0] q,
            output reg [7:0] m, output [7:0] t, output reg [3:0] st);
  always @(posedge clk) begin
    if (rst) q <= 8'd0;
    else q <= q + d;
  end
  always @(posedge clk) begin
    if (rst) st <= 4'd0;
    else case (st)
      4'd0: st <= 4'd1;
      4'd1: st <= 4'd2;
      default: st <= 4'd0;
    endcase
  end
  always @(*) begin
    if (sel[0]) m = d;
    else m = 8'h0f;
  end
  assign t = sel[1] ? d : 8'h3c;
endmodule
