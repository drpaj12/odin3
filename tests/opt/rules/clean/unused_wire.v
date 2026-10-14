// clean/unused_wire: a $-named wire left by a removed cell is deleted
module t(input a, input b, output y);
  assign y = (a & b) | a;
  wire t = a ^ b;
endmodule
