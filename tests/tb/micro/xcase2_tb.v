// --case-xz-never: casez with an x item bit, and a constant selector
module xcase2_tb;
  reg [2:0] cmd;
  wire [3:0] y, k;
  integer i;
  xcase2 u (.cmd(cmd), .y(y), .k(k));
  initial begin
    for (i = 0; i < 8; i = i + 1) begin
      cmd = i;
      #1 $display("cmd=%0d y=%h k=%h", cmd, y, k);
    end
    $finish;
  end
endmodule
