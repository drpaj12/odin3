// A memory with an asynchronous read port and a clocked write port: a behavioural reg array.
module mem_async(
    input clk,
    input we,
    input [3:0] wa,
    input [3:0] ra,
    input [7:0] d,
    output [7:0] q
);
    reg [7:0] m [0:15];
    always @(posedge clk) if (we) m[wa] <= d;
    assign q = m[ra];
endmodule
