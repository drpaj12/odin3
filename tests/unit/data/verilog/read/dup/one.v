// m twice in one file: the second definition is the error.
module m (input wire a, output wire y);
    assign y = a;
endmodule
module m (input wire a, output wire y);
    assign y = ~a;
endmodule
