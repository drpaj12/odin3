attribute \top 1
module \m
  wire input 1 \clk
  wire input 2 \en
  wire input 3 \set
  wire input 4 \clr
  wire input 5 \d
  wire output 6 \q
  wire $n11
  wire $n14
  wire $n16
  cell $mux $c11
    parameter \WIDTH 1
    connect \A 1'0
    connect \B 1'1
    connect \S \set
    connect \Y $n11
  end
  cell $mux $c14
    parameter \WIDTH 1
    connect \A $n11
    connect \B 1'0
    connect \S \clr
    connect \Y $n14
  end
  cell $mux $c16
    parameter \WIDTH 1
    connect \A 1'0
    connect \B 1'1
    connect \S \clr
    connect \Y $n16
  end
  cell $dffsre $c18
    parameter \WIDTH 1
    parameter \CLK_POLARITY 1
    parameter \SET_POLARITY 1
    parameter \CLR_POLARITY 1
    parameter \EN_POLARITY 1
    connect \CLK \clk
    connect \SET $n14
    connect \CLR $n16
    connect \EN \en
    connect \D \d
    connect \Q \q
  end
end
