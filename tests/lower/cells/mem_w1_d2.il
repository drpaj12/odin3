attribute \top 1
module \m
  wire input 1 \clk
  wire input 2 \we
  wire input 3 \wa
  wire input 4 \wd
  wire input 5 \ra
  wire output 6 \rd
  wire $n7
  wire $n8
  wire $n10
  wire $n12
  cell $mem $c11
    parameter \MEMID "\\mem"
    parameter \SIZE 2
    parameter \OFFSET 0
    parameter \ABITS 1
    parameter \WIDTH 1
    parameter \RD_PORTS 1
    parameter \WR_PORTS 1
    parameter \RD_CLK_ENABLE 1'0
    parameter \RD_CLK_POLARITY 1'0
    parameter \RD_TRANSPARENT 1'0
    parameter \WR_CLK_ENABLE 1'1
    parameter \WR_CLK_POLARITY 1'1
    parameter \INIT 0'
    connect \RD_CLK 1'x
    connect \RD_EN 1'1
    connect \RD_ADDR \ra
    connect \RD_DATA $n7
    connect \WR_CLK \clk
    connect \WR_EN $n12
    connect \WR_ADDR $n8
    connect \WR_DATA $n10
  end
  attribute \full_case 1
  cell $mux $c12
    parameter \WIDTH 1
    connect \A 1'x
    connect \B \wa
    connect \S \we
    connect \Y $n8
  end
  attribute \full_case 1
  cell $mux $c13
    parameter \WIDTH 1
    connect \A 1'x
    connect \B \wd
    connect \S \we
    connect \Y $n10
  end
  attribute \full_case 1
  cell $mux $c14
    parameter \WIDTH 1
    connect \A 1'0
    connect \B 1'1
    connect \S \we
    connect \Y $n12
  end
  connect \rd $n7
end
