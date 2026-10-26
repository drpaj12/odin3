module gen_clone (input clk, input [1:0] en, input [1:0] d, output reg [1:0] q);
  genvar i;
  generate
    for (i = 0; i < 2; i = i + 1) begin : lane
      always @(posedge clk)
        if (en[i]) q[i] <= d[i];
    end
  endgenerate
endmodule
