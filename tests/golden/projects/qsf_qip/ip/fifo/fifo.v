// Two-deep buffer around ram2 (generated-IP style).
module fifo2 (
    input  wire       clk,
    input  wire       we,
    input  wire [3:0] d,
    output wire [3:0] q
);
    reg wp = 1'b0;

    always @(posedge clk)
        if (we)
            wp <= ~wp;
    ram2 u_ram (.clk(clk), .we(we), .wa(wp), .ra(~wp), .d(d), .q(q));
endmodule
