// E21: a path whose first component is a function is accepted (Yosys: the auto-wire f.x), with a
// warning; the module's own name before an undeclared name is the same (top.nope).
module m (input a, output y, output z);
  function f;
    input x;
    f = x;
  endfunction
  assign y = f.x;
  assign z = m.nope;
endmodule
