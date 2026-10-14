module \top
  wire width 8 input 1 \x
  wire width 8 input 2 \z
  wire width 4 output 3 \o
  wire width 4 output 4 \p
  wire width 8 output 5 \q
  wire width 4 \u4a.a
  wire width 4 \u4a.b
  wire width 4 \u4a.y
  wire width 4 \u4b.a
  wire width 4 \u4b.b
  wire width 4 \u4b.y
  wire width 8 \u8.a
  wire width 8 \u8.b
  wire width 8 \u8.y
  wire $n33
  wire $n34
  wire $n35
  wire $n36
  wire $n37
  wire $n38
  wire $n39
  wire $n40
  wire $n41
  wire $n42
  wire $n43
  wire $n44
  wire $n45
  wire $n46
  wire $n47
  wire $n48
  wire $n49
  wire $n50
  wire $n51
  wire $n52
  wire $n53
  wire $n54
  wire $n55
  wire $n56
  wire $n57
  wire $n58
  wire $n59
  wire $n60
  wire $n61
  wire $n62
  wire $n63
  wire $n64
  cell $and $c9
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \x [3:0]
    connect \B \z [3:0]
    connect \Y { $n36 $n35 $n34 $n33 }
  end
  cell $xor $c10
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \x [3:0]
    connect \B \z [3:0]
    connect \Y { $n40 $n39 $n38 $n37 }
  end
  cell $or $c11
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A { $n36 $n35 $n34 $n33 }
    connect \B { $n40 $n39 $n38 $n37 }
    connect \Y \o
  end
  cell $and $c12
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \z [3:0]
    connect \B \x [3:0]
    connect \Y { $n44 $n43 $n42 $n41 }
  end
  cell $xor $c13
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \z [3:0]
    connect \B \x [3:0]
    connect \Y { $n48 $n47 $n46 $n45 }
  end
  cell $or $c14
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A { $n44 $n43 $n42 $n41 }
    connect \B { $n48 $n47 $n46 $n45 }
    connect \Y \p
  end
  cell $and $c15
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 8
    parameter \Y_WIDTH 8
    connect \A \x
    connect \B \z
    connect \Y { $n56 $n55 $n54 $n53 $n52 $n51 $n50 $n49 }
  end
  cell $xor $c16
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 8
    parameter \Y_WIDTH 8
    connect \A \x
    connect \B \z
    connect \Y { $n64 $n63 $n62 $n61 $n60 $n59 $n58 $n57 }
  end
  cell $or $c17
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 8
    parameter \Y_WIDTH 8
    connect \A { $n56 $n55 $n54 $n53 $n52 $n51 $n50 $n49 }
    connect \B { $n64 $n63 $n62 $n61 $n60 $n59 $n58 $n57 }
    connect \Y \q
  end
  connect \u4a.a \x [3:0]
  connect \u4a.b \z [3:0]
  connect \u4a.y \o
  connect \u4b.a \z [3:0]
  connect \u4b.b \x [3:0]
  connect \u4b.y \p
  connect \u8.a \x
  connect \u8.b \z
  connect \u8.y \q
end
