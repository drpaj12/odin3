attribute \top 1
module \t
  wire input 1 \clk
  wire input 2 \rst_n
  wire width 4 input 3 \d
  wire width 4 output 4 \q
  wire $n15
  wire $n18
  wire $n19
  cell $logic_not $c5
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \rst_n
    connect \Y $n15
  end
  cell $not $c9
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \rst_n
    connect \Y $n18
  end
  cell $reduce_or $c10
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A $n18
    connect \Y $n19
  end
  cell $adff $c11
    parameter \WIDTH 4
    parameter \CLK_POLARITY 0
    parameter \ARST_POLARITY 1
    parameter \ARST_VALUE 4'0101
    connect \CLK \clk
    connect \ARST $n19
    connect \D \d
    connect \Q \q
  end
end
