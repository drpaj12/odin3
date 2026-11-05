// Behavioural model of the vendor (Xilinx) primitive `IBUF` (Odin III's own description, MIT). Read
// only with `read_verilog --vendor-prims`, for an instance of an undefined module of that name.
// The input buffer is a wire; the attributes are accepted and ignored.
module IBUF #(
    parameter IBUF_DELAY_VALUE = "0",
    parameter IFD_DELAY_VALUE = "AUTO",
    parameter IOSTANDARD = "DEFAULT"
) (
    output O,
    input I
);
    assign O = I;
endmodule
