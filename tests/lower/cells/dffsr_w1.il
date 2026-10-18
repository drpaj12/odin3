attribute \top 1
module \m
  wire input 1 \clk
  wire input 2 \set
  wire input 3 \clr
  wire input 4 \d
  wire output 5 \q
  wire $n9
  wire $n12
  wire $n14
  cell $mux $c9
    parameter \WIDTH 1
    connect \A 1'0
    connect \B 1'1
    connect \S \set
    connect \Y $n9
  end
  cell $mux $c12
    parameter \WIDTH 1
    connect \A $n9
    connect \B 1'0
    connect \S \clr
    connect \Y $n12
  end
  cell $mux $c14
    parameter \WIDTH 1
    connect \A 1'0
    connect \B 1'1
    connect \S \clr
    connect \Y $n14
  end
  cell $dffsr $c15
    parameter \WIDTH 1
    parameter \CLK_POLARITY 1
    parameter \SET_POLARITY 1
    parameter \CLR_POLARITY 1
    connect \CLK \clk
    connect \SET $n12
    connect \CLR $n14
    connect \D \d
    connect \Q \q
  end
end
