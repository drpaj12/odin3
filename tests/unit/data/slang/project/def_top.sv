// WIDTH is defined by the project (tests/unit/data/slang/project/define.o3proj).
module def_top (
    input  logic [`WIDTH-1:0] a,
    output wire  [`WIDTH-1:0] y
);
    assign y = a;
endmodule
