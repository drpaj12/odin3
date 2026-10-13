attribute \top 1
module \t
  wire input 1 \clk
  wire input 2 \d
  wire input 3 \z
  wire input 4 \o
  wire output 5 \q
  wire \cl
  wire $n10
  cell $mux $c10
    parameter \WIDTH 1
    connect \A 1'0
    connect \B 1'1
    connect \S \o
    connect \Y $n10
  end
  cell $adff $c17
    parameter \WIDTH 1
    parameter \CLK_POLARITY 1
    parameter \ARST_POLARITY 1
    parameter \ARST_VALUE 1'1
    connect \CLK \clk
    connect \ARST $n10
    connect \D \d
    connect \Q \q
  end
  connect \cl 1'0
end
