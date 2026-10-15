module subprobe (input [7:0] ua, input [7:0] ub, input signed [7:0] sa, input signed [7:0] sb,
  output [7:0] uy, output [8:0] uy9, output signed [7:0] sy, output signed [8:0] sy9);
  assign uy = ua - ub;
  assign uy9 = {1'b0,ua} - {1'b0,ub};
  assign sy = sa - sb;
  assign sy9 = sa - sb;
endmodule
