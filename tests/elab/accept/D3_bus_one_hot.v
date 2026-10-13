// D3 (accepted, 2D-7b): three drivers on an 8-bit internal bus, one hot enables, a reader behind.
module d3_bus (input [7:0] d0, d1, d2, input [1:0] sel, output [7:0] y);
  wire [7:0] bus;
  assign bus = (sel == 2'd0) ? d0 : 8'bz;
  assign bus = (sel == 2'd1) ? d1 : 8'bz;
  assign bus = (sel == 2'd2) ? d2 : 8'bz;
  assign y = ~bus;
endmodule
