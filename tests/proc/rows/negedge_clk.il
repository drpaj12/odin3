attribute \top 1
module \t
  wire input 1 \clk
  wire width 4 input 2 \d
  wire width 4 output 3 \q
  cell $dff $c5
    parameter \WIDTH 4
    parameter \CLK_POLARITY 0
    connect \CLK \clk
    connect \D \d
    connect \Q \q
  end
end
