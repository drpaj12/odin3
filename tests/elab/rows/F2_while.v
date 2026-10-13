// F2: while — variants: while in a constant function (SHOULD) (dropped: while in an always, REJECT)
module f2_while (input clk, input [3:0] a, output reg [3:0] c, output [31:0] y);
  function integer log2_floor (input integer n);
    integer v;
    begin
      log2_floor = 0;
      v = n;
      while (v > 1) begin
        v = v / 2;
        log2_floor = log2_floor + 1;
      end
    end
  endfunction
  localparam L = log2_floor(16);
  assign y = L;
  always @(posedge clk)
    c <= a;
endmodule
