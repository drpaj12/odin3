// A feed-through port, an output tied to a constant, and an unconnected output port.
module thru (input a, output y);
  assign y = a;
endmodule
module tied (input a, output y, output k);
  assign y = ~a;
  assign k = 1'b1;
endmodule
module top (input x, output o, output p, output q);
  thru t (.a(x), .y(o));
  tied d (.a(x), .y(p), .k(q));
  tied e (.a(x), .y(), .k());
endmodule
