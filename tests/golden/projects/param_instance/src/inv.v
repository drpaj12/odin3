// W-bit inverter.
module inv #(parameter W = 1) (
    input  wire [W-1:0] a,
    output wire [W-1:0] y
);
    assign y = ~a;
endmodule
