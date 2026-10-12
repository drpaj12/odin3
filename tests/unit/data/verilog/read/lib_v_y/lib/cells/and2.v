// and2 under the second libext (.v): never read, .vh comes first.
module and2 (input wire a, input wire b, output wire y);
    assign y = a | b;
endmodule
