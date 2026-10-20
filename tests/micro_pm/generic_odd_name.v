// 4.2 test (h): 'odd-name' is a model whose name is not a Verilog identifier; it is not
// registered, so this instance is an unresolved module.
module generic_odd_name (
    input x,
    output y
);
  \odd-name u_odd (.x(x), .y(y));
endmodule
