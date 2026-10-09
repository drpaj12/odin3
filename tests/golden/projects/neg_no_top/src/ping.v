// ping instantiates pong and pong instantiates ping: no module is uninstantiated.
module ping (
    input  wire a,
    output wire y
);
    pong u_pong (.a(a), .y(y));
endmodule
