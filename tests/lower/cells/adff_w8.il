attribute \top 1
module \m
  wire input 1 \clk
  wire input 2 \rst
  wire width 8 input 3 \d
  wire width 8 output 4 \q
  cell $adff $c9
    parameter \WIDTH 8
    parameter \CLK_POLARITY 1
    parameter \ARST_POLARITY 1
    parameter \ARST_VALUE 8'10100101
    connect \CLK \clk
    connect \ARST \rst
    connect \D \d
    connect \Q \q
  end
end
