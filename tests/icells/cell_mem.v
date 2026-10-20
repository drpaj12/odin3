// 5.2: a memory with initial contents, one write port and two read ports (the word level keeps
// a $mem instance with its INIT bits and port arrays).
module cell_mem(input clk, input we, input [3:0] wa, input [7:0] wd, input [3:0] ra, input [3:0] rb,
                output [7:0] qa, output reg [7:0] qb);
  reg [7:0] mem [0:15];
  initial begin
    mem[0] = 8'h12;
    mem[1] = 8'hA5;
    mem[15] = 8'hFF;
  end
  always @(posedge clk) begin
    if (we) mem[wa] <= wd;
    qb <= mem[rb];
  end
  assign qa = mem[ra];
endmodule
