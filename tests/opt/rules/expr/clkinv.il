attribute \top 1
module \t
  wire input 1 \clk
  wire input 2 \d
  wire output 3 \q
  wire \nclk
  cell $not $c4
    parameter \A_SIGNED 0
    parameter \A_WIDTH 1
    parameter \Y_WIDTH 1
    connect \A \clk
    connect \Y \nclk
  end
  cell $dff $c7
    parameter \WIDTH 1
    parameter \CLK_POLARITY 0
    connect \CLK \clk
    connect \D \d
    connect \Q \q
  end
end
