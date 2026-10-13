// T9: one variable assigned with both = and <= on one control path (accepted, with a warning).
module m (input c, a, b, output reg r);
  always @(posedge c) begin
    r = a;
    r <= b;
  end
endmodule
