// force / release of a wire (driven by an assign) and of a reg; a force follows its operands
module force_tb;
  reg [3:0] a, r, src;
  wire [3:0] w;
  assign w = a + 1;
  initial begin
    a = 1; r = 5; src = 9;
    #1 $display("w=%0d r=%0d", w, r);
    force w = 4'd12;
    force r = src;
    #1 $display("forced w=%0d r=%0d", w, r);
    a = 2; r = 7; src = 3;
    #1 $display("still forced w=%0d r=%0d", w, r);
    release w;
    release r;
    #1 $display("released w=%0d r=%0d", w, r);
    r = 6;
    #1 $display("assigned r=%0d", r);
    force w = a;
    force r = a + 1;
    #1 $display("w=%0d", w);
    a = 8;
    #1 $display("follows w=%0d once r=%0d", w, r);
    $finish;
  end
endmodule
