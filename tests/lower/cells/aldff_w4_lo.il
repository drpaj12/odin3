attribute \top 1
module \m
  wire input 1 \clk
  wire input 2 \ld
  wire width 4 input 3 \ad
  wire width 4 input 4 \d
  wire width 4 output 5 \q
  cell $aldff $c12
    parameter \WIDTH 4
    parameter \CLK_POLARITY 1
    parameter \ALOAD_POLARITY 0
    connect \CLK \clk
    connect \ALOAD \ld
    connect \AD \ad
    connect \D \d
    connect \Q \q
  end
end
