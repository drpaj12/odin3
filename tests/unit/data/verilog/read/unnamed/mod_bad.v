// An unnamed module instance is illegal (1364-2005 §12.1.2).
module sub (input wire a, output wire y);
    assign y = a;
endmodule

module top (input wire a, output wire y);
    sub (a, y);
endmodule
