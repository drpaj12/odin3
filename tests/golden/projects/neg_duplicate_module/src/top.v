// top and helper; src/other.v defines helper again.
module top (
    input  wire a,
    output wire y
);
    helper u_helper (.a(a), .y(y));
endmodule

module helper (
    input  wire a,
    output wire y
);
    assign y = ~a;
endmodule
