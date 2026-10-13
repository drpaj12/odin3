// clean/unused_cell: a cell whose output nothing reads is removed
module t(input a, input b, output y);
  wire t = a & b;
  assign y = a | b;
endmodule
