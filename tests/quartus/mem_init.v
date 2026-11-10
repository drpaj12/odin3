// A memory with initial contents.
module mem_init(
    input [1:0] a,
    output [7:0] q
);
    reg [7:0] m [0:3];
    initial begin
        m[0] = 8'h11;
        m[1] = 8'h22;
        m[2] = 8'h33;
        m[3] = 8'h44;
    end
    assign q = m[a];
endmodule
