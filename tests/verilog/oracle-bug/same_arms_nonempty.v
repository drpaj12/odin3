// Yosys 0.55 opt_muxtree bug (2F OP-13, spec 17 #82): a mux output that feeds two slices of one $pmux gets the
// first slice's select knowledge written into its data pins. 2D's case method for a variable select write makes
// exactly that shape (the write's temporaries feed every arm of the next case). Odin III's RTLIL is right
// (Yosys 0.55 `eval` before opt, odin3 proc + opt and Yosys 0.69 all agree); 0.55 loses the write after opt.
// elab_oracle_muxtree_bug: elab-equiv is equivalent on Yosys 0.69 and NOT equivalent on 0.55 (stale when 0.55 is fixed).
module gtop(input [1:0] a, input [7:0] b, input [3:0] c, input [1:0] s, output reg [7:0] y3);
  always @(*) begin
    y3 = b;
    y3[{1'b1,s} +: 2] = a;
    casez (a) 2'b1?: y3[0] = 1'b1; 2'b0?: y3[0] = 1'b1; default: y3 = c; endcase
  end
endmodule
