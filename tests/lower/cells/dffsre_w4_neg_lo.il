attribute \top 1
module \m
  wire input 1 \clk
  wire input 2 \en
  wire input 3 \set
  wire input 4 \clr
  wire width 4 input 5 \d
  wire width 4 output 6 \q
  wire $n27
  wire $n28
  wire $n29
  wire $n30
  wire $n35
  wire $n36
  wire $n37
  wire $n38
  wire $n40
  wire $n41
  wire $n42
  wire $n43
  wire $n48
  wire $n49
  wire $n50
  wire $n51
  cell $mux $c24
    parameter \WIDTH 4
    connect \A 4'1010
    connect \B 4'0000
    connect \S \set
    connect \Y { $n30 $n29 $n28 $n27 }
  end
  cell $mux $c25
    parameter \WIDTH 4
    connect \A 4'0101
    connect \B 4'0000
    connect \S \set
    connect \Y { $n38 $n37 $n36 $n35 }
  end
  cell $mux $c26
    parameter \WIDTH 4
    connect \A 4'0000
    connect \B { $n30 $n29 $n28 $n27 }
    connect \S \clr
    connect \Y { $n43 $n42 $n41 $n40 }
  end
  cell $mux $c27
    parameter \WIDTH 4
    connect \A 4'1111
    connect \B { $n38 $n37 $n36 $n35 }
    connect \S \clr
    connect \Y { $n51 $n50 $n49 $n48 }
  end
  cell $dffsre $c28
    parameter \WIDTH 4
    parameter \CLK_POLARITY 0
    parameter \SET_POLARITY 1
    parameter \CLR_POLARITY 1
    parameter \EN_POLARITY 0
    connect \CLK \clk
    connect \SET { $n43 $n42 $n41 $n40 }
    connect \CLR { $n51 $n50 $n49 $n48 }
    connect \EN \en
    connect \D \d
    connect \Q \q
  end
end
