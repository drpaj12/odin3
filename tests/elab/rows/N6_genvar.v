// N6: genvar — variants: module-level genvar, a genvar list, a genvar declared inside a generate region
module n6_genvar (input [3:0] a, output [3:0] y, z);
  genvar i, j;
  for (i = 0; i < 4; i = i + 1) begin : g
    assign y[i] = a[i];
  end
  generate
    genvar k;
    for (k = 0; k < 4; k = k + 1) begin : h
      assign z[k] = ~a[k];
    end
  endgenerate
endmodule
