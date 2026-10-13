attribute \top 1
module \t
  wire input 1 \clk
  wire input 2 \r1
  wire input 3 \r2
  wire input 4 \r3
  wire width 4 input 5 \d
  wire width 4 output 6 \a
  wire width 4 output 7 \b
  wire width 4 output 8 \c
  wire $n26
  wire $n32
  wire $n37
  wire $n38
  wire $n39
  wire $n40
  wire $n41
  wire $n42
  cell $eq $c10
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \r1
    connect \B 1'1
    connect \Y $n26
  end
  cell $eq $c13
    parameter \A_SIGNED 0
    parameter \B_SIGNED 0
    parameter \A_WIDTH 1
    parameter \B_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \r2
    connect \B 1'0
    connect \Y $n32
  end
  cell $not $c15
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \r3
    connect \Y $n37
  end
  cell $reduce_or $c17
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \r1
    connect \Y $n38
  end
  cell $adff $c18
    parameter \WIDTH 4
    parameter \CLK_POLARITY 1
    parameter \ARST_POLARITY 1
    parameter \ARST_VALUE 4'0001
    connect \CLK \clk
    connect \ARST $n38
    connect \D \d
    connect \Q \a
  end
  cell $not $c19
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \r2
    connect \Y $n39
  end
  cell $reduce_or $c20
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A $n39
    connect \Y $n40
  end
  cell $adff $c21
    parameter \WIDTH 4
    parameter \CLK_POLARITY 1
    parameter \ARST_POLARITY 1
    parameter \ARST_VALUE 4'0010
    connect \CLK \clk
    connect \ARST $n40
    connect \D \d
    connect \Q \b
  end
  cell $not $c22
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \r3
    connect \Y $n41
  end
  cell $reduce_or $c23
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A $n41
    connect \Y $n42
  end
  cell $adff $c24
    parameter \WIDTH 4
    parameter \CLK_POLARITY 1
    parameter \ARST_POLARITY 1
    parameter \ARST_VALUE 4'0011
    connect \CLK \clk
    connect \ARST $n42
    connect \D \d
    connect \Q \c
  end
end
