attribute \top 1
module \m
  wire input 1 \clk
  wire input 2 \we
  wire width 3 input 3 \wa
  wire width 3 input 4 \wd
  wire width 3 input 5 \ra
  wire width 3 output 6 \rd
  wire $n15
  wire $n16
  wire $n17
  wire $n18
  wire $n19
  wire $n20
  wire $n24
  wire $n25
  wire $n26
  wire $n48
  cell $mem $c11
    parameter \MEMID "\\mem"
    parameter \SIZE 8
    parameter \OFFSET -2
    parameter \ABITS 3
    parameter \WIDTH 3
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
    connect \RD_DATA { $n17 $n16 $n15 }
    connect \WR_CLK \clk
    connect \WR_EN { $n48 $n48 $n48 }
    connect \WR_ADDR { $n20 $n19 $n18 }
    connect \WR_DATA { $n26 $n25 $n24 }
  end
  attribute \full_case 1
  cell $mux $c12
    parameter \WIDTH 3
    connect \A 3'xxx
    connect \B \wa
    connect \S \we
    connect \Y { $n20 $n19 $n18 }
  end
  attribute \full_case 1
  cell $mux $c13
    parameter \WIDTH 3
    connect \A 3'xxx
    connect \B \wd
    connect \S \we
    connect \Y { $n26 $n25 $n24 }
  end
  attribute \full_case 1
  cell $mux $c15
    parameter \WIDTH 1
    connect \A 1'0
    connect \B 1'1
    connect \S \we
    connect \Y $n48
  end
  connect \rd { $n17 $n16 $n15 }
end
