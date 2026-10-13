attribute \top 1
module \t
  wire input 1 \clk
  wire input 2 \d
  wire input 3 \z
  wire input 4 \al
  wire output 5 \q
  wire \ad
  cell $adff $c12
    parameter \WIDTH 1
    parameter \CLK_POLARITY 1
    parameter \ARST_POLARITY 1
    parameter \ARST_VALUE 1'0
    connect \CLK \clk
    connect \ARST \al
    connect \D \d
    connect \Q \q
  end
  connect \ad 1'0
end
