// q in a -y directory: never read (the -v file lib/vq.v defines q).
module q (input wire a, output wire y);
    assign y = ~a;
endmodule
