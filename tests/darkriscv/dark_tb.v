// Port-only testbench for darksocv (DarkRISCV): no hierarchical names, so it runs under iverilog
// and under Odin III sim_tb alike. One line per cycle at the falling clock edge:
// cycle UART_TXD LED OPORT and three of the four DEBUG bits
// (bus request, halt, instruction ack; DEBUG[1], the data ack, is x in iverilog on every UART access). Reset is held for the first 1000 ns, as darksimv.v does.
`timescale 1 ns / 1 ns
module dark_tb;
  parameter CYCLES = 2000;
  reg clock;
  reg res;
  integer cycle;
  wire txd;
  wire [31:0] led;
  wire [31:0] oport;
  wire [3:0] dbg;

  darksocv soc0 (
    .XCLK(clock), .XRES(res), .UART_RXD(1'b1), .UART_TXD(txd),
    .LED(led), .IPORT(32'd0), .OPORT(oport), .DEBUG(dbg)
  );

  initial begin
    clock = 0; res = 1; cycle = 0;
  end

  always #5 clock = ~clock;

  initial #1000 res = 0;

  always @(negedge clock) begin
    $display("%0d %b %h %h %b %b %b", cycle, txd, led, oport, dbg[3], dbg[2], dbg[0]);
    cycle = cycle + 1;
    if (cycle == CYCLES) $finish;
  end
endmodule
