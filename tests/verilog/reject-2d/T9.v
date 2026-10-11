// T9: one variable assigned with both = and <= (REJECT).
module m (input c, a, b, output reg r);
  always @(posedge c) begin
    r = a;
    r <= b;
  end
endmodule
