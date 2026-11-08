// EXPECT: name longer than 63 characters
module sublong_tb;
  leaf instance_name_that_is_long_but_below_the_lexer_limit_abcdefg (.a(1'b0));
  initial #1 $finish;
endmodule
module leaf(input a);
  reg a_register_with_a_long_name_too;
endmodule
