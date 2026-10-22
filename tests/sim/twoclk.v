module twoclk(input c1, input c2, input d, output reg q1, output reg q2);
    always @(posedge c1) q1 <= d;
    always @(posedge c2) q2 <= d;
endmodule
