// L11: attributes (* *) — variants: on a module, a header port, declarations, a binary, a unary and a conditional operator, an always, a statement, a case, an instance; several specs in one instance with and without values; two consecutive instances
(* top_module, version = 1 *)
module l11_attributes ((* clock *) input clk, input a, b, output y, z, output reg q, r);
  (* keep *) wire w;
  (* keep, init = "x" *) (* dont_touch = 1 *) reg t;
  assign w = a & (* op_attr *) b;
  assign y = ~ (* unary_attr *) w;
  assign z = a ? (* cond_attr *) b : w;
  (* proc *) always @(posedge clk) begin
    (* stmt_attr *) q <= w;
    (* case_attr = 1 *) case (a)
      1'b0: r <= 1'b0;
      default: r <= 1'b1;
    endcase
  end
  (* inst_attr *) l11_leaf u (.d(a), .e());
endmodule

module l11_leaf (input d, output e);
  assign e = d;
endmodule
