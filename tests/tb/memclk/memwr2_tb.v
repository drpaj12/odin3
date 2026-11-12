// Port 0 alone, port 1 alone, both at once on different addresses and on the same address.
module memwr2_tb;
  reg ca = 0, cb = 0, wea = 0, web = 0;
  reg [2:0] waa = 0, wab = 0, ra = 0;
  reg [7:0] wda = 0, wdb = 0;
  wire [7:0] q;
  integer i;
  memwr2 dut(.ca(ca), .cb(cb), .wea(wea), .web(web), .waa(waa), .wab(wab), .wda(wda), .wdb(wdb),
             .ra(ra), .q(q));
  initial begin
    for (i = 0; i < 8; i = i + 1) begin
      #1 wea = 1; web = 1; waa = i; wab = i; wda = i * 11; wdb = i * 13 + 1;
      #1 ca = 1; cb = 1;
      #1 ca = 0; cb = 0;
    end
    for (i = 0; i < 64; i = i + 1) begin
      #1;
      wea = (i % 5) != 1;
      web = (i % 7) != 3;
      waa = (i * 3 + i / 4) % 8;
      wab = (i % 4 == 3) ? waa : (i * 5 + i / 3) % 8;
      wda = i * 37 + 11;
      wdb = i * 41 + 7;
      ra = (i % 4 == 3) ? waa : (i * 3 + i / 5) % 8;
      #1;
      case (i % 3)
        0: ca = 1;
        1: cb = 1;
        2: begin ca = 1; cb = 1; end
      endcase
      #1;
      $display("%0d waa=%0d wab=%0d ra=%0d q=%h", i, waa, wab, ra, q);
      ca = 0; cb = 0;
    end
    $finish;
  end
endmodule
