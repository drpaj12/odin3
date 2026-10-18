attribute \top 1
module \m
  wire input 1 \clk
  wire input 2 \en
  wire input 3 \d
  wire output 4 \q
  cell $dffe $c9
    parameter \WIDTH 1
    parameter \CLK_POLARITY 1
    parameter \EN_POLARITY 1
    connect \CLK \clk
    connect \EN \en
    connect \D \d
    connect \Q \q
  end
end
