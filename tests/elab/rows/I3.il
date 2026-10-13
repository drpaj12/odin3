# generated: elab-check --write
attribute \top 1
module \i3_port_exprs
  wire width 2 input 1 \p
  wire width 2 input 2 \q
  wire width 4 input 3 \r
  wire width 2 output 4 \s
  wire width 2 output 5 \t
  wire width 2 output 6 \v
  wire width 2 output 7 \w
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
  cell $add $c10
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 4
    parameter \B_WIDTH 32
    parameter \Y_WIDTH 32
    connect \A \r
    connect \B 32'00000000000000000000000000000001
    connect \Y { $n50 $n49 $n48 $n47 $n46 $n45 $n44 $n43 $n42 $n41 $n40 $n39 $n38 $n37 $n36 $n35 $n34 $n33 $n32 $n31 $n30 $n29 $n28 $n27 $n26 $n25 $n24 $n23 $n22 $n21 $n20 $n19 }
  end
  cell \i3_leaf \u1
    connect \a { $n22 $n21 $n20 $n19 }
    connect \y { \s \t }
  end
  cell \i3_leaf \u2
    connect \a { \p \q }
    connect \y { \v \w }
  end
end
module \i3_leaf
  wire width 4 input 1 \a
  wire width 4 output 2 \y
  cell $not $c3
    parameter \A_SIGNED 0
    parameter \A_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A \a
    connect \Y \y
  end
end
