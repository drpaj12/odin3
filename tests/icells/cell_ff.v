// 5.2: set/reset flip-flop ($dffsr), asynchronous reset ($adff), a latch ($dlatch), a
// synchronous reset with enable.
module cell_ff(input clk, input en, input arst, input set, input srst, input [3:0] d,
               output reg [3:0] q_sr, output reg [3:0] q_ar, output reg [3:0] q_sy, output reg [3:0] q_l);
  always @(posedge clk or posedge arst or posedge set)
    if (arst) q_sr <= 4'h0; else if (set) q_sr <= 4'hF; else q_sr <= d;
  always @(posedge clk or posedge arst)
    if (arst) q_ar <= 4'h5; else q_ar <= d;
  always @(posedge clk)
    if (srst) q_sy <= 4'h0; else if (en) q_sy <= d;
  always @* if (en) q_l = d;
endmodule
