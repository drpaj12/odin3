attribute \top 1
module \m
  wire input 1 \clk
  wire input 2 \en
  wire input 3 \set
  wire input 4 \clr
  wire width 4 input 5 \d
  wire width 4 output 6 \q
  wire $n23
  wire $n24
  wire $n25
  wire $n26
  wire $n31
  wire $n32
  wire $n33
  wire $n34
  wire $n35
  wire $n36
  wire $n37
  wire $n38
  wire $n43
  wire $n44
  wire $n45
  wire $n46
  cell $mux $c11
    parameter \WIDTH 4
    connect \A 4'0000
    connect \B 4'1010
    connect \S \set
    connect \Y { $n26 $n25 $n24 $n23 }
  end
  cell $mux $c13
    parameter \WIDTH 4
    connect \A 4'0000
    connect \B 4'0101
    connect \S \set
    connect \Y { $n34 $n33 $n32 $n31 }
  end
  cell $mux $c14
    parameter \WIDTH 4
    connect \A { $n26 $n25 $n24 $n23 }
    connect \B 4'0000
    connect \S \clr
    connect \Y { $n38 $n37 $n36 $n35 }
  end
  cell $mux $c16
    parameter \WIDTH 4
    connect \A { $n34 $n33 $n32 $n31 }
    connect \B 4'1111
    connect \S \clr
    connect \Y { $n46 $n45 $n44 $n43 }
  end
  cell $dffsre $c18
    parameter \WIDTH 4
    parameter \CLK_POLARITY 0
    parameter \SET_POLARITY 1
    parameter \CLR_POLARITY 1
    parameter \EN_POLARITY 1
    connect \CLK \clk
    connect \SET { $n38 $n37 $n36 $n35 }
    connect \CLR { $n46 $n45 $n44 $n43 }
    connect \EN \en
    connect \D \d
    connect \Q \q
  end
end
