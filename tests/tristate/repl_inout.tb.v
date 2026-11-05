module tb;
    reg den, extoe;
    reg [3:0] dout, ext;
    wire [3:0] din;
`ifdef SPLIT
    wire [3:0] dq_o, dq_oe;
    wire [3:0] dq;
    genvar g;
    generate for (g = 0; g < 4; g = g + 1) begin : pin
        assign dq[g] = dq_oe[g] ? dq_o[g] : (extoe ? ext[g] : 1'bz);
    end endgenerate
    top dut(.dq_i(dq), .dq_o(dq_o), .dq_oe(dq_oe), .den(den), .dout(dout), .din(din));
`else
    wire [3:0] dq;
    assign dq = extoe ? ext : 4'bz;
    top dut(.dq(dq), .den(den), .dout(dout), .din(din));
`endif
    integer i, v;
    initial begin
        for (i = 0; i < 4; i = i + 1) begin
            for (v = 0; v < 16; v = v + 1) begin
                {den, extoe} = i[1:0];
                dout = v[3:0];
                ext = v[3:0] * 4'd7;
                #1;
                if (den != !extoe) $display("%b %b %h %h din=%h", den, extoe, dout, ext, din);
            end
        end
        $finish;
    end
endmodule
