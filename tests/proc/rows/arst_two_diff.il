attribute \top 1
module \t
  wire input 1 \clk
  wire input 2 \r1
  wire input 3 \r2
  wire width 4 input 4 \d
  wire width 4 output 5 \q
  wire $n16
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
  cell $logic_not $c6
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \r2
    connect \Y $n16
  end
  cell $not $c10
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \r2
    connect \Y $n19
  end
  cell $mux $c11
    parameter \WIDTH 4
    connect \A 4'0000
    connect \B 4'1111
    connect \S $n19
    connect \Y { $n23 $n22 $n21 $n20 }
  end
  cell $not $c12
    parameter \A_SIGNED 0
    parameter \A_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A 4'1111
    connect \Y { $n27 $n26 $n25 $n24 }
  end
  cell $mux $c13
    parameter \WIDTH 4
    connect \A 4'0000
    connect \B { $n27 $n26 $n25 $n24 }
    connect \S $n19
    connect \Y { $n31 $n30 $n29 $n28 }
  end
  cell $mux $c14
    parameter \WIDTH 4
    connect \A { $n23 $n22 $n21 $n20 }
    connect \B 4'0000
    connect \S \r1
    connect \Y { $n35 $n34 $n33 $n32 }
  end
  cell $not $c15
    parameter \A_SIGNED 0
    parameter \A_WIDTH 4
    parameter \Y_WIDTH 4
    connect \A 4'0000
    connect \Y { $n39 $n38 $n37 $n36 }
  end
  cell $mux $c16
    parameter \WIDTH 4
    connect \A { $n31 $n30 $n29 $n28 }
    connect \B { $n39 $n38 $n37 $n36 }
    connect \S \r1
    connect \Y { $n43 $n42 $n41 $n40 }
  end
  cell $dffsr $c17
    parameter \WIDTH 4
    parameter \CLK_POLARITY 1
    parameter \SET_POLARITY 1
    parameter \CLR_POLARITY 1
    connect \CLK \clk
    connect \SET { $n35 $n34 $n33 $n32 }
    connect \CLR { $n43 $n42 $n41 $n40 }
    connect \D \d
    connect \Q \q
  end
end
