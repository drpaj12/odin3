// Behavioural model of the vendor (Xilinx) primitive `BUFG` (Odin III's own description, MIT). Read
// only with `read_verilog --vendor-prims`, for an instance of an undefined module of that name.
// The global clock buffer is a wire: the clock net is a net like any other, with no skew model.
module BUFG (
    output O,
    input I
);
    assign O = I;
endmodule
