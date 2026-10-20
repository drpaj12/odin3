// The RTL of adder_inst, the reference for the soft `adder` (D18).
module adder_inst (
    input a,
    input b,
    input cin,
    output cout,
    output s
);
    assign {cout, s} = a + b + cin;
endmodule
