// See ping.v.
module pong (
    input  wire a,
    output wire y
);
    ping u_ping (.a(a), .y(y));
endmodule
