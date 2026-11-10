// $dff on each clock edge and $dffe with each enable polarity (write_verilog --quartus).
module ff_plain(
    input clk,
    input en,
    input [3:0] d,
    output reg [3:0] q_pos,
    output reg [3:0] q_neg,
    output reg [3:0] q_en_hi,
    output reg [3:0] q_en_lo
);
    always @(posedge clk) q_pos <= d;
    always @(negedge clk) q_neg <= d;
    always @(posedge clk) if (en) q_en_hi <= d;
    always @(posedge clk) if (!en) q_en_lo <= d;
endmodule
