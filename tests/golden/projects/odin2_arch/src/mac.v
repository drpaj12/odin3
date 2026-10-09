// Multiply-add: the subject of the Odin II multiply/adder thresholds.
module mac (
    input  wire [7:0]  a,
    input  wire [7:0]  b,
    input  wire [15:0] c,
    output wire [15:0] y
);
    assign y = a * b + c;
endmodule
