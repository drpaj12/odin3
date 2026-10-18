module m(input [7:0] a, input [7:0] b0, input [7:0] b1, input [7:0] b2, input [7:0] b3, input [3:0] s, output reg [7:0] y);
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
