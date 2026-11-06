// Conditions with x: if/while/for/wait take x as false, case compares with ===, casez and casex
// mask z (and ?) or x and z, ?: merges both sides on an unknown condition.
module xcond_tb;
  reg c;
  reg [3:0] n;
  reg [7:0] v;
  integer i;
  initial begin
    c = 1'bx;
    n = 4'bx;
    $display("ter %b %b", c ? 8'b10101010 : 8'b10011001, n[0] ? 4'b1100 : 4'b1100);
    c = 1'bz;
    $display("ter %b", c ? 8'b1z1x1010 : 8'b1z0x1001);
    if (c) $display("if then"); else $display("if else");
    if (!c) $display("if! then"); else $display("if! else");
    if (n == 4'b0000) $display("n0"); else $display("n!0");
    case (c)
      1'b0: $display("case 0");
      1'bx: $display("case x");
      1'bz: $display("case z");
      default: $display("case default");
    endcase
    case (n)
      4'b0000: $display("n is 0");
      4'bxxxx: $display("n is x");
      default: $display("n default");
    endcase
    casez (8'b1010zzzz)
      8'b1010????: $display("casez 1");
      default: $display("casez default");
    endcase
    casez (8'b1010xxxx)
      8'b1010????: $display("casez 2");
      default: $display("casez default 2");
    endcase
    casez (8'b10100110)
      8'b0???????: $display("casez 3a");
      8'b1?1?z11?: $display("casez 3b");
      default: $display("casez default 3");
    endcase
    casez (8'b1010x110)
      8'b10100110: $display("casez 4a");
      default: $display("casez default 4");
    endcase
    casex (8'b1010xxxx)
      8'b10100000: $display("casex 1");
      default: $display("casex default");
    endcase
    casex (8'b10100000)
      8'b1x1z0000: $display("casex 2");
      default: $display("casex default 2");
    endcase
    casex (8'b10100000)
      8'b0x1z0000: $display("casex 3");
      default: $display("casex default 3");
    endcase
    i = 0;
    while (c) begin $display("while %0d", i); i = i + 1; c = 0; end
    for (i = 0; n < 4; i = i + 1) $display("for %0d", i);
    v = 8'bx;
    wait (v[0] !== 1'bx) $display("wait done %b", v);
    $finish;
  end
  initial begin
    #3 v[0] = 1'b0;
  end
endmodule
