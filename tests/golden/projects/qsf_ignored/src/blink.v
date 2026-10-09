// LED blinker: a 24-bit prescaler drives one output.
module blink (
    input  wire clk,
    output wire led
);
    reg [23:0] count = 24'd0;

    always @(posedge clk)
        count <= count + 24'd1;
    assign led = count[23];
endmodule
