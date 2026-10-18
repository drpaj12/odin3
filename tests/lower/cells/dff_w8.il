attribute \top 1
module \m
  wire input 1 \clk
  wire width 8 input 2 \d
  wire width 8 output 3 \q
  cell $dff $c5
    parameter \WIDTH 8
    parameter \CLK_POLARITY 1
    connect \CLK \clk
    connect \D \d
    connect \Q \q
  end
end
