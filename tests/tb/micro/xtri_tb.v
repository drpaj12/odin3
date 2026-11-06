// A testbench wire with several drivers (two assigns and a DUT output) resolves per bit: z gives
// way, equal values stay, a 0/1 clash or an x gives x.
module xtri_tb;
  reg en1, en2, oe;
  reg [7:0] d1, d2, d;
  wire [7:0] bus;
  wire [7:0] lone;
  assign bus = en1 ? d1 : 8'bz;
  assign bus = en2 ? d2 : 8'bzzzz_zzzz;
  xtri dut(.oe(oe), .d(d), .y(bus));
  initial begin
    en1 = 0; en2 = 0; oe = 0; d1 = 8'h0f; d2 = 8'h3c; d = 8'h55;
    #1 $display("none   %b lone %b", bus, lone);
    en1 = 1;
    #1 $display("en1    %b", bus);
    en2 = 1;
    #1 $display("en1+2  %b", bus);
    en1 = 0;
    #1 $display("en2    %b", bus);
    oe = 1;
    #1 $display("en2+oe %b", bus);
    en2 = 0;
    #1 $display("oe     %b", bus);
    d = 8'b01xz01xz;
    #1 $display("oe d=x %b", bus);
    en1 = 1'bx;
    #1 $display("en1=x  %b", bus);
    $finish;
  end
endmodule
