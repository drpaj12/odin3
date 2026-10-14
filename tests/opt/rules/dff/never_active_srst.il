attribute \top 1
module \t
  wire input 1 \clk
  wire input 2 \d
  wire input 3 \z
  wire output 4 \q
  wire \r
  cell $dff $c12
    parameter \WIDTH 1
    parameter \CLK_POLARITY 1
    connect \CLK \clk
    connect \D \d
    connect \Q \q
  end
  connect \r 1'0
end
