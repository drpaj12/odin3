attribute \top 1
module \m
  wire input 1 \clk
  wire input 2 \en
  wire input 3 \ld
  wire input 4 \ad
  wire input 5 \d
  wire output 6 \q
  cell $aldffe $c12
    parameter \WIDTH 1
    parameter \CLK_POLARITY 1
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
