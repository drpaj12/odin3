module top (  // tests/flatten/mem.v:10
  (* src = "tests/flatten/mem.v:10.13-10.22" *) input clk,  // tests/flatten/mem.v:10
  (* src = "tests/flatten/mem.v:10.24-10.32" *) input we,  // tests/flatten/mem.v:10
  (* src = "tests/flatten/mem.v:10.34-10.50" *) input [3:0] addr,  // tests/flatten/mem.v:10
  (* src = "tests/flatten/mem.v:10.52-10.65" *) input [7:0] d,  // tests/flatten/mem.v:10
  (* src = "tests/flatten/mem.v:10.67-10.82" *) output [7:0] q1,  // tests/flatten/mem.v:10
  (* src = "tests/flatten/mem.v:10.84-10.99" *) output [7:0] q2  // tests/flatten/mem.v:10
);
  (* src = "tests/flatten/mem.v:3.13-3.22|tests/flatten/mem.v:11.7-11.58" *) wire \u1.clk ;  // tests/flatten/mem.v:3
  (* src = "tests/flatten/mem.v:3.24-3.32|tests/flatten/mem.v:11.7-11.58" *) wire \u1.we ;  // tests/flatten/mem.v:3
  (* src = "tests/flatten/mem.v:3.34-3.50|tests/flatten/mem.v:11.7-11.58" *) wire [3:0] \u1.addr ;  // tests/flatten/mem.v:3
  (* src = "tests/flatten/mem.v:3.52-3.65|tests/flatten/mem.v:11.7-11.58" *) wire [7:0] \u1.d ;  // tests/flatten/mem.v:3
  (* src = "tests/flatten/mem.v:3.67-3.85|tests/flatten/mem.v:11.7-11.58" *) wire [7:0] \u1.q ;  // tests/flatten/mem.v:3
  (* src = "tests/flatten/mem.v:3.13-3.22|tests/flatten/mem.v:12.7-12.60" *) wire \u2.clk ;  // tests/flatten/mem.v:3
  (* src = "tests/flatten/mem.v:3.24-3.32|tests/flatten/mem.v:12.7-12.60" *) wire \u2.we ;  // tests/flatten/mem.v:3
  (* src = "tests/flatten/mem.v:3.34-3.50|tests/flatten/mem.v:12.7-12.60" *) wire [3:0] \u2.addr ;  // tests/flatten/mem.v:3
  (* src = "tests/flatten/mem.v:3.52-3.65|tests/flatten/mem.v:12.7-12.60" *) wire [7:0] \u2.d ;  // tests/flatten/mem.v:3
  (* src = "tests/flatten/mem.v:3.67-3.85|tests/flatten/mem.v:12.7-12.60" *) wire [7:0] \u2.q ;  // tests/flatten/mem.v:3
  (* src = "tests/flatten/mem.v:5.3-8.6|tests/flatten/mem.v:11.7-11.58" *) wire \$n43 ;  // tests/flatten/mem.v:5
  (* src = "tests/flatten/mem.v:5.3-8.6|tests/flatten/mem.v:11.7-11.58" *) wire \$n44 ;  // tests/flatten/mem.v:5
  (* src = "tests/flatten/mem.v:5.3-8.6|tests/flatten/mem.v:11.7-11.58" *) wire \$n45 ;  // tests/flatten/mem.v:5
  (* src = "tests/flatten/mem.v:5.3-8.6|tests/flatten/mem.v:11.7-11.58" *) wire \$n46 ;  // tests/flatten/mem.v:5
  (* src = "tests/flatten/mem.v:5.3-8.6|tests/flatten/mem.v:11.7-11.58" *) wire \$n47 ;  // tests/flatten/mem.v:5
  (* src = "tests/flatten/mem.v:5.3-8.6|tests/flatten/mem.v:11.7-11.58" *) wire \$n48 ;  // tests/flatten/mem.v:5
  (* src = "tests/flatten/mem.v:5.3-8.6|tests/flatten/mem.v:11.7-11.58" *) wire \$n49 ;  // tests/flatten/mem.v:5
  (* src = "tests/flatten/mem.v:5.3-8.6|tests/flatten/mem.v:11.7-11.58" *) wire \$n50 ;  // tests/flatten/mem.v:5
  (* src = "tests/flatten/mem.v:6.13-6.28|tests/flatten/mem.v:11.7-11.58" *) wire \$n51 ;  // tests/flatten/mem.v:6
  (* src = "tests/flatten/mem.v:6.13-6.28|tests/flatten/mem.v:11.7-11.58" *) wire \$n52 ;  // tests/flatten/mem.v:6
  (* src = "tests/flatten/mem.v:6.13-6.28|tests/flatten/mem.v:11.7-11.58" *) wire \$n53 ;  // tests/flatten/mem.v:6
  (* src = "tests/flatten/mem.v:6.13-6.28|tests/flatten/mem.v:11.7-11.58" *) wire \$n54 ;  // tests/flatten/mem.v:6
  (* src = "tests/flatten/mem.v:6.13-6.28|tests/flatten/mem.v:11.7-11.58" *) wire \$n55 ;  // tests/flatten/mem.v:6
  (* src = "tests/flatten/mem.v:6.13-6.28|tests/flatten/mem.v:11.7-11.58" *) wire \$n56 ;  // tests/flatten/mem.v:6
  (* src = "tests/flatten/mem.v:6.13-6.28|tests/flatten/mem.v:11.7-11.58" *) wire \$n57 ;  // tests/flatten/mem.v:6
  (* src = "tests/flatten/mem.v:6.13-6.28|tests/flatten/mem.v:11.7-11.58" *) wire \$n58 ;  // tests/flatten/mem.v:6
  (* src = "tests/flatten/mem.v:6.13-6.28|tests/flatten/mem.v:11.7-11.58" *) wire \$n59 ;  // tests/flatten/mem.v:6
  (* src = "tests/flatten/mem.v:6.13-6.28|tests/flatten/mem.v:11.7-11.58" *) wire \$n60 ;  // tests/flatten/mem.v:6
  (* src = "tests/flatten/mem.v:6.13-6.28|tests/flatten/mem.v:11.7-11.58" *) wire \$n61 ;  // tests/flatten/mem.v:6
  (* src = "tests/flatten/mem.v:6.13-6.28|tests/flatten/mem.v:11.7-11.58" *) wire \$n62 ;  // tests/flatten/mem.v:6
  (* src = "tests/flatten/mem.v:6.13-6.28|tests/flatten/mem.v:11.7-11.58" *) wire \$n63 ;  // tests/flatten/mem.v:6
  (* src = "tests/flatten/mem.v:6.13-6.28|tests/flatten/mem.v:11.7-11.58" *) wire \$n64 ;  // tests/flatten/mem.v:6
  (* src = "tests/flatten/mem.v:6.13-6.28|tests/flatten/mem.v:11.7-11.58" *) wire \$n65 ;  // tests/flatten/mem.v:6
  (* src = "tests/flatten/mem.v:6.13-6.28|tests/flatten/mem.v:11.7-11.58" *) wire \$n66 ;  // tests/flatten/mem.v:6
  (* src = "tests/flatten/mem.v:6.13-6.28|tests/flatten/mem.v:11.7-11.58" *) wire \$n67 ;  // tests/flatten/mem.v:6
  (* src = "tests/flatten/mem.v:6.13-6.28|tests/flatten/mem.v:11.7-11.58" *) wire \$n68 ;  // tests/flatten/mem.v:6
  (* src = "tests/flatten/mem.v:6.13-6.28|tests/flatten/mem.v:11.7-11.58" *) wire \$n69 ;  // tests/flatten/mem.v:6
  (* src = "tests/flatten/mem.v:6.13-6.28|tests/flatten/mem.v:11.7-11.58" *) wire \$n70 ;  // tests/flatten/mem.v:6
  (* src = "tests/flatten/mem.v:5.3-8.6|tests/flatten/mem.v:12.7-12.60" *) wire \$n71 ;  // tests/flatten/mem.v:5
  (* src = "tests/flatten/mem.v:5.3-8.6|tests/flatten/mem.v:12.7-12.60" *) wire \$n72 ;  // tests/flatten/mem.v:5
  (* src = "tests/flatten/mem.v:5.3-8.6|tests/flatten/mem.v:12.7-12.60" *) wire \$n73 ;  // tests/flatten/mem.v:5
  (* src = "tests/flatten/mem.v:5.3-8.6|tests/flatten/mem.v:12.7-12.60" *) wire \$n74 ;  // tests/flatten/mem.v:5
  (* src = "tests/flatten/mem.v:5.3-8.6|tests/flatten/mem.v:12.7-12.60" *) wire \$n75 ;  // tests/flatten/mem.v:5
  (* src = "tests/flatten/mem.v:5.3-8.6|tests/flatten/mem.v:12.7-12.60" *) wire \$n76 ;  // tests/flatten/mem.v:5
  (* src = "tests/flatten/mem.v:5.3-8.6|tests/flatten/mem.v:12.7-12.60" *) wire \$n77 ;  // tests/flatten/mem.v:5
  (* src = "tests/flatten/mem.v:5.3-8.6|tests/flatten/mem.v:12.7-12.60" *) wire \$n78 ;  // tests/flatten/mem.v:5
  (* src = "tests/flatten/mem.v:6.13-6.28|tests/flatten/mem.v:12.7-12.60" *) wire \$n79 ;  // tests/flatten/mem.v:6
  (* src = "tests/flatten/mem.v:6.13-6.28|tests/flatten/mem.v:12.7-12.60" *) wire \$n80 ;  // tests/flatten/mem.v:6
  (* src = "tests/flatten/mem.v:6.13-6.28|tests/flatten/mem.v:12.7-12.60" *) wire \$n81 ;  // tests/flatten/mem.v:6
  (* src = "tests/flatten/mem.v:6.13-6.28|tests/flatten/mem.v:12.7-12.60" *) wire \$n82 ;  // tests/flatten/mem.v:6
  (* src = "tests/flatten/mem.v:6.13-6.28|tests/flatten/mem.v:12.7-12.60" *) wire \$n83 ;  // tests/flatten/mem.v:6
  (* src = "tests/flatten/mem.v:6.13-6.28|tests/flatten/mem.v:12.7-12.60" *) wire \$n84 ;  // tests/flatten/mem.v:6
  (* src = "tests/flatten/mem.v:6.13-6.28|tests/flatten/mem.v:12.7-12.60" *) wire \$n85 ;  // tests/flatten/mem.v:6
  (* src = "tests/flatten/mem.v:6.13-6.28|tests/flatten/mem.v:12.7-12.60" *) wire \$n86 ;  // tests/flatten/mem.v:6
  (* src = "tests/flatten/mem.v:6.13-6.28|tests/flatten/mem.v:12.7-12.60" *) wire \$n87 ;  // tests/flatten/mem.v:6
  (* src = "tests/flatten/mem.v:6.13-6.28|tests/flatten/mem.v:12.7-12.60" *) wire \$n88 ;  // tests/flatten/mem.v:6
  (* src = "tests/flatten/mem.v:6.13-6.28|tests/flatten/mem.v:12.7-12.60" *) wire \$n89 ;  // tests/flatten/mem.v:6
  (* src = "tests/flatten/mem.v:6.13-6.28|tests/flatten/mem.v:12.7-12.60" *) wire \$n90 ;  // tests/flatten/mem.v:6
  (* src = "tests/flatten/mem.v:6.13-6.28|tests/flatten/mem.v:12.7-12.60" *) wire \$n91 ;  // tests/flatten/mem.v:6
  (* src = "tests/flatten/mem.v:6.13-6.28|tests/flatten/mem.v:12.7-12.60" *) wire \$n92 ;  // tests/flatten/mem.v:6
  (* src = "tests/flatten/mem.v:6.13-6.28|tests/flatten/mem.v:12.7-12.60" *) wire \$n93 ;  // tests/flatten/mem.v:6
  (* src = "tests/flatten/mem.v:6.13-6.28|tests/flatten/mem.v:12.7-12.60" *) wire \$n94 ;  // tests/flatten/mem.v:6
  (* src = "tests/flatten/mem.v:6.13-6.28|tests/flatten/mem.v:12.7-12.60" *) wire \$n95 ;  // tests/flatten/mem.v:6
  (* src = "tests/flatten/mem.v:6.13-6.28|tests/flatten/mem.v:12.7-12.60" *) wire \$n96 ;  // tests/flatten/mem.v:6
  (* src = "tests/flatten/mem.v:6.13-6.28|tests/flatten/mem.v:12.7-12.60" *) wire \$n97 ;  // tests/flatten/mem.v:6
  (* src = "tests/flatten/mem.v:6.13-6.28|tests/flatten/mem.v:12.7-12.60" *) wire \$n98 ;  // tests/flatten/mem.v:6
  reg [7:0] \$c18 ;
  reg [7:0] \$c23 ;
  assign \u1.clk = clk;
  assign \u1.we = we;
  assign \u1.addr = addr;
  assign \u1.d = d;
  assign \u1.q = q1;
  assign \u2.clk = clk;
  assign \u2.addr = addr;
  assign \u2.q = q2;
  assign \u2.we = ~we;  // tests/flatten/mem.v:12
  assign \u2.d = ~d;  // tests/flatten/mem.v:12
  (* src = "tests/flatten/mem.v:4.13-4.23|tests/flatten/mem.v:11.7-11.58" *)
  \$mem #(.MEMID("u1.mem"), .SIZE(16), .OFFSET(0), .ABITS(4), .WIDTH(8), .RD_PORTS(1), .WR_PORTS(1), .RD_CLK_ENABLE(1'b0), .RD_CLK_POLARITY(1'b0), .RD_TRANSPARENT(1'b0), .WR_CLK_ENABLE(1'b1), .WR_CLK_POLARITY(1'b1), .INIT("")) \$c14 (  // tests/flatten/mem.v:4
    .RD_CLK(1'bx),
    .RD_EN(1'b1),
    .RD_ADDR(addr),
    .RD_DATA({\$n50 , \$n49 , \$n48 , \$n47 , \$n46 , \$n45 , \$n44 , \$n43 }),
    .WR_CLK(clk),
    .WR_EN({\$n70 , \$n69 , \$n68 , \$n67 , \$n66 , \$n65 , \$n64 , \$n63 }),
    .WR_ADDR({\$n54 , \$n53 , \$n52 , \$n51 }),
    .WR_DATA({\$n62 , \$n61 , \$n60 , \$n59 , \$n58 , \$n57 , \$n56 , \$n55 })
  );
  // (* full_case = 1 *)
  assign {\$n54 , \$n53 , \$n52 , \$n51 } = we ? addr : 4'bxxxx;  // tests/flatten/mem.v:6
  // (* full_case = 1 *)
  assign {\$n62 , \$n61 , \$n60 , \$n59 , \$n58 , \$n57 , \$n56 , \$n55 } = we ? d : 8'bxxxxxxxx;  // tests/flatten/mem.v:6
  // (* full_case = 1 *)
  assign {\$n70 , \$n69 , \$n68 , \$n67 , \$n66 , \$n65 , \$n64 , \$n63 } = we ? 8'b11111111 : 8'b00000000;  // tests/flatten/mem.v:6
  (* src = "tests/flatten/mem.v:5.3-8.6|tests/flatten/mem.v:11.7-11.58" *)
  always @(posedge clk) \$c18 <= {\$n50 , \$n49 , \$n48 , \$n47 , \$n46 , \$n45 , \$n44 , \$n43 };  // tests/flatten/mem.v:5
  assign q1 = \$c18 ;
  (* src = "tests/flatten/mem.v:4.13-4.23|tests/flatten/mem.v:12.7-12.60" *)
  \$mem #(.MEMID("u2.mem"), .SIZE(16), .OFFSET(0), .ABITS(4), .WIDTH(8), .RD_PORTS(1), .WR_PORTS(1), .RD_CLK_ENABLE(1'b0), .RD_CLK_POLARITY(1'b0), .RD_TRANSPARENT(1'b0), .WR_CLK_ENABLE(1'b1), .WR_CLK_POLARITY(1'b1), .INIT("")) \$c19 (  // tests/flatten/mem.v:4
    .RD_CLK(1'bx),
    .RD_EN(1'b1),
    .RD_ADDR(addr),
    .RD_DATA({\$n78 , \$n77 , \$n76 , \$n75 , \$n74 , \$n73 , \$n72 , \$n71 }),
    .WR_CLK(clk),
    .WR_EN({\$n98 , \$n97 , \$n96 , \$n95 , \$n94 , \$n93 , \$n92 , \$n91 }),
    .WR_ADDR({\$n82 , \$n81 , \$n80 , \$n79 }),
    .WR_DATA({\$n90 , \$n89 , \$n88 , \$n87 , \$n86 , \$n85 , \$n84 , \$n83 })
  );
  // (* full_case = 1 *)
  assign {\$n82 , \$n81 , \$n80 , \$n79 } = \u2.we ? addr : 4'bxxxx;  // tests/flatten/mem.v:6
  // (* full_case = 1 *)
  assign {\$n90 , \$n89 , \$n88 , \$n87 , \$n86 , \$n85 , \$n84 , \$n83 } = \u2.we ? \u2.d : 8'bxxxxxxxx;  // tests/flatten/mem.v:6
  // (* full_case = 1 *)
  assign {\$n98 , \$n97 , \$n96 , \$n95 , \$n94 , \$n93 , \$n92 , \$n91 } = \u2.we ? 8'b11111111 : 8'b00000000;  // tests/flatten/mem.v:6
  (* src = "tests/flatten/mem.v:5.3-8.6|tests/flatten/mem.v:12.7-12.60" *)
  always @(posedge clk) \$c23 <= {\$n78 , \$n77 , \$n76 , \$n75 , \$n74 , \$n73 , \$n72 , \$n71 };  // tests/flatten/mem.v:5
  assign q2 = \$c23 ;
endmodule
