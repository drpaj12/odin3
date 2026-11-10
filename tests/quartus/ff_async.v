// $adff with active-high and active-low reset, reset values 0, 5 and 15; $adffe.
module ff_async(
    input clk,
    input arst,
    input arst_n,
    input en,
    input [3:0] d,
    output reg [3:0] q_hi0,
    output reg [3:0] q_hi5,
    output reg [3:0] q_lo15,
    output reg [3:0] q_en
);
    always @(posedge clk or posedge arst) if (arst) q_hi0 <= 4'd0; else q_hi0 <= d;
    always @(posedge clk or posedge arst) if (arst) q_hi5 <= 4'd5; else q_hi5 <= d;
    always @(posedge clk or negedge arst_n) if (!arst_n) q_lo15 <= 4'd15; else q_lo15 <= d;
    always @(posedge clk or posedge arst) if (arst) q_en <= 4'd9; else if (en) q_en <= d;
endmodule
