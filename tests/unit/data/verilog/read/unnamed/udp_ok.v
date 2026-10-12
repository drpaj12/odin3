// An unnamed instance of a UDP of the same file is legal (1364-2005 §12.1.2).
primitive myudp (o, a);
    output o;
    input a;
    table
        0 : 1;
        1 : 0;
    endtable
endprimitive

module top (input wire a, output wire y);
    myudp (y, a);
endmodule
