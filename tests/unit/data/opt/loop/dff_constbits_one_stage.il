module \m
  wire input 1 \clk
  wire output 2 \q1
  wire output 3 \q2
  wire output 4 \q3
  cell $dff \f1
    parameter \WIDTH 1
    parameter \CLK_POLARITY 1
    connect \CLK \clk
    connect \D 1'0
    connect \Q \q1
  end
  cell $dff \f2
    parameter \WIDTH 1
    parameter \CLK_POLARITY 1
    connect \CLK \clk
    connect \D \q1
    connect \Q \q2
  end
  cell $dff \f3
    parameter \WIDTH 1
    parameter \CLK_POLARITY 1
    connect \CLK \clk
    connect \D \q2
    connect \Q \q3
  end
end
