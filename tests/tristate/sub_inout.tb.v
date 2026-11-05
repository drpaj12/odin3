module tb;
    reg oe, d, ext, extoe;
    wire q;
`ifdef SPLIT
    wire sda_o, sda_oe;
    wire sda = sda_oe ? sda_o : (extoe ? ext : 1'bz);
    top dut(.sda_i(sda), .sda_o(sda_o), .sda_oe(sda_oe), .oe(oe), .d(d), .q(q));
`else
    wire sda;
    assign sda = extoe ? ext : 1'bz;
    top dut(.sda(sda), .oe(oe), .d(d), .q(q));
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
