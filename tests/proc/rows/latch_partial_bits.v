// latch_partial_bits: latch on part of a vector
// yosys -V: Yosys 0.55 (git sha1 60f126cd0, g++ 13.3.0-6ubuntu2~24.04.1 -fPIC -O3)
module t (input en, input [3:0] d, output reg [7:0] y);
  always @* begin
    y[7:4] = d;
    if (en) y[3:0] = ~d;
  end
endmodule
