// K8: automatic and recursive — variants: function automatic, a recursive constant function, task automatic
module k8_automatic (input clk, input [3:0] a, output [31:0] y, output reg [3:0] q);
  function automatic integer fact (input integer n);
    if (n <= 1)
      fact = 1;
    else
      fact = n * fact(n - 1);
  endfunction
  task automatic copy (input [3:0] src, output [3:0] dst);
    dst = src;
  endtask
  localparam F4 = fact(4);
  assign y = F4;
  always @(posedge clk)
    copy(a, q);
endmodule
