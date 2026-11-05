// the sdr_ctrl shape: a replicated z, sized by a parameter, on an inout bus read back inside.
module top #(parameter DW = 4) (inout [DW-1:0] dq, input den, input [DW-1:0] dout, output [DW-1:0] din);
    assign dq = (den == 1'b0) ? dout : {DW{1'bz}};
    assign din = dq;
endmodule
