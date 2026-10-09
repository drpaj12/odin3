// Two multipliers and a small RAM: the subjects of the project's mapping rules.
module top (
    input  wire        clk,
    input  wire [11:0] a,
    input  wire [11:0] b,
    input  wire [3:0]  c,
    input  wire [3:0]  d,
    input  wire        we,
    input  wire [5:0]  addr,
    input  wire [7:0]  wdata,
    output reg  [23:0] p_big,
    output reg  [7:0]  p_small,
    output reg  [7:0]  rdata
);
    reg [7:0] mem [0:63];

    always @(posedge clk) begin
        p_big <= a * b;
        p_small <= c * d;
        if (we)
            mem[addr] <= wdata;
        rdata <= mem[addr];
    end
endmodule
