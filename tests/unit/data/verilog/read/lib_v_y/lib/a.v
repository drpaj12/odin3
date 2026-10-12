// The first -v file: thru and inv.
module thru (input wire i, output wire o);
    assign o = i;
endmodule

module inv (input wire i, output wire o);
    assign o = !i;
endmodule
