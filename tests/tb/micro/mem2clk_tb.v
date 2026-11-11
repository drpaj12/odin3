// The write clock and the read clock take edges alone and together (the read sees the memory as
// it was before a write of the same instant), against iverilog.
module mem2clk_tb;
  reg wclk = 0, rclk = 0, we = 0;
  reg [2:0] wa = 0, ra = 0;
  reg [7:0] wd = 0;
  wire [7:0] rq, aq;
  integer i;
  mem2clk dut(.wclk(wclk), .rclk(rclk), .we(we), .wa(wa), .wd(wd), .ra(ra), .rq(rq), .aq(aq));
  initial begin
    // every word and the read register get a value first (2-state runs print no x)
    for (i = 0; i < 8; i = i + 1) begin
      #1 we = 1; wa = i; wd = i * 17 + 3;
      #1 wclk = 1;
      #1 wclk = 0;
    end
    #1 ra = 2;
    #1 rclk = 1;
    #1 rclk = 0;
    for (i = 0; i < 48; i = i + 1) begin
      #1;
      we = (i % 3) != 2;
      wa = (i * 5 + i / 4) % 8;
      wd = i * 37 + 11;
      ra = (i * 3 + i / 5) % 8;
      #1;
      case (i % 4)
        0: wclk = 1;
        1: rclk = 1;
        2: begin wclk = 1; rclk = 1; end
        3: begin rclk = 1; wclk = 1; end
      endcase
      #1;
      $display("%0d wa=%0d ra=%0d rq=%h aq=%h", i, wa, ra, rq, aq);
      wclk = 0; rclk = 0;
    end
    $finish;
  end
endmodule
