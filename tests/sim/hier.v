// S1 fixture: two instances of one stage, an internal wire, a register opt removes, a kept one.
module stage(input clk, input d, output reg q);
    reg inner;
    always @(posedge clk) begin
        inner <= d;
        q <= inner;
    end
endmodule
module hier(input clk, input [3:0] a, output [3:0] y, output z);
    wire mid;
    reg dead_r;
    (* keep *) reg kept_r;
    stage u0(.clk(clk), .d(a[0]), .q(mid));
    stage u1(.clk(clk), .d(mid), .q(z));
    always @(posedge clk) begin
        dead_r <= a[1];
        kept_r <= a[2];
    end
    assign y = ~a;
endmodule
