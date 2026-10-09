// Read as Verilog (listed before -sv).
module leaf (
    input  wire [3:0] a,
    output wire [3:0] y
);
    assign y = ~a;
endmodule
