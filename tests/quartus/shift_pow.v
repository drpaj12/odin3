// A variable bit select past the end of a 5-bit word (reads x, $shiftx) and a constant power ($pow) as plain assigns.
module shift_pow(
    input [4:0] a,
    input [2:0] i,
    output y,
    output [7:0] p
);
    assign y = a[i];
    assign p = 8'd3 ** 2;
endmodule
