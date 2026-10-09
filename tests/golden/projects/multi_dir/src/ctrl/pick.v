// Registered 2:1 multiplexer.
module pick (
    input  wire       clk,
    input  wire       sel,
    input  wire [4:0] d0,
    input  wire [4:0] d1,
    output reg  [4:0] q
);
    always @(posedge clk)
        q <= sel ? d1 : d0;
endmodule
