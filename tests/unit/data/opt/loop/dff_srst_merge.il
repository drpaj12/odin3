module \m
  wire input 1 \clk
  wire input 2 \d
  wire input 3 \r
  wire input 4 \e
  wire input 5 \a
  wire output 6 \q
  wire \m1
  wire \m2
  cell $mux \mx
    parameter \WIDTH 1
    connect \A \d
    connect \B 1'0
    connect \S \r
    connect \Y \m1
  end
  cell $dff \f
    parameter \WIDTH 1
    parameter \CLK_POLARITY 1
    connect \CLK \clk
    connect \D \m1
    connect \Q \q
  end
end
