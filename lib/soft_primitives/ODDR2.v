// Behavioural model of the vendor (Xilinx Spartan-6) primitive `ODDR2` (Odin III's own description,
// MIT). Read only with `read_verilog --vendor-prims`, for an instance of an undefined module of
// that name. D0 is captured on the rising edge of C0 and D1 on the rising edge of C1; Q shows D0
// while C0 is high and D1 while C1 is high, which is the vendor's behaviour when C1 is the
// inverse of C0 (the only way the OpenCores designs use it). R and S are synchronous (SRTYPE
// "ASYNC" is read as "SYNC"); R wins over S; CE gates both captures. The attributes are accepted;
// INIT is not applied (no initial value in the soft logic).
module ODDR2 #(
    parameter DDR_ALIGNMENT = "NONE",
    parameter INIT = 1'b0,
    parameter SRTYPE = "SYNC"
) (
    output Q,
    input C0,
    input C1,
    input CE,
    input D0,
    input D1,
    input R,
    input S
);
    reg q0;
    reg q1;
    always @(posedge C0) begin
        if (R) q0 <= 1'b0;
        else if (S) q0 <= 1'b1;
        else if (CE) q0 <= D0;
    end
    always @(posedge C1) begin
        if (R) q1 <= 1'b0;
        else if (S) q1 <= 1'b1;
        else if (CE) q1 <= D1;
    end
    assign Q = C0 ? q0 : q1;
endmodule
