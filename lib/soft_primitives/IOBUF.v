// Behavioural model of the vendor (Xilinx) primitive `IOBUF` (Odin III's own description, MIT).
// Read only with `read_verilog --vendor-prims`, for an instance of an undefined module of that
// name. The bidirectional buffer drives IO with I while T is low (high impedance while T is high)
// and always reads IO into O; the tri-state pass turns the driver into logic below the top. The
// attributes are accepted and ignored.
module IOBUF #(
    parameter DRIVE = 12,
    parameter IBUF_DELAY_VALUE = "0",
    parameter IFD_DELAY_VALUE = "AUTO",
    parameter IOSTANDARD = "DEFAULT",
    parameter SLEW = "SLOW"
) (
    output O,
    inout IO,
    input I,
    input T
);
    assign IO = T ? 1'bz : I;
    assign O = IO;
endmodule
