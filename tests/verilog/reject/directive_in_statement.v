// A pass-through directive inside a statement.
module m (input a, output reg y);
  always @* begin
    `celldefine
    y = a;
  end
endmodule
