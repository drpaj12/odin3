// 3C-2: an explicit `adder #(4)` instance (the soft unit of adder_lib.v).
module adder_use (
    input  [3:0] a,
    input  [3:0] b,
    input        cin,
    output       cout,
    output [3:0] s
);
  adder #(.WIDTH(4)) u (.a(a), .b(b), .cin(cin), .cout(cout), .sumout(s));
endmodule
