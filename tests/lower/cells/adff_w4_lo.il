attribute \top 1
module \m
  wire input 1 \clk
  wire input 2 \rst
  wire width 4 input 3 \d
  wire width 4 output 4 \q
  cell $adff $c12
    parameter \WIDTH 4
    parameter \CLK_POLARITY 1
    parameter \ARST_POLARITY 0
    parameter \ARST_VALUE 4'0101
    connect \CLK \clk
    connect \ARST \rst
    connect \D \d
    connect \Q \q
  end
end
