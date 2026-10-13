attribute \top 1
module \t
  wire input 1 \clk
  wire input 2 \rst
  wire width 4 input 3 \d
  wire width 4 input 4 \e
  wire width 4 output 5 \a
  wire width 4 output 6 \b
  wire $n29
  wire $n30
  wire $n31
  wire $n32
  wire $n33
  wire $n34
  cell $reduce_or $c10
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \rst
    connect \Y $n29
  end
  cell $adff $c11
    parameter \WIDTH 4
    parameter \CLK_POLARITY 1
    parameter \ARST_POLARITY 1
    parameter \ARST_VALUE 4'0000
    connect \CLK \clk
    connect \ARST $n29
    connect \D \d
    connect \Q \a
  end
  cell $reduce_or $c12
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \rst
    connect \Y $n30
  end
  cell $mux $c13
    parameter \WIDTH 4
    connect \A \e
    connect \B \b
    connect \S $n30
    connect \Y { $n34 $n33 $n32 $n31 }
  end
  cell $dff $c14
    parameter \WIDTH 4
    parameter \CLK_POLARITY 1
    connect \CLK \clk
    connect \D { $n34 $n33 $n32 $n31 }
    connect \Q \b
  end
end
