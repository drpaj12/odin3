module m(input a, input b0, input b1, input [1:0] s, output reg y);
  always @* begin
    y = a;
    (* parallel_case *) case (1'b1)
      s[0]: y = b0;
      s[1]: y = b1;
    endcase
  end
endmodule
