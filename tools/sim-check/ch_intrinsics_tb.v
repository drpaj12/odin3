// Testbench for the VTR benchmark ch_intrinsics (top module memset): reset, then several
// start/finish rounds with different m, c, n; prints every output each cycle. The line-for-line
// comparison of iverilog on the RTL against `odin3 sim_tb` on the elaborated netlist is
// tools/sim-check/ch_intrinsics_vs_iverilog.sh.
`timescale 1 ns / 1 ns
module ch_intrinsics_tb;
  parameter CYCLES = 600;
  reg clk, reset, start;
  reg [31:0] m, c, n;
  integer cycle;
  wire finish;
  // memset instantiates its own memory_controller
  wire [31:0] return_val, memory_controller_address, memory_controller_in, memory_controller_out;
  wire memory_controller_write_enable;
  memset dut (.clk(clk), .reset(reset), .start(start), .finish(finish), .return_val(return_val),
              .m(m), .c(c), .n(n), .memory_controller_write_enable(memory_controller_write_enable),
              .memory_controller_address(memory_controller_address),
              .memory_controller_in(memory_controller_in),
              .memory_controller_out(memory_controller_out));
  initial begin clk = 0; reset = 1; start = 0; m = 0; c = 32'h5a; n = 8; cycle = 0; end
  always #5 clk = ~clk;
  always @(negedge clk) begin
    $display("%0d %b %h %h %b %h %h", cycle, finish, return_val, memory_controller_address,
             memory_controller_write_enable, memory_controller_in, memory_controller_out);
    #2;
    reset = cycle < 3;
    start = (cycle % 100) == 10;
    m = (cycle / 100) * 16;
    c = 32'h5a + cycle / 100;
    n = 4 + (cycle / 100) * 3;
    cycle = cycle + 1;
    if (cycle == CYCLES) $finish;
  end
endmodule
