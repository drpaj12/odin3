attribute \top 1
module \m
  wire input 1 \clk
  wire input 2 \en
  wire width 8 input 3 \d
  wire width 8 output 4 \q
  cell $dffe $c9
    parameter \WIDTH 8
    parameter \CLK_POLARITY 1
    parameter \EN_POLARITY 1
    connect \CLK \clk
    connect \EN \en
    connect \D \d
    connect \Q \q
  end
end
