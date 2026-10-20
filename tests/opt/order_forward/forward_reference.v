// WR-7 (amended, PHASE3 #51): a forward reference keeps the statements' order. `t2` is used before
// the assign that defines it, and `t2 = t1 + 5 - 2` before the one for `t1`; Yosys's front end makes
// the cells in statement order (the consumer's first), so its wreduce leaves $add and $sub 32 bits
// wide (measured with Yosys's own front end). The order walk does not follow a net that carries a
// wire (`t1`, `t2`), so Odin III keeps node-ID order here and agrees; following every driver would
// narrow them (the order_operand_first.v result) and differ from Yosys.
module forward_reference(input clk, input [4:0] counter, input [1:0] mdivk,
                         output reg u, output reg v);
  wire [31:0] t2;
  wire [31:0] t1;
  always @(posedge clk) u <= (counter == t2);
  assign t2 = t1 + 5 - 2;
  assign t1 = mdivk - 1;
  always @(posedge clk) v <= (counter == mdivk - 1);
endmodule
