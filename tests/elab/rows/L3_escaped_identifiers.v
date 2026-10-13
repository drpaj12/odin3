// L3: escaped identifiers — variants: \a+b with an operator byte, \bus[0] with brackets, \module and \wire (keywords as names), \1st (leading digit), \y equal to the plain y
module l3_escaped (input \a+b , \bus[0] , output \module , \1st , y);
  wire \wire ;
  assign \wire = \a+b ;
  assign \module = \wire ;
  assign \1st = \bus[0] ;
  assign \y = \a+b ;
endmodule
