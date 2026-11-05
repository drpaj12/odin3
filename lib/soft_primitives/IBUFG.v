// Behavioural model of the vendor (Xilinx) primitive `IBUFG` (Odin III's own description, MIT).
// Read only with `read_verilog --vendor-prims`, for an instance of an undefined module of that
// name. The clock input buffer is a wire; the attributes are accepted and ignored.
module IBUFG #(
    parameter IBUF_DELAY_VALUE = "0",
    parameter IOSTANDARD = "DEFAULT"
) (
    output O,
    input I
);
    assign O = I;
endmodule
