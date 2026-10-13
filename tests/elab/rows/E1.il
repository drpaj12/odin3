# generated: elab-check --write
attribute \top 1
module \e1_top
  wire width 4 input 1 \a
  wire width 4 input 2 \b
  wire width 4 input 3 \c
  wire width 4 output 4 \y1
  wire width 4 output 5 \y2
  wire width 4 output 6 \y3
  wire width 4 output 7 \y4
  wire width 4 output 8 \y5
  wire width 4 output 9 \y6
  wire width 4 output 10 \y7
  wire width 4 output 11 \y8
  wire width 4 output 12 \y9
  wire width 4 output 13 \y10
  wire width 4 output 14 \z1
  wire width 4 output 15 \z2
  wire width 4 output 16 \z3
  wire width 4 output 17 \z4
  cell \e1_bitwise \u1
    connect \a \a
    connect \b \b
    connect \c \c
    connect \y1 \y1
    connect \y2 \y2
    connect \y3 \y3
    connect \y4 \y4
    connect \y5 \y5
    connect \y6 \y6
    connect \y7 \y7
    connect \y8 \y8
    connect \y9 \y9
    connect \y10 \y10
  end
  cell \e1_binary_nand_nor \u2
    connect \a \a
    connect \b \b
    connect \c \c
    connect \y1 \z1
    connect \y2 \z2
    connect \y3 \z3
    connect \y4 \z4
  end
end
module \e1_bitwise
  wire width 4 input 1 \a
  wire width 4 input 2 \b
  wire width 4 input 3 \c
  wire width 4 output 4 \y1
  wire width 4 output 5 \y2
  wire width 4 output 6 \y3
  wire width 4 output 7 \y4
  wire width 4 output 8 \y5
  wire width 4 output 9 \y6
  wire width 4 output 10 \y7
  wire width 4 output 11 \y8
  wire width 4 output 12 \y9
  wire width 4 output 13 \y10
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
  wire $n65
  wire $n66
  wire $n67
  wire $n68
  cell $not $c14
    parameter \A_SIGNED 0
    parameter \A_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \a
    connect \Y \y1
  end
  cell $and $c15
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \a
    connect \B \b
    connect \Y \y2
  end
  cell $or $c16
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \a
    connect \B \b
    connect \Y \y3
  end
  cell $xor $c17
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \a
    connect \B \b
    connect \Y \y4
  end
  cell $xnor $c18
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \a
    connect \B \b
    connect \Y \y5
  end
  cell $xnor $c19
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \a
    connect \B \b
    connect \Y \y6
  end
  cell $and $c20
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \a
    connect \B \b
    connect \Y { $n56 $n55 $n54 $n53 }
  end
  cell $or $c21
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A { $n56 $n55 $n54 $n53 }
    connect \B \c
    connect \Y \y7
  end
  cell $and $c22
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \b
    connect \B \c
    connect \Y { $n60 $n59 $n58 $n57 }
  end
  cell $or $c23
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \a
    connect \B { $n60 $n59 $n58 $n57 }
    connect \Y \y8
  end
  cell $xor $c24
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \b
    connect \B \c
    connect \Y { $n64 $n63 $n62 $n61 }
  end
  cell $or $c25
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \a
    connect \B { $n64 $n63 $n62 $n61 }
    connect \Y \y9
  end
  cell $and $c26
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \b
    connect \B \c
    connect \Y { $n68 $n67 $n66 $n65 }
  end
  cell $xor $c27
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \a
    connect \B { $n68 $n67 $n66 $n65 }
    connect \Y \y10
  end
end
module \e1_binary_nand_nor
  wire width 4 input 1 \a
  wire width 4 input 2 \b
  wire width 4 input 3 \c
  wire width 4 output 4 \y1
  wire width 4 output 5 \y2
  wire width 4 output 6 \y3
  wire width 4 output 7 \y4
  wire $n29
  wire $n30
  wire $n31
  wire $n32
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
  cell $and $c8
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \a
    connect \B \b
    connect \Y { $n32 $n31 $n30 $n29 }
  end
  cell $not $c9
    parameter \A_SIGNED 0
    parameter \A_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A { $n32 $n31 $n30 $n29 }
    connect \Y \y1
  end
  cell $or $c10
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \a
    connect \B \b
    connect \Y { $n36 $n35 $n34 $n33 }
  end
  cell $not $c11
    parameter \A_SIGNED 0
    parameter \A_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A { $n36 $n35 $n34 $n33 }
    connect \Y \y2
  end
  attribute \k 1
  cell $and $c12
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \a
    connect \B \b
    connect \Y { $n40 $n39 $n38 $n37 }
  end
  cell $not $c13
    parameter \A_SIGNED 0
    parameter \A_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A { $n40 $n39 $n38 $n37 }
    connect \Y { $n44 $n43 $n42 $n41 }
  end
  cell $or $c14
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A { $n44 $n43 $n42 $n41 }
    connect \B \c
    connect \Y \y3
  end
  cell $xor $c15
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \b
    connect \B \c
    connect \Y { $n48 $n47 $n46 $n45 }
  end
  cell $or $c16
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \a
    connect \B { $n48 $n47 $n46 $n45 }
    connect \Y { $n52 $n51 $n50 $n49 }
  end
  cell $not $c17
    parameter \A_SIGNED 0
    parameter \A_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A { $n52 $n51 $n50 $n49 }
    connect \Y \y4
  end
end
