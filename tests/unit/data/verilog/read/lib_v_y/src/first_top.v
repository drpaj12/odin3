// thru comes from the -v files: lib/a.v first.
module top (input wire i, output wire o);
    thru u_thru (.i(i), .o(o));
endmodule
