module m(input [4:0] a, input [4:0] b0, input [4:0] b1, input [4:0] b2, input [2:0] s, output reg [4:0] y);
  always @* begin
    y = a;
    (* parallel_case *) case (1'b1)
      s[0]: y = b0;
      s[1]: y = b1;
      s[2]: y = b2;
    endcase
  end
endmodule
