// A tri-state on a top-level output: Parmys drops the enable (opt_expr -mux_undef), so under
// --vtr-compat q is wired to d. Golden: tri_out.parmys.blif (tools/run-oracle.sh --tool parmys,
// k6_frac). PHASE2 decision 28.
module tri_out(input [3:0] d, input en, output [3:0] q);
  assign q = en ? d : 4'bz;
endmodule
