attribute \top 1
module \m
  wire input 1 \clk
  wire input 2 \en
  wire input 3 \rst
  wire width 8 input 4 \d
  wire width 8 output 5 \q
  cell $adffe $c12
    parameter \WIDTH 8
    parameter \CLK_POLARITY 1
    parameter \EN_POLARITY 1
    parameter \ARST_POLARITY 1
    parameter \ARST_VALUE 8'10100101
    connect \CLK \clk
    connect \ARST \rst
    connect \EN \en
    connect \D \d
    connect \Q \q
  end
end
