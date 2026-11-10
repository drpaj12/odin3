// $sdff with each reset polarity, $sdffe (reset beats enable) and $sdffce (enable gates reset).
module ff_sync(
    input clk,
    input srst,
    input srst_n,
    input en,
    input [3:0] d,
    output reg [3:0] q_hi,
    output reg [3:0] q_lo,
    output reg [3:0] q_re,
    output reg [3:0] q_ce
);
    always @(posedge clk) if (srst) q_hi <= 4'd6; else q_hi <= d;
    always @(posedge clk) if (!srst_n) q_lo <= 4'd15; else q_lo <= d;
    always @(posedge clk) if (srst) q_re <= 4'd2; else if (en) q_re <= d;
    always @(posedge clk) if (en) begin if (srst) q_ce <= 4'd1; else q_ce <= d; end
endmodule
