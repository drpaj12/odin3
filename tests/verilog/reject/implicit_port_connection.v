// SystemVerilog's .* port connection.
module leaf (input a, output y);
  assign y = a;
endmodule

module m (input a, output y);
  leaf u (.*);
endmodule
