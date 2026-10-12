module \top
  wire input 1 \a
  wire input 2 \b
  wire input 3 \c
  wire input 4 \s
  wire input 5 \t
  wire input 6 \clk
  wire input 7 \ad
  wire width 2 input 8 \d
  wire width 2 output 9 \q
  wire width 2 \w
  wire width 2 \en
  cell $mux \m
    parameter \WIDTH 2
    connect \A { \a \a }
    connect \B { \b \b }
    connect \S \s
    connect \Y \w
  end
  cell $dff \f
    parameter \CLK_POLARITY 1
    parameter \WIDTH 2
    connect \CLK \clk
    connect \D \w
    connect \Q \en
  end
  cell $mem \mem
    parameter \MEMID "$mem1"
    parameter \SIZE 2
    parameter \OFFSET 0
    parameter \ABITS 1
    parameter \WIDTH 2
    parameter \INIT 4'xxxx
    parameter \RD_PORTS 1
    parameter \RD_CLK_ENABLE 1'0
    parameter \RD_CLK_POLARITY 1'0
    parameter \RD_TRANSPARENT 1'0
    parameter \WR_PORTS 1
    parameter \WR_CLK_ENABLE 1'1
    parameter \WR_CLK_POLARITY 1'1
    connect \RD_CLK 1'0
    connect \RD_EN 1'1
    connect \RD_ADDR \ad
    connect \RD_DATA \q
    connect \WR_CLK \clk
    connect \WR_EN \en
    connect \WR_ADDR \ad
    connect \WR_DATA \d
  end
end
