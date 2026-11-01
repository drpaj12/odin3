// Port-only testbench for single_cycle_BRAM_top (Arduino-on-RISC-V): no hierarchical names, so it
// runs under iverilog and under Odin III sim_tb alike. One line per cycle at the falling clock
// edge: cycle PC PORTB_data_out PORTD_data_out. The two interrupt pins pulse at fixed cycles.
`timescale 1 ns / 1 ns
module mcu_tb;
  parameter CYCLES = 2000;
  reg clock;
  reg reset;
  reg start;
  reg scan;
  reg pind2;
  reg pind3;
  integer cycle;
  wire [31:0] pc;
  wire [7:0] portb;
  wire [7:0] portd;

  single_cycle_BRAM_top dut (
    .clock(clock), .reset(reset), .start(start), .PC(pc),
    .PORTB_data_out(portb), .PORTD_data_out(portd),
    .PIND2_in(pind2), .PIND3_in(pind3), .scan(scan)
  );

  initial begin
    clock = 0; reset = 1; start = 0; scan = 0; pind2 = 0; pind3 = 0; cycle = 0;
  end

  always #5 clock = ~clock;

  initial begin
    #52 reset = 0;
    #10 start = 1;
    #10 start = 0;
  end

  always @(negedge clock) begin
    $display("%0d %h %h %h", cycle, pc, portb, portd);
    pind2 = (cycle >= 200 && cycle < 206);
    pind3 = (cycle >= 400 && cycle < 406);
    cycle = cycle + 1;
    if (cycle == CYCLES) $finish;
  end
endmodule
