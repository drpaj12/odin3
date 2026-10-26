`define STEP(r, v) if (v) r <= r + 4'd1; else r <= r
module macro_body (input clk, input en, input dn, output reg [3:0] c, output reg [3:0] d);
  always @(posedge clk) begin
    `STEP(c, en);
  end
  always @(posedge clk) begin
    `STEP(d, dn);
  end
endmodule
