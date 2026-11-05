// a tri-state through a submodule port: two instances drive one internal bus (needs flatten).
module drv(input en, input d, inout b);
    assign b = en ? d : 1'bz;
endmodule
module top(input e1, input e2, input d1, input d2, output y);
    wire bus;
    drv u1(.en(e1), .d(d1), .b(bus));
    drv u2(.en(e2), .d(d2), .b(bus));
    assign y = ~bus;
endmodule
