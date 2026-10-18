attribute \top 1
module \m
  wire input 1 \clk
  wire input 2 \en
  wire input 3 \rst
  wire width 4 input 4 \d
  wire width 4 output 5 \q
  cell $sdffce $c13
    parameter \WIDTH 4
    parameter \CLK_POLARITY 1
    parameter \EN_POLARITY 1
    parameter \SRST_POLARITY 1
    parameter \SRST_VALUE 4'0101
    connect \CLK \clk
    connect \SRST \rst
    connect \EN \en
    connect \D \d
    connect \Q \q
  end
end
