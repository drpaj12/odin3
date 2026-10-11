// inv is defined here and in lib/a.v; this one wins.
module top (input wire i, output wire o);
    wire n;
    thru u_thru (.i(i), .o(n));
    inv u_inv (.i(n), .o(o));
endmodule

module inv (input wire i, output wire o);
    assign o = ~i;
endmodule
