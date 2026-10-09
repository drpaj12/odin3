// Switches to LEDs, inverted.
module top (
    input  wire [1:0] SW,
    output wire [1:0] LEDR
);
    assign LEDR = ~SW;
endmodule
