// an inout of the top driven through a submodule (the i2cslave shape).
module drv(input en, input d, inout b);
    assign b = en ? d : 1'bz;
endmodule
module top(inout sda, input oe, input d, output q);
    drv u(.en(oe), .d(d), .b(sda));
    assign q = sda;
endmodule
