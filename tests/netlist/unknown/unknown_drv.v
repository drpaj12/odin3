// unknown_drv: two unknown cells wired to driven nets (spec G-7, EL-11). MYSTERY takes two module
// inputs and shares net w with OTHER, which drives a module output. With --auto-blackbox the port
// directions come from votes: A, B are inputs (module inputs), OTHER.Q is an output (module output
// reads it); nothing known says which of MYSTERY.Q and OTHER.D drives w, but MYSTERY has no output
// and OTHER has one, so the tie-break makes MYSTERY.Q the output (the WARNING says so).
module unknown_drv (input a, input b, output y);
    wire w;
    MYSTERY u1 (.A(a), .B(b), .Q(w));
    OTHER u2 (.D(w), .Q(y));
endmodule
