attribute \top 1
module \t
  wire input 1 \clk
  wire input 2 \rst
  wire width 4 input 3 \d
  wire width 4 input 4 \v
  wire width 4 output 5 \q
  wire $n20
  cell $reduce_or $c8
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \rst
    connect \Y $n20
  end
  cell $aldff $c9
    parameter \WIDTH 4
    parameter \CLK_POLARITY 1
    parameter \ALOAD_POLARITY 1
    connect \CLK \clk
    connect \ALOAD $n20
    connect \AD \v
    connect \D \d
    connect \Q \q
  end
end
