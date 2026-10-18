// 3C-2: unsigned 8-bit minus 5-bit into 9 bits; Y[8] is the borrow (Parmys loses it, probe subwide).
module sub_pad_u (
    input  [7:0] a,
    input  [4:0] b,
    output [8:0] y
);
  assign y = {1'b0, a} - {4'b0, b};
endmodule
