// Behavioural model of the vendor (Xilinx) primitive `OBUFT` (Odin III's own description, MIT).
// Read only with `read_verilog --vendor-prims`, for an instance of an undefined module of that
// name. The 3-state output buffer drives I while T is low and is high impedance while T is high;
// the tri-state pass turns a driver below the top into logic. The attributes are ignored.
module OBUFT #(
    parameter DRIVE = 12,
    parameter IOSTANDARD = "DEFAULT",
    parameter SLEW = "SLOW"
) (
    output O,
    input I,
    input T
);
    assign O = T ? 1'bz : I;
endmodule
