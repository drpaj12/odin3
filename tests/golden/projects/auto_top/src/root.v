// The only module nobody instantiates: the top, although it is neither first nor last.
module root (
    input  wire [5:0] x,
    output wire [1:0] y
);
    wire [1:0] m;

    mid u_mid (.x(x), .y(m));
    assign y = ~m;
endmodule
