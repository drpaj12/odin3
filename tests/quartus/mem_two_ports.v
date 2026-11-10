// Two write ports on one clock (the second wins on one address) and two read ports.
module mem_two_ports(
    input clk,
    input we1,
    input we2,
    input [2:0] a1,
    input [2:0] a2,
    input [2:0] ra,
    input [7:0] d1,
    input [7:0] d2,
    output [7:0] q_async,
    output reg [7:0] q_sync
);
    reg [7:0] m [0:7];
    always @(posedge clk) begin
        if (we1) m[a1] <= d1;
        if (we2) m[a2] <= d2;
        q_sync <= m[ra];
    end
    assign q_async = m[ra];
endmodule
