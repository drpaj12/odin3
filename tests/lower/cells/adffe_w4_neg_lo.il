attribute \top 1
module \m
  wire input 1 \clk
  wire input 2 \en
  wire input 3 \rst
  wire width 4 input 4 \d
  wire width 4 output 5 \q
  cell $adffe $c17
    parameter \WIDTH 4
    parameter \CLK_POLARITY 0
    parameter \EN_POLARITY 0
    parameter \ARST_POLARITY 0
    parameter \ARST_VALUE 4'0101
    connect \CLK \clk
    connect \ARST \rst
    connect \EN \en
    connect \D \d
    connect \Q \q
  end
end
