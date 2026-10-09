// Hand-written Verilog equivalent of this case (GHDL cannot write it: it names both
// entities `module core`). alpha.core is an inverter, beta.core rotates left by one.
module alpha_core (
    input  wire [3:0] a,
    output wire [3:0] y
);
    assign y = ~a;
endmodule

module beta_core (
    input  wire [3:0] a,
    output wire [3:0] y
);
    assign y = {a[2:0], a[3]};
endmodule

module top (
    input  wire [3:0] a,
    output wire [3:0] y
);
    wire [3:0] t;

    alpha_core u_inv (.a(a), .y(t));
    beta_core u_rot (.a(t), .y(y));
endmodule
