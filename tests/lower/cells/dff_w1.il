attribute \top 1
module \m
  wire input 1 \clk
  wire input 2 \d
  wire output 3 \q
  cell $dff $c5
    parameter \WIDTH 1
    parameter \CLK_POLARITY 1
    connect \CLK \clk
    connect \D \d
    connect \Q \q
  end
end
