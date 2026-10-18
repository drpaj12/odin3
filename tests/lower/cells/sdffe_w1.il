attribute \top 1
module \m
  wire input 1 \clk
  wire input 2 \en
  wire input 3 \rst
  wire input 4 \d
  wire output 5 \q
  cell $sdffe $c12
    parameter \WIDTH 1
    parameter \CLK_POLARITY 1
    parameter \EN_POLARITY 1
    parameter \SRST_POLARITY 1
    parameter \SRST_VALUE 1'1
    connect \CLK \clk
    connect \SRST \rst
    connect \EN \en
    connect \D \d
    connect \Q \q
  end
end
