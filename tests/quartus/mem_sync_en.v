// A registered read with an enable (not an altsyncram shape): a clocked read of the reg array.
module mem_sync_en(
    input clk,
    input we,
    input re,
    input [3:0] a,
    input [7:0] d,
    output reg [7:0] q
);
    reg [7:0] m [0:15];
    always @(posedge clk) begin
        if (we) m[a] <= d;
        if (re) q <= m[a];
    end
endmodule
