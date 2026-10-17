module m(input [1:0] a, input [1:0] b0, input [1:0] b1, input [1:0] b2, input [1:0] b3, input [3:0] s, output reg [1:0] y);
  always @* begin
    y = a;
    (* parallel_case *) case (1'b1)
      s[0]: y = b0;
      s[1]: y = b1;
      s[2]: y = b2;
      s[3]: y = b3;
    endcase
  end
endmodule
