// A function and a task with a local array: whole-word writes, a bit of a word written, a
// variable word read; each call has its own array.
module t (input clk, input [3:0] s, input [7:0] d, output [7:0] y, output [7:0] z,
          output reg [7:0] q);
  function [7:0] f(input [3:0] k);
    reg [7:0] m [7:0];
    integer i;
    begin
      for (i = 0; i < 8; i = i + 1) begin
        m[i] = 8'd0;
        m[i][i] = 1'b1;
      end
      f = m[k];
    end
  endfunction
  task g(input [3:0] k, output [7:0] r);
    reg [7:0] w [0:1];
    begin
      w[0] = d;
      w[1] = ~d;
      r = w[k[0]];
    end
  endtask
  assign y = f(s) ^ d;
  assign z = f(3) ^ f(5);
  reg [7:0] r;
  always @(posedge clk) begin
    g(s, r);
    q <= r ^ f(s);
  end
endmodule
