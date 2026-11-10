// $aldff: asynchronous load of a data input.
module ff_aload(
    input clk,
    input aload,
    input [3:0] ad,
    input [3:0] d,
    output reg [3:0] q
);
    always @(posedge clk or posedge aload) if (aload) q <= ad; else q <= d;
endmodule
