attribute \top 1
module \t
  wire input 1 \clk
  wire width 4 input 2 \d
  attribute \init 4'1001
  wire width 4 output 3 \q
  cell $dff $c5
    parameter \WIDTH 4
    parameter \CLK_POLARITY 1
    connect \CLK \clk
    connect \D \d
    connect \Q \q
  end
end
