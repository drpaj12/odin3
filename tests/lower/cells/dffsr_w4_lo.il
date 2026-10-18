attribute \top 1
module \m
  wire input 1 \clk
  wire input 2 \set
  wire input 3 \clr
  wire width 4 input 4 \d
  wire width 4 output 5 \q
  wire $n21
  wire $n22
  wire $n23
  wire $n24
  wire $n29
  wire $n30
  wire $n31
  wire $n32
  wire $n34
  wire $n35
  wire $n36
  wire $n37
  wire $n42
  wire $n43
  wire $n44
  wire $n45
  cell $dffsr $c19
    parameter \WIDTH 4
    parameter \CLK_POLARITY 1
    parameter \SET_POLARITY 1
    parameter \CLR_POLARITY 1
    connect \CLK \clk
    connect \SET { $n37 $n36 $n35 $n34 }
    connect \CLR { $n45 $n44 $n43 $n42 }
    connect \D \d
    connect \Q \q
  end
  cell $mux $c20
    parameter \WIDTH 4
    connect \A 4'1010
    connect \B 4'0000
    connect \S \set
    connect \Y { $n24 $n23 $n22 $n21 }
  end
  cell $mux $c21
    parameter \WIDTH 4
    connect \A 4'0101
    connect \B 4'0000
    connect \S \set
    connect \Y { $n32 $n31 $n30 $n29 }
  end
  cell $mux $c22
    parameter \WIDTH 4
    connect \A 4'0000
    connect \B { $n24 $n23 $n22 $n21 }
    connect \S \clr
    connect \Y { $n37 $n36 $n35 $n34 }
  end
  cell $mux $c23
    parameter \WIDTH 4
    connect \A 4'1111
    connect \B { $n32 $n31 $n30 $n29 }
    connect \S \clr
    connect \Y { $n45 $n44 $n43 $n42 }
  end
end
