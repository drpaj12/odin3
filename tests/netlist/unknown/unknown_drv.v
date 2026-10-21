// unknown_drv: two unknown cells wired to driven nets (spec G-7, EL-11). MYSTERY takes two module
// inputs and drives a net that OTHER reads; OTHER drives a module output. With --auto-blackbox the
// port directions come from the nets: A, B and D are inputs, Q is an output (both cells).
module unknown_drv (input a, input b, output y);
    wire w;
    MYSTERY u1 (.A(a), .B(b), .Q(w));
    OTHER u2 (.D(w), .Q(y));
endmodule
