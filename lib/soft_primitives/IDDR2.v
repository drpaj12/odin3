// Behavioural model of the vendor (Xilinx Spartan-6) primitive `IDDR2` (Odin III's own description,
// MIT). Read only with `read_verilog --vendor-prims`, for an instance of an undefined module of
// that name. Q0 captures D on the rising edge of C0 and Q1 on the rising edge of C1 (DDR_ALIGNMENT
// "NONE"; the other alignments are read as "NONE"). R and S are synchronous (SRTYPE "ASYNC" is read
// as "SYNC"); R wins over S; CE gates both captures. INIT_Q0 and INIT_Q1 are accepted, not applied.
module IDDR2 #(
    parameter DDR_ALIGNMENT = "NONE",
    parameter INIT_Q0 = 1'b0,
    parameter INIT_Q1 = 1'b0,
    parameter SRTYPE = "SYNC"
) (
    output reg Q0,
    output reg Q1,
    input C0,
    input C1,
    input CE,
    input D,
    input R,
    input S
);
    always @(posedge C0) begin
        if (R) Q0 <= 1'b0;
        else if (S) Q0 <= 1'b1;
        else if (CE) Q0 <= D;
    end
    always @(posedge C1) begin
        if (R) Q1 <= 1'b0;
        else if (S) Q1 <= 1'b1;
        else if (CE) Q1 <= D;
    end
endmodule
