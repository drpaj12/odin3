// Port-only testbench for darksocv (DarkRISCV): no hierarchical names, so it runs under iverilog
// and under Odin III sim_tb alike. One line per cycle at the falling clock edge:
// cycle UART_TXD LED OPORT and DEBUG[3:2] (bus request, halt; DEBUG[1], the data ack, is x in
// iverilog on every UART access, DEBUG[0] the instruction-ack toggle is left out). A UART
// receiver (8N1, BIT clocks per bit; prepare.sh sets 100 Mbit/s on the 100 MHz board clock, a
// divisor of 1, so the bit period is 2 clocks) prints "UART <cycle> <hex> <char>" for each received byte
// (a line that does not start with a digit, so the trace compare skips it; run.sh compares the
// decoded text itself). Reset is held for the first 1000 ns, as darksimv.v does.
`timescale 1 ns / 1 ns
module dark_tb;
  parameter CYCLES = 2000;
  parameter BIT = 2;
  reg clock;
  reg res;
  integer cycle;
  integer rx_cnt;
  integer rx_bit;
  reg [7:0] rx_data;
  reg rx_busy;
  wire txd;
  wire [31:0] led;
  wire [31:0] oport;
  wire [3:0] dbg;

  darksocv soc0 (
    .XCLK(clock), .XRES(res), .UART_RXD(1'b1), .UART_TXD(txd),
    .LED(led), .IPORT(32'd0), .OPORT(oport), .DEBUG(dbg)
  );

  initial begin
    clock = 0; res = 1; cycle = 0; rx_busy = 0; rx_cnt = 0; rx_bit = 0; rx_data = 8'd0;
  end

  always #5 clock = ~clock;

  initial #1000 res = 0;

  always @(negedge clock) begin
    $display("%0d %b %h %h %b %b", cycle, txd, led, oport, dbg[3], dbg[2]);
    if (!rx_busy) begin
      if (txd === 1'b0) begin
        rx_busy = 1; rx_cnt = BIT + BIT / 2; rx_bit = 0;
      end
    end else begin
      rx_cnt = rx_cnt - 1;
      if (rx_cnt == 0) begin
        rx_cnt = BIT;
        if (rx_bit < 8) begin
          rx_data = {txd, rx_data[7:1]};
          rx_bit = rx_bit + 1;
        end else begin
          if (rx_data >= 8'd32 && rx_data < 8'd127) $display("UART %0d %h %c", cycle, rx_data, rx_data);
          else $display("UART %0d %h .", cycle, rx_data);
          rx_busy = 0;
        end
      end
    end
    cycle = cycle + 1;
    if (cycle == CYCLES) $finish;
  end
endmodule
