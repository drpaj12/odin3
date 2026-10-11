// The -v file: a primitive foo (ignored: the design's module wins) and bar.
primitive foo (o, a);
    output o;
    input a;
    table
        0 : 1;
        1 : 0;
    endtable
endprimitive

module bar (input wire i, output wire o);
    assign o = i;
endmodule
