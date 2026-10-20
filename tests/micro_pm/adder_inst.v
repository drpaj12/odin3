// An explicit `adder` hard-block instance (VTR's primitive, one bit with carry in and out). On an
// arch with no `adder` model --vtr-compat builds it as soft logic (D18, agent default).
module adder_inst (
    input a,
    input b,
    input cin,
    output cout,
    output s
);
    adder u (.a(a), .b(b), .cin(cin), .cout(cout), .sumout(s));
endmodule
