// Shift register, DEPTH stages of WIDTH bits; the project overrides both parameters.
module top #(
    parameter WIDTH = 4,
    parameter DEPTH = 2
) (
    input  wire             clk,
    input  wire [WIDTH-1:0] d,
    output wire [WIDTH-1:0] q
);
    wire [WIDTH-1:0] stage [0:DEPTH];

    assign stage[0] = d;
    genvar i;
    generate
        for (i = 0; i < DEPTH; i = i + 1) begin : g_stage
            stage_reg #(.W(WIDTH)) u_reg (.clk(clk), .d(stage[i]), .q(stage[i+1]));
        end
    endgenerate
    assign q = stage[DEPTH];
endmodule
