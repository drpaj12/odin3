# generated: elab-check --write
attribute \top 1
module \p6_defparam
  wire width 8 input 1 \a
  wire width 8 output 2 \y1
  wire width 8 output 3 \y2
  wire width 16 output 4 \y3
  wire $n41
  wire $n42
  wire $n43
  wire $n44
  wire $n45
  wire $n46
  wire $n47
  wire $n48
  attribute \parameter:W 32'00000000000000000000000000000010
  cell \p6_leaf$W=2 \u1
    connect \a \a
    connect \y \y1
  end
  cell \p6_mid$u_leaf.W=3 \u_mid
    connect \a \a
    connect \y \y2
  end
  attribute \parameter:W 32'00000000000000000000000000000101
  cell \p6_leaf$W=5 \g[0].u
    connect \a \a
    connect \y \y3 [7:0]
  end
  attribute \parameter:W 32'00000000000000000000000000000001
  cell \p6_leaf \g[1].u
    connect \a \a
    connect \y \y3 [15:8]
  end
  attribute \parameter:W 32'00000000000000000000000000000110
  cell \p6_leaf$W=6 \h[0].u
    connect \a \a
    connect \y { $n48 $n47 $n46 $n45 $n44 $n43 $n42 $n41 }
  end
end
module \p6_leaf$W=2
  wire width 8 input 1 \a
  wire width 8 output 2 \y
  wire $n19
  wire $n20
  wire $n21
  wire $n22
  wire $n23
  wire $n24
  wire $n25
  wire $n26
  wire $n27
  wire $n28
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
  cell $add $c5
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 32
    parameter \Y_WIDTH 32
    connect \A \a
    connect \B 32'00000000000000000000000000000010
    connect \Y { $n50 $n49 $n48 $n47 $n46 $n45 $n44 $n43 $n42 $n41 $n40 $n39 $n38 $n37 $n36 $n35 $n34 $n33 $n32 $n31 $n30 $n29 $n28 $n27 $n26 $n25 $n24 $n23 $n22 $n21 $n20 $n19 }
  end
  connect \y { $n26 $n25 $n24 $n23 $n22 $n21 $n20 $n19 }
end
module \p6_mid$u_leaf.W=3
  wire width 8 input 1 \a
  wire width 8 output 2 \y
  attribute \parameter:W 32'00000000000000000000000000000011
  cell \p6_leaf$W=3 \u_leaf
    connect \a \a
    connect \y \y
  end
end
module \p6_leaf$W=5
  wire width 8 input 1 \a
  wire width 8 output 2 \y
  wire $n19
  wire $n20
  wire $n21
  wire $n22
  wire $n23
  wire $n24
  wire $n25
  wire $n26
  wire $n27
  wire $n28
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
  cell $add $c5
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 32
    parameter \Y_WIDTH 32
    connect \A \a
    connect \B 32'00000000000000000000000000000101
    connect \Y { $n50 $n49 $n48 $n47 $n46 $n45 $n44 $n43 $n42 $n41 $n40 $n39 $n38 $n37 $n36 $n35 $n34 $n33 $n32 $n31 $n30 $n29 $n28 $n27 $n26 $n25 $n24 $n23 $n22 $n21 $n20 $n19 }
  end
  connect \y { $n26 $n25 $n24 $n23 $n22 $n21 $n20 $n19 }
end
module \p6_leaf
  wire width 8 input 1 \a
  wire width 8 output 2 \y
  wire $n19
  wire $n20
  wire $n21
  wire $n22
  wire $n23
  wire $n24
  wire $n25
  wire $n26
  wire $n27
  wire $n28
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
  cell $add $c5
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 32
    parameter \Y_WIDTH 32
    connect \A \a
    connect \B 32'00000000000000000000000000000001
    connect \Y { $n50 $n49 $n48 $n47 $n46 $n45 $n44 $n43 $n42 $n41 $n40 $n39 $n38 $n37 $n36 $n35 $n34 $n33 $n32 $n31 $n30 $n29 $n28 $n27 $n26 $n25 $n24 $n23 $n22 $n21 $n20 $n19 }
  end
  connect \y { $n26 $n25 $n24 $n23 $n22 $n21 $n20 $n19 }
end
module \p6_leaf$W=6
  wire width 8 input 1 \a
  wire width 8 output 2 \y
  wire $n19
  wire $n20
  wire $n21
  wire $n22
  wire $n23
  wire $n24
  wire $n25
  wire $n26
  wire $n27
  wire $n28
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
  cell $add $c5
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 32
    parameter \Y_WIDTH 32
    connect \A \a
    connect \B 32'00000000000000000000000000000110
    connect \Y { $n50 $n49 $n48 $n47 $n46 $n45 $n44 $n43 $n42 $n41 $n40 $n39 $n38 $n37 $n36 $n35 $n34 $n33 $n32 $n31 $n30 $n29 $n28 $n27 $n26 $n25 $n24 $n23 $n22 $n21 $n20 $n19 }
  end
  connect \y { $n26 $n25 $n24 $n23 $n22 $n21 $n20 $n19 }
end
module \p6_leaf$W=3
  wire width 8 input 1 \a
  wire width 8 output 2 \y
  wire $n19
  wire $n20
  wire $n21
  wire $n22
  wire $n23
  wire $n24
  wire $n25
  wire $n26
  wire $n27
  wire $n28
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
  cell $add $c5
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 8
    parameter \B_WIDTH 32
    parameter \Y_WIDTH 32
    connect \A \a
    connect \B 32'00000000000000000000000000000011
    connect \Y { $n50 $n49 $n48 $n47 $n46 $n45 $n44 $n43 $n42 $n41 $n40 $n39 $n38 $n37 $n36 $n35 $n34 $n33 $n32 $n31 $n30 $n29 $n28 $n27 $n26 $n25 $n24 $n23 $n22 $n21 $n20 $n19 }
  end
  connect \y { $n26 $n25 $n24 $n23 $n22 $n21 $n20 $n19 }
end
