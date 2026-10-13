// Task 23: repeat runs its body n times; reads see the blocking value.
module t (input [3:0] a, b, output reg [3:0] y);
  always @* begin
    y = a;
    repeat (3) y = y + b;
  end
endmodule
