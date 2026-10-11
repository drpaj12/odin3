// S1: specify (IGNORE) — variants: a specparam list, parallel and full paths, a delay list, an edge-sensitive path, conditional and ifnone paths, timing checks, an endspecify word inside a comment in the body
module s1_specify (input clk, a, b, input [1:0] v, output y, output [1:0] w);
  assign y = a & b;
  assign w = v;
  specify
    specparam tA = 1, tB = 2;
    (a => y) = 1;
    (b *> y) = (1, 2);
    (v *> w) = tA;
    (posedge clk => (y +: a)) = (tB, tB);
    if (a) (b => y) = 3;
    ifnone (b => y) = 4;
    $setup(a, posedge clk, 1);
    $hold(posedge clk, a, 1);
    // endspecify inside a comment does not end the body
  endspecify
endmodule
