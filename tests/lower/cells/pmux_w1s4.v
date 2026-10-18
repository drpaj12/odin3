module m(input a, input b0, input b1, input b2, input b3, input [3:0] s, output reg y);
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
