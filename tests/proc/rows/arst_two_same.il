attribute \top 1
module \t
  wire input 1 \clk
  wire input 2 \r1
  wire input 3 \r2
  wire width 4 input 4 \d
  wire width 4 output 5 \q
  wire $n18
  cell $reduce_or $c9
    parameter \A_SIGNED 0
    parameter \A_WIDTH 2
    parameter \Y_WIDTH 1
    connect \A { \r2 \r1 }
    connect \Y $n18
  end
  cell $adff $c10
    parameter \WIDTH 4
    parameter \CLK_POLARITY 1
    parameter \ARST_POLARITY 1
    parameter \ARST_VALUE 4'0000
    connect \CLK \clk
    connect \ARST $n18
    connect \D \d
    connect \Q \q
  end
end
