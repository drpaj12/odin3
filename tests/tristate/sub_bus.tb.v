// exclusive enables only (one driver on, or none: skipped)
module tb;
    reg e1, e2, d1, d2;
    wire y;
    top dut(.e1(e1), .e2(e2), .d1(d1), .d2(d2), .y(y));
    integer i;
    initial begin
        for (i = 0; i < 16; i = i + 1) begin
            {e1, e2, d1, d2} = i[3:0];
            #1;
            if (e1 != e2) $display("%b %b %b %b y=%b", e1, e2, d1, d2, y);
        end
        $finish;
    end
endmodule
