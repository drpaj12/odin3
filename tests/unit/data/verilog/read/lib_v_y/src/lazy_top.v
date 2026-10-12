module top (input wire a, output wire y);
    sub u_sub (.a(a), .y(y));
endmodule

module sub (input wire a, output wire y);
    assign y = a;
endmodule
