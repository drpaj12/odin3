module mixclk(input [1:0] ck, output reg q);
    always @(posedge ck[0]) q <= ck[1];
endmodule
