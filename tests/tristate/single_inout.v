// inout read and write at the top: one tri-state driver of the pin, the pin read back inside.
module top(inout p, input oe, input d, output q);
    assign p = oe ? d : 1'bz;
    assign q = ~p;
endmodule
