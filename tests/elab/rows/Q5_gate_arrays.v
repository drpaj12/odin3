// Q5: gate instance arrays — variants: and g[3:0] (…) with vector terminals, an ascending array not n[0:3] (…)
module q5_gate_arrays (input [3:0] a, b, output [3:0] y, z);
  and g[3:0] (y, a, b);
  not n[0:3] (z, a);
endmodule
