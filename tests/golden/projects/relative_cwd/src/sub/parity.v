// Even parity.
module parity (
    input  wire [7:0] d,
    output wire       p
);
    assign p = ^d;
endmodule
