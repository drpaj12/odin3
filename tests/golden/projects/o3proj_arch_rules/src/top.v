// Two multipliers and a 4 x 4 RAM: the subjects of the project's mapping rules.
module top (
    input  wire        clk,
    input  wire [11:0] a,
    input  wire [11:0] b,
    input  wire [3:0]  c,
    input  wire [3:0]  d,
    input  wire        we,
    input  wire [1:0]  addr,
    input  wire [3:0]  wdata,
    output reg  [23:0] p_big,
    output reg  [7:0]  p_small,
    output reg  [3:0]  rdata
);
    reg [3:0] mem [0:3];

    always @(posedge clk) begin
        p_big <= a * b;
        p_small <= c * d;
        if (we)
            mem[addr] <= wdata;
        rdata <= mem[addr];
    end
endmodule
