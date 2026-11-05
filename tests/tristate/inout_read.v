// inout only read: nothing drives it inside, so it becomes an input.
module top(inout p, output q, output r);
    assign q = ~p;
    assign r = p;
endmodule
