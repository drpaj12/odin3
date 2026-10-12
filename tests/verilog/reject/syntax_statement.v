// A syntax error in a statement: an assignment without its right-hand side.
module m (input a, output reg y);
  always @* begin
    y = ;
  end
endmodule
