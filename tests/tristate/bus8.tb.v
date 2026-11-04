module tb;
    reg oe_a, oe_b;
    reg [3:0] a, b, ext;
    reg extoe;
    wire [3:0] q;
`ifdef SPLIT
    wire [3:0] db_o, db_oe;
    wire [3:0] db;
    genvar g;
    generate for (g = 0; g < 4; g = g + 1) begin : pin
        assign db[g] = db_oe[g] ? db_o[g] : (extoe ? ext[g] : 1'bz);
    end endgenerate
    top dut(.db_i(db), .db_o(db_o), .db_oe(db_oe), .oe_a(oe_a), .oe_b(oe_b), .a(a), .b(b), .q(q));
`else
    wire [3:0] db;
    assign db = extoe ? ext : 4'bz;
    top dut(.db(db), .oe_a(oe_a), .oe_b(oe_b), .a(a), .b(b), .q(q));
`endif
    integer i, v;
    initial begin
        for (i = 0; i < 8; i = i + 1) begin
            for (v = 0; v < 16; v = v + 1) begin
                {oe_a, oe_b, extoe} = i[2:0];
                a = v[3:0];
                b = v[3:0] + 4'd3;
                ext = v[3:0] * 4'd5;
                #1;
                if (oe_a + oe_b + extoe == 1) $display("%b %b %b %h %h %h q=%h", oe_a, oe_b, extoe, a, b, ext, q);
            end
        end
        $finish;
    end
endmodule
