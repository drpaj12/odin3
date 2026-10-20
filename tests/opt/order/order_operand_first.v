// WR-7 (amended, PHASE3 #51): wreduce visits a cell after its operands, as Yosys's front end creates
// them. The $sub for mdivk-1 narrows to A2:B1 first, then the $add becomes signed A3:B4 and the next
// $sub signed A5:B3. Visited in node-ID order (the elaborator numbers a consumer first) the $add and
// the second $sub stay 32 bits wide: 69 adder stages after hardmap against Parmys's 16.
module order_operand_first(input clk, input [4:0] counter, input [1:0] mdivk, output reg u);
  always @(posedge clk) u <= (counter == mdivk - 1) ^ (counter == mdivk - 1 + 5 - 2);
endmodule
