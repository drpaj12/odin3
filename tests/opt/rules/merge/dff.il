attribute \top 1
module \t
  wire input 1 \clk
  wire input 2 \d
  wire output 3 \q
  wire output 4 \r
  cell $dff $c9
    parameter \WIDTH 1
    parameter \CLK_POLARITY 1
    connect \CLK \clk
    connect \D \d
    connect \Q \q
  end
  connect \r \q
end
