// EXPECT: $fopen needs a constant file name
module fopen_tb;
  reg [31:0] name;
  integer fd;
  initial begin
    name = 5;
    fd = $fopen(name);
    $display("not reached");
    $finish;
  end
endmodule
