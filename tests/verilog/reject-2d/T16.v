// T16: a procedural continuous assignment (REJECT).
module m (input a, output reg r);
  initial begin
    assign r = a;
  end
endmodule
