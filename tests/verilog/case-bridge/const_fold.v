// Task 22: a parameter condition and a constant selector fold to the branch they take.
module t (input a, b, c, output reg y, z);
  parameter P = 1;
  always @* begin
    if (P) y = a; else y = b;
    case (2'd2)
      2'd1: z = a;
      2'd2: z = c;
      default: z = b;
    endcase
  end
endmodule
