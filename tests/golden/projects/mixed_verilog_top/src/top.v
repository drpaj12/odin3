// Verilog top instantiating the VHDL entity scale.
module top (
    input  wire       clk,
    input  wire [3:0] x,
    output reg  [3:0] y
);
    wire [3:0] s;

    scale u_scale (.x(x), .y(s));
    always @(posedge clk)
        y <= s;
endmodule
