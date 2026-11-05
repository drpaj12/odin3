// -DSPLIT: the design after the tristate pass (p_i/p_o/p_oe); else the RTL with its inout p.
module tb;
    reg oe, d, ext, extoe;
    wire q;
`ifdef SPLIT
    wire p_o, p_oe;
    wire p = p_oe ? p_o : (extoe ? ext : 1'bz);
    top dut(.p_i(p), .p_o(p_o), .p_oe(p_oe), .oe(oe), .d(d), .q(q));
`else
    wire p;
    assign p = extoe ? ext : 1'bz;
    top dut(.p(p), .oe(oe), .d(d), .q(q));
`endif
    integer i;
    initial begin
        for (i = 0; i < 16; i = i + 1) begin
            {oe, d, ext, extoe} = i[3:0];
            #1;
            if ((oe || extoe) && !(oe && extoe)) $display("%b %b %b %b q=%b", oe, d, ext, extoe, q);
        end
        $finish;
    end
endmodule
