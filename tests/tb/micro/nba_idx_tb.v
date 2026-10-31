module nba_idx_tb;
  integer i;
  reg [7:0] mem [0:3];
  reg [3:0] v;
  reg [1:0] hi, lo;
  initial begin
    for (i = 0; i < 4; i = i + 1) mem[i] <= i + 10;
    #1 $display("%0d %0d %0d %0d", mem[0], mem[1], mem[2], mem[3]);
    v = 0;
    for (i = 0; i < 4; i = i + 1) v[i] <= 1;
    #1 $display("v=%b", v);
    i = 1;
    {hi, lo} <= 4'b1001;
    i = 2;
    #1 $display("%b %b", hi, lo);
    i = 0;
    mem[i] <= 8'd99;
    i = 3;
    #1 $display("%0d %0d", mem[0], mem[3]);
    $finish;
  end
endmodule
