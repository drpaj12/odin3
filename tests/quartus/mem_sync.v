// A memory with a registered read (old data on a read during write) and a write port.
module mem_sync(
    input clk,
    input we,
    input [3:0] a,
    input [7:0] d,
    output reg [7:0] q
);
    reg [7:0] m [0:15];
    always @(posedge clk) begin
        if (we) m[a] <= d;
        q <= m[a];
    end
endmodule
