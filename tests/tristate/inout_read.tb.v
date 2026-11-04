module tb;
    reg e;
    wire ext = e;
    wire q, r;
`ifdef SPLIT
    top dut(.p_i(ext), .q(q), .r(r));
`else
    top dut(.p(ext), .q(q), .r(r));
`endif
    integer i;
    initial begin
        for (i = 0; i < 2; i = i + 1) begin
            e = i[0];
            #1;
            $display("%b q=%b r=%b", ext, q, r);
        end
        $finish;
    end
endmodule
