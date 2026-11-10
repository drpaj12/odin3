// $dlatch with each enable polarity and $adlatch.
module latch(
    input en,
    input arst,
    input [3:0] d,
    output reg [3:0] q_hi,
    output reg [3:0] q_lo,
    output reg [3:0] q_ar
);
    always @* if (en) q_hi = d;
    always @* if (!en) q_lo = d;
    always @* if (arst) q_ar = 4'd6; else if (en) q_ar = d;
endmodule
