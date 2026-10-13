# generated: elab-check --write
attribute \top 1
module \i5_several
  wire width 4 input 1 \a
  wire width 4 input 2 \b
  wire width 4 output 3 \y1
  wire width 4 output 4 \y2
  wire width 4 output 5 \y3
  wire width 4 output 6 \y4
  attribute \parameter:W 32'00000000000000000000000000000001
  cell \i5_leaf \u1
    connect \a \a
    connect \y \y1
  end
  attribute \parameter:W 32'00000000000000000000000000000001
  cell \i5_leaf \u2
    connect \a \b
    connect \y \y2
  end
  attribute \parameter:W 32'00000000000000000000000000000010
  cell \i5_leaf$W=2 \u3
    connect \a \a
    connect \y \y3
  end
  attribute \parameter:W 32'00000000000000000000000000000010
  cell \i5_leaf$W=2 \u4
    connect \a \b
    connect \y \y4
  end
end
module \i5_leaf
  wire width 4 input 1 \a
  wire width 4 output 2 \y
  wire $n11
  wire $n12
  wire $n13
  wire $n14
  wire $n15
  wire $n16
  wire $n17
  wire $n18
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
  cell $add $c5
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 32
    parameter \Y_WIDTH 32
    connect \A \a
    connect \B 32'00000000000000000000000000000001
    connect \Y { $n42 $n41 $n40 $n39 $n38 $n37 $n36 $n35 $n34 $n33 $n32 $n31 $n30 $n29 $n28 $n27 $n26 $n25 $n24 $n23 $n22 $n21 $n20 $n19 $n18 $n17 $n16 $n15 $n14 $n13 $n12 $n11 }
  end
  connect \y { $n14 $n13 $n12 $n11 }
end
module \i5_leaf$W=2
  wire width 4 input 1 \a
  wire width 4 output 2 \y
  wire $n11
  wire $n12
  wire $n13
  wire $n14
  wire $n15
  wire $n16
  wire $n17
  wire $n18
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
  cell $add $c5
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 32
    parameter \Y_WIDTH 32
    connect \A \a
    connect \B 32'00000000000000000000000000000010
    connect \Y { $n42 $n41 $n40 $n39 $n38 $n37 $n36 $n35 $n34 $n33 $n32 $n31 $n30 $n29 $n28 $n27 $n26 $n25 $n24 $n23 $n22 $n21 $n20 $n19 $n18 $n17 $n16 $n15 $n14 $n13 $n12 $n11 }
  end
  connect \y { $n14 $n13 $n12 $n11 }
end
