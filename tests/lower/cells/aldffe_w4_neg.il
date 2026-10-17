attribute \top 1
module \m
  wire input 1 \clk
  wire input 2 \en
  wire input 3 \ld
  wire width 4 input 4 \ad
  wire width 4 input 5 \d
  wire width 4 output 6 \q
  cell $aldffe $c12
    parameter \WIDTH 4
    parameter \CLK_POLARITY 0
    parameter \EN_POLARITY 1
    parameter \ALOAD_POLARITY 1
    connect \CLK \clk
    connect \ALOAD \ld
    connect \AD \ad
    connect \EN \en
    connect \D \d
    connect \Q \q
  end
end
