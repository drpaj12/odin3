// G3: declarations and processes in a loop-generate block — variants: localparam, wire, reg, assign, always and a module instance in a named block; wire and assign in an unnamed block
module g3_leaf (input d, output q);
  assign q = ~d;
endmodule

module g3_gen_items (input clk, input [3:0] a, output [3:0] y, z, w);
  genvar i;
  for (i = 0; i < 4; i = i + 1) begin : lane
    localparam P = 1;
    wire t;
    reg r;
    assign t = a[i] ^ P;
    always @(posedge clk)
      r <= t;
    assign y[i] = r;
    g3_leaf u (.d(a[i]), .q(z[i]));
  end
  for (i = 0; i < 4; i = i + 1) begin
    wire n;
    assign n = ~a[i];
    assign w[i] = n;
  end
endmodule
