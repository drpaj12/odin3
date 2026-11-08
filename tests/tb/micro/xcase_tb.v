// an x/z bit of a plain case item never matches (read_verilog --case-xz-never)
module xcase_tb;
  reg [2:0] cmd;
  reg [3:0] a;
  wire [3:0] y;
  integer i;
  xcase u (.cmd(cmd), .a(a), .y(y));
  initial begin
    a = 4'h3;
    for (i = 0; i < 8; i = i + 1) begin
      cmd = i;
      #1 $display("cmd=%0d y=%h", cmd, y);
    end
    $finish;
  end
endmodule
