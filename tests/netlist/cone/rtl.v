// cone: a combinational cone with reconvergence (t feeds both y0 and y1).
module cone (input a, input b, input c, input d, output y0, output y1);
    wire t = (a & b) | (c ^ d);
    assign y0 = t & a;
    assign y1 = ~t | c;
endmodule
