// 4.4 micro: separate read and write ports, the read under its own enable `ren`.
module mem_rden_dp (
    input clk,
    input wen,
    input ren,
    input [4:0] waddr,
    input [4:0] raddr,
    input [3:0] data,
    output reg [3:0] q
);
    reg [3:0] mem [31:0];

    always @(posedge clk) begin
        if (wen) mem[waddr] <= data;
        if (ren) q <= mem[raddr];
    end
endmodule
