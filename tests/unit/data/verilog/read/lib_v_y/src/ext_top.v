// and2 and mux2 come from the -y directory lib/cells.
module top (input wire a, input wire b, input wire s, output wire y);
    wire n;
    and2 u_and (.a(a), .b(b), .y(n));
    mux2 u_mux (.a(a), .b(n), .s(s), .y(y));
endmodule
