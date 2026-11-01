// A scratch variable T that two always blocks assign (blocking) and read only after their own
// assignment: Yosys gives each block a flip-flop nothing reads; Odin III used to refuse it (T10).
// Cut down from ITC-99 b15 (TEMPORARY). Front-end triage 2026-10-31.
module t (input clk, input [7:0] a, input [7:0] b, output reg [3:0] x, output reg [3:0] y);
  reg [7:0] T;
  always @(posedge clk) begin
    T = a + 8'd1;
    if (T[0]) x <= T[3:0];
    else x <= T[7:4];
  end
  always @(posedge clk) begin
    T = b + 8'd2;
    y <= T[7:4];
  end
endmodule
