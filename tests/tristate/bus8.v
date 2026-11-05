// a bus of the top inout: eight bits, each driven by its own enable pair (the a-z80 shape).
module top(inout [3:0] db, input oe_a, input oe_b, input [3:0] a, input [3:0] b, output [3:0] q);
    assign db = oe_a ? a : 4'bz;
    assign db = oe_b ? b : 4'bz;
    assign q = db;
endmodule
