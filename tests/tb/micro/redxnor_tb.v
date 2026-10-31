module redxnor_tb;
  reg [3:0] a;
  integer i;
  initial begin
    for (i = 0; i < 16; i = i + 1) begin
      a = i;
      $display("%0d %b %b %b %b", i, ^~a, ~^a, ^a, (^~a) & 1'b1);
    end
    $finish;
  end
endmodule
