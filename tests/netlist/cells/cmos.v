// cmos.v: the cell library of the Phase 6.1 netlist fixtures (spec G-2). Port names are fixed by
// this file; cmos_map.v (the Yosys techmap) and the mapped netlists use them.

module BUF (input A, output Y);
    assign Y = A;
endmodule

module NOT (input A, output Y);
    assign Y = ~A;
endmodule

module NAND2 (input A, input B, output Y);
    assign Y = ~(A & B);
endmodule

module NOR2 (input A, input B, output Y);
    assign Y = ~(A | B);
endmodule

// Positive-edge flip-flop.
module DFF (input C, input D, output reg Q);
    always @(posedge C)
        Q <= D;
endmodule

// Positive-edge flip-flop with asynchronous set (S) and reset (R); reset wins.
module DFFSR (input C, input D, input S, input R, output reg Q);
    always @(posedge C, posedge S, posedge R)
        if (R)
            Q <= 1'b0;
        else if (S)
            Q <= 1'b1;
        else
            Q <= D;
endmodule
