// err_nonconst_reset: reset leaves a register assigned outside the reset if non-constant (E2)
// yosys -V: Yosys 0.55 (git sha1 60f126cd0, g++ 13.3.0-6ubuntu2~24.04.1 -fPIC -O3)
module t (input clk, rst, input [3:0] data, datain, output reg [3:0] dataout);
  reg [3:0] mem1, mem2;
  // dataout is assigned outside the reset if (the shape of paj_top's dpram)
  always @(posedge clk or posedge rst) begin
    dataout <= data;
    if (rst) begin
      mem1 <= datain;
      mem2 <= mem1;
    end
  end
endmodule
