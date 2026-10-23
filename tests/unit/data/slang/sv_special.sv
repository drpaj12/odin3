// Fixture: a module literally named m$2 must not collide with the name the adapter generates
// for the second parameterization of m (review Low); the second body becomes m$3.
module m #(parameter int W = 1) (
    input  wire [W-1:0] a,
    output wire [W-1:0] y
);
    assign y = a;
endmodule

module m$2 (
    input  wire a,
    output wire y
);
    assign y = a;
endmodule

module top (
    input  wire       a,
    input  wire [1:0] b,
    output wire       y0,
    output wire       y1,
    output wire [1:0] y2
);
    m$2 u_lit (.a(a), .y(y0));
    m #(.W(1)) u1 (.a(a), .y(y1));
    m #(.W(2)) u2 (.a(b), .y(y2));
endmodule
