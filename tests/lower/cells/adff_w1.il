attribute \top 1
module \m
  wire input 1 \clk
  wire input 2 \rst
  wire input 3 \d
  wire output 4 \q
  cell $adff $c8
    parameter \WIDTH 1
    parameter \CLK_POLARITY 1
    parameter \ARST_POLARITY 1
    parameter \ARST_VALUE 1'1
    connect \CLK \clk
    connect \ARST \rst
    connect \D \d
    connect \Q \q
  end
end
