module top (  // tests/flatten/params.v:5
  input [7:0] x,  // tests/flatten/params.v:5
  input [7:0] z,  // tests/flatten/params.v:5
  output [3:0] o,  // tests/flatten/params.v:5
  output [3:0] p,  // tests/flatten/params.v:5
  output [7:0] q  // tests/flatten/params.v:5
);
  wire [3:0] \u4a.a ;  // tests/flatten/params.v:2
  wire [3:0] \u4a.b ;  // tests/flatten/params.v:2
  wire [3:0] \u4a.y ;  // tests/flatten/params.v:2
  wire [3:0] \u4b.a ;  // tests/flatten/params.v:2
  wire [3:0] \u4b.b ;  // tests/flatten/params.v:2
  wire [3:0] \u4b.y ;  // tests/flatten/params.v:2
  wire [7:0] \u8.a ;  // tests/flatten/params.v:2
  wire [7:0] \u8.b ;  // tests/flatten/params.v:2
  wire [7:0] \u8.y ;  // tests/flatten/params.v:2
  wire \$n33 ;  // tests/flatten/params.v:3
  wire \$n34 ;  // tests/flatten/params.v:3
  wire \$n35 ;  // tests/flatten/params.v:3
  wire \$n36 ;  // tests/flatten/params.v:3
  wire \$n37 ;  // tests/flatten/params.v:3
  wire \$n38 ;  // tests/flatten/params.v:3
  wire \$n39 ;  // tests/flatten/params.v:3
  wire \$n40 ;  // tests/flatten/params.v:3
  wire \$n41 ;  // tests/flatten/params.v:3
  wire \$n42 ;  // tests/flatten/params.v:3
  wire \$n43 ;  // tests/flatten/params.v:3
  wire \$n44 ;  // tests/flatten/params.v:3
  wire \$n45 ;  // tests/flatten/params.v:3
  wire \$n46 ;  // tests/flatten/params.v:3
  wire \$n47 ;  // tests/flatten/params.v:3
  wire \$n48 ;  // tests/flatten/params.v:3
  wire \$n49 ;  // tests/flatten/params.v:3
  wire \$n50 ;  // tests/flatten/params.v:3
  wire \$n51 ;  // tests/flatten/params.v:3
  wire \$n52 ;  // tests/flatten/params.v:3
  wire \$n53 ;  // tests/flatten/params.v:3
  wire \$n54 ;  // tests/flatten/params.v:3
  wire \$n55 ;  // tests/flatten/params.v:3
  wire \$n56 ;  // tests/flatten/params.v:3
  wire \$n57 ;  // tests/flatten/params.v:3
  wire \$n58 ;  // tests/flatten/params.v:3
  wire \$n59 ;  // tests/flatten/params.v:3
  wire \$n60 ;  // tests/flatten/params.v:3
  wire \$n61 ;  // tests/flatten/params.v:3
  wire \$n62 ;  // tests/flatten/params.v:3
  wire \$n63 ;  // tests/flatten/params.v:3
  wire \$n64 ;  // tests/flatten/params.v:3
  assign \u4a.a = x[3:0];
  assign \u4a.b = z[3:0];
  assign \u4a.y = o;
  assign \u4b.a = z[3:0];
  assign \u4b.b = x[3:0];
  assign \u4b.y = p;
  assign \u8.a = x;
  assign \u8.b = z;
  assign \u8.y = q;
  assign {\$n36 , \$n35 , \$n34 , \$n33 } = x[3:0] & z[3:0];  // tests/flatten/params.v:3
  assign {\$n40 , \$n39 , \$n38 , \$n37 } = x[3:0] ^ z[3:0];  // tests/flatten/params.v:3
  assign o = {\$n36 , \$n35 , \$n34 , \$n33 } | {\$n40 , \$n39 , \$n38 , \$n37 };  // tests/flatten/params.v:3
  assign {\$n44 , \$n43 , \$n42 , \$n41 } = z[3:0] & x[3:0];  // tests/flatten/params.v:3
  assign {\$n48 , \$n47 , \$n46 , \$n45 } = z[3:0] ^ x[3:0];  // tests/flatten/params.v:3
  assign p = {\$n44 , \$n43 , \$n42 , \$n41 } | {\$n48 , \$n47 , \$n46 , \$n45 };  // tests/flatten/params.v:3
  assign {\$n56 , \$n55 , \$n54 , \$n53 , \$n52 , \$n51 , \$n50 , \$n49 } = x & z;  // tests/flatten/params.v:3
  assign {\$n64 , \$n63 , \$n62 , \$n61 , \$n60 , \$n59 , \$n58 , \$n57 } = x ^ z;  // tests/flatten/params.v:3
  assign q = {\$n56 , \$n55 , \$n54 , \$n53 , \$n52 , \$n51 , \$n50 , \$n49 } | {\$n64 , \$n63 , \$n62 , \$n61 , \$n60 , \$n59 , \$n58 , \$n57 };  // tests/flatten/params.v:3
endmodule
