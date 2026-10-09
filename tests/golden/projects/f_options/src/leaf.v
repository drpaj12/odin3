// Read as Verilog: listed after lists/sv.f, whose -sv ends with that list.
module leaf (
    input  wire [3:0] a,
    output wire [3:0] y
);
    assign y = ~a;
endmodule
