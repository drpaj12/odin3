// -v library file: its modules are used only when something instantiates them.
module dff (
    input  wire clk,
    input  wire d,
    output reg  q
);
    always @(posedge clk)
        q <= d;
endmodule

module inv (
    input  wire a,
    output wire y
);
    assign y = ~a;
endmodule

module never_used (
    input  wire a,
    output wire y
);
    assign y = a;
endmodule
