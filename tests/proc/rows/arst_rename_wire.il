attribute \top 1
module \t
  wire input 1 \clk
  wire input 2 \rst
  wire input 3 \rn
  wire width 4 input 4 \d
  wire width 4 output 5 \a
  wire width 4 output 6 \b
  wire \r2
  wire \r3
  wire $n28
  wire $n29
  wire $n30
  cell $not $c7
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \rn
    connect \Y \r3
  end
  cell $reduce_or $c12
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \rst
    connect \Y $n28
  end
  cell $adff $c13
    parameter \WIDTH 4
    parameter \CLK_POLARITY 1
    parameter \ARST_POLARITY 1
    parameter \ARST_VALUE 4'0000
    connect \CLK \clk
    connect \ARST $n28
    connect \D \d
    connect \Q \a
  end
  cell $not $c14
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \rn
    connect \Y $n29
  end
  cell $reduce_or $c15
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A $n29
    connect \Y $n30
  end
  cell $adff $c16
    parameter \WIDTH 4
    parameter \CLK_POLARITY 1
    parameter \ARST_POLARITY 1
    parameter \ARST_VALUE 4'1001
    connect \CLK \clk
    connect \ARST $n30
    connect \D \d
    connect \Q \b
  end
  connect \r2 \rst
end
