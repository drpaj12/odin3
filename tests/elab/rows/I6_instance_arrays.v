// I6: arrays of instances — variants: inv u[3:0] (…) with vector connections, an ascending array range
module i6_inv (input a, output y);
  assign y = ~a;
endmodule

module i6_instance_array (input [3:0] a, output [3:0] y, z);
  i6_inv u[3:0] (.a(a), .y(y));
  i6_inv v[0:3] (a, z);
endmodule
