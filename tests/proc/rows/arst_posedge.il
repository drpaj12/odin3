attribute \top 1
module \t
  wire input 1 \clk
  wire input 2 \rst
  wire width 4 input 3 \d
  wire width 4 output 4 \q
  wire $n17
  cell $reduce_or $c8
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \rst
    connect \Y $n17
  end
  cell $adff $c9
    parameter \WIDTH 4
    parameter \CLK_POLARITY 1
    parameter \ARST_POLARITY 1
    parameter \ARST_VALUE 4'0000
    connect \CLK \clk
    connect \ARST $n17
    connect \D \d
    connect \Q \q
  end
end
