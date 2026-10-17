// 3C-2: the n-bit soft adder a design gets from Parmys's vtr_primitives.v (read with --lib), so
// that an `adder #(W)` instance flattens to `$add` cells (spec A-9).
module adder #(
    parameter WIDTH = 1
) (
    input [WIDTH-1:0] a,
    input [WIDTH-1:0] b,
    input cin,
    output cout,
    output [WIDTH-1:0] sumout
);
  assign {cout, sumout} = a + b + cin;
endmodule
