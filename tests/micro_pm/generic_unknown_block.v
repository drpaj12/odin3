// 4.2 B-6: 'foo' is not a model of the architecture (read with --auto-blackbox; the ports of an
// implicit black box are inout).
module generic_unknown_block (
    inout i,
    inout o
);
  foo u_foo (.i(i), .o(o));
endmodule
