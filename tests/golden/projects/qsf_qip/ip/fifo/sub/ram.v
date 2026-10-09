// 2 x 4 register file, registered read.
module ram2 (
    input  wire       clk,
    input  wire       we,
    input  wire       wa,
    input  wire       ra,
    input  wire [3:0] d,
    output reg  [3:0] q
);
    reg [3:0] r0 = 4'd0, r1 = 4'd0;

    always @(posedge clk) begin
        if (we && !wa) r0 <= d;
        if (we && wa) r1 <= d;
        q <= ra ? r1 : r0;
    end
endmodule
