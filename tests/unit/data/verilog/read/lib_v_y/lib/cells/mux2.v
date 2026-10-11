// A library module instantiating another library module (inv2, also in lib/cells).
module mux2 (input wire a, input wire b, input wire s, output wire y);
    wire sn;
    inv2 u_inv (.a(s), .y(sn));
    assign y = (a & sn) | (b & s);
endmodule
