// F3: repeat with a constant count — variants: repeat (4) with a block, repeat with a parameter count
module f3_repeat (input [7:0] a, output reg [7:0] y, z);
  parameter N = 3;
  always @* begin
    y = a;
    repeat (4) begin
      y = y << 1;
    end
  end
  always @* begin
    z = a;
    repeat (N)
      z = z + 1;
  end
endmodule
