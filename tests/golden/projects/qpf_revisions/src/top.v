// Multiply-accumulate with STAGES output registers (default 1); each revision sets STAGES.
module top #(parameter STAGES = 1) (
    input  wire       clk,
    input  wire [3:0] a,
    input  wire [3:0] b,
    output wire [7:0] y
);
    wire [7:0] p;

    mac u_mac (.clk(clk), .a(a), .b(b), .acc(p));
    delay #(.N(STAGES)) u_delay (.clk(clk), .d(p), .q(y));
endmodule
