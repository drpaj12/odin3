// x in the second library directory: never read (lib/d1 is searched first).
module x (input wire a, output wire y);
    assign y = ~a;
endmodule
