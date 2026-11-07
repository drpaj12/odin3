// Memories are x until written; a read with an unknown or out-of-range index is x and a write
// through one is dropped; $readmemh/$readmemb take x and z digits.
module xmem_tb;
  reg [7:0] m [0:7];
  reg [3:0] h [0:3];
  reg [11:0] b [0:3];
  reg [2:0] k;
  integer i;
  initial begin
    $display("fresh %b %h", m[0], m[7]);
    m[1] = 8'h5a;
    k = 3'bx;
    $display("read %h %h %h", m[1], m[k], m[i]);
    m[k] = 8'hff;
    for (i = 0; i < 8; i = i + 1) $display("m[%0d]=%h", i, m[i]);
    $readmemh("xmem.hex", m);
    for (i = 0; i < 8; i = i + 1) $display("hex m[%0d]=%b", i, m[i]);
    $readmemb("xmem.bin", b);
    for (i = 0; i < 4; i = i + 1) $display("bin b[%0d]=%b", i, b[i]);
    $readmemh("xmem2.hex", h, 1, 2);
    for (i = 0; i < 4; i = i + 1) $display("part h[%0d]=%b", i, h[i]);
    $finish;
  end
endmodule
