// One explicit instance of each LPM cell, named ports and #(...) parameters (5.1 Task 3).
module read_instances(input [7:0] a, input [7:0] b, input clk, input [3:0] ad,
                      output [7:0] s, output lt, output [7:0] p, output [7:0] q);
  wire [15:0] prod;
  lpm_add_sub #(.lpm_width(8), .lpm_direction("SUB"), .lpm_representation("UNSIGNED")) add0 (
      .dataa(a), .datab(b), .result(s));
  lpm_compare #(.lpm_width(8), .lpm_representation("SIGNED")) cmp0 (
      .dataa(a), .datab(b), .alb(lt));
  lpm_mult #(.lpm_widtha(8), .lpm_widthb(8), .lpm_widthp(16)) mul0 (
      .dataa(a), .datab(b), .result(prod));
  assign p = prod[7:0];
  altsyncram #(.operation_mode("SINGLE_PORT"), .width_a(8), .widthad_a(4), .numwords_a(16),
               .intended_device_family("Stratix IV")) ram0 (
      .clock0(clk), .address_a(ad), .data_a(a), .wren_a(1'b1), .q_a(q));
endmodule
