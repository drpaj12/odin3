// Odin III sim_tb takes no tasks yet, so the stimulus is written out.
module membyte_tb;
    reg clk, we;
    reg [1:0] a;
    reg [3:0] be;
    reg [31:0] d;
    wire [31:0] qa, qb;
    membyte dut (.clk(clk), .we(we), .a(a), .be(be), .d(d), .qa(qa), .qb(qb));
    initial clk = 0;
    always #5 clk = ~clk;
    initial begin
        we = 0; a = 0; be = 0; d = 0;
        @(posedge clk);
        a = 0; be = 4'hf; d = 32'h11223344; we = 1; @(posedge clk); #1 we = 0;
        a = 1; be = 4'hf; d = 32'h55667788; we = 1; @(posedge clk); #1 we = 0;
        a = 2; be = 4'hf; d = 32'h99aabbcc; we = 1; @(posedge clk); #1 we = 0;
        a = 3; be = 4'hf; d = 32'hddeeff00; we = 1; @(posedge clk); #1 we = 0;
        a = 0; we = 0; @(posedge clk); #1 $display("a=0 qa=%h qb=%h", qa, qb);
        a = 1; we = 0; @(posedge clk); #1 $display("a=1 qa=%h qb=%h", qa, qb);
        a = 2; we = 0; @(posedge clk); #1 $display("a=2 qa=%h qb=%h", qa, qb);
        a = 3; we = 0; @(posedge clk); #1 $display("a=3 qa=%h qb=%h", qa, qb);
        a = 0; be = 4'h1; d = 32'hffffffa1; we = 1; @(posedge clk); #1 we = 0;
        a = 0; we = 0; @(posedge clk); #1 $display("a=0 qa=%h qb=%h", qa, qb);
        a = 0; be = 4'h2; d = 32'hffffb2ff; we = 1; @(posedge clk); #1 we = 0;
        a = 0; we = 0; @(posedge clk); #1 $display("a=0 qa=%h qb=%h", qa, qb);
        a = 0; be = 4'h4; d = 32'hffc3ffff; we = 1; @(posedge clk); #1 we = 0;
        a = 0; we = 0; @(posedge clk); #1 $display("a=0 qa=%h qb=%h", qa, qb);
        a = 0; be = 4'h8; d = 32'hd4ffffff; we = 1; @(posedge clk); #1 we = 0;
        a = 0; we = 0; @(posedge clk); #1 $display("a=0 qa=%h qb=%h", qa, qb);
        a = 1; be = 4'h5; d = 32'h0a0b0c0d; we = 1; @(posedge clk); #1 we = 0;
        a = 1; we = 0; @(posedge clk); #1 $display("a=1 qa=%h qb=%h", qa, qb);
        a = 1; be = 4'h0; d = 32'hdeadbeef; we = 1; @(posedge clk); #1 we = 0;
        a = 1; we = 0; @(posedge clk); #1 $display("a=1 qa=%h qb=%h", qa, qb);
        a = 2; be = 4'ha; d = 32'h12345678; we = 1; @(posedge clk); #1 we = 0;
        a = 2; we = 0; @(posedge clk); #1 $display("a=2 qa=%h qb=%h", qa, qb);
        a = 0; we = 0; @(posedge clk); #1 $display("a=0 qa=%h qb=%h", qa, qb);
        a = 1; we = 0; @(posedge clk); #1 $display("a=1 qa=%h qb=%h", qa, qb);
        a = 2; we = 0; @(posedge clk); #1 $display("a=2 qa=%h qb=%h", qa, qb);
        a = 3; we = 0; @(posedge clk); #1 $display("a=3 qa=%h qb=%h", qa, qb);
        $finish;
    end
endmodule
