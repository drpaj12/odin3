module memx_tb;
  reg clk, we, we2;
  reg [2:0] wa, wa2, ra;
  reg [3:0] d, d2, be;
  wire [3:0] qa, q;
  integer i;
  memx dut(.clk(clk), .we(we), .we2(we2), .wa(wa), .wa2(wa2), .ra(ra), .d(d), .d2(d2), .be(be), .qa(qa), .q(q));
  always #5 clk = ~clk;
  initial begin
    clk = 0; we = 0; we2 = 0; wa = 0; wa2 = 0; ra = 0; d = 0; d2 = 0; be = 4'hf;
    for (i = 0; i < 8; i = i + 1) begin ra = i; @(posedge clk); #1 $display("init ra=%b qa=%b q=%b", ra, qa, q); end
    // x read addresses
    ra = 3'bxxx; @(posedge clk); #1 $display("xa ra=%b qa=%b q=%b", ra, qa, q);
    ra = 3'b0x0; @(posedge clk); #1 $display("x0x ra=%b qa=%b q=%b", ra, qa, q);
    ra = 3'b1xx; @(posedge clk); #1 $display("1xx ra=%b qa=%b q=%b", ra, qa, q);
    ra = 3'bx11; @(posedge clk); #1 $display("x11 ra=%b qa=%b q=%b", ra, qa, q);
    ra = 3'bz01; @(posedge clk); #1 $display("z01 ra=%b qa=%b q=%b", ra, qa, q);
    // x write enable of the whole write
    @(negedge clk); wa = 2; d = 4'h9; we = 1'bx; ra = 2;
    @(posedge clk); #1 $display("xwe ra=%b qa=%b q=%b", ra, qa, q);
    @(negedge clk); we = 0; wa = 2; ra = 2;
    @(posedge clk); #1 $display("after ra=%b qa=%b q=%b", ra, qa, q);
    // x write address poisons the words it may reach
    @(negedge clk); we = 1; wa = 3'b01x; d = 4'h6; ra = 0;
    @(posedge clk); #1 $display("xwa ra=%b qa=%b q=%b", ra, qa, q);
    for (i = 0; i < 8; i = i + 1) begin ra = i; @(posedge clk); #1 $display("scan ra=%b qa=%b q=%b", ra, qa, q); end
    // an x bit of the byte enable
    @(negedge clk); we = 1; wa = 4; be = 4'b10x1; d = 4'h5; ra = 4;
    @(posedge clk); #1 $display("xbe ra=%b qa=%b q=%b", ra, qa, q);
    // second port wins; x data
    @(negedge clk); we = 1; wa = 5; be = 4'hf; d = 4'ha; we2 = 1; wa2 = 5; d2 = 4'bx01x; ra = 5;
    @(posedge clk); #1 $display("two ra=%b qa=%b q=%b", ra, qa, q);
    @(negedge clk); we = 0; we2 = 1; wa2 = 3'bxxx; d2 = 4'h3;
    @(posedge clk); #1 $display("xwa2 ra=%b qa=%b q=%b", ra, qa, q);
    for (i = 0; i < 8; i = i + 1) begin ra = i; @(posedge clk); #1 $display("end ra=%b qa=%b q=%b", ra, qa, q); end
    $finish;
  end
endmodule
