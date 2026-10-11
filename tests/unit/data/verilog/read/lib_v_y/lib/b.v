// The second -v file: thru again (ignored, a.v is first).
module thru (input wire i, output wire o);
    assign o = ~i;
endmodule
