attribute \top 1
module \m
  wire input 1 \clk
  wire input 2 \set
  wire input 3 \clr
  wire width 4 input 4 \d
  wire width 4 output 5 \q
  wire $n18
  wire $n19
  wire $n20
  wire $n21
  wire $n26
  wire $n27
  wire $n28
  wire $n29
  wire $n30
  wire $n31
  wire $n32
  wire $n33
  wire $n38
  wire $n39
  wire $n40
  wire $n41
  cell $mux $c9
    parameter \WIDTH 4
    connect \A 4'0000
    connect \B 4'1010
    connect \S \set
    connect \Y { $n21 $n20 $n19 $n18 }
  end
  cell $mux $c11
    parameter \WIDTH 4
    connect \A 4'0000
    connect \B 4'0101
    connect \S \set
    connect \Y { $n29 $n28 $n27 $n26 }
  end
  cell $mux $c12
    parameter \WIDTH 4
    connect \A { $n21 $n20 $n19 $n18 }
    connect \B 4'0000
    connect \S \clr
    connect \Y { $n33 $n32 $n31 $n30 }
  end
  cell $mux $c14
    parameter \WIDTH 4
    connect \A { $n29 $n28 $n27 $n26 }
    connect \B 4'1111
    connect \S \clr
    connect \Y { $n41 $n40 $n39 $n38 }
  end
  cell $dffsr $c15
    parameter \WIDTH 4
    parameter \CLK_POLARITY 0
    parameter \SET_POLARITY 1
    parameter \CLR_POLARITY 1
    connect \CLK \clk
    connect \SET { $n33 $n32 $n31 $n30 }
    connect \CLR { $n41 $n40 $n39 $n38 }
    connect \D \d
    connect \Q \q
  end
end
