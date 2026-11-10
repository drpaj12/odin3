// A variable bit select ($shiftx) and a constant power ($pow) as plain assigns.
module shift_pow(
    input [7:0] a,
    input [2:0] i,
    output y,
    output [7:0] p
);
    assign y = a[i];
    assign p = 8'd3 ** 2;
endmodule
