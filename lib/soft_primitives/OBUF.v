// Behavioural model of the vendor (Xilinx) primitive `OBUF` (Odin III's own description, MIT). Read
// only with `read_verilog --vendor-prims`, for an instance of an undefined module of that name.
// The output buffer is a wire; the attributes are accepted and ignored.
module OBUF #(
    parameter DRIVE = 12,
    parameter IOSTANDARD = "DEFAULT",
    parameter SLEW = "SLOW"
) (
    output O,
    input I
);
    assign O = I;
endmodule
