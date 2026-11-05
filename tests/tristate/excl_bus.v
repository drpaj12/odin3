// two exclusive drivers of an internal net.
module top(input e1, input e2, input d1, input d2, output y);
    wire b;
    assign b = e1 ? d1 : 1'bz;
    assign b = e2 ? d2 : 1'bz;
    assign y = ~b;
endmodule
