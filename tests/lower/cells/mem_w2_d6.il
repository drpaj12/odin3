attribute \top 1
module \m
  wire input 1 \clk
  wire input 2 \we
  wire width 3 input 3 \wa
  wire width 2 input 4 \wd
  wire width 3 input 5 \ra
  wire width 2 output 6 \rd
  wire $n13
  wire $n14
  wire $n15
  wire $n16
  wire $n17
  wire $n21
  wire $n22
  wire $n39
  cell $mem $c11
    parameter \MEMID "\\mem"
    parameter \SIZE 6
    parameter \OFFSET 0
    parameter \ABITS 3
    parameter \WIDTH 2
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
    connect \RD_DATA { $n14 $n13 }
    connect \WR_CLK \clk
    connect \WR_EN { $n39 $n39 }
    connect \WR_ADDR { $n17 $n16 $n15 }
    connect \WR_DATA { $n22 $n21 }
  end
  attribute \full_case 1
  cell $mux $c12
    parameter \WIDTH 3
    connect \A 3'xxx
    connect \B \wa
    connect \S \we
    connect \Y { $n17 $n16 $n15 }
  end
  attribute \full_case 1
  cell $mux $c13
    parameter \WIDTH 2
    connect \A 2'xx
    connect \B \wd
    connect \S \we
    connect \Y { $n22 $n21 }
  end
  attribute \full_case 1
  cell $mux $c15
    parameter \WIDTH 1
    connect \A 1'0
    connect \B 1'1
    connect \S \we
    connect \Y $n39
  end
  connect \rd { $n14 $n13 }
end
