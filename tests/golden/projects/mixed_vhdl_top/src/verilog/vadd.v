// Verilog adder, instantiated from VHDL as a component.
module vadd (
    input  wire [3:0] a,
    input  wire [3:0] b,
    output wire [3:0] s
);
    assign s = a + b;
endmodule
