module hx_tb;
    reg clk = 0;
    reg ld = 0;
    reg [1:0] d = 0;
    wire [3:0] y, z;
    hx dut(.clk(clk), .ld(ld), .d(d), .y(y), .z(z));
    initial begin
        #1 $display("before %b %b", y, z);
        d = 2'b11; ld = 1; #1 clk = 1; #1 clk = 0; ld = 0;
        #1 $display("after %b %b", y, z);
        $finish;
    end
endmodule
