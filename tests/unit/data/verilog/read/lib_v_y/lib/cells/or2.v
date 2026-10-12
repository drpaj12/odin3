// Nobody instantiates or2: a library directory is searched, never read whole.
module or2 (input wire a, input wire b, output wire y);
    assign y = a | b;
endmodule
