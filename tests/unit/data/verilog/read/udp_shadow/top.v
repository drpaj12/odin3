module top (input wire a, output wire y);
    foo (y, a);
    bar u_bar (.i(a), .o());
endmodule

module foo (output wire o, input wire i);
    assign o = i;
endmodule
