// A register with an initial value and no reset: the netlist keeps the power-up value as an
// `initial` (write_verilog --quartus), so it equals the RTL from time 0.
module ff_init(
    input clk,
    input [3:0] d,
    output reg [3:0] q_word,
    output reg q_bit,
    output reg [3:0] q_en
);
    initial q_word = 4'b0101;
    initial q_bit = 1'b1;
    initial q_en = 4'd9;
    always @(posedge clk) q_word <= q_word ^ d;
    always @(posedge clk) q_bit <= ~q_bit;
    always @(posedge clk) if (d[0]) q_en <= ~q_en ^ d;
endmodule
