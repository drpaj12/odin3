// clean/purge_wire: -purge: an unused public wire is deleted
// options: -purge
module t(input a, input b, output y);
  wire keep_me = a & b;
  assign y = a | b;
endmodule
