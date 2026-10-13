// Task 22: a blocking write through a variable select (case temporaries over every position).
module t (input [1:0] s, input d, input [3:0] a, output reg [3:0] y);
  always @* begin
    y = a;
    y[s] = d;
  end
endmodule
