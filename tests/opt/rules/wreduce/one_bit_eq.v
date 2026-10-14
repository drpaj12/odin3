// $eq with a 8-bit Y: only bit 0 is a result (wreduce pre-step, one-bit results)
module one_bit_eq(input [7:0] a, input [7:0] b, output [7:0] y);
  assign y = a == b;
endmodule
