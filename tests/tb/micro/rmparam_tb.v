module rmparam_tb;
  reg [2:0] addr;
  wire [7:0] dout;
  integer i;
  rmparam dut(.addr(addr), .dout(dout));
  initial begin
    for (i = 0; i < 8; i = i + 1) begin
      addr = i;
      #1 $display("addr=%0d dout=%h", addr, dout);
    end
    $finish;
  end
endmodule
