// Parity of a byte through a sub-module; the project files live in proj/.
module top (
    input  wire [7:0] d,
    output wire       p
);
    parity u_par (.d(d), .p(p));
endmodule
